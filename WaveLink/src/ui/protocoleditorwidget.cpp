#include "protocoleditorwidget.h"
#include "ProtocolChecksum.h"
#include "protocolchecksumhelp.h"
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QStyledItemDelegate>
#include <QValidator>
#include <QRegularExpressionValidator>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QFontMetrics>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QDialog>
#include <QDialogButtonBox>
#include <QStandardItemModel>
#include <QTextDocument>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QScrollArea>
#include <QtMath>

namespace {
class ProtocolDescriptionEdit : public QPlainTextEdit {
public:
    ProtocolDescriptionEdit() {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        connect(this, &QPlainTextEdit::textChanged, this, [this] { queueFit(hasFocus()); });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { queueFit(hasFocus()); });
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this] { queueFit(); });
        queueFit();
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QPlainTextEdit::resizeEvent(event); queueFit();
    }
    void showEvent(QShowEvent *event) override {
        QPlainTextEdit::showEvent(event); queueFit();
    }
    void changeEvent(QEvent *event) override {
        QPlainTextEdit::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) queueFit();
    }
    void wheelEvent(QWheelEvent *event) override {
        // There is only one scroll surface: the enclosing configuration page.
        event->ignore();
    }
private:
    void queueFit(bool followCursor = false) {
        if (m_fitting) return;
        m_followCursor = m_followCursor || followCursor;
        if (m_pending) return;
        m_pending = true;
        QTimer::singleShot(0, this, [this] {
            m_pending = false; m_fitting = true;
            // QPlainTextDocumentLayout::documentSize().height() counts lines,
            // not pixels. Lay out every block, including offscreen wrapped text.
            qreal content = 2 * document()->documentMargin();
            for (auto block = document()->begin(); block.isValid(); block = block.next())
                content += blockBoundingRect(block).height();
            const int chrome = 2 * frameWidth() + contentsMargins().top() + contentsMargins().bottom();
            const int wanted = qMin(QWIDGETSIZE_MAX, qMax(3 * fontMetrics().lineSpacing() + chrome, qCeil(content) + chrome + 2));
            if (height() != wanted || minimumHeight() != wanted || maximumHeight() != wanted) setFixedHeight(wanted);
            // Editing can move the internal scroll position before layout grows.
            verticalScrollBar()->setValue(0); horizontalScrollBar()->setValue(0);
            m_fitting = false;
            const bool follow = m_followCursor; m_followCursor = false;
            if (follow) QTimer::singleShot(0, this, [this] {
                if (!hasFocus()) return;
                for (auto parent = parentWidget(); parent; parent = parent->parentWidget())
                    if (auto scroll = qobject_cast<QScrollArea *>(parent)) {
                        const auto position = viewport()->mapTo(scroll->widget(), cursorRect().center());
                        scroll->ensureVisible(position.x(), position.y(), 0, fontMetrics().lineSpacing()); break;
                    }
            });
        });
    }
    bool m_pending = false, m_fitting = false, m_followCursor = false;
};
QString upperHex(QString text) {
    // Only convert ASCII a-f. Unicode case folding (for example ﬀ -> FF)
    // must not turn a forbidden character into apparently valid HEX bytes.
    for (int i = 0; i < text.size(); ++i) if (text.at(i) >= 'a' && text.at(i) <= 'f')
        text[i] = QChar(text.at(i).unicode() - ('a' - 'A'));
    return text;
}
class HexValidator : public QValidator {
public:
    explicit HexValidator(QObject *parent) : QValidator(parent) {}
    State validate(QString &input, int &) const override {
        // Reject the entire invalid insertion/paste; never silently strip bytes.
        // Use exactly the same character rules as the persisted HEX parser.
        for (QChar ch : input) if (!ch.isSpace() &&
            !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return Invalid;
        input = upperHex(input);
        // Odd nibble counts and empty required fields remain editable. The
        // profile validator still checks complete bytes/limits before saving.
        return Acceptable;
    }
};
class HexLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit HexLineEdit(const QString &value, QWidget *parent) : QLineEdit(upperHex(value), parent) {
        setValidator(new HexValidator(this));
    }
public slots:
    // The native context menu resolves this slot on the derived meta-object.
    // Normalize BEFORE inserting: Qt 5.14 clears undo history when a validator
    // rewrites the completed text. Never change the system clipboard instead.
    void paste() {
        if (isReadOnly()) return;
        const QString content = upperHex(QApplication::clipboard()->text());
        if (content.isEmpty() && !hasSelectedText()) return;
        separateUndo(); QLineEdit::insert(content); separateUndo();
    }
protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->matches(QKeySequence::Paste)) { paste(); event->accept(); return; }
        QKeyEvent uppercase(event->type(), event->key(), event->modifiers(),
            event->nativeScanCode(), event->nativeVirtualKey(), event->nativeModifiers(),
            upperHex(event->text()), event->isAutoRepeat(), event->count());
        QLineEdit::keyPressEvent(&uppercase); event->setAccepted(uppercase.isAccepted());
    }
    void inputMethodEvent(QInputMethodEvent *event) override {
        QInputMethodEvent uppercase(event->preeditString(), event->attributes());
        uppercase.setCommitString(upperHex(event->commitString()), event->replacementStart(), event->replacementLength());
        QLineEdit::inputMethodEvent(&uppercase); event->setAccepted(uppercase.isAccepted());
    }
private:
    void separateUndo() {
        // QLineEdit offers no public undo separator. A cursor round-trip makes
        // paste a distinct command without rewriting text or losing selection.
        if (hasSelectedText() || text().isEmpty()) return;
        const int position = cursorPosition();
        setCursorPosition(position > 0 ? position - 1 : 1); setCursorPosition(position);
    }
};
QLineEdit *hexEdit(const QString &value = {}, QWidget *parent = nullptr) {
    return new HexLineEdit(value, parent);
}
void addOption(QFormLayout *form, const QString &name, QWidget *field, const QString &help) {
    auto label = new QLabel(name); label->setToolTip(help); field->setToolTip(help);
    const QString key = field->objectName().startsWith("protocol_") ? field->objectName().mid(9) : field->objectName();
    label->setObjectName("protocol_label_" + key); form->addRow(label, field);
}
class ConditionHexDelegate : public QStyledItemDelegate {
public:
    explicit ConditionHexDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &) const override {
        auto edit = hexEdit({}, parent); edit->setObjectName("protocolConditionHexEditor");
        edit->setToolTip("只接受 0–9、A–F；小写自动转为大写，可用空格分隔字节。每个字节两位，不填写 0x 前缀。"); return edit;
    }
};
class FieldNumberValidator : public QRegularExpressionValidator {
public:
    FieldNumberValidator(bool integer, bool optional, QObject *parent = nullptr)
        : QRegularExpressionValidator(parent), m_integer(integer) {
        const QString pattern = integer ? QStringLiteral("[0-9]+")
            : QStringLiteral("[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?");
        setRegularExpression(QRegularExpression(optional ? "(?:" + pattern + ")?" : pattern));
    }
    State validate(QString &input, int &position) const override {
        const auto state = QRegularExpressionValidator::validate(input, position);
        if (state != Acceptable || input.isEmpty()) return state;
        bool ok = false;
        if (m_integer) input.toInt(&ok);
        else { const double value = input.toDouble(&ok); ok = ok && qIsFinite(value); }
        // Keep temporarily incomplete/out-of-range numbers editable, but never
        // commit overflow, NaN/Inf or an empty required value to the table.
        return ok ? Acceptable : Intermediate;
    }
private:
    bool m_integer;
};
class FieldNumberDelegate : public QStyledItemDelegate {
public:
    FieldNumberDelegate(bool integer, bool optional, QObject *parent, std::function<bool()> freeText = {})
        : QStyledItemDelegate(parent), m_integer(integer), m_optional(optional), m_freeText(std::move(freeText)) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (m_freeText && m_freeText()) return QStyledItemDelegate::createEditor(parent, option, index);
        auto edit = new QLineEdit(parent); edit->setObjectName(m_integer ? "protocolIntegerCellEditor" : "protocolFloatCellEditor");
        edit->setValidator(new FieldNumberValidator(m_integer, m_optional, edit));
        edit->setToolTip(m_integer ? "仅允许非负整数（0、1、2…），不接受中文、小数、负数或其他字符。"
            : m_optional ? "可留空表示不限制；或填写有限数值，支持负数、小数及科学计数法（如 -0.5、1e-3），小数点使用英文句点，不接受中文。"
            : "填写有限数值，支持负数、小数及科学计数法（如 -0.5、1e-3），小数点使用英文句点；不可留空，不接受中文。");
        return edit;
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        if (m_freeText && m_freeText()) { QStyledItemDelegate::setModelData(editor, model, index); return; }
        auto edit = qobject_cast<QLineEdit *>(editor); if (!edit) return;
        QString text = edit->text(); int position = text.size(); FieldNumberValidator validator(m_integer, m_optional);
        if (validator.validate(text, position) == QValidator::Acceptable) model->setData(index, text, Qt::EditRole);
    }
private:
    bool m_integer, m_optional;
    std::function<bool()> m_freeText;
};
class FieldChoiceDelegate : public QStyledItemDelegate {
public:
    explicit FieldChoiceDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &index) const override {
        auto c = new QComboBox(parent);
        if (index.column() == 4) for (auto type : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "float32", "float64"}) c->addItem(type, type);
        else {
            c->addItem("默认", ""); c->setItemData(0, "使用此协议配置中的默认大小端，不单独覆盖。", Qt::ToolTipRole);
            c->addItem("小端", "little"); c->addItem("大端", "big");
        }
        return c;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        auto c = qobject_cast<QComboBox *>(editor); c->setCurrentIndex(qMax(0, c->findData(index.data(Qt::EditRole).toString())));
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        model->setData(index, qobject_cast<QComboBox *>(editor)->currentData(), Qt::EditRole);
    }
protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.column() == 5) {
            const auto order = index.data(Qt::EditRole).toString();
            option->text = order.isEmpty() ? "默认" : order == "little" ? "小端" : order == "big" ? "大端" : order;
        }
    }
};
QString escaped(QString s) { return s.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t"); }
QString unescaped(const QString &s) {
    QString result;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            auto next = s[i + 1];
            if (next == 'n' || next == 'r' || next == 't' || next == '\\') {
                result += next == 'n' ? QChar('\n') : next == 'r' ? QChar('\r') : next == 't' ? QChar('\t') : QChar('\\'); ++i; continue;
            }
        }
        result += s[i];
    }
    return result;
}
QJsonValue numeric(const QString &s) {
    bool ok = false; double d = s.toDouble(&ok);
    return ok ? QJsonValue(d) : QJsonValue(s); // Validator explains invalid input.
}
QString cell(QTableWidget *t, int row, int col) { auto item = t->item(row, col); return item ? item->text() : QString(); }
QComboBox *choices(const QStringList &labels, const QStringList &values) {
    auto combo = new QComboBox;
    for (int i = 0; i < labels.size(); ++i) combo->addItem(labels[i], values[i]);
    return combo;
}
QSpinBox *spin(int min, int max, int value) {
    auto s = new QSpinBox; s->setRange(min, max); s->setValue(value); s->setKeyboardTracking(false); return s;
}
QJsonObject textExample() {
    return QJsonDocument::fromJson(R"JSON({"revision":1,"name":"文本采集","description":"样例：DATA:25.34,3.30\\r\\n","type":"text",
    "frame":{"encoding":"utf8","terminatorHex":"0D0A","prefix":"DATA:","suffix":"","delimiterMode":"literal","delimiter":",","trimFields":true,"maxFrameBytes":4096},
    "channels":[{"id":"temperature","name":"温度","unit":"°C","column":0,"scale":1,"bias":0},{"id":"voltage","name":"电压","unit":"V","column":1,"scale":1,"bias":0}]})JSON").object();
}
}
bool ProtocolEditorWidget::isBinaryProtocol() const { const auto type = m_type->currentData().toString(); return type == "fixed" || type == "length" || type == "delimiter"; }
bool ProtocolEditorWidget::isKeyValueProtocol() const { return m_type->currentData().toString() == "keyvalue"; }
QStringList ProtocolEditorWidget::protocolTypeNames() {
    return {"文本分隔类型协议", "二进制固定长度类型协议", "二进制长度字段类型协议", "多通道键值协议", "二进制首尾分隔类型协议"};
}
QJsonObject ProtocolEditorWidget::example(bool binary) {
    auto result = binary ? QJsonDocument::fromJson(R"JSON({"revision":1,"name":"二进制采集","description":"样例 HEX：AA 55 E6 09 33 33 53 40 E7 0D 0A","type":"binary","byteOrder":"little",
    "frame":{"mode":"fixed","headerHex":"AA55","sizeBytes":11,"tailHex":"0D0A","payloadOffset":2,"payloadBytes":6,"maxFrameBytes":4096},
    "checksum":{"algorithm":"sum8","range":{"offset":2,"length":6},"field":{"offset":8,"type":"uint8"}},
    "channels":[{"id":"temperature","name":"温度","unit":"°C","offset":0,"type":"int16","scale":0.01,"bias":0},{"id":"voltage","name":"电压","unit":"V","offset":2,"type":"float32","scale":1,"bias":0}]})JSON").object() : textExample();
    result.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces)); return DataProtocols::ProtocolProfileValidator::standardizeBinary(result, true);
}
QJsonObject ProtocolEditorWidget::lengthExample() {
    auto result = example(true);
    result.insert("name", "变长二进制采集");
    result.insert("description", "长度字段为数据字节数；总帧长 = 长度值 + 6，开销由系统计算。样例 HEX：AA 55 06 E6 09 33 33 53 40 65 0D 0A");
    result.insert("frame", QJsonObject{{"mode", "length"}, {"headerHex", "AA55"}, {"tailHex", "0D0A"}, {"payloadOffset", 3}, {"payloadBytes", 6},
        {"minFrameBytes", 12}, {"maxFrameBytes", 4096}, {"lengthField", QJsonObject{{"offset", 2}, {"type", "uint8"}, {"unitBytes", 1}, {"adjustment", 6}}}});
    result.insert("checksum", QJsonObject{{"algorithm", "crc8-smbus"}, {"range", QJsonObject{{"offset", 2}, {"endOffset", 3}}},
        {"field", QJsonObject{{"from", "end"}, {"offset", 3}, {"type", "uint8"}}}});
    return DataProtocols::ProtocolProfileValidator::standardizeBinary(result, true);
}
QJsonObject ProtocolEditorWidget::dynamicArrayExample() {
    return QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"revision", 1},
        {"name", "二进制长度字段帧协议"}, {"description", "动态数组：AA55 + uint16 数据字节数 + N 个 float32 + 0D0A。小端，无校验；发送多少路就读取多少路（最多 256 路）。可选择数据校验、帧头校验及其他数组类型。这是可配置的 TinyFrame 式长度帧，不是 TinyFrame 标准报文模板。"},
        {"type", "binary"}, {"byteOrder", "little"}, {"outOfRangePolicy", "nan-channel"}, {"autoFrameLayout", true},
        {"array", QJsonObject{{"type", "float32"}, {"minChannels", 1}, {"maxChannels", 256}}},
        {"frame", QJsonObject{{"mode", "length"}, {"headerHex", "AA55"}, {"tailHex", "0D0A"},
            {"payloadOffset", 4}, {"payloadEndOffset", 2}, {"minFrameBytes", 10}, {"maxFrameBytes", 4096},
            {"lengthField", QJsonObject{{"offset", 2}, {"type", "uint16"}, {"byteOrder", "little"}, {"meaning", "payload-bytes"}, {"unitBytes", 1}, {"adjustment", 6}}}}},
        {"checksum", QJsonObject{{"algorithm", "none"}}}, {"channels", QJsonArray{}}};
}
QJsonObject ProtocolEditorWidget::delimitedArrayExample() {
    auto d = dynamicArrayExample(); d.insert("name", "二进制首尾分隔协议");
    d.insert("description", "JustFloat 式动态数组：N 个小端 float32 + 00 00 80 7F，无帧头、无长度字段、无校验，默认 1–256 路。通道表仅覆盖名称、单位、备注和换算。样例为原始二进制字节，不是 HEX 文本。数据内出现相同帧尾（包括跨数值边界）可能误切帧；首段从半帧开始且恰好满足结构时无法判断是否完整。可靠性要求高时使用长度字段与校验。异常片段整段拒绝，不截取尾部数值。");
    d.insert("frame", QJsonObject{{"mode", "delimiter"}, {"headerHex", ""}, {"tailHex", "0000807F"}, {"payloadOffset", 0},
        {"payloadEndOffset", 4}, {"minFrameBytes", 8}, {"maxFrameBytes", 4096}});
    return d;
}
QWidget *ProtocolEditorWidget::control(const QString &key, QWidget *widget) {
    m_controls.insert(key, widget); widget->setObjectName("protocol_" + key);
    auto dirty = [this] { if (!m_loading) emit edited(); };
    if (auto e = qobject_cast<QLineEdit *>(widget)) connect(e, &QLineEdit::textChanged, this, dirty);
    if (auto c = qobject_cast<QComboBox *>(widget)) connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), this, dirty);
    if (auto s = qobject_cast<QSpinBox *>(widget)) connect(s, QOverload<int>::of(&QSpinBox::valueChanged), this, dirty);
    if (auto c = qobject_cast<QCheckBox *>(widget)) connect(c, &QCheckBox::toggled, this, dirty);
    return widget;
}
QString ProtocolEditorWidget::value(const QString &key) const {
    if (key == "frameMode") return m_type->currentData().toString();
    auto widget = m_controls.value(key);
    if (auto e = qobject_cast<QLineEdit *>(widget)) return e->text();
    if (auto c = qobject_cast<QComboBox *>(widget)) return c->currentData().toString();
    return {};
}
int ProtocolEditorWidget::number(const QString &key) const { return qobject_cast<QSpinBox *>(m_controls.value(key))->value(); }
ProtocolEditorWidget::ProtocolEditorWidget(QWidget *parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(4, 4, 4, 4); layout->setSpacing(10); layout->setAlignment(Qt::AlignTop);
    auto basicWidget = new QWidget; basicWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto basic = new QFormLayout(basicWidget); basic->setContentsMargins(0, 0, 0, 0); basic->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); basic->setVerticalSpacing(8); basic->setFormAlignment(Qt::AlignTop);
    m_name = new QLineEdit; m_name->setMaxLength(64); m_name->setObjectName("protocolName");
    m_name->setStyleSheet("QLineEdit#protocolName { padding: 6px 8px; border: 1px solid #4a92bf; border-radius: 4px; }"
        "QLineEdit#protocolName:focus { border-color: #1675b5; }");
    updateNameAppearance();
    m_description = new ProtocolDescriptionEdit; m_description->setObjectName("protocolDescription");
    m_type = choices(protocolTypeNames(), {"text", "fixed", "length", "keyvalue", "delimiter"}); m_type->setObjectName("protocolType");
    addOption(basic, "协议名称", m_name, "协议在列表和主界面中的显示名称，最多 64 个字符。列表随编辑实时更新，保存后才写入协议库。");
    addOption(basic, "协议类型", m_type, "文本分隔：一行一帧。多通道键值：每行一个键和值，所有配置通道收到后组帧。二进制固定长度：总帧长固定。长度字段：按长度分帧，可用动态数组或固定字段。首尾分隔：按帧头和/或帧尾分隔动态数组，无长度字段；默认 JustFloat 式配置。");
    addOption(basic, "协议说明", m_description, "填写此协议的用途、报文示例和设备说明；输入框随内容和换行自动增高，使用协议配置区域右侧滚动条查看全部内容。列表右键“协议说明”也可查看。不影响数据解码。"); layout->addWidget(basicWidget);
    m_frameSummary = new QLabel; m_frameSummary->setObjectName("protocolFrameSummary");
    m_frameSummary->setWordWrap(true); m_frameSummary->setTextFormat(Qt::PlainText);
    m_frameSummary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_frameSummary->setStyleSheet("font-weight: bold; padding: 6px; border: 1px solid palette(mid); border-radius: 3px;");
    m_frameSummary->setToolTip("按当前字节位置实时计算帧结构，可选中复制。动态数组按实际有效数据区长度计算通道数量；固定字段布局只读取通道表。其他/保留字节不提取为通道。位置重叠或超界需修正后才能保存。");
    layout->addWidget(m_frameSummary);
    // A stacked layout measures all pages (including hidden length/CRC rows).
    // A normal box layout excludes the inactive page from every size calculation.
    m_frames = new QWidget; m_frames->setObjectName("protocolFrames");
    auto frameLayout = new QVBoxLayout(m_frames); frameLayout->setContentsMargins(0, 0, 0, 0); frameLayout->setSpacing(0);
    m_frames->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); layout->addWidget(m_frames);
    m_devicePresetRow = new QWidget; m_devicePresetRow->setObjectName("protocol_devicePresetRow");
    auto deviceLayout = new QHBoxLayout(m_devicePresetRow); deviceLayout->setContentsMargins(0, 0, 0, 0);
    auto deviceEnabled = new QCheckBox("启用 TCP 服务器预设");
    deviceLayout->addWidget(control("devicePresetEnabled", deviceEnabled)); deviceLayout->addWidget(new QLabel("端口"));
    deviceLayout->addWidget(control("devicePresetPort", spin(1, 65535, 4970)));
    auto separate = new QCheckBox("自动分离通道视图"); separate->setChecked(true); deviceLayout->addWidget(control("devicePresetSeparate", separate));
    addOption(basic, "设备预设（可选）", m_devicePresetRow, "仅 CBF/IR/RED 模板提供，创建时默认启用，可按需关闭。在未连接状态应用协议时设置 TCP 服务器和端口，并可自动分离通道视图；不会自动开始监听。这是设备配置，不属于通用解码规则。");
    connect(deviceEnabled, &QCheckBox::toggled, this, [this](bool enabled) { m_controls["devicePresetPort"]->setEnabled(enabled); m_controls["devicePresetSeparate"]->setEnabled(enabled); });
    auto text = new QWidget; auto form = new QFormLayout(text); m_textForm = form; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setVerticalSpacing(8); form->setFormAlignment(Qt::AlignTop);
    addOption(form, "文本编码", control("encoding", choices({"UTF-8", "ASCII"}, {"utf8", "ascii"})), "按设备的文本编码解码。UTF-8 支持中文前后缀；ASCII 仅接受 ASCII 字节，遇到非 ASCII 或无效编码时拒绝整帧。");
    addOption(form, "固定前缀（可空）", control("prefix", new QLineEdit), "必须匹配每帧文本开头的固定内容，例如 DATA:。匹配后移除，再提取数值列；留空表示不要求前缀。支持 \\r、\\n、\\t、\\\\ 转义，不是 HEX。");
    addOption(form, "固定后缀（可空）", control("suffix", new QLineEdit), "必须匹配每帧文本末尾的固定内容（不含结束符）。匹配后移除；留空表示不要求后缀。支持 \\r、\\n、\\t、\\\\ 转义，不是 HEX。");
    addOption(form, "分隔方式", control("delimiterMode", choices({"字面分隔（保留空列）", "空白合并"}, {"literal", "whitespace"})), "字面分隔使用指定字符或字符串，并保留空列。空白合并按连续空白（例如空格、Tab）分列，忽略首尾和重复空白，此时不使用下方分隔符。");
    addOption(form, "分隔符", control("delimiter", new QLineEdit(",")), "字面分隔模式下，用于分开数值列的文本，例如逗号或 \\t（Tab）。支持 \\r、\\n、\\t、\\\\，必须非空，解码后最多 16 个字符；列号从 0 开始。空白合并模式下此项禁用。");
    auto endingRow = new QWidget; endingRow->setObjectName("protocol_terminatorRow"); auto endingColumn = new QVBoxLayout(endingRow); endingColumn->setContentsMargins(0, 0, 0, 0); endingColumn->setSpacing(4);
    auto endingLayout = new QHBoxLayout; endingLayout->setContentsMargins(0, 0, 0, 0); endingLayout->setSpacing(8); endingColumn->addLayout(endingLayout);
    auto endingPreset = new QComboBox; endingPreset->setObjectName("protocolTerminatorPreset");
    const QVector<QPair<QString, QString>> endings{{"\\r\\n (0D0A)", "0D0A"}, {"\\n (0A)", "0A"}, {"\\r (0D)", "0D"}, {"\\n\\r (0A0D)", "0A0D"},
        {"NUL (00)", "00"}, {"ETX (03)", "03"}, {"EOT (04)", "04"}, {"自定义 HEX", "custom"}};
    for (const auto &ending : endings) endingPreset->addItem(ending.first, ending.second);
    auto endingEdit = qobject_cast<QLineEdit *>(control("terminatorHex", hexEdit("0D0A"))); endingEdit->setPlaceholderText("必填：HEX，如 0D0A");
    endingLayout->addWidget(endingPreset, 1); endingLayout->addWidget(endingEdit, 1);
    const QString endingHelp = "标记一帧文本结束的字节，必须填写，最多 16 字节。左侧选择常用结束符，右侧可手动输入其他 HEX。只接受十六进制字符及空白，小写自动大写；不填写 0x 或 \\n，每个字节两位。NUL 是 00 字节，不是无结束符；不提供‘无’选项。结束符不参与数值列提取。";
    addOption(form, "结束符 HEX", endingRow, endingHelp); form->labelForField(endingRow)->setObjectName("protocol_label_terminatorHex");
    endingPreset->setToolTip(endingHelp); endingEdit->setToolTip(endingHelp);
    // Do not leave an empty hidden QFormLayout row (Qt 5 retains its spacing).
    auto endingHint = new QLabel; endingHint->setObjectName("protocolTerminatorHint"); endingHint->setWordWrap(true); endingHint->setStyleSheet("color: #d9534f;"); endingColumn->addWidget(endingHint);
    connect(endingPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [endingPreset, endingEdit] {
        const auto hex = endingPreset->currentData().toString(); if (hex != "custom") endingEdit->setText(hex);
    });
    auto syncEnding = [endingPreset, endingEdit, endingHint] {
        QByteArray bytes; const bool complete = DataProtocols::ProtocolProfileValidator::parseHex(endingEdit->text(), bytes);
        const auto hex = complete ? QString::fromLatin1(bytes.toHex().toUpper()) : QString();
        const int preset = endingPreset->findData(hex);
        { const QSignalBlocker block(endingPreset); endingPreset->setCurrentIndex(preset >= 0 ? preset : endingPreset->count() - 1); }
        const bool valid = complete && !bytes.isEmpty() && bytes.size() <= 16; endingHint->setVisible(!valid);
        endingHint->setText(endingEdit->text().trimmed().isEmpty() ? "请选择或输入结束符（HEX），文本协议必须有结束符。"
            : !complete ? "结束符必须是完整的 HEX 字节，每个字节两位。" : "结束符最多 16 字节，请缩短后再保存。");
    };
    connect(endingEdit, &QLineEdit::textChanged, this, syncEnding); syncEnding();
    auto trim = new QCheckBox("去除字段首尾空白"); trim->setChecked(true); trim->setToolTip("解析数值前去除每一列两端的空白；不改变列号，也不合并字面分隔产生的空列。"); form->addRow(control("trimFields", trim));
    auto syntax = new QLabel; syntax->setObjectName("protocolTextSyntax"); syntax->setWordWrap(true); form->addRow(syntax);
    auto keyOption = [this, form](const QString &name, const QString &key, QWidget *widget, const QString &help) {
        widget = control(key, widget); widget->setProperty("keyValueOnly", true); addOption(form, name, widget, help);
    };
    keyOption("组帧超时（ms）", "groupTimeoutMs", spin(0, 600000, 1000), "从本组第一个有效键开始计时，超时丢弃未完成帧，从下一有效行重新收集。0 表示不限制，可能将长时间前后的值组成一帧。不要小于设备发送完整一组数据所需时间。");
    keyOption("重复键处理", "duplicateKeyPolicy", choices({"重新开始一帧（推荐）", "用最新值替换"}, {"restart", "replace"}), "所有通道到齐前再次收到同一键：重新开始清空旧半帧，以当前行作为新组起点；替换模式保留其他键，仅更新重复键，兼容旧 CBF 协议行为。");
    keyOption("未知键 / 非目标行", "unknownKeyPolicy", choices({"忽略（保留正在组帧的数据）", "拒绝并清空未完成帧"}, {"ignore", "reject-group"}), "处理未配置的键或前后缀不匹配的行。已配置键的无效数值始终清空当前半帧，避免拼接旧值。");
    for (int row = 0; row < form->rowCount(); ++row) m_textRows.append(qMakePair(form->itemAt(row, QFormLayout::LabelRole) ? form->itemAt(row, QFormLayout::LabelRole)->widget() : nullptr,
        form->itemAt(row, QFormLayout::FieldRole) ? form->itemAt(row, QFormLayout::FieldRole)->widget() : form->itemAt(row, QFormLayout::SpanningRole)->widget()));
    m_textFrame = text; text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); frameLayout->addWidget(text);
    auto binary = new QWidget; auto bform = new QFormLayout(binary); bform->setContentsMargins(0, 0, 0, 0); bform->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); bform->setVerticalSpacing(8); bform->setFormAlignment(Qt::AlignTop);
    m_binaryForm = bform;
    addOption(bform, "默认大小端", control("byteOrder", choices({"小端", "大端"}, {"little", "big"})), "默认数值字节顺序，同时用于多字节长度和校验字段。小端低字节在前，大端高字节在前；固定字段可对单个通道覆盖大小端，动态数组统一使用此设置。");
    auto boundaryOption = [this, bform](const QString &key, const QString &caption, const QString &initial,
        const QVector<QPair<QString, QString>> &options, QComboBox *&preset, const QString &help) {
        auto row = new QWidget; row->setObjectName("protocol_" + key + "Row");
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto layout = new QHBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(8);
        preset = new QComboBox; preset->setObjectName(key == "headerHex" ? "protocolHeaderPreset" : "protocolTailPreset");
        preset->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); preset->setMinimumContentsLength(6);
        preset->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        preset->setAccessibleName(caption + "常用字节预设");
        for (const auto &option : options) {
            preset->addItem(option.first, option.second);
            preset->setItemData(preset->count() - 1, option.second == "custom" ? "保留当前字节，在右侧输入自定义 HEX。"
                : option.second.isEmpty() ? "真正的空内容，不是 00 字节。"
                : "填入 " + option.second + "；仅设置边界字节，不代表完整协议模板。", Qt::ToolTipRole);
        }
        auto edit = qobject_cast<QLineEdit *>(control(key, hexEdit(initial)));
        edit->setPlaceholderText("可手动输入 HEX，如 AA55"); edit->setAccessibleName(caption + "字节内容");
        layout->addWidget(preset, 1); layout->addWidget(edit, 1);
        addOption(bform, caption, row, help); bform->labelForField(row)->setObjectName("protocol_label_" + key);
        preset->setToolTip(help); edit->setToolTip(help);
        // Presets are only a view of the existing HEX field, not additional
        // persisted rules. Loading/importing never replaces stored bytes.
        auto sync = [preset, edit] {
            QByteArray bytes; const bool complete = DataProtocols::ProtocolProfileValidator::parseHex(edit->text(), bytes);
            const int index = complete ? preset->findData(QString::fromLatin1(bytes.toHex().toUpper())) : -1;
            const QSignalBlocker block(preset); preset->setCurrentIndex(index >= 0 ? index : preset->findData("custom"));
        };
        connect(preset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [preset, edit, sync](int index) {
            // Disabled choices also cannot overwrite bytes if selected by a
            // programmatic call. A manually emptied required header remains
            // editable and is explained by the red frame summary instead.
            auto model = qobject_cast<QStandardItemModel *>(preset->model());
            if (model && index >= 0 && !model->item(index)->isEnabled()) { sync(); return; }
            const QString hex = preset->currentData().toString();
            if (hex != "custom") edit->setText(hex);
        });
        connect(edit, &QLineEdit::textChanged, this, sync); sync();
    };
    const QString boundaryHint = "左侧为常用字节预设，右侧始终可以手动输入自定义 HEX；预设只填写字节，不代替完整协议配置。‘无’表示空内容，不是 00。只接受 0–9、A–F 及空白，小写自动转大写，不填写 0x，每个字节两位，最多 256 字节。";
    boundaryOption("headerHex", "帧头 HEX（可空）", "AA55", {{"无帧头", ""}, {"AA55", "AA55"}, {"55AA", "55AA"}, {"自定义 HEX", "custom"}}, m_headerPreset,
        boundaryHint + "固定长度和首尾分隔类型的帧头、帧尾至少填写一项；长度字段类型必须有帧头。首尾分隔没有固定帧长：只有帧尾时按帧尾结束，仅帧头时收到下一帧头才结束上一帧；同时配置两者时必须配对。数据内不能出现分隔标记。固定长度类型仅有帧尾且没有数据校验时，按总帧长检查帧尾位置并丢弃长度不符的片段。");
    boundaryOption("tailHex", "帧尾 HEX（可空）", "0D0A", {{"无帧尾", ""}, {"CRLF（0D0A）", "0D0A"}, {"LF（0A）", "0A"}, {"CR（0D）", "0D"}, {"JustFloat 小端（0000807F）", "0000807F"}, {"自定义 HEX", "custom"}}, m_tailPreset,
        boundaryHint + "帧尾必须匹配实际帧末，计入总帧长。无帧尾表示不检查帧尾，固定长度类型此时必须填写帧头。配置帧头和帧尾时两者都必须匹配。JustFloat 小端帧尾为 0000807F（4 字节），选择后仍需核对数据区、帧长、大小端和校验。");
    addOption(bform, "总帧长（含头尾/校验）", control("sizeBytes", spin(1, 65536, 11)), "固定长度协议每帧的总字节数，包含帧头、数据区、校验、帧尾及其他字节。必须能容纳所有配置字段，且不超过最大帧长。");
    addOption(bform, "数据提取方式", control("binaryLayout", choices({"动态数组（自动读取全部通道）", "固定字段（按通道表提取）"}, {"array", "fields"})), "动态数组：所有数值使用统一类型，按有效数据区长度自动计算通道数，通道表仅覆盖名称和换算。固定字段：按表中每个字段的类型和偏移提取，适合混合类型和旧协议。模式变更仅影响当前草稿，请核对设备报文后保存。");
    addOption(bform, "数组数据类型", control("arrayType", choices({"float32（4 字节，推荐）", "float64（8 字节）", "int8", "uint8", "int16", "uint16", "int32", "uint32"}, {"float32", "float64", "int8", "uint8", "int16", "uint16", "int32", "uint32"})), "动态数据区中每个数值的统一类型。多字节继承协议默认大小端；不能自动猜测类型。有效数据区必须是类型宽度的整数倍，否则整帧拒绝，不丢弃尾部字节。");
    addOption(bform, "最少通道数", control("arrayMin", spin(1, 256, 1)), "每帧至少包含多少个完整数值。自动布局会据此计算最小总帧长；不要求通道表有对应行。");
    addOption(bform, "最多通道数", control("arrayMax", spin(1, 256, 256)), "每帧允许的最多通道数，最大 256。超出时拒绝整帧并显示诊断，不静默截断数据。与最大帧长共同限制异常报文。");
    auto autoFrame = new QCheckBox("自动安排帧结构（推荐）"); autoFrame->setChecked(true);
    addOption(bform, "便捷布局", control("autoFrameLayout", autoFrame), "自动排列：帧头 + 长度字段 + 头部保留字节 + 帧头校验 + 动态数据 + 尾部保留字节 + 数据校验 + 帧尾；自动计算偏移、尾部距离、最小帧长和校验范围。关闭后可手动适配数据区与长度字段位置；校验位置、范围和长度修正仍统一自动计算。");
    addOption(bform, "头部保留字节数", control("prefixBytes", spin(0, 1024, 0)), "自动布局中，长度字段之后、帧头校验之前预留的字节，可存放消息类型、序号、设备编号等，不作为波形数值。可用固定字节条件筛选指定消息类型；不自动解释时间戳或序号。");
    addOption(bform, "尾部保留字节数", control("trailerBytes", spin(0, 1024, 0)), "自动布局中，动态数据之后、数据校验之前预留的字节，不作为通道。数据校验包含这些保留字节。生成样例时默认填 00，固定字节条件可指定内容。");
    addOption(bform, "最小总帧长（字节）", control("minFrameBytes", spin(1, 65536, 11)), "变长协议允许的最短合法报文。动态数组需容纳最少通道数和头尾开销；自动布局时系统计算。低于此值或超过最大帧长时拒绝，并重新寻找帧头。");
    addOption(bform, "长度字段位置（帧内）", control("lengthOffset", spin(0, 65536, 2)), "长度字段第一个字节在完整帧中的位置，从 0 开始，包含帧头。例如两字节帧头后紧跟长度字节时填 2。");
    addOption(bform, "长度字段类型", control("lengthType", choices({"uint8（1 字节）", "uint16（2 字节）", "uint32（4 字节）"}, {"uint8", "uint16", "uint32"})), "设备发送的无符号长度值 n，uint8/uint16/uint32 分别占 1/2/4 字节，多字节跟随默认大小端。n 的含义由设备协议规定：可代表数据区长度，也可代表整帧长度。按所选长度含义和帧结构自动计算开销，不需手动填写单位或修正；必须匹配设备实际长度定义。");
    m_legacyLengthHint = new QLabel; m_legacyLengthHint->setObjectName("protocolLegacyLengthHint"); m_legacyLengthHint->setWordWrap(true); bform->addRow(m_legacyLengthHint);
    addOption(bform, "长度字段含义", control("lengthMeaning", choices({"数据区字节数（推荐）", "整帧字节数", "通道数量（动态数组）"}, {"payload-bytes", "frame-bytes", "elements"})), "数据区字节数：仅包含数组数值，系统加上头尾开销。整帧字节数：包含全部内容。通道数量：系统乘以数组类型宽度再加开销。头尾开销由系统根据帧结构自动计算，不需要设置长度修正。固定字段模式的数据区字节数包括超出固定提取字段的附加数据。");
    addOption(bform, "长度单位（字节）", control("lengthUnit", spin(1, 65536, 1)), "一个长度单位代表多少字节。长度字段按字节计数时填 1，按 16 位字计数时填 2。总帧长 = 长度值 × 此单位 + 修正值。");
    addOption(bform, "总帧长修正（可为负）", control("lengthAdjustment", spin(-65536, 65536, 5)), "总帧长 = 长度字段值 × 长度单位 + 修正值。长度只表示数据区时，应补上帧头、长度字段、校验和帧尾等字节数；长度已包含整帧时通常填 0。");
    m_lengthSummary = new QLabel; m_lengthSummary->setObjectName("protocolLengthSummary"); m_lengthSummary->setWordWrap(true); bform->addRow(m_lengthSummary);
    addOption(bform, "数据区起点（帧内）", control("payloadOffset", spin(0, 65536, 2)), "固定通道数据区的第一个字节在完整帧中的位置，从 0 开始，包含帧头和长度字段。通道表内的偏移以此位置为起点，而不是以帧起点为起点。");
    addOption(bform, "固定数据区字节数", control("payloadBytes", spin(1, 65536, 6)), "用于读取通道字段的固定数据区长度。全部通道必须位于该范围内；固定字段模式的变长帧可在其后携带附加内容参与校验，但不增加通道；需要随报文读取全部数值时请选择动态数组。");
    addOption(bform, "数据区终点距帧末", control("payloadEndOffset", spin(0, 65536, 2)), "动态数据区终点 = 实际总帧长 − 此距离，不含终点。填尾部保留字节、校验和帧尾等总字节数；不会将这些字节解析为数值。自动布局时系统计算。");
    auto automatic = new QCheckBox("数据区与通道自动关联");
    automatic->setToolTip("开启后，通道数量/类型更新数据区大小；增大数据区补齐完整的 float32 通道，剩余不足 4 字节作为保留字节。缩小并删除通道前询问确认。帧长、尾部校验位置随数据区调整，请按真实设备协议核对；特殊布局可关闭此项手动配置。");
    bform->addRow(control("autoChannelLayout", automatic));
    auto headerAlgorithms = new QComboBox;
    for (const auto &p : DataProtocols::ProtocolChecksum::presets()) { headerAlgorithms->addItem(p.name, p.id); headerAlgorithms->setItemData(headerAlgorithms->count() - 1, p.parameters, Qt::ToolTipRole); }
    addOption(bform, "帧头校验（可选）", control("headerAlgorithm", headerAlgorithms), "TinyFrame 式双校验：先校验固定头部（包括长度字段），通过后才信任帧长并等待数据。无校验时保持简单格式；建议不稳定链路启用 CRC16。须与设备算法和字节序一致；下方说明/C代码按钮也适用于此项。");
    addOption(bform, "帧头校验范围起点", control("headerCheckStart", spin(0, 65536, 0)), "从完整帧起点 0 开始的校验范围起点。自动布局覆盖从帧头到帧头校验字段之前的所有字节，包括长度和保留字节。");
    addOption(bform, "帧头校验范围字节数", control("headerCheckLength", spin(1, 65536, 4)), "固定头部参与校验的字节数，不包含帧头校验字段自身。整个范围必须在动态数据之前。");
    addOption(bform, "帧头校验字段位置", control("headerCheckOffset", spin(0, 65536, 4)), "帧头校验值第一个字节的绝对偏移，须在数据区前且不与长度字段重叠。多字节校验值使用协议默认大小端。");
    auto algorithms = new QComboBox;
    for (const auto &p : DataProtocols::ProtocolChecksum::presets()) { algorithms->addItem(p.name, p.id); algorithms->setItemData(algorithms->count() - 1, p.parameters, Qt::ToolTipRole); }
    auto algorithmRow = new QWidget; algorithmRow->setObjectName("protocol_algorithmRow"); auto algorithmLayout = new QHBoxLayout(algorithmRow); algorithmLayout->setContentsMargins(0, 0, 0, 0);
    control("algorithm", algorithms); algorithmLayout->addWidget(algorithms, 1);
    auto checksumHelp = new QPushButton("说明 / C 代码"); checksumHelp->setObjectName("protocolChecksumHelp"); algorithmLayout->addWidget(checksumHelp);
    const QString algorithmHint = "按设备协议选择无校验、SUM、XOR 或 CRC。旁边说明按钮包含全部算法参数、标准自检向量和可复制的 C99 代码；校验失败仍拒绝整帧。";
    algorithms->setToolTip(algorithmHint); checksumHelp->setToolTip(algorithmHint);
    addOption(bform, "校验算法", algorithmRow, algorithmHint); bform->labelForField(algorithmRow)->setObjectName("protocol_label_algorithm");
    connect(checksumHelp, &QPushButton::clicked, this, [this] { showProtocolChecksumHelp(this, value("algorithm")); });
    auto rule = new QLabel("统一数据校验：从帧起点 0 开始，包含帧头、长度、数据及保留字节；不含校验位自身和帧尾。校验位紧贴帧尾之前；无帧尾时位于整帧末尾。多字节校验值跟随协议默认大小端。选择无校验时不添加校验字节。");
    rule->setObjectName("protocolChecksumRule"); rule->setWordWrap(true);
    m_checksumSummary = new QLabel; m_checksumSummary->setObjectName("protocolChecksumSummary"); m_checksumSummary->setWordWrap(true);
    addOption(bform, "校验范围起点（帧内）", control("checkStart", spin(0, 65536, 0)), "参与校验计算的第一个字节，从 0 开始。新建默认起点 0：定长到固定数据区结束；变长动态到尾部校验字段之前，包含附加数据。不含终点、校验自身和帧尾。按设备协议可改动，已有规则不覆盖。");
    addOption(bform, "校验范围方式", control("checkRangeMode", choices({"指定字节数", "直到帧末指定位置（不含终点）"}, {"fixed", "end"})), "指定字节数：从校验范围起点计算固定数量的字节。直到帧末指定位置：终点 = 实际总帧长 − 指定距离，不包含终点，适合覆盖变长帧的额外内容。");
    addOption(bform, "校验范围字节数", control("checkLength", spin(1, 65536, 8)), "从校验起点参与计算的字节数。默认起点 0、范围到固定数据区结束：字节数 = 数据区起点 + 数据区字节数。例：两字节帧头 + 六字节数据，填 8。终点不含，校验字段不能计入自身。");
    addOption(bform, "校验终点距帧末（字节）", control("checkEndOffset", spin(0, 65536, 3)), "校验范围终点 = 实际总帧长 − 此距离，终点不参与计算。例如尾部为 CRC16（2 字节）和帧尾（1 字节），填 3 可在 CRC 前结束校验范围。");
    addOption(bform, "校验字段定位方式", control("checkFrom", choices({"从帧起点偏移", "从帧末向前定位"}, {"start", "end"})), "选择校验值存放位置的参考点。从帧起点按偏移定位；从帧末定位时使用实际总帧长，适合长度可变但校验值固定在尾部的协议。");
    addOption(bform, "校验字段偏移 / 距帧末", control("checkOffset", spin(0, 65536, 8)), "从帧起点定位时填校验字段第一个字节的偏移（从 0 开始）。从帧末定位时填校验字段起点距帧末的距离，包含校验字段自身。例如 CRC16 后有 1 字节帧尾时填 3。");
    addOption(bform, "多字节校验值大小端", control("checkByteOrder", choices({"小端", "大端"}, {"little", "big"})), "设备发送 CRC16/CRC32 等多字节校验值的字节顺序，与 CRC 位反射及通道大小端无关。单字节 SUM/XOR/CRC8 不需要此配置。");
    auto note = new QLabel("所有偏移从 0 开始。固定长度帧的帧头、帧尾至少填写一项；有帧头时寻找帧头。仅有帧尾且没有数据校验时，按总帧长检查帧尾位置。配置两者时都须匹配，错位或坏帧后重新寻找有效帧。长度字段帧必须有帧头和长度字段。校验范围不含终点且不能包含校验字段。"); note->setObjectName("protocolBinaryNote"); note->setWordWrap(true);
    auto conditionGroup = new QGroupBox("固定字节条件（可选）"); conditionGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum); auto cl = new QVBoxLayout(conditionGroup); cl->setSpacing(8);
    conditionGroup->setObjectName("protocolConditionsGroup");
    conditionGroup->setToolTip("指定帧内某些位置必须出现的固定字节，例如命令码或设备类型。最多 16 项，全部匹配才接受整帧；偏移以完整帧起点为 0，不是数据区起点。");
    auto conditionHint = new QLabel("用于筛选报文，例如偏移 2 必须为 01（命令码）。全部条件匹配才解析；不需要筛选可留空，不代替校验。");
    conditionHint->setObjectName("protocolConditionsHint"); conditionHint->setWordWrap(true); cl->addWidget(conditionHint);
    // A maximum alone allows the form to shrink an empty table to its header.
    // Keep a usable viewport through checksum changes and height-for-width updates.
    m_conditions = new QTableWidget(0, 2); m_conditions->setHorizontalHeaderLabels({"帧内偏移", "必须匹配的 HEX"}); m_conditions->setFixedHeight(135);
    m_conditions->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_conditions->setObjectName("protocolConditions"); cl->addWidget(m_conditions);
    m_conditions->horizontalHeaderItem(0)->setToolTip("固定匹配字节的起始位置，以完整帧起点为 0，范围不能超出帧长。");
    m_conditions->horizontalHeaderItem(1)->setToolTip("此位置必须匹配的字节序列，必填，最多 256 字节。只接受 HEX（如 AA 55），可用空格分隔，小写自动转为大写，每个字节两位，不填写 0x。");
    m_conditions->setItemDelegateForColumn(1, new ConditionHexDelegate(m_conditions));
    m_conditions->setItemDelegateForColumn(0, new FieldNumberDelegate(true, false, m_conditions));
    auto cr = new QHBoxLayout; auto add = new QPushButton("添加条件"), remove = new QPushButton("删除条件"); cr->addWidget(add); cr->addWidget(remove); cl->addLayout(cr);
    add->setObjectName("protocolAddCondition"); remove->setObjectName("protocolDeleteCondition");
    connect(add, &QPushButton::clicked, this, [this] { if (m_conditions->rowCount() < 16) { int row = m_conditions->rowCount(); m_conditions->insertRow(row); m_conditions->setItem(row, 0, new QTableWidgetItem("0")); m_conditions->setItem(row, 1, new QTableWidgetItem("01")); } });
    connect(remove, &QPushButton::clicked, this, [this] { if (m_conditions->currentRow() >= 0) { m_conditions->removeRow(m_conditions->currentRow()); emit edited(); } });
    // Keep the table/group out of QFormLayout's spanning rows. Qt 5 can
    // underestimate a nested group's height-for-width after removing CRC rows,
    // making its buttons overlap the table. A normal vertical layout measures
    // the whole group independently and puts all spare height below it.
    auto binaryContainer = new QWidget; binaryContainer->setObjectName("protocolBinarySection"); auto binaryLayout = new QVBoxLayout(binaryContainer);
    binaryLayout->setContentsMargins(0, 0, 0, 0); binaryLayout->setSpacing(8); binaryLayout->setAlignment(Qt::AlignTop);
    // Wrapped explanatory blocks are measured at their actual full width,
    // rather than sharing QFormLayout's cached spanning-row height.
    binaryLayout->addWidget(binary); binaryLayout->addWidget(rule);
    binaryLayout->addWidget(m_checksumSummary); binaryLayout->addWidget(note); binaryLayout->addWidget(conditionGroup);
    binary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_binaryFrame = binaryContainer; binaryContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum); frameLayout->addWidget(binaryContainer);
    // Qt 5.14 still reserves inter-row spacing for hidden form rows. Remove
    // inactive rows from the layout, retaining their labels and field values.
    while (bform->rowCount()) {
        auto row = bform->takeRow(0);
        QWidget *label = row.labelItem ? row.labelItem->widget() : nullptr;
        QWidget *field = row.fieldItem->widget();
        if (label && label->objectName().isEmpty()) label->setObjectName("protocol_label_" + field->objectName().mid(9));
        m_binaryRows.append(qMakePair(label, field));
        delete row.labelItem; delete row.fieldItem;
    }
    auto limitsWidget = new QWidget; limitsWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); auto limits = new QFormLayout(limitsWidget); limits->setContentsMargins(0, 0, 0, 0);
    addOption(limits, "最大帧长（字节）", control("maxFrameBytes", spin(1, 65536, 4096)), "单帧允许的最大字节数，上限 65536。文本类型包含结束符；二进制类型包含帧头、数据、校验和帧尾。用于限制报文长度，不是波形历史或磁盘缓存上限，过小会拒绝合法报文。"); layout->addWidget(limitsWidget);
    auto channelTitle = new QLabel("通道配置（数值提取）"); channelTitle->setStyleSheet("font-weight: bold;"); layout->addWidget(channelTitle);
    auto policyWidget = new QWidget; policyWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); auto policy = new QFormLayout(policyWidget); policy->setContentsMargins(0, 0, 0, 0);
    policy->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); policy->setVerticalSpacing(8); policy->setFormAlignment(Qt::AlignTop);
    addOption(policy, "录制保存数据类型", control("recordingValueMode", choices({"原始数值（默认）", "换算后数值"}, {"raw", "converted"})),
        "每个协议独立保存。原始数值：CSV/WLR 保存通道系数、换算偏移及超限 NaN 替代前的值；换算后数值：保存原始值 × 系数 + 换算偏移，并保留超限处理产生的 NaN。两者都不包含界面数字滤波。未配置通道按系数 1、偏移 0，保留原值。仅影响之后开始的录制；保存数据、独立协议导入导出和全局配置均沿用此选择，导入录制文件不再次换算。");
    addOption(policy, "通道超限处理", control("outOfRangePolicy", choices({"超限通道标记为 NaN，其他通道保留", "拒绝整帧"}, {"nan-channel", "reject-frame"})),
        "仅针对换算后的有限数值超出最小/最大范围。NaN 模式让超限通道在绘图中显示断点，其他通道保留。原始录制保留超限前的读数，换算录制和波形导出保存替代的 NaN。设备发送的 NaN 按缺失采样点保留。帧结构或校验错误、无效文本、无穷大（Inf）和换算溢出会拒绝整帧。新建二进制、多通道键值协议默认 NaN 模式，文本分隔默认拒绝整帧。"); layout->addWidget(policyWidget);
    m_dataRegionHint = new QLabel; m_dataRegionHint->setObjectName("protocolDataRegionHint"); m_dataRegionHint->setWordWrap(true); layout->addWidget(m_dataRegionHint);
    m_channelHint = new QLabel; m_channelHint->setObjectName("protocolChannelsHint"); m_channelHint->setWordWrap(true); layout->addWidget(m_channelHint);
    m_fields = new QTableWidget(0, 11); m_fields->setObjectName("protocolFields");
    m_fields->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_fields->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fields->setHorizontalHeaderLabels({"字段 ID", "通道名称", "单位", "列号 / 偏移", "数据类型", "大小端", "系数", "换算偏移", "最小值（可空）", "最大值（可空）", "备注"});
    m_fields->setColumnHidden(0, true); m_fields->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_fields->horizontalHeader()->setStretchLastSection(true);
    m_fields->setItemDelegateForColumn(4, new FieldChoiceDelegate(m_fields));
    m_fields->setItemDelegateForColumn(5, new FieldChoiceDelegate(m_fields));
    m_fields->setItemDelegateForColumn(3, new FieldNumberDelegate(true, false, m_fields, [this] { return isKeyValueProtocol(); }));
    for (int col : {6, 7, 8, 9}) m_fields->setItemDelegateForColumn(col, new FieldNumberDelegate(false, col >= 8, m_fields));
    const QStringList fieldHints{
        "通道的稳定标识，用于记忆显示勾选和颜色，新增通道时自动生成。",
        "通道在波形、通道列表和录制文件中的名称。留空保存时自动使用 Ch1、Ch2 等默认名称；导入文件时恢复保存的名称。",
        "数值的显示单位，例如 °C、V；可输入中英文、数字和符号，只作为说明，不会自动进行单位换算。",
        "文本列号从 0 开始，0 对应第 1 列。切换到文本协议时自动转为 0、1、2 等连续列号；连续布局的新增、删除和移动会同步重排。可手动指定非连续列号，此后保留该映射，新通道使用首个未配置列。二进制偏移相对数据区，按类型字节宽度计算。已有/导入布局加载时保留。",
        "双击选择设备字段类型：有符号/无符号 8、16、32 位整数，或 32/64 位浮点数。字段宽度必须完全位于固定数据区。",
        "双击选择该通道的字节顺序；留空使用协议默认大小端。只影响多字节数值。",
        "换算值 = 原始数值 × 系数 + 换算偏移，例如整数温度乘以 0.01 转为实际温度。仅接受有限数值，支持负数、小数及科学计数法，不可留空。用于绘图、分析和当前波形导出；录制是否换算由上方录制保存数据类型决定。默认系数 1。",
        "乘以系数后再加上的数值偏移，通常填 0。仅接受有限数值，支持负数、小数及科学计数法，不可留空。不是字段的字节位置；录制选择换算后数值时同样应用，选择原始数值时不应用。默认偏移 0。",
        "换算后的最小值，包含边界，留空不限制。仅接受有限数值，支持负数、小数及科学计数法。低于范围时按上方策略拒绝整帧或仅该通道记 NaN，不裁剪为边界值。",
        "换算后的最大值，包含边界，留空不限制。仅接受有限数值，支持负数、小数及科学计数法。超出范围时按上方策略拒绝整帧或仅该通道记 NaN，不裁剪为边界值。",
        "此通道的单行备注，默认留空，可输入中英文、数字和符号（最多 1024 个字符）。随协议保存、复制及独立/全局配置导入导出，不参与解析、数值换算或录制数值。"};
    for (int col = 0; col < fieldHints.size(); ++col) m_fields->horizontalHeaderItem(col)->setToolTip(fieldHints[col]);
    fitChannelHeaders();
    layout->addWidget(m_fields);
    auto actions = new QHBoxLayout; auto addFieldBtn = new QPushButton("添加通道"), deleteField = new QPushButton("删除通道"), up = new QPushButton("通道前移"), down = new QPushButton("通道后移");
    addFieldBtn->setObjectName("protocolAddField");
    deleteField->setObjectName("protocolDeleteField"); up->setObjectName("protocolFieldUp"); down->setObjectName("protocolFieldDown");
    for (auto btn : {addFieldBtn, deleteField, up, down}) actions->addWidget(btn); layout->addLayout(actions);
    layout->addStretch(1); // All spare height belongs HERE, never between forms.
    connect(addFieldBtn, &QPushButton::clicked, this, &ProtocolEditorWidget::addField);
    connect(deleteField, &QPushButton::clicked, this, [this] { if (m_fields->currentRow() >= 0) { m_fields->removeRow(m_fields->currentRow()); recalculateOffsets(); updateChannelTableHeight(); emit edited(); } });
    auto move = [this](int delta) {
        int row = m_fields->currentRow(), next = row + delta;
        if (row < 0 || next < 0 || next >= m_fields->rowCount()) return;
        { const QSignalBlocker block(m_fields);
          for (int c = 0; c < m_fields->columnCount(); ++c) { auto a = m_fields->takeItem(row, c), b = m_fields->takeItem(next, c); m_fields->setItem(row, c, b); m_fields->setItem(next, c, a); } }
        recalculateOffsets(); m_fields->selectRow(next); emit edited();
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); }); connect(down, &QPushButton::clicked, this, [move] { move(1); });
    auto dirty = [this] { if (!m_loading) emit edited(); };
    connect(m_name, &QLineEdit::textChanged, this, dirty); connect(m_description, &QPlainTextEdit::textChanged, this, dirty);
    connect(m_fields, &QTableWidget::itemChanged, this, [this, dirty](QTableWidgetItem *item) {
        if (item->column() == 10 && item->toolTip() != item->text()) { const QSignalBlocker block(m_fields); item->setToolTip(item->text()); }
        if (!m_loading && !m_updatingFields && m_type->currentIndex() == 0 && item->column() == 3)
            m_textColumnsAutomatic = sequentialTextColumns();
        if (!m_loading && !m_updatingFields && item->column() == 4) recalculateOffsets(); dirty();
    }); connect(m_conditions, &QTableWidget::itemChanged, this, dirty);
    connect(m_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, dirty] {
        if (!m_loading && value("frameMode") == "delimiter") {
            const QSignalBlocker block(m_controls["binaryLayout"]), headerBlock(m_controls["headerAlgorithm"]);
            qobject_cast<QComboBox *>(m_controls["binaryLayout"])->setCurrentIndex(0);
            qobject_cast<QComboBox *>(m_controls["headerAlgorithm"])->setCurrentIndex(0);
            if (!m_base.contains("array")) {
                const QSignalBlocker fieldsBlock(m_fields);
                for (int row = 0; row < m_fields->rowCount(); ++row) m_fields->item(row, 3)->setText(QString::number(row));
                qobject_cast<QCheckBox *>(m_controls["autoFrameLayout"])->setChecked(true);
            }
        }
        if (!m_loading && m_type->currentIndex() == 0) m_textColumnsAutomatic = true;
        if (!m_loading && isKeyValueProtocol()) {
            const QSignalBlocker block(m_fields);
            for (int row = 0; row < m_fields->rowCount(); ++row) m_fields->item(row, 3)->setText(QString::number(row));
            if (m_fields->rowCount() == 0) addField();
            if (value("delimiter").isEmpty() || value("delimiter") == ",") qobject_cast<QLineEdit *>(m_controls["delimiter"])->setText(":");
        }
        updateType(); if (!m_loading) recalculateOffsets(); dirty();
    });
    connect(m_fields->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this] { updateChannelTableHeight(); });
    connect(m_fields->verticalHeader(), &QHeaderView::sectionResized, this, [this] { updateChannelTableHeight(); });
    auto delimiter = qobject_cast<QComboBox *>(m_controls["delimiterMode"]);
    connect(delimiter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { m_controls["delimiter"]->setEnabled(isKeyValueProtocol() || value("delimiterMode") == "literal"); });
    connect(qobject_cast<QComboBox *>(m_controls["algorithm"]), QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        const int width = qMax(1, DataProtocols::ProtocolChecksum::width(value("algorithm")));
        if (!m_loading && value("frameMode") == "length" && m_base.value("checksum").toObject().value("algorithm").toString("none") == "none") {
            QByteArray tail; DataProtocols::ProtocolProfileValidator::parseHex(value("tailHex"), tail);
            for (auto key : {"checkEndOffset", "checkOffset"}) {
                const bool dynamic = key == QString("checkEndOffset") ? value("checkRangeMode") == "end" : value("checkFrom") == "end";
                if (dynamic && number(key) == tail.size() + m_checksumUiWidth) qobject_cast<QSpinBox *>(m_controls[key])->setValue(tail.size() + width);
            }
        }
        m_checksumUiWidth = width;
    });
    for (auto k : {"lengthType", "algorithm", "checkRangeMode", "byteOrder"})
        connect(qobject_cast<QComboBox *>(m_controls[k]), QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ProtocolEditorWidget::updateBinaryControls);
    for (auto k : {"lengthUnit", "lengthAdjustment", "minFrameBytes", "maxFrameBytes"})
        connect(qobject_cast<QSpinBox *>(m_controls[k]), QOverload<int>::of(&QSpinBox::valueChanged), this, &ProtocolEditorWidget::updateBinaryControls);
    connect(qobject_cast<QComboBox *>(m_controls["binaryLayout"]), QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ProtocolEditorWidget::updateType);
    connect(this, &ProtocolEditorWidget::edited, this, [this] { if (isBinaryProtocol()) { updateArrayLayout(); updateBinaryControls(); } });
    connect(this, &ProtocolEditorWidget::edited, this, &ProtocolEditorWidget::updateFrameSummary);
    connect(this, &ProtocolEditorWidget::edited, this, &ProtocolEditorWidget::updateDataRegionHint);
    connect(qobject_cast<QSpinBox *>(m_controls["payloadBytes"]), QOverload<int>::of(&QSpinBox::valueChanged), this, &ProtocolEditorWidget::syncFieldsFromDataRegion);
    connect(qobject_cast<QCheckBox *>(m_controls["autoChannelLayout"]), &QCheckBox::toggled, this, [this](bool checked) {
        if (m_loading) return;
        const int bytes = number("payloadBytes");
        if (checked) { m_syncingDataRegion = true; recalculateOffsets(); m_syncingDataRegion = false; syncFieldsFromDataRegion(bytes); }
        updateDataRegionHint(); emit edited();
    });
    updateType(); updateBinaryControls();
}
void ProtocolEditorWidget::updateBinaryControls() {
    updateArrayLayout();
    const bool lengthFramed = value("frameMode") == "length", delimiterFramed = value("frameMode") == "delimiter";
    const bool variable = lengthFramed || delimiterFramed, checksum = value("algorithm") != "none";
    const bool array = dynamicArray(), automatic = automaticFrameLayout();
    auto visible = [this](const QString &key, bool show) {
        auto w = m_controls.value(key); w->setVisible(show);
        for (const auto &row : m_binaryRows) if (row.second == w && row.first) row.first->setVisible(show);
    };
    visible("sizeBytes", !variable);
    visible("binaryLayout", lengthFramed);
    for (auto key : {"arrayType", "arrayMin", "arrayMax", "autoFrameLayout"}) visible(key, array);
    visible("lengthMeaning", lengthFramed);
    visible("payloadOffset", !automatic); visible("payloadEndOffset", array && !automatic);
    for (auto key : {"prefixBytes", "trailerBytes"}) visible(key, automatic);
    visible("payloadBytes", !array); visible("autoChannelLayout", !array);
    visible("headerAlgorithm", !delimiterFramed && (lengthFramed || m_base.contains("headerChecksum")));
    for (auto key : {"headerCheckStart", "headerCheckLength", "headerCheckOffset"}) visible(key, false);
    for (auto key : {"minFrameBytes", "lengthOffset", "payloadOffset", "payloadEndOffset", "headerCheckStart", "headerCheckLength", "headerCheckOffset"})
        m_controls[key]->setEnabled(!automatic);
    findChild<QLabel *>("protocol_label_headerHex")->setText(lengthFramed ? "帧头 HEX（必填）" : "帧头 HEX（可空）");
    findChild<QLabel *>("protocol_label_tailHex")->setText("帧尾 HEX（可空）");
    if (auto model = qobject_cast<QStandardItemModel *>(m_headerPreset->model()))
        model->item(m_headerPreset->findData(""))->setEnabled(!lengthFramed);
    if (auto model = qobject_cast<QStandardItemModel *>(m_tailPreset->model())) model->item(m_tailPreset->findData(""))->setEnabled(true);
    visible("minFrameBytes", variable && !automatic); visible("lengthOffset", lengthFramed && !automatic);
    visible("lengthType", lengthFramed);
    for (auto key : {"lengthUnit", "lengthAdjustment"}) { visible(key, false); m_controls[key]->setEnabled(false); }
    m_lengthSummary->setVisible(lengthFramed);
    auto meaning = qobject_cast<QComboBox *>(m_controls["lengthMeaning"]);
    if (auto model = qobject_cast<QStandardItemModel *>(meaning->model())) model->item(meaning->findData("elements"))->setEnabled(array);
    if (!array && meaning->currentData() == QJsonValue("elements").toVariant()) { const QSignalBlocker block(meaning); meaning->setCurrentIndex(0); updateArrayLayout(); }
    m_legacyLengthHint->setVisible(false);
    m_lengthSummary->setText(QString("长度含义：%1。n 是设备发送的长度值，多字节跟随默认大小端。总帧长 T = n×%2 + (%3)，开销由系统根据最小总帧长和固定数据区字节数计算，无需设置修正值。允许总帧长 %4–%5 字节。额外数据可改变帧长，但固定字段模式只提取通道表定义的数值。")
        .arg(qobject_cast<QComboBox *>(m_controls["lengthMeaning"])->currentText()).arg(number("lengthUnit")).arg(number("lengthAdjustment"))
        .arg(number("minFrameBytes")).arg(number("maxFrameBytes")));
    if (array) m_lengthSummary->setText(QString("长度含义：%1。总帧长 T = n × %2 + (%3)；动态通道数 N = (T − %4 − %5) / %6。数组必须完整且在通道数限制内。%7")
        .arg(qobject_cast<QComboBox *>(m_controls["lengthMeaning"])->currentText()).arg(number("lengthUnit")).arg(number("lengthAdjustment"))
        .arg(number("payloadOffset")).arg(number("payloadEndOffset")).arg(DataProtocols::ProtocolProfileValidator::typeBytes(value("arrayType")))
        .arg(automatic ? "位置与开销自动计算，无需手动修改修正值。" : "当前为手动布局，请核对起点和尾部距离。"));
    if (auto note = findChild<QLabel *>("protocolBinaryNote")) note->setText(delimiterFramed
        ? "首尾分隔：帧头、帧尾至少填一项。仅帧尾时由帧尾结束；仅帧头时等待下一个帧头结束上一帧，最后一帧暂存；同时配置时必须配对，新帧头可重新同步。自动读取完整数组，不完整数值、超限数量或校验错误整段拒绝。数据内（含跨数值边界）不能包含分隔标记；无帧头时首段是否完整无法完全判断。可靠采集建议长度字段 + 校验。"
        : array
        ? "动态数组按实际长度自动提取全部通道，帧尾在长度指定的帧末匹配；普通标记字节不会直接截断数据。前方帧不完整时，发现后续完整有效帧可恢复同步。建议使用帧头校验保护长度、数据校验保护整帧；算法必须与设备一致。保留字节可用于消息类型和序号，固定字节条件可筛选数据帧。"
        : "所有偏移从 0 开始。固定字段只读取通道表，多出的字节不增加通道。固定长度帧的帧头、帧尾至少填一项；配置头尾时两者均须匹配。仅帧尾且无校验时，丢弃长度不符的片段。错位或坏帧后重新寻找有效帧。长度字段类型必须有帧头和长度字段。校验范围不含终点和自身。");
    for (auto k : {"checkStart", "checkRangeMode", "checkFrom", "checkOffset"}) visible(k, false);
    visible("checkLength", false); visible("checkEndOffset", false);
    visible("checkByteOrder", false);
    for (auto key : {"checkStart", "checkRangeMode", "checkFrom", "checkOffset", "checkLength", "checkEndOffset"}) m_controls[key]->setEnabled(!automatic);
    m_checksumSummary->setVisible(checksum);
    m_checksumSummary->setText(DataProtocols::ProtocolChecksum::description(value("algorithm")) + "\n校验范围为 [0, T − 校验字节数 − 帧尾字节数)，保留字节也参与数据校验。");
    while (m_binaryForm->rowCount()) {
        auto row = m_binaryForm->takeRow(0); delete row.labelItem; delete row.fieldItem;
    }
    for (const auto &row : m_binaryRows) if (!row.second->isHidden()) {
        if (row.first) m_binaryForm->addRow(row.first, row.second); else m_binaryForm->addRow(row.second);
    }
    m_binaryForm->activate(); m_binaryFrame->updateGeometry(); m_frames->updateGeometry(); updateGeometry();
    updateFrameSummary();
}
bool ProtocolEditorWidget::dynamicArray() const { return value("frameMode") == "delimiter" || (value("frameMode") == "length" && value("binaryLayout") == "array"); }
bool ProtocolEditorWidget::automaticFrameLayout() const {
    return dynamicArray() && qobject_cast<QCheckBox *>(m_controls.value("autoFrameLayout"))->isChecked();
}
void ProtocolEditorWidget::updateArrayLayout() {
    if (m_loading || m_syncingFrame || !isBinaryProtocol()) return;
    m_syncingFrame = true;
    auto set = [this](const QString &key, int n) { const QSignalBlocker block(m_controls[key]); qobject_cast<QSpinBox *>(m_controls[key])->setValue(n); };
    auto choose = [this](const QString &key, const QString &v) { const QSignalBlocker block(m_controls[key]); auto c = qobject_cast<QComboBox *>(m_controls[key]); c->setCurrentIndex(c->findData(v)); };
    QByteArray head, tail;
    if (DataProtocols::ProtocolProfileValidator::parseHex(value("headerHex"), head) && DataProtocols::ProtocolProfileValidator::parseHex(value("tailHex"), tail)) {
        const bool delimiterFramed = value("frameMode") == "delimiter";
        const int hc = delimiterFramed ? 0 : DataProtocols::ProtocolChecksum::width(value("headerAlgorithm"));
        const int dc = DataProtocols::ProtocolChecksum::width(value("algorithm"));
        if (automaticFrameLayout()) {
            const int start = head.size() + (delimiterFramed ? 0 : DataProtocols::ProtocolProfileValidator::typeBytes(value("lengthType"))) + number("prefixBytes");
            set("lengthOffset", head.size()); set("payloadOffset", start + hc);
            set("payloadEndOffset", number("trailerBytes") + dc + tail.size());
            set("minFrameBytes", start + hc + number("arrayMin") * DataProtocols::ProtocolProfileValidator::typeBytes(value("arrayType")) + number("payloadEndOffset"));
        }
        set("headerCheckStart", 0); set("headerCheckLength", number("payloadOffset") - hc); set("headerCheckOffset", number("payloadOffset") - hc);
        set("checkStart", 0); choose("checkRangeMode", "end"); choose("checkFrom", "end");
        set("checkOffset", dc + tail.size()); set("checkEndOffset", dc + tail.size());
        set("checkLength", qMax(1, number(value("frameMode") != "fixed" ? "minFrameBytes" : "sizeBytes") - dc - tail.size()));
        choose("checkByteOrder", value("byteOrder"));
    }
    if (value("frameMode") == "length") {
        const int width = dynamicArray() ? DataProtocols::ProtocolProfileValidator::typeBytes(value("arrayType")) : 1;
        set("lengthUnit", value("lengthMeaning") == "elements" && dynamicArray() ? width : 1);
        set("lengthAdjustment", value("lengthMeaning") == "frame-bytes" ? 0 : dynamicArray()
            ? number("payloadOffset") + number("payloadEndOffset") : number("minFrameBytes") - number("payloadBytes"));
    }
    m_syncingFrame = false;
}
void ProtocolEditorWidget::updateFrameSummary() {
    if (!m_frameSummary || !m_controls.contains("maxFrameBytes")) return;
    const bool binary = isBinaryProtocol(), lengthFramed = value("frameMode") == "length", delimiterFramed = value("frameMode") == "delimiter";
    const bool variable = lengthFramed || delimiterFramed;
    m_frameSummary->setVisible(binary); if (!binary) return;
    auto show = [this](QString text) {
        DataProtocols::ProtocolProfile checked; QString error;
        const bool valid = profile(checked, error);
        if (!valid) text += "\n配置冲突：" + error;
        // Inherit normal text from the active theme. palette(text) can refer
        // to the unstyled application palette and become black on a dark UI.
        m_frameSummary->setStyleSheet(QString("font-weight: bold; padding: 6px; border: 1px solid %1; border-radius: 3px;%2")
            .arg(valid ? "palette(mid)" : "#E85C5C", valid ? QString() : QString(" color: #E85C5C;")));
        m_frameSummary->setText(text);
    };
    QByteArray head, tail;
    if (!DataProtocols::ProtocolProfileValidator::parseHex(value("headerHex"), head)
        || !DataProtocols::ProtocolProfileValidator::parseHex(value("tailHex"), tail)) {
        show("帧结构：请先填写完整的 HEX 字节（每字节两位）。"); return;
    }
    const int hc = delimiterFramed ? 0 : DataProtocols::ProtocolChecksum::width(value("headerAlgorithm"));
    const int dc = DataProtocols::ProtocolChecksum::width(value("algorithm"));
    const int total = number(variable ? "minFrameBytes" : "sizeBytes");
    QStringList parts;
    if (automaticFrameLayout()) {
        parts << QString("帧头（%1）").arg(head.size());
        if (lengthFramed) parts << QString("长度（%1）").arg(DataProtocols::ProtocolProfileValidator::typeBytes(value("lengthType")));
        parts << QString("头部保留（%1）").arg(number("prefixBytes"));
        if (!delimiterFramed) parts << QString("帧头校验（%1）").arg(hc);
        parts
            << QString("数据（N×%1）").arg(DataProtocols::ProtocolProfileValidator::typeBytes(value("arrayType")))
            << QString("尾部保留（%1）").arg(number("trailerBytes")) << QString("数据校验（%1）").arg(dc) << QString("帧尾（%1）").arg(tail.size());
    } else {
        // Preserve explicit gaps and symbolic positions; never present a
        // fictional contiguous structure for a conflicting/manual layout.
        struct Region { QString name; int coefficient, offset, bytes; bool array = false; };
        QVector<Region> regions;
        if (!head.isEmpty()) regions.append({"帧头", 0, 0, head.size()});
        if (lengthFramed) regions.append({"长度", 0, number("lengthOffset"), DataProtocols::ProtocolProfileValidator::typeBytes(value("lengthType"))});
        if (hc) regions.append({"帧头校验", 0, number("payloadOffset") - hc, hc});
        regions.append({"数据", 0, number("payloadOffset"), dynamicArray() ? total - number("payloadOffset") - number("payloadEndOffset") : number("payloadBytes"), dynamicArray()});
        if (dc) regions.append({"数据校验", 1, -tail.size() - dc, dc});
        if (!tail.isEmpty()) regions.append({"帧尾", 1, -tail.size(), tail.size()});
        std::stable_sort(regions.begin(), regions.end(), [total](const Region &x, const Region &y) { return x.coefficient * total + x.offset < y.coefficient * total + y.offset; });
        auto expression = [variable, total](int c, int n) {
            if (!variable || !c) return QString::number(c * total + n);
            return QString(c == 1 ? "T" : "−T") + (n > 0 ? QString(" + %1").arg(n) : n < 0 ? QString(" − %1").arg(-n) : QString());
        };
        int endCoefficient = 0, endOffset = 0;
        for (const auto &region : regions) {
            const int c = region.coefficient - endCoefficient, n = region.offset - endOffset;
            if ((variable && c) || c * total + n != 0) parts << "其他/保留（" + expression(c, n) + "）";
            parts << (region.array ? QString("数据（N×%1）").arg(DataProtocols::ProtocolProfileValidator::typeBytes(value("arrayType"))) : QString("%1（%2）").arg(region.name).arg(region.bytes));
            endCoefficient = region.array ? 1 : region.coefficient;
            endOffset = region.array ? -number("payloadEndOffset") : region.offset + region.bytes;
        }
        const int c = 1 - endCoefficient, n = -endOffset;
        if ((variable && c) || c * total + n != 0) parts << "其他/保留（" + expression(c, n) + "）";
    }
    QString text = "帧结构：" + parts.join(" + ") + "，单位均为字节。";
    if (dynamicArray()) text += QString("\n总帧长 T = %1 + N×%2 字节；N 为实际通道数（%3–%4），类型 %5。\n动态数据区 = [%6, T − %7)，自动读取全部完整数值；通道表不限制数量。")
        .arg(number("payloadOffset") + number("payloadEndOffset")).arg(DataProtocols::ProtocolProfileValidator::typeBytes(value("arrayType")))
        .arg(number("arrayMin")).arg(number("arrayMax")).arg(value("arrayType")).arg(number("payloadOffset")).arg(number("payloadEndOffset"));
    else text += QString("\n总帧长：%1 字节；固定数据区提取 %2 字节。").arg(variable ? "T" : QString::number(total)).arg(number("payloadBytes"));
    if (lengthFramed) text += QString("\n长度字段含义：%1；T = n×%2 + (%3)，开销由系统计算。允许总帧长 %4–%5 字节。")
        .arg(qobject_cast<QComboBox *>(m_controls["lengthMeaning"])->currentText()).arg(number("lengthUnit")).arg(number("lengthAdjustment")).arg(number("minFrameBytes")).arg(number("maxFrameBytes"));
    if (delimiterFramed) {
        const QString framing = head.isEmpty() ? "仅帧尾：匹配帧尾 " + QString::fromLatin1(tail.toHex(' ').toUpper())
            : tail.isEmpty() ? "仅帧头：等待下一个帧头 " + QString::fromLatin1(head.toHex(' ').toUpper()) + " 结束上一帧；最后一帧暂存，不按超时截帧"
            : "首尾配对：匹配帧头 " + QString::fromLatin1(head.toHex(' ').toUpper()) + " 与帧尾 " + QString::fromLatin1(tail.toHex(' ').toUpper()) + "；新帧头可重新同步";
        text += QString("\n分帧方式：%1。没有长度字段，允许总帧长 %2–%3 字节。数组须完整，否则整段拒绝；数据内不能包含分隔标记。")
            .arg(framing).arg(number("minFrameBytes")).arg(number("maxFrameBytes"));
    }
    if (dc) text += "\n数据校验覆盖帧起点 0 到校验位之前（含保留字节），不含校验位和帧尾。";
    show(text);
}
void ProtocolEditorWidget::updateType() {
    const bool binary = isBinaryProtocol(), keyValue = isKeyValueProtocol();
    while (m_textForm->rowCount()) { auto row = m_textForm->takeRow(0); delete row.labelItem; delete row.fieldItem; }
    for (const auto &row : m_textRows) {
        const bool show = row.second->property("keyValueOnly").toBool() ? keyValue : row.second != m_controls["delimiterMode"] || !keyValue;
        row.second->setVisible(show); if (row.first) row.first->setVisible(show);
        if (show) { if (row.first) m_textForm->addRow(row.first, row.second); else m_textForm->addRow(row.second); }
    }
    auto delimiterLabel = findChild<QLabel *>("protocol_label_delimiter"); delimiterLabel->setText(keyValue ? "键值分隔符" : "分隔符");
    const QString separatorHelp = keyValue ? "分开通道键和数值的文本，如冒号或等号。每行只提取一个键和值；通道键不能含此分隔符。支持转义，必须非空。" : "字面分隔模式下分开数值列的文本；支持转义，必须非空。空白合并模式不使用此项。";
    m_controls["delimiter"]->setToolTip(separatorHelp); delimiterLabel->setToolTip(separatorHelp);
    m_controls["delimiter"]->setEnabled(keyValue || value("delimiterMode") == "literal");
    findChild<QLabel *>("protocolTextSyntax")->setText(keyValue ? "每行一个键和值，所有配置通道收到后组成一帧，按通道表顺序输出；键区分大小写，可为数字或文本，不能重复。没有帧序号的键值流无法完全识别丢包，建议选择重复键重启和合适的超时。" : "前缀、后缀、分隔符支持 \\r、\\n、\\t、\\\\；结束符使用 HEX。按列读取全部数值列，最多 256 路，空列或无效值拒绝整帧。");
    const bool preset = keyValue && m_base.contains("devicePreset"); m_devicePresetRow->setVisible(preset); findChild<QLabel *>("protocol_label_devicePresetRow")->setVisible(preset);
    m_textFrame->setVisible(!binary); m_binaryFrame->setVisible(binary);
    m_fields->setColumnHidden(4, !binary || dynamicArray()); m_fields->setColumnHidden(5, !binary || dynamicArray());
    m_fields->horizontalHeaderItem(3)->setText(keyValue ? "通道键" : dynamicArray() ? "通道序号（从 0）" : binary ? "数据偏移（字节）" : "列号");
    m_fields->horizontalHeaderItem(3)->setToolTip(keyValue ? "匹配接收行中分隔符左侧的键，区分大小写，可为数字或文本。不能重复或含分隔符。移动行只改变输出顺序，不改写键。" : dynamicArray()
        ? "数组元素序号，从 0 开始，0 对应 Ch1。只覆盖该位置的名称、单位和换算；不改变接收数量。不允许重复。配置行排序不改变数组线序。"
        : binary ? "相对固定数据区起点的字节偏移，从 0 开始。新建/移动字段会按类型宽度重新计算；手动布局可覆盖偏移。" : "文本数值列号，从 0 开始：0 对应第 1 列。类型切换及连续布局的增删/移动自动重排；手动非连续映射保留。空列不会自动移位。");
    m_channelHint->setText(dynamicArray() ? "此表为可选的通道覆盖配置，不限制实际通道数量。序号从 0 开始：0 对应 Ch1；未配置项使用 ChN、系数 1、偏移 0、无单位。按数组线序输出，较短帧中不存在的覆盖项不输出；表格前移/后移仅调整配置行顺序，不改变线序。数据类型和大小端由上方统一指定。" : binary
        ? "每行定义一路波形数值：按数据区内偏移、数据类型和大小端读取，再按系数/换算偏移换算。偏移从 0 开始；大小端“默认”继承上方协议设置。"
        : "此表只配置需要自定义名称、单位或换算的列，列号从 0 开始。先按表格顺序输出已配置通道，再按原列顺序追加未配置列：默认 ChN、无单位、原始数值。可不配置任何通道；每帧最多 256 路，空列或无效值拒绝整帧。");
    fitChannelHeaders();
    if (keyValue) m_channelHint->setText("每行配置一个必须收到的通道键。按表格顺序输出，不自动加入未知键；新增自动分配未使用的数字键，增删或移动不会改写已有键。最多 256 路，名称、单位、换算与范围同其他协议。");
    updateArrayLayout();
    updateBinaryControls();
    updateDataRegionHint();
}
void ProtocolEditorWidget::fitChannelHeaders() {
    if (!m_fields) return;
    const QFontMetrics metrics(m_fields->horizontalHeader()->font());
    for (int col = 1; col < m_fields->columnCount(); ++col) {
        const int width = qMax(col == 10 ? 180 : col == 1 ? 130 : 75, metrics.horizontalAdvance(m_fields->horizontalHeaderItem(col)->text()) + 28);
        if (m_fields->columnWidth(col) < width) m_fields->setColumnWidth(col, width);
    }
    updateChannelTableHeight();
}
void ProtocolEditorWidget::updateChannelTableHeight() {
    if (!m_fields) return;
    int height = m_fields->horizontalHeader()->height() + 2 * m_fields->frameWidth();
    if (m_fields->rowCount() == 0) height += 2 * m_fields->verticalHeader()->defaultSectionSize();
    for (int row = 0; row < m_fields->rowCount(); ++row) height += m_fields->rowHeight(row);
    if (m_fields->horizontalScrollBar()->maximum() > 0) height += m_fields->horizontalScrollBar()->sizeHint().height();
    m_fields->setFixedHeight(height + 2); updateGeometry();
}
bool ProtocolEditorWidget::sequentialTextColumns() const {
    for (int row = 0; row < m_fields->rowCount(); ++row) {
        bool ok = false; const int column = cell(m_fields, row, 3).toInt(&ok);
        if (!ok || column != row) return false;
    }
    return true;
}
void ProtocolEditorWidget::recalculateOffsets() {
    const bool text = m_type->currentIndex() == 0;
    if (isKeyValueProtocol() || (text && !m_textColumnsAutomatic) || dynamicArray() || m_loading || m_updatingFields) return;
    m_updatingFields = true; const QSignalBlocker block(m_fields); int offset = 0;
    for (int row = 0; row < m_fields->rowCount(); ++row) {
        if (!m_fields->item(row, 3)) m_fields->setItem(row, 3, new QTableWidgetItem);
        m_fields->item(row, 3)->setText(QString::number(offset));
        offset += text ? 1 : DataProtocols::ProtocolProfileValidator::typeBytes(cell(m_fields, row, 4));
    }
    m_updatingFields = false;
    syncDataRegionFromFields();
}
bool ProtocolEditorWidget::automaticLayout() const {
    auto automatic = qobject_cast<QCheckBox *>(m_controls.value("autoChannelLayout"));
    return isBinaryProtocol() && !dynamicArray() && automatic && automatic->isChecked();
}
void ProtocolEditorWidget::updateDataRegionHint() {
    if (!m_dataRegionHint || !m_fields) return;
    const bool binary = isBinaryProtocol(); m_dataRegionHint->setVisible(binary);
    if (dynamicArray()) {
        const QSignalBlocker block(m_fields);
        for (int row = 0; row < m_fields->rowCount(); ++row) if (auto item = m_fields->item(row, 3)) item->setFlags(item->flags() | Qt::ItemIsEditable);
        m_dataRegionHint->setText(QString("动态数组 · %1 项可选覆盖 · 实际通道数由接收数据决定（最多 %2 路）；数据区不完整或超过限制时拒绝整帧。")
            .arg(m_fields->rowCount()).arg(number("arrayMax")));
        if (auto button = findChild<QPushButton *>("protocolDeleteField")) button->setEnabled(true);
        return;
    }
    int used = 0, end = 0;
    { const QSignalBlocker block(m_fields);
      for (int row = 0; row < m_fields->rowCount(); ++row) {
          const int width = DataProtocols::ProtocolProfileValidator::typeBytes(cell(m_fields, row, 4)); used += width;
          end = qMax(end, cell(m_fields, row, 3).toInt() + width);
          if (auto item = m_fields->item(row, 3)) item->setFlags(automaticLayout() ? item->flags() & ~Qt::ItemIsEditable : item->flags() | Qt::ItemIsEditable);
      } }
    m_dataRegionHint->setText(QString("%1 · %2 路通道 · 字段合计 %3 字节 · 固定数据区 %4 字节 · %5 字节未配置/保留%6")
        .arg(automaticLayout() ? "自动关联（默认新增 float32 / 4 字节）" : "手动布局")
        .arg(m_fields->rowCount()).arg(used).arg(number("payloadBytes")).arg(qMax(0, number("payloadBytes") - used))
        .arg(end > number("payloadBytes") ? "；存在越界，请核对偏移与数据区" : automaticLayout() && number("payloadBytes") - used >= 4 ? "；超过自动通道上限或存在间隙，不能静默截断" : QString()));
    if (auto button = findChild<QPushButton *>("protocolDeleteField")) button->setEnabled(!automaticLayout() || m_fields->rowCount() > 1);
}
void ProtocolEditorWidget::adjustFrameForDataRegion(int oldBytes, int newBytes) {
    const int delta = newBytes - oldBytes, oldEnd = number("payloadOffset") + oldBytes;
    QByteArray tail; DataProtocols::ProtocolProfileValidator::parseHex(value("tailHex"), tail);
    const int checkWidth = qMax(0, DataProtocols::ProtocolChecksum::width(value("algorithm")));
    auto set = [this](const QString &key, int n) { const QSignalBlocker block(m_controls[key]); qobject_cast<QSpinBox *>(m_controls[key])->setValue(n); };
    if (value("algorithm") != "none" && value("checkFrom") != "end" && number("checkOffset") >= oldEnd) set("checkOffset", number("checkOffset") + delta);
    if (value("checkRangeMode") != "end" && number("checkStart") <= number("payloadOffset") && number("checkStart") + number("checkLength") == oldEnd)
        set("checkLength", number("checkLength") + delta);
    if (delta) {
        const QSignalBlocker block(m_conditions);
        for (int row = 0; row < m_conditions->rowCount(); ++row) if (cell(m_conditions, row, 0).toInt() >= oldEnd)
            m_conditions->item(row, 0)->setText(QString::number(cell(m_conditions, row, 0).toInt() + delta));
    }
    int minimum = number("payloadOffset") + newBytes + tail.size();
    if (value("algorithm") != "none") {
        if (value("checkFrom") == "end") minimum = qMax(minimum, number("payloadOffset") + newBytes + number("checkOffset"));
        else minimum = qMax(minimum, number("checkOffset") + checkWidth + tail.size());
    }
    if (value("frameMode") == "length") minimum = qMax(minimum, number("lengthOffset") + DataProtocols::ProtocolProfileValidator::typeBytes(value("lengthType")) + tail.size());
    if (value("frameMode") == "fixed") set("sizeBytes", qMax(minimum, number("sizeBytes") + delta));
    else set("minFrameBytes", qMax(minimum, number("minFrameBytes") + delta));
    const int total = value("frameMode") == "fixed" ? number("sizeBytes") : number("minFrameBytes");
    if (total > number("maxFrameBytes")) set("maxFrameBytes", total);
    // Length meaning/unit/adjustment remain explicit device protocol settings.
}
void ProtocolEditorWidget::appendDefaultField(int offset) {
    const int row = m_fields->rowCount(); m_fields->insertRow(row); int suffix = row + 1; QString name;
    bool exists;
    do { name = QString("Ch%1").arg(suffix++); exists = false; for (int i = 0; i < row; ++i) if (cell(m_fields, i, 1) == name) exists = true; } while (exists);
    const QStringList data{QUuid::createUuid().toString(QUuid::WithoutBraces), name, "", QString::number(offset), "float32", "", "1", "0", "", "", ""};
    for (int col = 0; col < data.size(); ++col) m_fields->setItem(row, col, new QTableWidgetItem(data[col]));
}
void ProtocolEditorWidget::syncDataRegionFromFields() {
    if (m_loading || m_syncingDataRegion || !automaticLayout()) return;
    int bytes = 0;
    for (int row = 0; row < m_fields->rowCount(); ++row) bytes = qMax(bytes, cell(m_fields, row, 3).toInt() + DataProtocols::ProtocolProfileValidator::typeBytes(cell(m_fields, row, 4)));
    if (bytes <= 0) return;
    m_syncingDataRegion = true;
    { const QSignalBlocker block(m_controls["payloadBytes"]); qobject_cast<QSpinBox *>(m_controls["payloadBytes"])->setValue(bytes); }
    adjustFrameForDataRegion(m_linkedDataBytes, bytes); m_linkedDataBytes = bytes;
    m_syncingDataRegion = false; updateChannelTableHeight(); updateDataRegionHint(); updateFrameSummary();
}
void ProtocolEditorWidget::syncFieldsFromDataRegion(int bytes) {
    if (m_loading || m_syncingDataRegion) return;
    if (!automaticLayout()) { m_linkedDataBytes = bytes; updateDataRegionHint(); return; }
    QVector<int> removed;
    for (int row = 0; row < m_fields->rowCount(); ++row) if (cell(m_fields, row, 3).toInt() + DataProtocols::ProtocolProfileValidator::typeBytes(cell(m_fields, row, 4)) > bytes) removed.append(row);
    if (removed.size() == m_fields->rowCount() && bytes < 4) {
        const QSignalBlocker block(m_controls["payloadBytes"]); qobject_cast<QSpinBox *>(m_controls["payloadBytes"])->setValue(m_linkedDataBytes);
        updateDataRegionHint(); m_dataRegionHint->setText(m_dataRegionHint->text() + "；不能删光通道后只留下不足 4 字节。请保留完整字段或关闭自动关联，手动配置 int8/int16 等类型。"); updateFrameSummary(); return;
    }
    if (!removed.isEmpty()) {
        QDialog confirm(this); confirm.setObjectName("protocolDataRegionConfirm"); confirm.setWindowTitle("缩小数据区会删除通道"); confirm.setMinimumWidth(540); confirm.setSizeGripEnabled(true);
        auto layout = new QVBoxLayout(&confirm); QStringList names;
        for (int row : removed) if (names.size() < 5) names.append(cell(m_fields, row, 1));
        auto text = new QLabel(QString("数据区缩小到 %1 字节后，%2 个通道无法完整容纳，将删除这些通道的配置：\n%3\n\n原文件尚未改变。是否继续？").arg(bytes).arg(removed.size()).arg(names.join("、")));
        text->setWordWrap(true); text->setTextFormat(Qt::PlainText); layout->addWidget(text);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText("删除超出通道并缩小"); buttons->button(QDialogButtonBox::Cancel)->setText("取消"); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &confirm, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &confirm, &QDialog::reject);
        if (confirm.exec() != QDialog::Accepted) { const QSignalBlocker block(m_controls["payloadBytes"]); qobject_cast<QSpinBox *>(m_controls["payloadBytes"])->setValue(m_linkedDataBytes); updateDataRegionHint(); updateFrameSummary(); return; }
    }
    m_syncingDataRegion = true;
    { const QSignalBlocker block(m_fields);
      for (int i = removed.size() - 1; i >= 0; --i) m_fields->removeRow(removed[i]);
      int end = 0;
      for (int row = 0; row < m_fields->rowCount(); ++row) end = qMax(end, cell(m_fields, row, 3).toInt() + DataProtocols::ProtocolProfileValidator::typeBytes(cell(m_fields, row, 4)));
      while (end + 4 <= bytes && m_fields->rowCount() < DataProtocols::MaxChannels) { appendDefaultField(end); end += 4; }
    }
    adjustFrameForDataRegion(m_linkedDataBytes, bytes); m_linkedDataBytes = bytes;
    m_syncingDataRegion = false; updateChannelTableHeight(); updateDataRegionHint(); updateFrameSummary(); emit edited();
}
void ProtocolEditorWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event); fitChannelHeaders();
}
void ProtocolEditorWidget::updateNameAppearance() {
    if (!m_name) return;
    // Relative to the editor font, so changing the application's font/DPI
    // doesn't leave the title smaller or accumulate size on repeated events.
    QFont titleFont = font();
    if (titleFont.pointSizeF() > 0) titleFont.setPointSizeF(titleFont.pointSizeF() + 2);
    else titleFont.setPixelSize(qMax(1, titleFont.pixelSize()) + 3);
    titleFont.setWeight(QFont::DemiBold);
    if (m_name->font() != titleFont) m_name->setFont(titleFont);
}
void ProtocolEditorWidget::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange || event->type() == QEvent::ApplicationFontChange) {
        updateNameAppearance();
        QTimer::singleShot(0, this, [this] { fitChannelHeaders(); });
    }
}
void ProtocolEditorWidget::addField() {
    if (m_fields->rowCount() >= 256) return;
    int row = m_fields->rowCount();
    int offset = row == 0 ? 0 : cell(m_fields, row - 1, 3).toInt() + DataProtocols::ProtocolProfileValidator::typeBytes(cell(m_fields, row - 1, 4));
    if (dynamicArray()) {
        offset = 0;
        while (offset < number("arrayMax")) {
            bool used = false; for (int i = 0; i < row; ++i) used = used || cell(m_fields, i, 3).toInt() == offset;
            if (!used) break; ++offset;
        }
        if (offset >= number("arrayMax")) return;
    }
    if (m_type->currentIndex() == 0 || isKeyValueProtocol()) {
        offset = 0;
        // Never reuse an already configured column, including sparse imported
        // text mappings. Ordinary contiguous columns simply append at row N.
        for (; offset < DataProtocols::MaxChannels; ++offset) {
            bool used = false; for (int i = 0; i < row; ++i) used = used || (isKeyValueProtocol() ? cell(m_fields, i, 3) == QString::number(offset) : cell(m_fields, i, 3).toInt() == offset);
            if (!used) break;
        }
        if (offset >= DataProtocols::MaxChannels) return;
    }
    const QSignalBlocker block(m_fields); m_fields->insertRow(row);
    int number = dynamicArray() ? offset + 1 : row + 1; QString name;
    bool exists;
    do {
        name = QStringLiteral("Ch%1").arg(number++); exists = false;
        for (int i = 0; i < row; ++i) if (cell(m_fields, i, 1) == name) { exists = true; break; }
    } while (exists);
    const QStringList defaults{QUuid::createUuid().toString(QUuid::WithoutBraces), name, "", QString::number(offset), "float32", "", "1", "0", "", "", ""};
    for (int c = 0; c < defaults.size(); ++c) m_fields->setItem(row, c, new QTableWidgetItem(defaults[c]));
    if (m_type->currentIndex() == 0) recalculateOffsets();
    syncDataRegionFromFields(); updateChannelTableHeight(); emit edited();
}
void ProtocolEditorWidget::setProfile(const DataProtocols::ProtocolProfile &profile) {
    m_loading = true; m_base = profile.definition;
    m_name->setText(profile.name); m_description->setPlainText(profile.description);
    qobject_cast<QCheckBox *>(m_controls["autoChannelLayout"])->setChecked(m_base.value("autoChannelLayout").toBool(false));
    auto policy = qobject_cast<QComboBox *>(m_controls["outOfRangePolicy"]);
    policy->setCurrentIndex(qMax(0, policy->findData(m_base.value("outOfRangePolicy").toString("reject-frame"))));
    auto recording = qobject_cast<QComboBox *>(m_controls["recordingValueMode"]);
    recording->setCurrentIndex(qMax(0, recording->findData(m_base.value("recordingValueMode").toString("raw"))));
    const auto frame = m_base.value("frame").toObject();
    qobject_cast<QComboBox *>(m_controls["binaryLayout"])->setCurrentIndex(profile.isDynamicArray() ? 0 : 1);
    qobject_cast<QCheckBox *>(m_controls["autoFrameLayout"])->setChecked(m_base.value("autoFrameLayout").toBool(false));
    const auto array = m_base.value("array").toObject();
    auto arrayType = qobject_cast<QComboBox *>(m_controls["arrayType"]); arrayType->setCurrentIndex(qMax(0, arrayType->findData(array.value("type").toString("float32"))));
    qobject_cast<QSpinBox *>(m_controls["arrayMin"])->setValue(array.value("minChannels").toInt(1));
    qobject_cast<QSpinBox *>(m_controls["arrayMax"])->setValue(array.value("maxChannels").toInt(256));
    qobject_cast<QSpinBox *>(m_controls["payloadEndOffset"])->setValue(frame.value("payloadEndOffset").toInt(2));
    const auto kind = profile.isKeyValue() ? QString("keyvalue") : profile.definition.value("type") == QJsonValue("binary") ? frame.value("mode").toString() : QString("text");
    m_type->setCurrentIndex(qMax(0, m_type->findData(kind)));
    const auto preset = m_base.value("devicePreset").toObject();
    qobject_cast<QCheckBox *>(m_controls["devicePresetEnabled"])->setChecked(preset.value("enabled").toBool());
    qobject_cast<QSpinBox *>(m_controls["devicePresetPort"])->setValue(preset.value("port").toInt(4970));
    qobject_cast<QCheckBox *>(m_controls["devicePresetSeparate"])->setChecked(preset.value("separateViews").toBool(true));
    m_controls["devicePresetPort"]->setEnabled(preset.value("enabled").toBool()); m_controls["devicePresetSeparate"]->setEnabled(preset.value("enabled").toBool());
    // Reset mode-specific controls: never carry a previous scheme's hidden
    // length/end-relative settings into a newly selected fixed scheme.
    for (auto k : {"lengthType", "checkRangeMode", "checkFrom"}) qobject_cast<QComboBox *>(m_controls[k])->setCurrentIndex(0);
    for (const auto &p : {qMakePair(QString("minFrameBytes"), 11), qMakePair(QString("lengthOffset"), 2), qMakePair(QString("lengthUnit"), 1), qMakePair(QString("lengthAdjustment"), 5), qMakePair(QString("checkEndOffset"), 3)})
        qobject_cast<QSpinBox *>(m_controls[p.first])->setValue(p.second);
    for (auto it = m_controls.begin(); it != m_controls.end(); ++it) {
        auto v = frame.value(it.key()); if (it.key() == "byteOrder") v = m_base.value("byteOrder");
        if (v.isUndefined()) continue;
        if (auto e = qobject_cast<QLineEdit *>(it.value())) e->setText(escaped(v.toString()));
        if (auto s = qobject_cast<QSpinBox *>(it.value())) s->setValue(v.toInt());
        if (auto c = qobject_cast<QComboBox *>(it.value())) c->setCurrentIndex(qMax(0, c->findData(v.toString())));
        if (auto c = qobject_cast<QCheckBox *>(it.value())) c->setChecked(v.toBool());
    }
    const auto length = frame.value("lengthField").toObject();
    auto meaning = qobject_cast<QComboBox *>(m_controls["lengthMeaning"]); meaning->setCurrentIndex(qMax(0, meaning->findData(length.value("meaning").toString("custom"))));
    for (const auto &p : QVector<QPair<QString, QString>>{{"lengthOffset", "offset"}, {"lengthUnit", "unitBytes"}, {"lengthAdjustment", "adjustment"}})
        if (length.contains(p.second)) qobject_cast<QSpinBox *>(m_controls[p.first])->setValue(length.value(p.second).toInt());
    for (const auto &p : QVector<QPair<QString, QString>>{{"lengthType", "type"}}) {
        auto c = qobject_cast<QComboBox *>(m_controls[p.first]); c->setCurrentIndex(qMax(0, c->findData(length.value(p.second).toString())));
    }
    const auto check = m_base.value("checksum").toObject();
    // A no-checksum profile carries no range settings; initialize its hidden
    // controls independently of whichever profile was edited previously.
    if (check.value("algorithm").toString("none") == "none") {
        qobject_cast<QSpinBox *>(m_controls["checkStart"])->setValue(0);
        qobject_cast<QSpinBox *>(m_controls["checkLength"])->setValue(number("payloadOffset") + number("payloadBytes"));
        qobject_cast<QSpinBox *>(m_controls["checkOffset"])->setValue(number("payloadOffset") + number("payloadBytes"));
    }
    qobject_cast<QComboBox *>(m_controls["algorithm"])->setCurrentIndex(qMax(0, qobject_cast<QComboBox *>(m_controls["algorithm"])->findData(check.value("algorithm").toString("none"))));
    for (const auto &p : {qMakePair(QString("checkStart"), check.value("range").toObject().value("offset")), qMakePair(QString("checkLength"), check.value("range").toObject().value("length")), qMakePair(QString("checkOffset"), check.value("field").toObject().value("offset"))})
        if (!p.second.isUndefined()) qobject_cast<QSpinBox *>(m_controls[p.first])->setValue(p.second.toInt());
    auto cbo = qobject_cast<QComboBox *>(m_controls["checkByteOrder"]); cbo->setCurrentIndex(qMax(0, cbo->findData(check.value("field").toObject().value("byteOrder").toString("little"))));
    qobject_cast<QComboBox *>(m_controls["checkRangeMode"])->setCurrentIndex(check.value("range").toObject().contains("endOffset") ? 1 : 0);
    qobject_cast<QComboBox *>(m_controls["checkFrom"])->setCurrentIndex(check.value("field").toObject().value("from") == QJsonValue("end") ? 1 : 0);
    if (check.value("algorithm").toString("none") == "none" && (frame.value("mode") == QJsonValue("length") || profile.isDelimiterFramed())) {
        QByteArray tail; DataProtocols::ProtocolProfileValidator::parseHex(value("tailHex"), tail);
        qobject_cast<QComboBox *>(m_controls["checkRangeMode"])->setCurrentIndex(1);
        qobject_cast<QComboBox *>(m_controls["checkFrom"])->setCurrentIndex(1);
        qobject_cast<QSpinBox *>(m_controls["checkEndOffset"])->setValue(tail.size() + 1);
        qobject_cast<QSpinBox *>(m_controls["checkOffset"])->setValue(tail.size() + 1);
    }
    m_checksumUiWidth = qMax(1, DataProtocols::ProtocolChecksum::width(value("algorithm")));
    if (check.value("range").toObject().contains("endOffset")) qobject_cast<QSpinBox *>(m_controls["checkEndOffset"])->setValue(check.value("range").toObject().value("endOffset").toInt());
    const auto headerCheck = m_base.value("headerChecksum").toObject();
    auto headerAlgorithm = qobject_cast<QComboBox *>(m_controls["headerAlgorithm"]); headerAlgorithm->setCurrentIndex(qMax(0, headerAlgorithm->findData(headerCheck.value("algorithm").toString("none"))));
    qobject_cast<QSpinBox *>(m_controls["headerCheckStart"])->setValue(headerCheck.value("range").toObject().value("offset").toInt());
    qobject_cast<QSpinBox *>(m_controls["headerCheckLength"])->setValue(headerCheck.value("range").toObject().value("length").toInt(4));
    qobject_cast<QSpinBox *>(m_controls["headerCheckOffset"])->setValue(headerCheck.value("field").toObject().value("offset").toInt(4));
    QByteArray tail; DataProtocols::ProtocolProfileValidator::parseHex(value("tailHex"), tail);
    QByteArray head; DataProtocols::ProtocolProfileValidator::parseHex(value("headerHex"), head);
    qobject_cast<QSpinBox *>(m_controls["prefixBytes"])->setValue(qMax(0, number("payloadOffset") - (profile.isDelimiterFramed() ? head.size() : number("lengthOffset")
        + DataProtocols::ProtocolProfileValidator::typeBytes(value("lengthType")) + DataProtocols::ProtocolChecksum::width(value("headerAlgorithm")))));
    qobject_cast<QSpinBox *>(m_controls["trailerBytes"])->setValue(qMax(0, number("payloadEndOffset") - tail.size() - DataProtocols::ProtocolChecksum::width(value("algorithm"))));
    m_conditions->setRowCount(0);
    for (auto v : m_base.value("conditions").toArray()) {
        auto c = v.toObject(); int row = m_conditions->rowCount(); m_conditions->insertRow(row);
        m_conditions->setItem(row, 0, new QTableWidgetItem(QString::number(c.value("offset").toInt()))); m_conditions->setItem(row, 1, new QTableWidgetItem(c.value("equalsHex").toString()));
    }
    m_fields->setRowCount(0);
    for (auto v : m_base.value("channels").toArray()) {
        auto c = v.toObject(); int row = m_fields->rowCount(); m_fields->insertRow(row);
        QStringList data{c.value("id").toString(), c.value("name").toString(), c.value("unit").toString(), profile.isKeyValue() ? c.value("key").toString() : QString::number(c.value(profile.isDynamicArray() ? "index" : isBinaryProtocol() ? "offset" : "column").toInt()), c.value("type").toString("float32"), c.value("byteOrder").toString(), QString::number(c.value("scale").toDouble(1), 'g', 17), QString::number(c.value("bias").toDouble(), 'g', 17), c.contains("min") ? QString::number(c.value("min").toDouble(), 'g', 17) : QString(), c.contains("max") ? QString::number(c.value("max").toDouble(), 'g', 17) : QString(), c.value("note").toString()};
        for (int col = 0; col < data.size(); ++col) m_fields->setItem(row, col, new QTableWidgetItem(data[col]));
    }
    m_textColumnsAutomatic = sequentialTextColumns();
    m_linkedDataBytes = number("payloadBytes"); m_loading = false; updateType(); updateBinaryControls();
}
QJsonObject ProtocolEditorWidget::definition() const {
    const bool binary = isBinaryProtocol(), keyValue = isKeyValueProtocol();
    QJsonObject d{{"id", m_base.value("id")}, {"revision", m_base.value("revision")}, {"name", m_name->text()}, {"description", m_description->toPlainText()}, {"type", keyValue ? "keyvalue" : binary ? "binary" : "text"}};
    if (keyValue && m_base.contains("devicePreset")) d.insert("devicePreset", QJsonObject{{"enabled", qobject_cast<QCheckBox *>(m_controls["devicePresetEnabled"])->isChecked()},
        {"interface", "tcp-server"}, {"port", number("devicePresetPort")}, {"separateViews", qobject_cast<QCheckBox *>(m_controls["devicePresetSeparate"])->isChecked()}});
    if (value("outOfRangePolicy") != "reject-frame" || m_base.contains("outOfRangePolicy")) d.insert("outOfRangePolicy", value("outOfRangePolicy"));
    if (value("recordingValueMode") != "raw" || m_base.contains("recordingValueMode")) d.insert("recordingValueMode", value("recordingValueMode"));
    if (automaticLayout() || m_base.contains("autoChannelLayout")) d.insert("autoChannelLayout", automaticLayout());
    if (dynamicArray()) {
        d.insert("array", QJsonObject{{"type", value("arrayType")}, {"minChannels", number("arrayMin")}, {"maxChannels", number("arrayMax")}});
        d.insert("autoFrameLayout", automaticFrameLayout());
    }
    QJsonObject f{{"maxFrameBytes", number("maxFrameBytes")}};
    if (binary) {
        d.insert("byteOrder", value("byteOrder")); f.insert("mode", value("frameMode"));
        for (auto k : {"headerHex", "tailHex"}) f.insert(k, value(k));
        f.insert("payloadOffset", number("payloadOffset"));
        f.insert(dynamicArray() ? "payloadEndOffset" : "payloadBytes", number(dynamicArray() ? "payloadEndOffset" : "payloadBytes"));
        if (value("frameMode") == "delimiter") f.insert("minFrameBytes", number("minFrameBytes"));
        else if (value("frameMode") == "length") {
            f.insert("minFrameBytes", number("minFrameBytes"));
            QJsonObject length{{"offset", number("lengthOffset")}, {"type", value("lengthType")}, {"unitBytes", number("lengthUnit")}, {"adjustment", number("lengthAdjustment")}};
            length.insert("meaning", value("lengthMeaning"));
            if (value("lengthType") != "uint8") length.insert("byteOrder", value("byteOrder")); f.insert("lengthField", length);
        } else f.insert("sizeBytes", number("sizeBytes"));
        const auto algorithm = value("algorithm"); QJsonObject check{{"algorithm", algorithm}};
        if (algorithm != "none") {
            check.insert("range", QJsonObject{{"offset", number("checkStart")}, {value("checkRangeMode") == "end" ? "endOffset" : "length", value("checkRangeMode") == "end" ? number("checkEndOffset") : number("checkLength")}});
            const int width = DataProtocols::ProtocolChecksum::width(algorithm);
            QJsonObject field{{"offset", number("checkOffset")}, {"type", width == 4 ? "uint32" : width == 2 ? "uint16" : "uint8"}};
            if (value("checkFrom") == "end") field.insert("from", "end");
            else if (m_base.value("checksum").toObject().value("field").toObject().value("from") == QJsonValue("start")) field.insert("from", "start");
            if (width > 1) field.insert("byteOrder", value("checkByteOrder")); check.insert("field", field);
        }
        d.insert("checksum", check);
        if (value("frameMode") != "delimiter" && value("headerAlgorithm") != "none") {
            const int width = DataProtocols::ProtocolChecksum::width(value("headerAlgorithm"));
            QJsonObject field{{"offset", number("headerCheckOffset")}, {"type", width == 4 ? "uint32" : width == 2 ? "uint16" : "uint8"}};
            if (width > 1) field.insert("byteOrder", value("byteOrder"));
            d.insert("headerChecksum", QJsonObject{{"algorithm", value("headerAlgorithm")},
                {"range", QJsonObject{{"offset", number("headerCheckStart")}, {"length", number("headerCheckLength")}}}, {"field", field}});
        }
        QJsonArray conditions;
        for (int row = 0; row < m_conditions->rowCount(); ++row) conditions.append(QJsonObject{{"offset", numeric(cell(m_conditions, row, 0))}, {"equalsHex", cell(m_conditions, row, 1)}});
        if (!conditions.isEmpty()) d.insert("conditions", conditions);
    } else {
        f.insert("encoding", value("encoding")); f.insert("terminatorHex", value("terminatorHex"));
        f.insert("prefix", unescaped(value("prefix"))); f.insert("suffix", unescaped(value("suffix")));
        if (!keyValue) f.insert("delimiterMode", value("delimiterMode"));
        if (keyValue || value("delimiterMode") == "literal") f.insert("delimiter", unescaped(value("delimiter")));
        if (keyValue) {
            f.insert("groupMode", "all-channels"); f.insert("groupTimeoutMs", number("groupTimeoutMs"));
            f.insert("duplicateKeyPolicy", value("duplicateKeyPolicy")); f.insert("unknownKeyPolicy", value("unknownKeyPolicy"));
        }
        f.insert("trimFields", qobject_cast<QCheckBox *>(m_controls["trimFields"])->isChecked());
    }
    d.insert("frame", f); QJsonArray channels;
    for (int row = 0; row < m_fields->rowCount(); ++row) {
        QJsonObject c{{"id", cell(m_fields, row, 0)}, {"name", cell(m_fields, row, 1)}, {"unit", cell(m_fields, row, 2)}, {keyValue ? "key" : dynamicArray() ? "index" : binary ? "offset" : "column", keyValue ? QJsonValue(cell(m_fields, row, 3)) : numeric(cell(m_fields, row, 3))}, {"scale", numeric(cell(m_fields, row, 6))}, {"bias", numeric(cell(m_fields, row, 7))}};
        if (binary && !dynamicArray()) { c.insert("type", cell(m_fields, row, 4).trimmed()); if (!cell(m_fields, row, 5).isEmpty()) c.insert("byteOrder", cell(m_fields, row, 5).trimmed()); }
        if (!cell(m_fields, row, 8).isEmpty()) c.insert("min", numeric(cell(m_fields, row, 8)));
        if (!cell(m_fields, row, 9).isEmpty()) c.insert("max", numeric(cell(m_fields, row, 9)));
        if (!cell(m_fields, row, 10).isEmpty()) c.insert("note", cell(m_fields, row, 10));
        channels.append(c);
    }
    d.insert("channels", channels); return DataProtocols::ProtocolProfileValidator::standardizeBinary(d);
}
bool ProtocolEditorWidget::profile(DataProtocols::ProtocolProfile &output, QString &error) const {
    if (!isBinaryProtocol() && value("terminatorHex").trimmed().isEmpty()) {
        error = "请选择或输入结束符（HEX），文本分隔协议必须有结束符，不能选择‘无’。"; return false;
    }
    if (automaticLayout()) {
        int used = 0; for (int row = 0; row < m_fields->rowCount(); ++row) used += DataProtocols::ProtocolProfileValidator::typeBytes(cell(m_fields, row, 4));
        if (number("payloadBytes") - used >= 4) { error = "自动布局仍有完整的 float32 数据未配置，可能已超过 256 通道上限。请缩小数据区或使用明确的手动布局，不能静默截断。"; return false; }
    }
    return DataProtocols::ProtocolProfileValidator::profile(definition(), output, error);
}

#include "protocoleditorwidget.moc"
