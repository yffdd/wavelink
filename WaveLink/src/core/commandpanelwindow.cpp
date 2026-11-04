#include <QTimer>
#include <cmath>
#include "commandpanelwindow.h"
#include "panellayouteditor.h"
#include "paneluistyle.h"
#include "panelcolordialog.h"
#include "commandsliderconfig.h"
#include "commandswitchconfig.h"
#include "commandpanelconfig.h"
#include <QGroupBox>
#include <QPointer>
#include <QPainter>
#include <QSaveFile>
#include <QLockFile>
#include <QSignalBlocker>
#include <QSet>
#include <QUuid>
#include <QSettings>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QMimeData>
#include <QKeySequence>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QInputMethodEvent>
#include <QScreen>
#include <QWindow>
#include <QScrollBar>
#include <QAbstractButton>
#include <functional>
#include <QMap>
#include <QTranslator>
#include <QTextBrowser>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextOption>
#include <QToolTip>
#include <QHelpEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QDynamicPropertyChangeEvent>
#include <QtMath>
#include <QMessageBox>
#include <QDir>
#include <QStandardPaths>
#include <QFileDialog>
#include <QScrollArea>
#include <QSplitter>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QFormLayout>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QRegularExpression>

namespace {
const char LastCommandPanelKey[] = "CommandPanel/lastPanelId";
QString commandDescriptionTip(const QString &note, QWidget *owner = nullptr, const QPoint *position = nullptr) {
    // Screen coordinates and Qt font metrics are logical pixels, including
    // per-monitor DPI. Recompute on hover after moving to another screen.
    const QPoint at = position ? *position : owner ? owner->mapToGlobal(owner->rect().center()) : QPoint();
    QScreen *screen = QGuiApplication::screenAt(at);
    if (!screen && owner && owner->windowHandle()) screen = owner->windowHandle()->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QSize available = screen ? screen->availableGeometry().size() : QSize(1024, 768);
    const qreal width = qMax(40, qMin(640, available.width() * 3 / 5) - 24);
    QFont font = owner ? owner->font() : QToolTip::font(); font.setPointSize(10); font.setBold(false);
    const QFontMetricsF metrics(font, owner);
    const int maxLines = qMax(4, int((qMin(600, available.height() * 7 / 10) - 28) / qMax(qreal(1), metrics.lineSpacing())));
    const bool empty = note.trimmed().isEmpty();
    // Bound layout work even for imported megabyte-sized descriptions. This
    // limits the preview only; the complete stored description is untouched.
    QString text = empty ? QString("暂无说明") : note.left(32768);
    bool truncated = !empty && text.size() < note.size();
    if (!text.isEmpty() && text.at(text.size() - 1).isHighSurrogate()) text.chop(1);
    text.replace("\r\n", "\n"); text.replace('\r', '\n');
    auto wrap = [&](const QString &input, int limit) {
        QStringList lines;
        for (const auto &paragraph : input.split('\n')) {
            if (paragraph.isEmpty()) lines.append(QString());
            else {
                QTextLayout layout(paragraph, font, owner);
                QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); layout.setTextOption(option);
                layout.beginLayout();
                while (lines.size() < limit) {
                    auto line = layout.createLine(); if (!line.isValid()) break;
                    line.setLineWidth(width);
                    lines.append(paragraph.mid(line.textStart(), line.textLength()));
                }
                layout.endLayout();
            }
            if (lines.size() >= limit) break;
        }
        return lines;
    };
    auto lines = wrap(text, maxLines + 1);
    truncated = truncated || lines.size() > maxLines;
    if (truncated) {
        const auto hint = wrap("说明较长，仅显示前面部分。\n右键编辑命令可查看完整说明。", maxLines);
        lines = lines.mid(0, qMax(1, maxLines - hint.size() - 2));
        while (lines.size() > 1 && lines.last().trimmed().isEmpty()) lines.removeLast();
        lines.append("…"); lines.append(QString()); lines.append(hint);
    }
    text = lines.join('\n');
    // Always escape user text: descriptions may contain <tags>, ampersands or
    // literal commands. Only the preserved line breaks are rich text markup.
    return "<qt>" + text.toHtmlEscaped().replace('\n', "<br>") + "</qt>";
}
QString commandToolTipStyle(bool isDark) {
    return QString("QToolTip { background-color: %1; color: %2; border: 1px solid %3; padding: 8px; font-size: 10pt; font-weight: normal; }")
        .arg(isDark ? "#2b2b2b" : "#ffffff", isDark ? "#eeeeee" : "#202020", isDark ? "#777777" : "#9aa0a6");
}
bool listSlider(const CommandDef &def) { return def.sliderMode == static_cast<int>(CommandSliderMode::List); }
double defaultSliderValue(const CommandDef &def) { return listSlider(def) ? def.defaultListIndex : def.defaultValue; }
QString upperCommandHex(QString text) {
    for (int i = 0; i < text.size(); ++i) if (text[i] >= 'a' && text[i] <= 'f') text[i] = QChar(text[i].unicode() - 32);
    return text;
}
bool validListHex(const QString &text, bool complete = true) {
    return CommandSliderConfig::validHex(text, complete);
}
bool validSliderList(const CommandDef &def, QString &error) {
    if (def.sliderValues.size() > 256) { error = "列表最多允许 256 项。"; return false; }
    if (def.widgetType != static_cast<int>(CommandWidgetType::Slider) || !listSlider(def)) return true;
    if (def.sliderValues.isEmpty()) { error = "列表模式至少需要一项参数，请先添加列表项。"; return false; }
    if (def.defaultListIndex < 0 || def.defaultListIndex >= def.sliderValues.size()) { error = "请选择有效的默认列表项。"; return false; }
    for (int i = 0; i < def.sliderValues.size(); ++i) {
        if (def.sliderValues[i].isEmpty()) { error = QString("编号 %1 的参数内容不能为空。").arg(i); return false; }
        if (def.isHex && !validListHex(def.sliderValues[i])) { error = QString("编号 %1 必须填写完整的 HEX 字节，每个字节两位，仅接受 0–9、A–F 和空白。").arg(i); return false; }
    }
    return true;
}
bool readSliderList(const QJsonObject &object, CommandDef &def, QString &error) {
    if (!CommandSliderConfig::validate(object, error)) return false;
    def.sliderMode = object["sliderMode"].toInt(0); def.defaultListIndex = object["defaultListIndex"].toInt(0);
    if (object.contains("sliderValues")) {
        for (const auto &value : object["sliderValues"].toArray()) {
            def.sliderValues.append(def.isHex ? upperCommandHex(value.toString()) : value.toString());
        }
    }
    return validSliderList(def, error);
}
void writeSliderList(QJsonObject &object, const CommandDef &def) {
    object["sliderMode"] = def.sliderMode;
    object["sliderValues"] = QJsonArray::fromStringList(def.sliderValues);
    object["defaultListIndex"] = def.defaultListIndex;
}
void writeSwitchConfig(QJsonObject &object, const CommandDef &def) {
    if (def.widgetType != static_cast<int>(CommandWidgetType::Switch)) return;
    object["switchDefaultState"] = def.switchDefaultState;
    if (def.switchNeedsRepair) object["switchNeedsRepair"] = true;
    QJsonArray states;
    for (const auto &state : def.switchStates)
        states.append(QJsonObject{{"payload", state.payload}, {"annotation", state.annotation}, {"color", state.color}});
    object["switchStates"] = states;
}
bool validSwitchConfig(const CommandDef &def, QString &error) {
    if (def.widgetType == 2 && def.switchNeedsRepair) { error = "开关配置待补全，已禁止发送。请编辑并填写两个状态的命令后保存。"; return false; }
    QJsonObject object{{"widgetType", def.widgetType}, {"isHex", def.isHex},
        {"appendIndex", def.appendIndex}, {"customAppendPayload", def.customAppendPayload}};
    writeSwitchConfig(object, def);
    return CommandSwitchConfig::validate(object, error);
}
void readSwitchConfig(const QJsonObject &object, CommandDef &def) {
    if (def.widgetType != static_cast<int>(CommandWidgetType::Switch)) return;
    def.switchDefaultState = object["switchDefaultState"].toInt(0);
    def.switchNeedsRepair = object["switchNeedsRepair"].toBool();
    const auto states = object["switchStates"].toArray();
    for (int i = 0; i < 2; ++i) {
        const auto state = states[i].toObject();
        def.switchStates[i] = {def.isHex ? upperCommandHex(state["payload"].toString()) : state["payload"].toString(),
                               state["annotation"].toString(), state["color"].toString()};
    }
    def.autoSend = false; def.intervalMs = 0; def.extraPayload.clear();
}
CommandDef safeSwitchDef(CommandDef def) {
    def.widgetType = static_cast<int>(CommandWidgetType::Switch);
    def.autoSend = false; def.intervalMs = 0; def.payload.clear(); def.extraPayload.clear();
    return def;
}
QString switchStateLabel(const CommandDef &def, int state) {
    const auto label = def.switchStates[state].annotation;
    return label.isEmpty() ? QString("状态 %1").arg(state) : label;
}
class CommandListHexValidator : public QValidator {
public:
    explicit CommandListHexValidator(QObject *parent) : QValidator(parent) {}
    State validate(QString &text, int &) const override {
        if (!validListHex(text, false)) return Invalid;
        text = upperCommandHex(text); return Acceptable;
    }
};
class CommandListDelegate : public QStyledItemDelegate {
public:
    CommandListDelegate(std::function<bool()> hex, QObject *parent) : QStyledItemDelegate(parent), m_hex(std::move(hex)) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &) const override {
        auto edit = new QLineEdit(parent); edit->setObjectName("commandListValueEditor");
        if (m_hex()) edit->setValidator(new CommandListHexValidator(edit));
        return edit;
    }
private:
    std::function<bool()> m_hex;
};
using CommandColorTranslator=PanelUi::ColorTranslator;
class CommandColorDialog : public PanelUi::ColorDialog {
public:
    CommandColorDialog(const QColor &initial,QWidget *parent):PanelUi::ColorDialog(initial,parent,"commandColor","选择按钮颜色","使用透明按钮","清除自定义颜色，恢复默认透明按钮；只在保存命令后生效。"){}
};
QString appendPayloadFor(const CommandDef &def)
{
    if (def.appendIndex == 1) return QStringLiteral("\r\n");
    if (def.appendIndex == 2) return QStringLiteral("\n\r");
    if (def.appendIndex == 3) return QStringLiteral("\r");
    if (def.appendIndex == 4) return QStringLiteral("\n");
    if (def.appendIndex == 5) return def.customAppendPayload;
    return QString();
}

QString valueTextFor(const CommandDef &def, double value)
{
    if (listSlider(def)) return def.sliderValues.value(qBound(0, qRound(value), qMax(0, def.sliderValues.size() - 1)));
    return QString::number(value, 'f', qBound(0, def.decimals, 8));
}

QString formatHexInput(const QString &text)
{
    QString hex;
    for (const QChar c : text) {
        if ((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c.toUpper() >= QLatin1Char('A') && c.toUpper() <= QLatin1Char('F'))) {
            hex += c.toUpper();
        }
    }
    QString result;
    for (int i = 0; i < hex.size(); ++i) {
        if (i > 0 && (i % 2) == 0) result += QLatin1Char(' ');
        result += hex.at(i);
    }
    return result;
}

// Keep byte formatting separate from QTextEdit's clipboard shortcuts. Blocking
// non-HEX key text also blocks Ctrl+V/C/X/Z (their text is a control character).
class CommandPayloadEdit : public QTextEdit {
public:
    explicit CommandPayloadEdit(QWidget *parent) : QTextEdit(parent) {
        setAcceptRichText(false);
        connect(this, &QTextEdit::textChanged, this, [this] {
            if (!m_hex || m_normalizing) return;
            const auto original = toPlainText();
            // Preserve legacy/imported templates rather than silently deleting
            // their non-HEX content merely by opening the editor.
            if (!validListHex(original, false)) return;
            const auto formatted = formatHexInput(original);
            if (formatted != original) {
                const auto cursor = textCursor();
                replaceHex(original, digitPosition(original, cursor.position()), true,
                           digitPosition(original, cursor.anchor()));
            }
        });
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this] { scheduleHeight(); });
        connect(this, &QTextEdit::textChanged, this, [this] { scheduleHeight(); });
    }
    void enableGrowingHeight() {
        m_growing = true;
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scheduleHeight();
    }
    void setHexMode(bool enabled) {
        m_hex = enabled;
        const auto original = toPlainText();
        if (enabled && validListHex(original, false) && formatHexInput(original) != original) {
            const auto cursor = textCursor();
            replaceHex(original, digitPosition(original, cursor.position()), true,
                       digitPosition(original, cursor.anchor()));
        }
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QTextEdit::resizeEvent(event);
        if (event->size().width() != event->oldSize().width()) scheduleHeight();
    }
    void showEvent(QShowEvent *event) override { QTextEdit::showEvent(event); scheduleHeight(); }
    void changeEvent(QEvent *event) override {
        QTextEdit::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
            || event->type() == QEvent::ContentsRectChange) scheduleHeight();
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (!m_hex || isReadOnly()) { QTextEdit::keyPressEvent(event); return; }
        if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)
            || event->matches(QKeySequence::Paste) || event->matches(QKeySequence::SelectAll)
            || event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo)) {
            QTextEdit::keyPressEvent(event); return;
        }
        if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
            const auto original = toPlainText();
            if (!validListHex(original, false)) { QTextEdit::keyPressEvent(event); return; }
            auto value = formatHexInput(original).remove(' ');
            const auto cursor = textCursor();
            int start = digitPosition(original, cursor.selectionStart()), end = digitPosition(original, cursor.selectionEnd());
            if (start == end) {
                if (event->key() == Qt::Key_Backspace && start > 0) --start;
                else if (event->key() == Qt::Key_Delete && end < value.size()) ++end;
            }
            if (end > start) { value.remove(start, end - start); replaceHex(value, start); }
            event->accept(); return;
        }
        if (!event->text().isEmpty() && (event->text().at(0).isPrint() || event->text().at(0).isSpace())) {
            insertHex(event->text()); event->accept(); return;
        }
        QTextEdit::keyPressEvent(event);
    }
    void inputMethodEvent(QInputMethodEvent *event) override {
        if (!m_hex || isReadOnly()) { QTextEdit::inputMethodEvent(event); return; }
        if (!event->commitString().isEmpty()) insertHex(event->commitString());
        event->accept();
    }
    bool canInsertFromMimeData(const QMimeData *source) const override {
        return m_hex ? source->hasText() && validListHex(source->text(), false) : QTextEdit::canInsertFromMimeData(source);
    }
    void insertFromMimeData(const QMimeData *source) override {
        if (!m_hex) QTextEdit::insertFromMimeData(source);
        else if (source->hasText() && !isReadOnly()) insertHex(source->text());
    }
private:
    void scheduleHeight() {
        if (!m_growing || m_heightScheduled) return;
        m_heightScheduled = true;
        QTimer::singleShot(0, this, [this] {
            m_heightScheduled = false;
            const auto doc = document(); const auto layout = doc->documentLayout();
            const qreal documentHeight = layout->documentSize().height(); // Finish pending wrapping first.
            int lines = 0; qreal tenthLineBottom = 0;
            for (auto block = doc->begin(); block.isValid() && lines <= 10; block = block.next()) {
                const auto textLayout = block.layout();
                if (!textLayout) continue;
                for (int i = 0; i < textLayout->lineCount() && lines <= 10; ++i) {
                    if (++lines == 10) {
                        const auto line = textLayout->lineAt(i);
                        tenthLineBottom = layout->blockBoundingRect(block).top() + line.y() + line.height();
                    }
                }
            }
            const bool overflow = lines > 10;
            const auto policy = overflow ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff;
            if (verticalScrollBarPolicy() != policy) setVerticalScrollBarPolicy(policy);
            const int chrome = height() - viewport()->height();
            const qreal minimum = 3 * fontMetrics().lineSpacing() + 2 * doc->documentMargin();
            const qreal content = overflow ? tenthLineBottom + doc->documentMargin() : documentHeight;
            const int wanted = qCeil(qMax(minimum, content)) + chrome;
            if (height() != wanted) setFixedHeight(wanted);
        });
    }
    static int digitPosition(const QString &text, int position) {
        int digits = 0;
        for (int i = 0; i < qMin(position, text.size()); ++i) if (!text[i].isSpace()) ++digits;
        return digits;
    }
    void replaceHex(const QString &text, int position, bool joinPrevious = false, int anchor = -1) {
        const auto formatted = formatHexInput(text);
        const int count = digitPosition(formatted, formatted.size());
        const auto displayPosition = [count](int n) { n = qBound(0, n, count); return n + (n > 0 ? (n - 1) / 2 : 0); };
        m_normalizing = true;
        auto cursor = textCursor();
        if (joinPrevious) cursor.joinPreviousEditBlock(); else cursor.beginEditBlock();
        cursor.select(QTextCursor::Document); cursor.insertText(formatted); cursor.endEditBlock();
        cursor.setPosition(qMin(formatted.size(), displayPosition(anchor < 0 ? position : anchor)));
        cursor.setPosition(qMin(formatted.size(), displayPosition(position)), QTextCursor::KeepAnchor);
        setTextCursor(cursor); m_normalizing = false;
    }
    void insertHex(const QString &text) {
        if (!validListHex(text, false)) return; // Reject malformed pastes atomically.
        const auto incoming = formatHexInput(text).remove(' ');
        if (incoming.isEmpty()) return; // Spaces/newlines are presentation only.
        const auto original = toPlainText(); const auto cursor = textCursor();
        if (!validListHex(original, false) && cursor.selectedText().size() != original.size()) return;
        auto value = formatHexInput(original).remove(' ');
        const int start = digitPosition(original, cursor.selectionStart()), end = digitPosition(original, cursor.selectionEnd());
        value.replace(start, end - start, incoming); replaceHex(value, start + incoming.size());
    }
    bool m_hex = false, m_normalizing = false;
    bool m_growing = false, m_heightScheduled = false;
};

QByteArray hexBytes(const QString &text)
{
    return QByteArray::fromHex(formatHexInput(text).toLatin1());
}

QByteArray appendBytesFor(const CommandDef &def, const QString &resolvedAppend)
{
    if (def.appendIndex == 1) return QByteArray("\r\n", 2);
    if (def.appendIndex == 2) return QByteArray("\n\r", 2);
    if (def.appendIndex == 3) return QByteArray("\r", 1);
    if (def.appendIndex == 4) return QByteArray("\n", 1);
    if (def.appendIndex == 5) return hexBytes(resolvedAppend);
    return QByteArray();
}
QString escapedPayload(const QByteArray &data, bool hex)
{
    if (hex) return QString::fromLatin1(data.toHex().toUpper());
    QString result = QString::fromUtf8(data);
    result.replace(QStringLiteral("\r"), QStringLiteral("\\r"));
    result.replace(QStringLiteral("\n"), QStringLiteral("\\n"));
    return result;
}

QByteArray buildCommandPayload(const CommandDef &def, bool hasValue = false, double value = 0.0)
{
    QString error;
    if (hasValue && listSlider(def) && !validSliderList(def, error)) return {};
    QString payload = def.payload;
    // Slider commands do not have an extra-command field. Ignore legacy
    // values from older configurations as a second line of protection.
    QString extra = def.widgetType == static_cast<int>(CommandWidgetType::Slider)
        ? QString()
        : def.extraPayload;
    QString append = appendPayloadFor(def);
    const QString valueText = hasValue ? valueTextFor(def, value) : QString();

    if (hasValue) {
        const int valuePos = payload.indexOf(QStringLiteral("{value}"), 0, Qt::CaseInsensitive);
        payload.replace(QStringLiteral("{value}"), valueText, Qt::CaseInsensitive);
        extra.replace(QStringLiteral("{value}"), valueText, Qt::CaseInsensitive);
        append.replace(QStringLiteral("{value}"), valueText, Qt::CaseInsensitive);

        if (def.widgetType == static_cast<int>(CommandWidgetType::Slider) && valuePos < 0) {
            payload += valueText;
        }
    }

    if (def.isHex && def.widgetType == static_cast<int>(CommandWidgetType::Slider) && hasValue) {
        const int valuePos = def.payload.indexOf(QStringLiteral("{value}"), 0, Qt::CaseInsensitive);
        QByteArray result;
        if (valuePos >= 0) {
            const QString prefix = def.payload.left(valuePos);
            const QString suffix = def.payload.mid(valuePos + QStringLiteral("{value}").size());
            result += hexBytes(prefix);
            result += listSlider(def) ? hexBytes(valueText) : valueText.toUtf8();
            result += hexBytes(suffix);
        } else {
            result += hexBytes(def.payload);
            result += listSlider(def) ? hexBytes(valueText) : valueText.toUtf8();
        }
        result += hexBytes(extra);
        result += appendBytesFor(def, append);
        return result;
    }

    QByteArray result;
    if (def.isHex) {
        result += hexBytes(payload);
        result += hexBytes(extra);
        result += appendBytesFor(def, append);
    } else {
        result += payload.toUtf8();
        result += extra.toUtf8();
        result += append.toUtf8();
    }
    return result;
}
QByteArray switchPayloadFor(const CommandDef &def, int state) {
    auto command = def;
    command.payload = def.switchStates[state].payload;
    command.extraPayload.clear();
    return buildCommandPayload(command);
}
QString previewFor(const CommandDef &def)
{
    const bool slider = def.widgetType == static_cast<int>(CommandWidgetType::Slider);
    const QByteArray data = buildCommandPayload(def, slider, defaultSliderValue(def));
    return escapedPayload(data, def.isHex);
}

void setFormFieldVisible(QFormLayout *layout, QWidget *field, bool visible)
{
    if (!layout || !field) return;
    field->setVisible(visible);
    if (QWidget *label = layout->labelForField(field)) label->setVisible(visible);
}
}
// --- RemarkDialog ---

RemarkDialog::RemarkDialog(const QString &remark, QWidget *parent)
    : QDialog(parent), isEditing(false)
{
    setWindowTitle("查看/编辑说明");
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    resize(400, 300);
    
    QVBoxLayout *layout = new QVBoxLayout(this);
    
    textEdit = new QTextEdit(this);
    textEdit->setPlaceholderText("请输入该命令面板的说明信息。这些信息将被保存在面板配置中。");
    textEdit->setText(remark);
    textEdit->setReadOnly(true);
    layout->addWidget(textEdit);
    
    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnEdit = new QPushButton("编辑", this);
    btnSave = new QPushButton("保存", this);
    btnSave->hide(); // Hidden initially
    btnClose = new QPushButton("关闭", this);
    
    btnLayout->addStretch();
    btnLayout->addWidget(btnEdit);
    btnLayout->addWidget(btnSave);
    btnLayout->addWidget(btnClose);
    
    layout->addLayout(btnLayout);
    
    connect(btnEdit, &QPushButton::clicked, this, &RemarkDialog::onEditClicked);
    connect(btnSave, &QPushButton::clicked, this, &RemarkDialog::onSaveClicked);
    connect(btnClose, &QPushButton::clicked, this, &RemarkDialog::reject);
}

QString RemarkDialog::getRemark() const {
    return textEdit->toPlainText();
}

void RemarkDialog::onEditClicked() {
    isEditing = true;
    textEdit->setReadOnly(false);
    textEdit->setFocus();
    btnEdit->hide();
    btnSave->show();
}

void RemarkDialog::onSaveClicked() {
    accept();
}

// --- CommandEditDialog ---

CommandEditDialog::CommandEditDialog(const CommandDef &def, QWidget *parent)
    : QDialog(parent),
      m_selectedColor(def.btnColor),
      m_originalAutoSend(def.autoSend),
      m_lastSliderMode(def.widgetType == static_cast<int>(CommandWidgetType::Slider))
{
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setWindowTitle(QString::fromUtf8("编辑命令 - ") + (def.name.isEmpty() ? QString::fromUtf8("新命令") : def.name));
    setMinimumWidth(520);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    lblFormula = new QLabel(this);
    lblFormula->setStyleSheet(QStringLiteral("color: #888888; font-size: 10pt;"));
    lblFormula->setWordWrap(true);
    mainLayout->addWidget(lblFormula);
    m_formLayout = new QFormLayout();
    m_formLayout->setFormAlignment(Qt::AlignTop); m_formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    comboWidgetType = new QComboBox(this);
    comboWidgetType->setObjectName("commandWidgetType");
    comboWidgetType->addItem(QString::fromUtf8("按钮命令"), static_cast<int>(CommandWidgetType::Button));
    comboWidgetType->addItem(QString::fromUtf8("滑条命令"), static_cast<int>(CommandWidgetType::Slider));
    comboWidgetType->addItem(QString::fromUtf8("开关命令"), static_cast<int>(CommandWidgetType::Switch));
    comboWidgetType->setCurrentIndex(qBound(0, def.widgetType, 2));
    m_formLayout->addRow(QString::fromUtf8("命令类型:"), comboWidgetType);

    editName = new QLineEdit(this);
    editName->setText(def.name.isEmpty() ? (comboWidgetType->currentIndex() == 1 ? QString::fromUtf8("新滑条命令") : QString::fromUtf8("新命令")) : def.name);
    editName->setPlaceholderText(QString::fromUtf8("命令名称"));
    m_formLayout->addRow(QString::fromUtf8("名称:"), editName);

    editPayload = new CommandPayloadEdit(this);
    editPayload->setObjectName("commandPayload");
    editPayload->setPlainText(def.payload);
    static_cast<CommandPayloadEdit *>(editPayload)->setHexMode(def.isHex);
    static_cast<CommandPayloadEdit *>(editPayload)->enableGrowingHeight();
    editPayload->setPlaceholderText(QString::fromUtf8("命令内容"));
    editPayload->installEventFilter(this);
    QWidget *payloadContainer = new QWidget(this);
    QVBoxLayout *payloadLayout = new QVBoxLayout(payloadContainer);
    payloadLayout->setContentsMargins(0, 0, 0, 0);
    payloadLayout->setSpacing(2);
    payloadContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    lblPayloadHint = new QLabel(payloadContainer);
    lblPayloadHint->setWordWrap(true);
    lblPayloadHint->setStyleSheet(QStringLiteral("color: #888888;"));
    payloadLayout->addWidget(editPayload);
    m_formLayout->addRow(QString::fromUtf8("命令/前缀:"), payloadContainer);

    editExtraPayload = new CommandPayloadEdit(this);
    editExtraPayload->setObjectName("commandExtraPayload");
    editExtraPayload->setPlainText(def.extraPayload);
    static_cast<CommandPayloadEdit *>(editExtraPayload)->setHexMode(def.isHex);
    static_cast<CommandPayloadEdit *>(editExtraPayload)->enableGrowingHeight();
    editExtraPayload->setPlaceholderText(QString::fromUtf8("附加命令内容"));
    editExtraPayload->installEventFilter(this);
    QWidget *extraContainer = new QWidget(this);
    QVBoxLayout *extraLayout = new QVBoxLayout(extraContainer);
    extraLayout->setContentsMargins(0, 0, 0, 0);
    extraLayout->setSpacing(2);
    extraContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    lblExtraHint = new QLabel(extraContainer);
    lblExtraHint->setWordWrap(true);
    lblExtraHint->setStyleSheet(QStringLiteral("color: #888888;"));
    extraLayout->addWidget(editExtraPayload);
    m_formLayout->addRow(QString::fromUtf8("附加:"), extraContainer);
    sliderOptions = new QWidget(this);
    auto sliderLayout = new QVBoxLayout(sliderOptions); sliderLayout->setContentsMargins(0, 0, 0, 0); sliderLayout->setSpacing(8);
    sliderLayout->setSizeConstraint(QLayout::SetMinimumSize);
    auto modeForm = new QFormLayout; modeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    comboSliderMode = new QComboBox(sliderOptions); comboSliderMode->setObjectName("commandSliderMode");
    comboSliderMode->addItems({"常规模式", "列表模式"}); comboSliderMode->setCurrentIndex(listSlider(def) ? 1 : 0);
    comboSliderMode->setToolTip("常规模式发送连续数值；列表模式按编号选择并发送对应参数内容，编号从 0 开始，步进始终为 1。");
    modeForm->addRow("滑条模式：", comboSliderMode); sliderLayout->addLayout(modeForm);
    regularSliderOptions = new QWidget(sliderOptions); regularSliderOptions->setObjectName("commandRegularSliderOptions");
    QFormLayout *sliderForm = new QFormLayout(regularSliderOptions);
    sliderForm->setContentsMargins(0, 0, 0, 0); sliderForm->setVerticalSpacing(6);
    sliderForm->setFormAlignment(Qt::AlignTop); sliderForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    spinMinValue = new QDoubleSpinBox(sliderOptions);
    spinMaxValue = new QDoubleSpinBox(sliderOptions);
    spinStepValue = new QDoubleSpinBox(sliderOptions);
    spinDefaultValue = new QDoubleSpinBox(sliderOptions);
    for (QDoubleSpinBox *box : {spinMinValue, spinMaxValue, spinStepValue, spinDefaultValue}) {
        box->setRange(-1000000000.0, 1000000000.0);
        box->setDecimals(8);
    }
    spinMinValue->setValue(def.minValue);
    spinMaxValue->setValue(def.maxValue);
    spinStepValue->setValue(def.stepValue > 0.0 ? def.stepValue : 1.0);
    spinDefaultValue->setValue(def.defaultValue);
    spinDecimals = new QSpinBox(sliderOptions);
    spinDecimals->setRange(0, 8);
    spinDecimals->setValue(qBound(0, def.decimals, 8));
    comboSliderSendMode = new QComboBox(sliderOptions);
    comboSliderSendMode->setObjectName("commandSliderSendMode");
    comboSliderSendMode->addItems({QString::fromUtf8("松开滑条发送"), QString::fromUtf8("滑动发送（每50ms）")});
    comboSliderSendMode->setCurrentIndex(def.sliderSendMode == 1 ? 1 : 0);
    sliderForm->addRow(QString::fromUtf8("最小值:"), spinMinValue);
    sliderForm->addRow(QString::fromUtf8("最大值:"), spinMaxValue);
    sliderForm->addRow(QString::fromUtf8("步进:"), spinStepValue);
    sliderForm->addRow(QString::fromUtf8("默认值:"), spinDefaultValue);
    sliderForm->addRow(QString::fromUtf8("小数位:"), spinDecimals);
    sliderLayout->addWidget(regularSliderOptions);
    listSliderOptions = new QWidget(sliderOptions); listSliderOptions->setObjectName("commandListSliderOptions");
    auto listLayout = new QVBoxLayout(listSliderOptions); listLayout->setContentsMargins(0, 0, 0, 0); listLayout->setSpacing(6);
    listLayout->setSizeConstraint(QLayout::SetMinimumSize); listSliderOptions->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto defaultForm = new QFormLayout; defaultForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    comboListDefault = new QComboBox(listSliderOptions); comboListDefault->setObjectName("commandListDefault");
    comboListDefault->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); comboListDefault->setMinimumContentsLength(12);
    comboListDefault->setToolTip("选择软件打开或命令更新后的默认编号；调整顺序时保留所选参数，删除默认项时选择邻近项。不会因修改设置自动发送。");
    defaultForm->addRow("默认值：", comboListDefault); listLayout->addLayout(defaultForm);
    sliderList = new QTableWidget(0, 2, listSliderOptions); sliderList->setObjectName("commandSliderList");
    sliderList->setHorizontalHeaderLabels({"编号", "参数内容"}); sliderList->verticalHeader()->hide();
    sliderList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    sliderList->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    sliderList->setSelectionBehavior(QAbstractItemView::SelectRows); sliderList->setSelectionMode(QAbstractItemView::SingleSelection);
    sliderList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sliderList->setItemDelegateForColumn(1, new CommandListDelegate([this] { return chkHex->isChecked(); }, sliderList));
    sliderList->horizontalHeaderItem(0)->setToolTip("系统按顺序自动生成 0–255；增减或移动列表项后重新编号，不参与实际发送。");
    sliderList->horizontalHeaderItem(1)->setToolTip("当前编号对应的实际参数；文本按 UTF-8 发送，HEX 按字节发送。双击编辑，每项单独一行。");
    listLayout->addWidget(sliderList);
    auto listActions = new QHBoxLayout;
    btnListAdd = new QPushButton("添加", listSliderOptions); btnListAdd->setObjectName("commandListAdd");
    btnListDelete = new QPushButton("删除", listSliderOptions); btnListDelete->setObjectName("commandListDelete");
    btnListUp = new QPushButton("↑ 上移", listSliderOptions); btnListUp->setObjectName("commandListUp");
    btnListDown = new QPushButton("↓ 下移", listSliderOptions); btnListDown->setObjectName("commandListDown");
    for (auto button : {btnListAdd, btnListDelete, btnListUp, btnListDown}) listActions->addWidget(button);
    listLayout->addLayout(listActions);
    auto listHint = new QLabel("编号从 0 开始，步进为 1；滑条及右侧输入框显示编号，发送对应参数。最多 256 项，超过 10 项在列表内滚动。", listSliderOptions);
    listHint->setWordWrap(true); listLayout->addWidget(listHint); sliderLayout->addWidget(listSliderOptions);
    auto sendForm = new QFormLayout; sendForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    sendForm->addRow("发送方式：", comboSliderSendMode); sliderLayout->addLayout(sendForm);
    for (const auto &value : def.sliderValues) {
        const int row = sliderList->rowCount(); sliderList->insertRow(row);
        auto number = new QTableWidgetItem(QString::number(row)); number->setFlags(number->flags() & ~Qt::ItemIsEditable);
        number->setData(Qt::UserRole, QUuid::createUuid().toString()); sliderList->setItem(row, 0, number);
        sliderList->setItem(row, 1, new QTableWidgetItem(def.isHex && validListHex(value, false) ? formatHexInput(value) : value));
    }
    m_formLayout->addRow(QString::fromUtf8("滑条参数:"), sliderOptions);
    sliderOptions->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    switchOptions = new QWidget(this);
    switchOptions->setObjectName("commandSwitchOptions");
    auto switchLayout = new QVBoxLayout(switchOptions);
    switchLayout->setContentsMargins(0, 0, 0, 0); switchLayout->setSpacing(8);
    switchLayout->setAlignment(Qt::AlignTop);
    switchOptions->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto switchDefaultRow = new QWidget(switchOptions);
    switchDefaultRow->setObjectName("commandSwitchDefaultRow");
    switchDefaultRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto switchDefaultForm = new QFormLayout(switchDefaultRow);
    switchDefaultForm->setContentsMargins(0, 0, 0, 0); switchDefaultForm->setFormAlignment(Qt::AlignTop);
    comboSwitchDefault = new QComboBox(switchOptions); comboSwitchDefault->setObjectName("commandSwitchDefault");
    comboSwitchDefault->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    comboSwitchDefault->setMinimumContentsLength(16); comboSwitchDefault->setMinimumWidth(0);
    comboSwitchDefault->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    comboSwitchDefault->setToolTip("只设置启动或重新载入后的显示状态，不自动发送。点击发送另一状态的命令。显示状态不是设备执行确认。");
    switchDefaultForm->addRow("默认状态：", comboSwitchDefault); switchLayout->addWidget(switchDefaultRow);
    for (int i = 0; i < 2; ++i) {
        auto group = new QGroupBox(QString("状态 %1").arg(i), switchOptions);
        group->setObjectName(QString("commandSwitchState%1").arg(i));
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto form = new QFormLayout(group); form->setFormAlignment(Qt::AlignTop);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setVerticalSpacing(6);
        switchAnnotation[i] = new QLineEdit(def.switchStates[i].annotation, group);
        switchAnnotation[i]->setObjectName(QString("commandSwitchAnnotation%1").arg(i));
        switchAnnotation[i]->setToolTip("按钮展示的状态名称，可输入任意文字；不参与发送。留空显示状态编号。");
        form->addRow("状态名称：", switchAnnotation[i]);
        auto input = new CommandPayloadEdit(group); switchPayload[i] = input;
        input->setObjectName(QString("commandSwitchPayload%1").arg(i));
        input->setPlainText(def.switchStates[i].payload); input->setHexMode(def.isHex); input->enableGrowingHeight();
        input->setPlaceholderText("切换到此状态时发送的命令；Ctrl+Enter 换行"); input->installEventFilter(this);
        input->setToolTip("仅此命令受 HEX 格式限制；自动大写、按字节空格分隔，支持粘贴连续或多行字节。状态名称和说明不受限制。");
        form->addRow("命令内容：", input);
        switchColors[i] = def.switchStates[i].color;
        switchColorButton[i] = new QPushButton(group);
        switchColorButton[i]->setObjectName(QString("commandSwitchColor%1").arg(i));
        const QColor color(switchColors[i]);
        switchColorButton[i]->setText("选择按钮颜色");
        switchColorButton[i]->setToolTip(color.isValid() ? "当前颜色：" + color.name() : "默认使用透明颜色；可在颜色窗口中恢复透明按钮。");
        if (color.isValid()) switchColorButton[i]->setStyleSheet("background-color: " + color.name() + "; color: " + (color.lightness() > 150 ? "#202020;" : "#ffffff;"));
        form->addRow("按钮颜色：", switchColorButton[i]);
        connect(switchColorButton[i], &QPushButton::clicked, this, [this, i] { chooseSwitchColor(i); });
        connect(switchAnnotation[i], &QLineEdit::textChanged, this, [this] { refreshSwitchDefaults(); updatePreview(); });
        connect(input, &QTextEdit::textChanged, this, &CommandEditDialog::updatePreview);
        switchLayout->addWidget(group);
    }
    { const QSignalBlocker block(comboSwitchDefault); refreshSwitchDefaults(); comboSwitchDefault->setCurrentIndex(qBound(0, def.switchDefaultState, 1)); }
    connect(comboSwitchDefault, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CommandEditDialog::updatePreview);
    m_formLayout->addRow("开关参数：", switchOptions);

    chkHex = new QCheckBox(QStringLiteral("HEX"), this);
    chkHex->setObjectName("commandHex");
    chkHex->setChecked(def.isHex);
    chkHex->setToolTip("HEX 模式支持粘贴连续字节或带空格、换行、制表符的十六进制数据，自动大写并按字节分隔；分隔空白不发送。不支持 0x 前缀、地址或注释。");
    for (auto input : {editPayload, editExtraPayload}) input->setToolTip("HEX 模式可直接粘贴多行字节或连续数据（如 5A5A5A），自动显示为大写并按字节加空格。空白仅用于排版，不发送；含非十六进制字符的粘贴不会改变原内容。");
    m_formLayout->addRow(QString::fromUtf8("格式:"), chkHex);

    comboAppend = new QComboBox(this);
    comboAppend->setObjectName("commandAppend");
    comboAppend->addItems({QString::fromUtf8("无追加"), QStringLiteral("追加 \\r\\n"), QStringLiteral("追加 \\n\\r"), QStringLiteral("追加 \\r"), QStringLiteral("追加 \\n"), QString::fromUtf8("自定义")});
    comboAppend->setCurrentIndex(qBound(0, def.appendIndex, 5));
    m_formLayout->addRow(QString::fromUtf8("追加:"), comboAppend);

    editCustomAppend = new CommandPayloadEdit(this);
    editCustomAppend->setObjectName("commandCustomAppend");
    editCustomAppend->setPlainText(def.customAppendPayload);
    static_cast<CommandPayloadEdit *>(editCustomAppend)->setHexMode(def.isHex);
    editCustomAppend->setFixedHeight(58);
    editCustomAppend->setPlaceholderText(QString::fromUtf8("自定义追加内容；HEX 模式下请输入十六进制"));
    editCustomAppend->setToolTip(editPayload->toolTip());
    editCustomAppend->installEventFilter(this);
    m_formLayout->addRow(QString::fromUtf8("自定义追加:"), editCustomAppend);

    timerOptions = new QWidget(this);
    QHBoxLayout *timerLayout = new QHBoxLayout(timerOptions);
    timerLayout->setContentsMargins(0, 0, 0, 0);
    chkAutoSend = new QCheckBox(QString::fromUtf8("定时"), timerOptions);
    spinInterval = new QSpinBox(timerOptions);
    spinInterval->setRange(10, 3600000);
    spinInterval->setSuffix(QStringLiteral(" ms"));
    spinInterval->setValue(def.intervalMs > 0 ? def.intervalMs : 1000);
    chkAutoSend->setChecked(def.intervalMs > 0);
    timerLayout->addWidget(chkAutoSend);
    timerLayout->addStretch();
    timerLayout->addWidget(spinInterval);
    m_formLayout->addRow(QString::fromUtf8("定时:"), timerOptions);
    timerOptions->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    btnColorPicker = new QPushButton(QString::fromUtf8("选择按钮颜色"), this);
    btnColorPicker->setObjectName("commandColorPicker");
    if (!m_selectedColor.isEmpty()) btnColorPicker->setStyleSheet(QStringLiteral("background-color: %1; color: #ffffff;").arg(m_selectedColor));
    m_formLayout->addRow(QString::fromUtf8("颜色:"), btnColorPicker);

    editNote = new QTextEdit(this);
    editNote->setObjectName("commandDescription");
    editNote->setPlainText(def.note);
    editNote->setPlaceholderText(QString::fromUtf8("填写命令用途或操作说明；鼠标悬停在命令上时展示，不参与发送"));
    editNote->installEventFilter(this);
    editNote->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto updateNoteHeight = [this]() {
        const int lineHeight = qMax(1, editNote->fontMetrics().lineSpacing());
        const qreal documentHeight = editNote->document()->documentLayout()->documentSize().height();
        const int contentLines = qMax(1, qCeil(documentHeight / lineHeight));
        const int visibleLines = qBound(4, contentLines, 10);
        editNote->setFixedHeight(visibleLines * lineHeight + 12);
        editNote->setVerticalScrollBarPolicy(contentLines > 10
                                                 ? Qt::ScrollBarAsNeeded
                                                 : Qt::ScrollBarAlwaysOff);
    };
    connect(editNote, &QTextEdit::textChanged, this, updateNoteHeight);
    QTimer::singleShot(0, this, updateNoteHeight);
    m_formLayout->addRow(QString::fromUtf8("说明:"), editNote);
    for (int row = 0; row < m_formLayout->rowCount(); ++row) {
        QLayoutItem *labelItem = m_formLayout->itemAt(row, QFormLayout::LabelRole);
        QLabel *label = labelItem ? qobject_cast<QLabel*>(labelItem->widget()) : nullptr;
        if (label) {
            label->setStyleSheet(QStringLiteral("font-size: 10pt;"));
            label->setFont(editName->font());
            label->setMinimumHeight(editName->sizeHint().height());
        }
    }

    for (QLabel *label : sliderOptions->findChildren<QLabel*>()) {
        label->setStyleSheet(QStringLiteral("font-size: 10pt;"));
        label->setFont(editName->font());
        label->setMinimumHeight(editName->sizeHint().height());
    }
    // Nested forms do not inherit the outer form's label styling.
    // Keep switch labels at least as readable as the other command settings.
    for (QLabel *label : switchOptions->findChildren<QLabel*>()) {
        auto labelFont = editName->font();
        const auto points = qMax(10.0, labelFont.pointSizeF());
        labelFont.setPointSizeF(points); label->setFont(labelFont);
        label->setStyleSheet(QString("font-size: %1pt;").arg(points));
        label->setMinimumHeight(qMax(editName->sizeHint().height(), label->fontMetrics().height() + 2));
    }
    auto formContainer = new QWidget(this); formContainer->setLayout(m_formLayout);
    // Extra window height belongs below the form, not between its rows.
    formContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto formScroll = new QScrollArea(this); formScroll->setObjectName("commandEditScroll"); formScroll->setWidgetResizable(true);
    formScroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    formScroll->setFrameShape(QFrame::NoFrame); formScroll->setWidget(formContainer); mainLayout->addWidget(formScroll, 1);
    lblPreview = new QLabel(this);
    lblPreview->setObjectName("commandPreview");
    lblPreview->setTextFormat(Qt::PlainText); lblPreview->setFixedHeight(lblPreview->fontMetrics().lineSpacing() * 3 + 8);
    lblPreview->setWordWrap(true);
    lblPreview->setStyleSheet(QStringLiteral("color: #888888; font-size: 10pt;"));
    mainLayout->addWidget(lblPreview);

    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *btnDelete = new QPushButton(QString::fromUtf8("删除"), this);
    QPushButton *btnSave = new QPushButton(QString::fromUtf8("保存"), this);
    btnSave->setObjectName("commandSave");
    QPushButton *btnCancel = new QPushButton(QString::fromUtf8("取消"), this);
    btnDelete->setStyleSheet(QStringLiteral("QPushButton { background-color: #8b0000; color: white; border: none; padding: 5px; border-radius: 3px; } QPushButton:hover { background-color: #a52a2a; }"));
    btnSave->setDefault(true);
    buttons->addWidget(btnDelete);
    buttons->addStretch();
    buttons->addWidget(btnSave);
    buttons->addWidget(btnCancel);
    mainLayout->addLayout(buttons);

    connect(comboWidgetType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CommandEditDialog::updateCommandTypeUi);
    connect(comboAppend, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CommandEditDialog::updateCommandTypeUi);
    connect(chkHex, &QCheckBox::toggled, this, &CommandEditDialog::onHexToggled);
    connect(btnColorPicker, &QPushButton::clicked, this, &CommandEditDialog::onColorClicked);
    connect(btnDelete, &QPushButton::clicked, this, &CommandEditDialog::onDeleteClicked);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    auto previewChanged = [this]() { updatePreview(); };
    connect(editName, &QLineEdit::textChanged, this, previewChanged);
    connect(editPayload, &QTextEdit::textChanged, this, previewChanged);
    connect(editExtraPayload, &QTextEdit::textChanged, this, previewChanged);
    connect(editCustomAppend, &QTextEdit::textChanged, this, previewChanged);
    connect(spinMinValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, previewChanged);
    connect(spinMaxValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, previewChanged);
    connect(spinStepValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, previewChanged);
    connect(spinDefaultValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, previewChanged);
    connect(spinDecimals, QOverload<int>::of(&QSpinBox::valueChanged), this, previewChanged);    auto updateSliderDecimals = [this](int decimals) {
        const int clamped = qBound(0, decimals, 8);
        for (QDoubleSpinBox *box : {spinMinValue, spinMaxValue, spinStepValue, spinDefaultValue}) {
            box->setDecimals(clamped);
        }
    };
    connect(spinDecimals, QOverload<int>::of(&QSpinBox::valueChanged), this, updateSliderDecimals);
    updateSliderDecimals(spinDecimals->value());

    connect(btnSave, &QPushButton::clicked, this, &CommandEditDialog::saveCommand);
    connect(comboSliderMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CommandEditDialog::updateSliderModeUi);
    connect(comboListDefault, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CommandEditDialog::updatePreview);
    connect(sliderList, &QTableWidget::itemChanged, this, [this] { if (!m_refreshingList) refreshSliderList(); });
    connect(sliderList, &QTableWidget::itemSelectionChanged, this, &CommandEditDialog::refreshSliderList);
    connect(btnListAdd, &QPushButton::clicked, this, [this] { addSliderListItem(chkHex->isChecked() ? "00" : "0"); });
    connect(btnListDelete, &QPushButton::clicked, this, [this] {
        const int row = sliderList->currentRow(); if (row < 0) return;
        { const QSignalBlocker block(sliderList); sliderList->removeRow(row); if (sliderList->rowCount()) sliderList->selectRow(qMin(row, sliderList->rowCount() - 1)); }
        refreshSliderList();
    });
    connect(btnListUp, &QPushButton::clicked, this, [this] { moveSliderListItem(-1); });
    connect(btnListDown, &QPushButton::clicked, this, [this] { moveSliderListItem(1); });
    refreshSliderList(); comboListDefault->setCurrentIndex(qBound(0, def.defaultListIndex, qMax(0, sliderList->rowCount() - 1)));
    updateSliderModeUi();
    updateCommandTypeUi();
    const auto screen = QGuiApplication::primaryScreen(); resize(640, screen ? qBound(400, screen->availableGeometry().height() - 80, 800) : 760);
}

void CommandEditDialog::updateCommandTypeUi()
{
    const bool slider = comboWidgetType->currentIndex() == 1;
    const bool toggle = comboWidgetType->currentIndex() == 2;
    if (slider && !m_lastSliderMode && editExtraPayload) {
        // A button command's extra payload must not follow it into a slider.
        editExtraPayload->clear();
    }
    m_lastSliderMode = slider;

    if (lblFormula) {
        lblFormula->setText(toggle ? QString("点击切换到另一状态，发送该状态命令 + 追加。默认状态不自动发送；状态仅表示最近请求，不代表设备确认。") : slider
            ? (comboSliderMode->currentIndex() == 1 ? QString("实际发送命令为：命令前缀 + 当前编号对应的列表参数 + 追加") : QString("实际发送命令为：命令前缀 + 滑条值 + 追加"))
            : QString::fromUtf8("实际发送命令为：命令 + 附加命令 + 追加"));
    }
    QWidget *payloadContainer = editPayload ? editPayload->parentWidget() : nullptr;
    QWidget *extraContainer = editExtraPayload ? editExtraPayload->parentWidget() : nullptr;
    setFormFieldVisible(m_formLayout, payloadContainer, !toggle);
    setFormFieldVisible(m_formLayout, extraContainer, !slider && !toggle);
    setFormFieldVisible(m_formLayout, sliderOptions, slider);
    setFormFieldVisible(m_formLayout, switchOptions, toggle);
    setFormFieldVisible(m_formLayout, btnColorPicker, !toggle);
    setFormFieldVisible(m_formLayout, editCustomAppend, comboAppend->currentIndex() == 5);
    setFormFieldVisible(m_formLayout, timerOptions, !slider && !toggle);

    if (editName->text() == "新命令" || editName->text() == "新滑条命令" || editName->text() == "新开关命令") {
        editName->setText(toggle ? "新开关命令" : slider ? "新滑条命令" : "新命令");
    }
    if (QLabel *commandLabel = qobject_cast<QLabel*>(m_formLayout->labelForField(payloadContainer))) {
        commandLabel->setText(slider ? QString::fromUtf8("命令前缀:") : QString::fromUtf8("命令:"));
        commandLabel->setStyleSheet(QStringLiteral("font-size: 10pt;"));
        commandLabel->setFont(editName->font());
        commandLabel->setMinimumHeight(editName->sizeHint().height());
    }
    editPayload->setPlaceholderText(slider
        ? (comboSliderMode->currentIndex() == 1 ? QString("命令前缀，例如：SET VALUE=\n发送对应列表项的参数内容，不发送编号。\nHEX 模式下，前缀与列表参数均按字节发送。")
            : QString::fromUtf8("命令前缀，例如：SET VALUE=\n（发送内容为“SET VALUE={Value}”，Value为滑条值）\n（使用 Ctrl+Enter 或 Alt+Enter 换行）"))
        : QString::fromUtf8("命令内容\n（使用 Ctrl+Enter 或 Alt+Enter 换行）"));
    editExtraPayload->setPlaceholderText(QString::fromUtf8("附加命令内容\n（使用 Ctrl+Enter 或 Alt+Enter 换行）"));
    if (QLabel *extraLabel = qobject_cast<QLabel*>(m_formLayout->labelForField(extraContainer))) {
        extraLabel->setStyleSheet(QStringLiteral("font-size: 10pt;"));
        extraLabel->setFont(editName->font());
        extraLabel->setMinimumHeight(editName->sizeHint().height());
    }
    updatePreview();
}
void CommandEditDialog::updateSliderModeUi()
{
    const bool list = comboSliderMode->currentIndex() == 1;
    regularSliderOptions->setVisible(!list); listSliderOptions->setVisible(list);
    if (list && sliderList->rowCount() == 0) addSliderListItem(chkHex->isChecked() ? "00" : "0");
    updateCommandTypeUi();
}
void CommandEditDialog::addSliderListItem(const QString &value)
{
    if (sliderList->rowCount() >= 256) return;
    const int row = sliderList->rowCount();
    { const QSignalBlocker block(sliderList); sliderList->insertRow(row);
      auto number = new QTableWidgetItem(QString::number(row)); number->setFlags(number->flags() & ~Qt::ItemIsEditable);
      number->setData(Qt::UserRole, QUuid::createUuid().toString()); sliderList->setItem(row, 0, number);
      sliderList->setItem(row, 1, new QTableWidgetItem(value)); sliderList->selectRow(row); }
    refreshSliderList();
}
void CommandEditDialog::moveSliderListItem(int delta)
{
    const int row = sliderList->currentRow(), next = row + delta;
    if (row < 0 || next < 0 || next >= sliderList->rowCount()) return;
    { const QSignalBlocker block(sliderList);
      for (int col = 0; col < 2; ++col) { auto a = sliderList->takeItem(row, col), b = sliderList->takeItem(next, col); sliderList->setItem(row, col, b); sliderList->setItem(next, col, a); }
      sliderList->selectRow(next); }
    refreshSliderList();
}
void CommandEditDialog::refreshSliderList()
{
    if (m_refreshingList) return;
    m_refreshingList = true;
    const QString selected = comboListDefault->currentData().toString(); const int oldIndex = comboListDefault->currentIndex();
    { const QSignalBlocker block(sliderList), defaults(comboListDefault); comboListDefault->clear();
      for (int row = 0; row < sliderList->rowCount(); ++row) {
          auto number = sliderList->item(row, 0), value = sliderList->item(row, 1); if (!number || !value) continue;
          number->setText(QString::number(row));
          if (chkHex->isChecked() && validListHex(value->text(), false)) value->setText(formatHexInput(value->text()));
          value->setToolTip(value->text());
          comboListDefault->addItem(QString("%1：%2").arg(row).arg(value->text()), number->data(Qt::UserRole));
          comboListDefault->setItemData(row, value->text(), Qt::ToolTipRole);
      }
      const int found = comboListDefault->findData(selected);
      comboListDefault->setCurrentIndex(found >= 0 ? found : qBound(0, oldIndex, qMax(0, sliderList->rowCount() - 1))); }
    const int count = sliderList->rowCount(), row = sliderList->currentRow();
    sliderList->setFixedHeight(sliderList->horizontalHeader()->sizeHint().height() + qBound(1, count, 10) * sliderList->verticalHeader()->defaultSectionSize() + 2 * sliderList->frameWidth());
    sliderList->setVerticalScrollBarPolicy(count > 10 ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    btnListAdd->setEnabled(count < 256); btnListDelete->setEnabled(row >= 0);
    btnListUp->setEnabled(row > 0); btnListDown->setEnabled(row >= 0 && row < count - 1);
    comboListDefault->setEnabled(count > 0); m_refreshingList = false; updatePreview();
    listSliderOptions->layout()->invalidate(); listSliderOptions->layout()->activate(); listSliderOptions->updateGeometry();
    sliderOptions->layout()->invalidate(); sliderOptions->layout()->activate(); sliderOptions->updateGeometry(); m_formLayout->invalidate();
}
void CommandEditDialog::saveCommand()
{
    const auto def = getDef(); QString error;
    if (!validSwitchConfig(def, error)) { QMessageBox::warning(this, "开关配置错误", error); return; }
    if (def.isHex && def.widgetType != static_cast<int>(CommandWidgetType::Switch)) {
        const bool slider = def.widgetType == static_cast<int>(CommandWidgetType::Slider);
        auto completeHex = [slider](const QString &text) {
            // Existing slider templates may have a value insertion boundary.
            const auto parts = slider ? text.split(QRegularExpression("\\{value\\}", QRegularExpression::CaseInsensitiveOption)) : QStringList{text};
            for (const auto &part : parts) if (!part.trimmed().isEmpty() && !validListHex(part)) return false;
            return true;
        };
        if (!completeHex(def.payload) || (!slider && !completeHex(def.extraPayload))
            || (def.appendIndex == 5 && !completeHex(def.customAppendPayload))) {
            QMessageBox::warning(this, "命令格式错误", "HEX 命令必须由完整字节组成，每个字节两位，仅接受 0–9、A–F 和空白。请检查命令、附加命令及自定义追加内容。"); return;
        }
    }
    if (def.widgetType == static_cast<int>(CommandWidgetType::Slider)) {
        if (!validSliderList(def, error)) { QMessageBox::warning(this, "参数错误", error); return; }
        if (!listSlider(def) && (spinMaxValue->value() <= spinMinValue->value() || spinStepValue->value() <= 0.0)) {
            QMessageBox::warning(this, "参数错误", "最大值必须大于最小值，步进必须大于 0。"); return;
        }
    }
    accept();
}
void CommandEditDialog::updatePreview()
{
    if (comboWidgetType->currentIndex() == static_cast<int>(CommandWidgetType::Switch)) {
        const auto def = getDef();
        const auto text = QString("状态 0：%1\n状态 1：%2").arg(escapedPayload(switchPayloadFor(def, 0), def.isHex),
                                                               escapedPayload(switchPayloadFor(def, 1), def.isHex));
        lblPreview->setText("发送预览（不含状态名称/说明）：\n" + text);
        lblPreview->setToolTip(text);
        return;
    }
    CommandDef previewDef;
    previewDef.widgetType = comboWidgetType->currentIndex();
    previewDef.payload = editPayload->toPlainText();
    previewDef.extraPayload = editExtraPayload->toPlainText();
    previewDef.customAppendPayload = editCustomAppend->toPlainText();
    previewDef.appendIndex = comboAppend->currentIndex();
    previewDef.isHex = chkHex->isChecked();
    previewDef.defaultValue = spinDefaultValue->value();
    previewDef.decimals = spinDecimals->value();
    previewDef.sliderMode = comboSliderMode->currentIndex(); previewDef.defaultListIndex = qMax(0, comboListDefault->currentIndex());
    for (int row = 0; row < sliderList->rowCount(); ++row) previewDef.sliderValues.append(sliderList->item(row, 1)->text());
    const QString text = previewFor(previewDef);
    lblPreview->setToolTip(text);
    lblPreview->setText(QString::fromUtf8("命令预览：") + (text.isEmpty() ? QString::fromUtf8("（空）") : text));
}
CommandDef CommandEditDialog::getDef() const
{
    CommandDef def;
    def.name = editName->text();
    def.payload = editPayload->toPlainText();
    def.extraPayload = comboWidgetType->currentIndex() == static_cast<int>(CommandWidgetType::Button)
        ? editExtraPayload->toPlainText() : QString();
    def.customAppendPayload = editCustomAppend->toPlainText();
    def.isHex = chkHex->isChecked();
    def.intervalMs = chkAutoSend->isChecked() ? spinInterval->value() : 0;
    def.autoSend = false;
    def.appendIndex = comboAppend->currentIndex();
    def.btnColor = m_selectedColor;
    def.note = editNote->toPlainText();
    def.widgetType = comboWidgetType->currentIndex();
    def.switchDefaultState = qMax(0, comboSwitchDefault->currentIndex());
    for (int i = 0; i < 2; ++i) def.switchStates[i] = {switchPayload[i]->toPlainText(), switchAnnotation[i]->text(), switchColors[i]};
    def.minValue = spinMinValue->value();
    def.maxValue = spinMaxValue->value();
    def.stepValue = spinStepValue->value();
    def.defaultValue = qBound(def.minValue, spinDefaultValue->value(), def.maxValue);
    def.decimals = spinDecimals->value();
    def.sliderSendMode = comboSliderSendMode->currentIndex();
    def.sliderMode = comboSliderMode->currentIndex(); def.defaultListIndex = qMax(0, comboListDefault->currentIndex());
    for (int row = 0; row < sliderList->rowCount(); ++row) def.sliderValues.append(sliderList->item(row, 1)->text());
    if (def.widgetType != static_cast<int>(CommandWidgetType::Button)) {
        def.intervalMs = 0;
        def.autoSend = false;
    }
    return def;
}
bool CommandEditDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == editNote && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        const bool plainVerticalKey = !(keyEvent->modifiers()
                                        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier));
        const bool singleVisualLine =
            editNote->document()->blockCount() <= 1
            && editNote->document()->documentLayout()->documentSize().height()
                   <= editNote->fontMetrics().lineSpacing() * 1.5;
        if (plainVerticalKey && singleVisualLine
            && (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down)) {
            QTextCursor cursor = editNote->textCursor();
            cursor.movePosition(keyEvent->key() == Qt::Key_Up
                                    ? QTextCursor::Start
                                    : QTextCursor::End);
            editNote->setTextCursor(cursor);
            return true;
        }
    }

    const bool isHexEditor = obj == editPayload || obj == editExtraPayload || obj == editCustomAppend
        || obj == switchPayload[0] || obj == switchPayload[1];
    if (isHexEditor && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
                if (!chkHex->isChecked()) {
                    QTextEdit *edit = qobject_cast<QTextEdit*>(obj);
                    if (edit) edit->insertPlainText(QStringLiteral("\n"));
                }
                return true;
            }
            saveCommand();
            return true;
        }
    }
    return QDialog::eventFilter(obj, event);
}

void CommandEditDialog::onHexToggled(bool checked) {
    if (!checked) for (auto input : switchPayload) {
        const auto text = input->toPlainText(); if (text.isEmpty()) continue;
        const auto bytes = hexBytes(text);
        if (!validListHex(text) || QString::fromUtf8(bytes).toUtf8() != bytes) {
            const QSignalBlocker block(chkHex); chkHex->setChecked(true);
            QMessageBox::warning(this, "无法切换为文本", "开关命令包含未完成的 HEX 字节或不能无损表示为 UTF-8 的二进制数据。为避免内容丢失，保留 HEX 模式。"); return;
        }
    }
    if (!checked) for (int row = 0; row < sliderList->rowCount(); ++row) {
        const auto text = sliderList->item(row, 1)->text(); const auto bytes = hexBytes(text);
        if (!validListHex(text) || QString::fromUtf8(bytes).toUtf8() != bytes) {
            const QSignalBlocker block(chkHex); chkHex->setChecked(true);
            QMessageBox::warning(this, "无法切换为文本", "列表中有未完成的 HEX 字节或不能无损表示为 UTF-8 的二进制参数。为避免内容丢失，保留 HEX 模式。请先调整该参数或继续使用 HEX。"); return;
        }
    }
    auto convert = [checked](QTextEdit *editor) {
        const QString text = editor->toPlainText();
        auto input = static_cast<CommandPayloadEdit *>(editor); input->setHexMode(false);
        if (checked) {
            const QByteArray bytes = text.toUtf8();
            editor->setPlainText(QString::fromLatin1(bytes.toHex(' ').toUpper()));
        } else {
            editor->setPlainText(QString::fromUtf8(QByteArray::fromHex(text.toLatin1())));
        }
        input->setHexMode(checked);
    };
    convert(editPayload);
    convert(editExtraPayload);
    convert(editCustomAppend);
    for (auto input : switchPayload) convert(input);
    { const QSignalBlocker block(sliderList);
      for (int row = 0; row < sliderList->rowCount(); ++row) {
          auto value = sliderList->item(row, 1); const auto text = value->text();
          value->setText(checked ? QString::fromLatin1(text.toUtf8().toHex(' ').toUpper()) : QString::fromUtf8(hexBytes(text)));
      } }
    QTextEdit *focusedEditor = qobject_cast<QTextEdit*>(QApplication::focusWidget());
    if (focusedEditor == editPayload || focusedEditor == editExtraPayload || focusedEditor == editCustomAppend) {
        QTextCursor cursor = focusedEditor->textCursor();
        cursor.movePosition(QTextCursor::End);
        focusedEditor->setTextCursor(cursor);
    }
    refreshSliderList();
    updatePreview();
}
void CommandEditDialog::onColorClicked() {
    CommandColorTranslator translator; // Includes the live screen-picker hint; removed on return/cancel.
    CommandColorDialog dialog(QColor(m_selectedColor.isEmpty() ? "#333333" : m_selectedColor), this);
    if (dialog.exec() != QDialog::Accepted) return;
    m_selectedColor = dialog.useTransparent ? QString() : dialog.selectedColor().name();
    btnColorPicker->setText(dialog.useTransparent ? "使用透明按钮（默认）" : "选择按钮颜色");
    btnColorPicker->setStyleSheet(m_selectedColor.isEmpty() ? QString() : "background-color: " + m_selectedColor + "; color: #ffffff;");
}

void CommandEditDialog::refreshSwitchDefaults() {
    const QSignalBlocker block(comboSwitchDefault);
    const int selected = qMax(0, comboSwitchDefault->currentIndex());
    comboSwitchDefault->clear();
    for (int i = 0; i < 2; ++i) {
        const auto label = QString("状态 %1：%2").arg(i).arg(switchAnnotation[i]->text().isEmpty() ? QString("状态 %1").arg(i) : switchAnnotation[i]->text());
        comboSwitchDefault->addItem(label, i); comboSwitchDefault->setItemData(i, label, Qt::ToolTipRole);
    }
    comboSwitchDefault->setCurrentIndex(selected);
}

void CommandEditDialog::chooseSwitchColor(int state) {
    CommandColorTranslator translator;
    CommandColorDialog dialog(QColor(switchColors[state].isEmpty() ? "#333333" : switchColors[state]), this);
    if (dialog.exec() != QDialog::Accepted) return;
    switchColors[state] = dialog.useTransparent ? QString() : dialog.selectedColor().name();
    switchColorButton[state]->setText("选择按钮颜色");
    const QColor color(switchColors[state]);
    switchColorButton[state]->setToolTip(color.isValid() ? "当前颜色：" + color.name() : "默认使用透明颜色；可在颜色窗口中恢复透明按钮。");
    switchColorButton[state]->setStyleSheet(!color.isValid() ? QString() : "background-color: " + color.name() + "; color: " + (color.lightness() > 150 ? "#202020;" : "#ffffff;"));
}

void CommandEditDialog::onDeleteClicked() {
    emit deleteRequested();
    reject();
}

// --- CommandButton ---

CommandButton::CommandButton(const CommandDef &def, QWidget *parent)
    : QPushButton(parent), m_def(def)
{
    if (m_def.id.isEmpty()) {
        m_def.id = QUuid::createUuid().toString();
    }
    
    setMinimumSize(100, 40);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(2);
    
    QLabel *m_lblName = new QLabel(this);
    m_lblName->setAlignment(Qt::AlignCenter);
    m_lblName->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_lblName->setStyleSheet("font-size: 12pt; font-weight: bold;");
    
    QLabel *m_lblDesc = new QLabel(this);
    m_lblDesc->setAlignment(Qt::AlignCenter);
    m_lblDesc->setAttribute(Qt::WA_TransparentForMouseEvents);
    
    layout->addWidget(m_lblName);
    layout->addWidget(m_lblDesc);
    
    m_lblName->setObjectName("lblName");
    m_lblDesc->setObjectName("lblDesc");
    
    updateDef(m_def);

    m_timer = new QTimer(this);
    m_rightPressTimer = new QTimer(this);
    m_rightPressTimer->setSingleShot(true);
    connect(m_rightPressTimer, &QTimer::timeout, this, [this]() {
        if (!m_rightPressed) return;
        m_rightLongPressed = true;
        m_rightPressed = false;
        editCommand();
    });
    connect(m_timer, &QTimer::timeout, this, &CommandButton::onTimerTimeout);

    connect(this, &QPushButton::clicked, this, [this](){ onSendClicked(false); });
    
    if (m_def.autoSend) {
        m_timer->start(m_def.intervalMs);
    }
}

CommandDef CommandButton::getDef() const {
    return m_def;
}

void CommandButton::forceStopTimer() {
    if (m_timer->isActive()) {
        m_timer->stop();
    }
    m_def.autoSend = false;
    updateDef(m_def);
    emit contentChanged();
}

void CommandButton::updateDef(const CommandDef &def) {
    m_def = def;
    setToolTip(commandDescriptionTip(def.note, this));
    QString baseStyle;
    if (!m_def.btnColor.isEmpty()) {
        QColor c(m_def.btnColor);
        QString pressedColor = c.darker(150).name();
        QString hoverColor = c.lighter(110).name();
        baseStyle = QString("QPushButton { background-color: %1; color: #ffffff; border: 1px solid #555555; border-radius: 4px; padding: 4px; } "
                            "QPushButton:hover { background-color: %2; } "
                            "QPushButton:pressed { background-color: %3; }").arg(m_def.btnColor).arg(hoverColor).arg(pressedColor);
    } else {
        baseStyle = "QPushButton { background-color: transparent; border: 1px solid #888888; border-radius: 4px; padding: 4px; color: inherit; } "
                    "QPushButton:hover { background-color: rgba(128, 128, 128, 0.3); } "
                    "QPushButton:pressed { background-color: rgba(128, 128, 128, 0.5); }";
    }

    if (m_def.autoSend) {
        baseStyle += " QPushButton { border: 1px solid #00ff00; }";
    }
    
    setStyleSheet(baseStyle + commandToolTipStyle(m_tooltipIsDark));
    update();
    resizeEvent(nullptr);
}

void CommandButton::applyTheme(bool isDark) {
    m_tooltipIsDark = isDark;
    CommandButton::updateDef(m_def);
}

void CommandButton::resizeEvent(QResizeEvent *event) {
    if (event) QPushButton::resizeEvent(event);
    
    QLabel *m_lblName = findChild<QLabel*>("lblName");
    QLabel *m_lblDesc = findChild<QLabel*>("lblDesc");
    if (!m_lblName || !m_lblDesc) return;
    
    int availableWidth = width() - 10;
    QString displayName = m_def.name.isEmpty() ? "未命名命令" : m_def.name;
    QString elidedName = fontMetrics().elidedText(displayName, Qt::ElideRight, qMax(20, availableWidth));
    
    m_lblName->setText(elidedName);
    
    if (m_def.intervalMs > 0) {
        if (m_def.autoSend) {
            m_lblDesc->setText("[▶ 发送中 " + QString::number(m_def.intervalMs) + "ms]");
        } else {
            m_lblDesc->setText("[定时 " + QString::number(m_def.intervalMs) + "ms]");
        }
        } else {
        const QString fullPreview = previewFor(m_def);
        QString firstLine = fullPreview.split(QRegExp("[\r\n]"), QString::SkipEmptyParts).value(0);
        QString payloadPreview = fontMetrics().elidedText(firstLine, Qt::ElideRight, qMax(20, availableWidth));
        m_lblDesc->setText(payloadPreview);
    }

    QPushButton::setText(""); // clear standard text
}

void CommandButton::editCommand() {
    CommandEditDialog dlg(m_def, this->window());
    connect(&dlg, &CommandEditDialog::deleteRequested, this, &CommandButton::deleteCommand);
    if (dlg.exec() == QDialog::Accepted) {
        CommandDef newDef = dlg.getDef();
        newDef.id = m_def.id;
        if (newDef.widgetType != static_cast<int>(CommandWidgetType::Button)) {
            emit replaceRequested(this, newDef);
            return;
        }
        updateDef(newDef);
        
        if (m_def.autoSend) {
            m_timer->start(m_def.intervalMs);
        } else {
            m_timer->stop();
        }
        emit contentChanged();
    }
}

void CommandButton::deleteCommand() {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("确认");
    msgBox.setText("确定要删除这条命令吗？");
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    
    PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(parentWidget()->parentWidget());
    bool isDark = panel ? panel->isDarkTheme() : true;
    if (isDark) {
        msgBox.setStyleSheet("QMessageBox { background-color: #1e1e1e; } QLabel { color: #dddddd; font-size: 15px; } QPushButton { background-color: #2F4F4F; color: white; padding: 6px 20px; border-radius: 3px; border: none; font-size: 14px; } QPushButton:hover { background-color: #3F5F5F; }");
    } else {
        msgBox.setStyleSheet("QLabel { font-size: 15px; } QPushButton { background-color: #2a6a2a; color: white; padding: 6px 20px; border-radius: 3px; border: none; font-size: 14px; } QPushButton:hover { background-color: #3a7a3a; }");
    }
    
    int ret = msgBox.exec();
    if (ret == QMessageBox::Yes) {
        forceStopTimer();
        emit deleteRequested(this);
    }
}

void CommandButton::contextMenuEvent(QContextMenuEvent *event) {
    if (event->reason() == QContextMenuEvent::Keyboard) emit commandMenuRequested(this,mapToGlobal(rect().center()));
    event->accept();
}

bool CommandButton::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        const auto help = static_cast<QHelpEvent *>(event);
        const auto position = help->globalPos();
        QToolTip::showText(position, commandDescriptionTip(getDef().note, this, &position), this);
        event->accept(); return true;
    }
    return QPushButton::event(event);
}

bool CommandButton::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::ToolTip) return CommandButton::event(event);
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent *mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton) {
            if (event->type() == QEvent::MouseButtonPress) mousePressEvent(mouse);
            else mouseReleaseEvent(mouse);
            return true;
        }
    }
    if (event->type() == QEvent::ContextMenu) {
        contextMenuEvent(static_cast<QContextMenuEvent*>(event));
        return true;
    }
    return QPushButton::eventFilter(obj, event);
}

void CommandButton::sendDataPacket(bool isAuto) {
    emit sendRequested(buildCommandPayload(m_def), isAuto);
}

void CommandButton::onSendClicked(bool /*isAuto*/) {
    if (m_def.intervalMs > 0) {
        setTimedSending(!m_def.autoSend);
    } else {
        sendDataPacket(false);
    }
}
bool CommandButton::timedSending() const {return m_timer&&m_timer->isActive();}
void CommandButton::setTimedSending(bool enabled){
    if(getDef().widgetType!=static_cast<int>(CommandWidgetType::Button)||m_def.intervalMs<=0)return;
    if(timedSending()==enabled&&m_def.autoSend==enabled)return;
    m_def.autoSend=enabled;
    if(enabled){m_timer->start(m_def.intervalMs);sendDataPacket(true);}else m_timer->stop();
    updateDef(m_def);emit contentChanged();
}

void CommandButton::onTimerTimeout() {
    sendDataPacket(true);
}

void CommandButton::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) {
        m_rightPressed = true;
        m_rightLongPressed = false;
        m_rightPressGlobalPos = event->globalPos();
        m_rightPressTimer->start(700);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        m_isDragging = false;
    }
    QPushButton::mousePressEvent(event);
}

void CommandButton::mouseMoveEvent(QMouseEvent *event) {
    if (!(event->buttons() & Qt::LeftButton)) {
        QPushButton::mouseMoveEvent(event);
        return;
    }
    if (!m_isDragging && (event->pos() - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_isDragging = true;
        setDown(false);
        emit dragStarted(this);
    }
    if (m_isDragging) {
        emit dragMoved(this, event->globalPos());
    } else {
        QPushButton::mouseMoveEvent(event);
    }
}

void CommandButton::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) {
        const bool shouldOpenMenu = m_rightPressed && !m_rightLongPressed;
        m_rightPressed = false;
        m_rightPressTimer->stop();
        event->accept();
        if (shouldOpenMenu) emit commandMenuRequested(this,event->globalPos());
        return;
    }
    if (event->button() == Qt::LeftButton && m_isDragging) {
        m_isDragging = false;
        setDown(false);
        emit dragReleased(this);
        event->accept();
        return;
    }
    QPushButton::mouseReleaseEvent(event);
}


// --- SliderCommandWidget ---

SliderCommandWidget::SliderCommandWidget(const CommandDef &def, QWidget *parent)
    : CommandButton(def, parent), m_sliderDef(def)
{
    if (m_sliderDef.id.isEmpty()) m_sliderDef.id = QUuid::createUuid().toString();
    if (m_sliderDef.maxValue <= m_sliderDef.minValue) m_sliderDef.maxValue = m_sliderDef.minValue + 100.0;
    if (m_sliderDef.stepValue <= 0.0) m_sliderDef.stepValue = 1.0;
    m_sliderDef.decimals = qBound(0, m_sliderDef.decimals, 8);

    setMinimumSize(3 * 100 + 2 * 10, 108);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    if (QVBoxLayout *baseLayout = qobject_cast<QVBoxLayout*>(layout())) {
        while (QLayoutItem *item = baseLayout->takeAt(0)) {
            if (QWidget *widget = item->widget()) { widget->hide(); widget->deleteLater(); }
            delete item;
        }
        baseLayout->setContentsMargins(8, 6, 8, 6);
        baseLayout->setSpacing(3);

        QHBoxLayout *row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);
        m_nameLabel = new QLabel(this);
        m_nameLabel->setObjectName("sliderCommandName");
        m_nameLabel->setMinimumWidth(100);
        m_nameLabel->setMaximumWidth(180);
        m_nameLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_nameLabel->setStyleSheet(QStringLiteral("font-size: 12pt; font-weight: bold;"));
        m_slider = new QSlider(Qt::Horizontal, this);
        m_slider->setObjectName("sliderCommandControl");
        m_valueEdit = new QDoubleSpinBox(this);
        m_valueEdit->setObjectName("sliderCommandValue");
        m_valueEdit->setMinimumWidth(90);
        m_valueEdit->setKeyboardTracking(false);
        row->addWidget(m_nameLabel);
        row->addWidget(m_slider, 1);
        row->addWidget(m_valueEdit);
        baseLayout->addLayout(row);

        m_rangeLabel = new QLabel(this);
        m_rangeLabel->setObjectName("sliderCommandRange");
        m_rangeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_rangeLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_rangeLabel->setMinimumWidth(0);
        m_rangeLabel->setStyleSheet(QStringLiteral("font-size: 8pt; font-weight: normal;"));
        m_rangeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        baseLayout->addWidget(m_rangeLabel);

        m_previewLabel = new QLabel(this);
        m_previewLabel->setObjectName("sliderCommandPreview");
        m_previewLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_previewLabel->setStyleSheet(QStringLiteral("font-size: 9pt; font-weight: normal;"));
        m_previewLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        baseLayout->addWidget(m_previewLabel);
    }

    configureValueRange();
    m_slider->installEventFilter(this);
    m_valueEdit->installEventFilter(this);
    for (QWidget *child : m_valueEdit->findChildren<QWidget*>()) child->installEventFilter(this);
    m_sendTimer = new QTimer(this);
    m_sendTimer->setSingleShot(true);
    m_sendTimer->setInterval(50);

    connect(m_slider, &QSlider::valueChanged, this, &SliderCommandWidget::onSliderValueChanged);
    connect(m_slider, &QSlider::sliderReleased, this, &SliderCommandWidget::onSliderReleased);
    connect(m_valueEdit, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &SliderCommandWidget::onSpinValueChanged);
    connect(m_valueEdit, &QDoubleSpinBox::editingFinished,
            this, &SliderCommandWidget::onValueEditingFinished);
    connect(m_sendTimer, &QTimer::timeout, this, &SliderCommandWidget::onThrottleTimeout);
    updateSliderStyle();
    updateValueWidgets(defaultSliderValue(m_sliderDef));
}
CommandDef SliderCommandWidget::getDef() const
{
    return m_sliderDef;
}

void SliderCommandWidget::forceStopTimer()
{
    if (m_sendTimer) m_sendTimer->stop();
    m_pendingSend = false;
}

void SliderCommandWidget::sendDataPacket(bool isAuto)
{
    sendCurrentValue(isAuto);
}
void SliderCommandWidget::sendOnce(){
    forceStopTimer();if(!m_valueEdit)return;
    {const QSignalBlocker blocker(m_valueEdit);m_valueEdit->interpretText();}
    updateValueWidgets(m_valueEdit->value());m_hasLastSentValue=false;sendCurrentValue(false);
}
void SliderCommandWidget::sendDefaultValue(){forceStopTimer();updateValueWidgets(defaultSliderValue(m_sliderDef));m_hasLastSentValue=false;sendCurrentValue(false);}
void SliderCommandWidget::sendListItem(int index){if(!listSlider(m_sliderDef)||index<0||index>=m_sliderDef.sliderValues.size())return;forceStopTimer();updateValueWidgets(index);m_hasLastSentValue=false;sendCurrentValue(false);}

void SliderCommandWidget::updateDef(const CommandDef &def)
{
    m_sliderDef = def;
    if (m_sliderDef.maxValue <= m_sliderDef.minValue) m_sliderDef.maxValue = m_sliderDef.minValue + 100.0;
    if (m_sliderDef.stepValue <= 0.0) m_sliderDef.stepValue = 1.0;
    m_sliderDef.decimals = qBound(0, m_sliderDef.decimals, 8);
    forceStopTimer(); configureValueRange();
    m_hasLastSentValue = false;
    m_lastSentValue = 0.0;
    m_pendingSend = false;
    updateSliderStyle();
    updateValueWidgets(defaultSliderValue(m_sliderDef));
}
void SliderCommandWidget::configureValueRange()
{
    const QSignalBlocker slider(m_slider), spin(m_valueEdit);
    const bool list = listSlider(m_sliderDef); const int last = qMax(0, m_sliderDef.sliderValues.size() - 1);
    m_slider->setRange(0, list ? last : 10000); m_slider->setSingleStep(1); m_slider->setPageStep(list ? 1 : 10);
    m_valueEdit->setDecimals(list ? 0 : m_sliderDef.decimals);
    m_valueEdit->setRange(list ? 0 : m_sliderDef.minValue, list ? last : m_sliderDef.maxValue);
    m_valueEdit->setSingleStep(list ? 1 : m_sliderDef.stepValue);
    m_valueEdit->setKeyboardTracking(false);
    m_slider->setEnabled(!list || !m_sliderDef.sliderValues.isEmpty()); m_valueEdit->setEnabled(m_slider->isEnabled());
    m_valueEdit->setToolTip(commandDescriptionTip(m_sliderDef.note, this));
}

double SliderCommandWidget::normalizedValue(double value) const
{
    if (listSlider(m_sliderDef)) return qBound(0, qRound(value), qMax(0, m_sliderDef.sliderValues.size() - 1));
    value = qBound(m_sliderDef.minValue, value, m_sliderDef.maxValue);
    const double steps = qRound64((value - m_sliderDef.minValue) / m_sliderDef.stepValue);
    return qBound(m_sliderDef.minValue, m_sliderDef.minValue + steps * m_sliderDef.stepValue, m_sliderDef.maxValue);
}

double SliderCommandWidget::sliderToValue(int value) const
{
    if (listSlider(m_sliderDef)) return normalizedValue(value);
    const double span = m_sliderDef.maxValue - m_sliderDef.minValue;
    if (span <= 0.0) return m_sliderDef.minValue;
    return normalizedValue(m_sliderDef.minValue + span * static_cast<double>(value) / 10000.0);
}

int SliderCommandWidget::valueToSlider(double value) const
{
    if (listSlider(m_sliderDef)) return qRound(normalizedValue(value));
    const double span = m_sliderDef.maxValue - m_sliderDef.minValue;
    if (span <= 0.0) return 0;
    return qBound(0, qRound((normalizedValue(value) - m_sliderDef.minValue) * 10000.0 / span), 10000);
}

void SliderCommandWidget::updateValueWidgets(double value)
{
    setToolTip(commandDescriptionTip(m_sliderDef.note, this));
    value = normalizedValue(value);
    m_updatingValueWidgets = true;
    if (m_slider) {
        m_slider->blockSignals(true);
        m_slider->setValue(valueToSlider(value));
        m_slider->blockSignals(false);
    }
    if (m_valueEdit) {
        m_valueEdit->blockSignals(true);
        m_valueEdit->setValue(value);
        m_valueEdit->blockSignals(false);
    }
    m_updatingValueWidgets = false;
    if (m_nameLabel) m_nameLabel->setText(m_sliderDef.name.isEmpty() ? QString::fromUtf8("新滑条命令") : m_sliderDef.name);
    if (m_rangeLabel) {
        m_rangeLabel->setText(listSlider(m_sliderDef) ? QString("编号范围 0–%1    当前编号 %2").arg(qMax(0, m_sliderDef.sliderValues.size() - 1)).arg(qRound(value)) : QString::fromUtf8("最小值 %1    当前值 %2    最大值 %3")
                              .arg(valueTextFor(m_sliderDef, m_sliderDef.minValue),
                                   valueTextFor(m_sliderDef, value),
                                   valueTextFor(m_sliderDef, m_sliderDef.maxValue)));
    }
    if (m_previewLabel) {
        m_previewLabel->setTextFormat(Qt::PlainText);
        const QString preview = escapedPayload(buildCommandPayload(m_sliderDef, true, value), m_sliderDef.isHex);
        m_previewLabel->setToolTip(commandDescriptionTip(m_sliderDef.note, this));
        m_previewLabel->setText(QString::fromUtf8("命令预览：") + (preview.isEmpty() ? QString::fromUtf8("（空）") : preview));
    }
}

void SliderCommandWidget::sendCurrentValue(bool isAuto)
{
    if (!m_valueEdit) return;
    QString error; if (!validSliderList(m_sliderDef, error)) { forceStopTimer(); if (m_previewLabel) m_previewLabel->setText("参数无效：" + error); return; }
    const double value = normalizedValue(m_valueEdit->value());
    if (m_hasLastSentValue && qFuzzyCompare(m_lastSentValue + 1.0, value + 1.0)) return;
    emit sendRequested(buildCommandPayload(m_sliderDef, true, value), isAuto);
    m_lastSentValue = value;
    m_hasLastSentValue = true;
    if (m_previewLabel) {
        const QString preview = escapedPayload(buildCommandPayload(m_sliderDef, true, value), m_sliderDef.isHex);
        m_previewLabel->setText(QString::fromUtf8("命令预览：") + (preview.isEmpty() ? QString::fromUtf8("（空）") : preview));
    }
}

void SliderCommandWidget::onSliderValueChanged(int value)
{
    updateValueWidgets(sliderToValue(value));
    if (m_sliderDef.sliderSendMode == 1) {
        m_pendingSend = true;
        if (!m_sendTimer->isActive()) m_sendTimer->start();
    }
}

void SliderCommandWidget::onSliderReleased()
{
    if (m_sendTimer) m_sendTimer->stop();
    if (m_sliderDef.sliderSendMode == 0 || m_pendingSend) sendCurrentValue(false);
    m_pendingSend = false;
}

void SliderCommandWidget::onSpinValueChanged(double value)
{
    if (m_updatingValueWidgets) return;
    updateValueWidgets(value);
    sendCurrentValue(false);
}

void SliderCommandWidget::onValueEditingFinished()
{
    if (!m_valueEdit || m_updatingValueWidgets) return;
    updateValueWidgets(m_valueEdit->value());
}
void SliderCommandWidget::onThrottleTimeout()
{
    if (!m_pendingSend) return;
    m_pendingSend = false;
    sendCurrentValue(true);
    if (m_slider && m_slider->isSliderDown()) m_sendTimer->start();
}

void SliderCommandWidget::updateSliderStyle()
{
    const QColor custom(m_sliderDef.btnColor);
    const bool hasCustomColor = !m_sliderDef.btnColor.isEmpty() && custom.isValid();
    const QColor background = hasCustomColor ? custom : QColor(m_isDark ? "#333333" : "#ffffff");
    // Preserve a user-selected command color, but choose readable text using
    // relative luminance rather than forcing white on a light card.
    auto linear = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const double luminance = 0.2126 * linear(background.redF()) + 0.7152 * linear(background.greenF()) + 0.0722 * linear(background.blueF());
    const QString foreground = luminance > 0.179 ? "#202020" : "#ffffff";
    QColor accent = hasCustomColor ? (m_isDark ? custom.lighter(170) : custom.darker(120)) : QColor(m_isDark ? "#62b8ee" : "#1675b5");
    if (accent.lightness() < 20 || accent.lightness() > 235) accent = QColor(m_isDark ? "#62b8ee" : "#1675b5");
    const QString border = m_isDark ? "#666666" : "#b8c2cc", track = m_isDark ? "#666666" : "#d6dbe0";
    setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 4px; }"
        "QLabel { color: %2; background: transparent; } QSlider { background: transparent; }"
        "QSlider::groove:horizontal { height: 6px; background: %4; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: %5; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 16px; margin: -6px 0; background: %5; border: 1px solid %3; border-radius: 8px; }"
        "QDoubleSpinBox { min-width: 90px; background: %6; color: %7; border: 1px solid %3; }")
        .arg(hasCustomColor ? background.name() : "transparent", foreground, border, track, accent.name(), m_isDark ? "#2b2b2b" : "#ffffff", m_isDark ? "#dddddd" : "#333333") + commandToolTipStyle(m_isDark));
}
void SliderCommandWidget::applyTheme(bool isDark)
{
    m_isDark = isDark;
    updateSliderStyle(); // Styling must not change values or trigger sends.
}
void SliderCommandWidget::contextMenuEvent(QContextMenuEvent *event)
{
    CommandButton::contextMenuEvent(event);
}

bool SliderCommandWidget::eventFilter(QObject *obj, QEvent *event)
{
    return CommandButton::eventFilter(obj, event);
}

void SliderCommandWidget::editSliderCommand()
{
    CommandEditDialog dialog(m_sliderDef, window());
    connect(&dialog, &CommandEditDialog::deleteRequested, this, &SliderCommandWidget::deleteSliderCommand);
    if (dialog.exec() != QDialog::Accepted) return;
    CommandDef newDef = dialog.getDef();
    newDef.id = m_sliderDef.id;
    if (newDef.widgetType != static_cast<int>(CommandWidgetType::Slider)) {
        emit replaceRequested(this, newDef);
        return;
    }
    updateDef(newDef);
    emit contentChanged();
}

void SliderCommandWidget::deleteSliderCommand()
{
    deleteCommand();
}

// --- SwitchCommandWidget ---
namespace {
// A visual indicator, not a second clickable control. The surrounding command
// owns mouse/keyboard/drag handling and changes state only after send admission.
class CommandSwitchIndicator : public QWidget {
public:
    explicit CommandSwitchIndicator(QWidget *parent) : QWidget(parent) {
        setObjectName("commandSwitchIndicator"); setFixedSize(48, 26);
        setAttribute(Qt::WA_TransparentForMouseEvents); setFocusPolicy(Qt::NoFocus);
    }
    void setAppearance(bool on, bool dark, const QColor &custom, bool failed) {
        m_on = on; m_dark = dark; m_custom = custom; m_failed = failed;
        setProperty("switchOn", on); update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF track = QRectF(rect()).adjusted(1.5, 2, -1.5, -2);
        QColor fill = m_on ? QColor(m_dark ? "#51b1ef" : "#1675b5") : QColor(m_dark ? "#59616c" : "#cbd2da");
        if (m_custom.isValid()) fill = m_custom.lightness() > 150
            ? m_custom.darker(m_on ? 160 : 125) : m_custom.lighter(m_on ? 180 : 125);
        QColor border = m_failed ? QColor(m_dark ? "#ff8b8b" : "#b42318") : fill.darker(115);
        QColor thumb = fill.lightness() > 205 ? QColor("#334155") : QColor("#ffffff");
        if (!isEnabled()) { fill.setAlpha(100); border.setAlpha(100); thumb.setAlpha(140); }
        painter.setPen(QPen(border, 1)); painter.setBrush(fill);
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
        const qreal diameter = track.height() - 4;
        const qreal x = m_on ? track.right() - diameter - 2 : track.left() + 2;
        const QRectF knob(x, track.top() + 2, diameter, diameter);
        painter.setPen(QPen(QColor(0, 0, 0, 25), 0.6)); painter.setBrush(thumb);
        painter.drawEllipse(knob);
    }
private:
    bool m_on = false, m_dark = true, m_failed = false;
    QColor m_custom;
};
}
SwitchCommandWidget::SwitchCommandWidget(const CommandDef &def, QWidget *parent)
    : CommandButton(safeSwitchDef(def), parent) {
    setObjectName("switchCommandButton");
    auto name = findChild<QLabel *>("lblName"), state = findChild<QLabel *>("lblDesc");
    auto oldLayout = layout();
    while (auto item = oldLayout->takeAt(0)) delete item;
    delete oldLayout;
    auto row = new QHBoxLayout(this); row->setContentsMargins(8, 3, 8, 3); row->setSpacing(8);
    auto text = new QVBoxLayout; text->setContentsMargins(0, 0, 0, 0); text->setSpacing(1);
    for (auto label : {name, state}) {
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter); label->setMinimumSize(0, 0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        text->addWidget(label);
    }
    row->addLayout(text, 1);
    m_indicator = new CommandSwitchIndicator(this); row->addWidget(m_indicator, 0, Qt::AlignVCenter | Qt::AlignRight);
    setCursor(Qt::PointingHandCursor);
    updateDef(def);
}
void SwitchCommandWidget::updateDef(const CommandDef &def) {
    m_switchDef = safeSwitchDef(def);
    if (m_switchDef.id.isEmpty()) m_switchDef.id = CommandButton::getDef().id;
    m_state = qBound(0, def.switchDefaultState, 1);
    m_lastError = def.switchNeedsRepair ? QString("开关配置待补全，已禁止发送。请右键编辑命令。") : QString();
    CommandButton::updateDef(m_switchDef);
    refreshDisplay();
}
void SwitchCommandWidget::applyTheme(bool isDark) {
    m_isDark = isDark; refreshDisplay(); // Never reset the state or send here.
}
void SwitchCommandWidget::resizeEvent(QResizeEvent *event) {
    if (event) QPushButton::resizeEvent(event);
    refreshDisplay();
}
void SwitchCommandWidget::refreshDisplay() {
    auto name = findChild<QLabel *>("lblName"), state = findChild<QLabel *>("lblDesc");
    if (!name || !state) return;
    const int available = qMax(0, width() - 26 - (m_indicator ? m_indicator->width() : 0));
    const bool compact = height() < 52;
    const auto nameStyle = QString("font-size: %1pt; font-weight: bold;").arg(compact ? 9 : 11);
    const auto stateStyle = QString("font-size: %1pt; font-weight: normal;").arg(compact ? 8 : 9);
    if (name->styleSheet() != nameStyle) name->setStyleSheet(nameStyle);
    if (state->styleSheet() != stateStyle) state->setStyleSheet(stateStyle);
    name->setTextFormat(Qt::PlainText); state->setTextFormat(Qt::PlainText);
    name->setText(name->fontMetrics().elidedText(m_switchDef.name.isEmpty() ? "新开关命令" : m_switchDef.name, Qt::ElideRight, available));
    const auto annotation = m_switchDef.switchNeedsRepair ? QString("⚠ 待补全 · 禁止发送")
        : (m_lastError.isEmpty() ? QString() : QString("⚠ ")) + switchStateLabel(m_switchDef, m_state);
    state->setText(state->fontMetrics().elidedText(annotation, Qt::ElideRight, available));
    const QColor custom(m_switchDef.switchStates[m_state].color);
    const bool colored = custom.isValid();
    const QString foreground = colored ? (custom.lightness() > 150 ? "#202020" : "#ffffff") : (m_isDark ? "#dddddd" : "#333333");
    const QString border = !m_lastError.isEmpty() ? (m_isDark ? "#ff8b8b" : "#b42318") : (m_state ? (m_isDark ? "#62b8ee" : "#1675b5") : (m_isDark ? "#888888" : "#999999"));
    const auto style = QString("QPushButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 0px; } "
                          "QPushButton:hover { background-color: %4; } QPushButton:focus { border: 2px solid %3; } QLabel { color: %2; background: transparent; }")
                      .arg(colored ? custom.name() : "transparent", foreground, border, colored ? custom.lighter(115).name() : "rgba(128,128,128,0.2)") + commandToolTipStyle(m_isDark);
    if (styleSheet() != style) setStyleSheet(style);
    if (m_indicator) static_cast<CommandSwitchIndicator *>(m_indicator)->setAppearance(m_state == 1, m_isDark, custom, !m_lastError.isEmpty());
    setToolTip(commandDescriptionTip(m_switchDef.note, this));
    setAccessibleName(m_switchDef.name); setAccessibleDescription(annotation + (m_lastError.isEmpty() ? QString() : "；" + m_lastError));
    update();
}
void SwitchCommandWidget::sendDataPacket(bool isAuto) {
    if (isAuto) return;
    sendState(1-m_state);
}
void SwitchCommandWidget::sendState(int next) {
    if (m_submitting||next<0||next>1) return;
    QString error;
    if (!validSwitchConfig(m_switchDef, error)) { m_lastError = error; refreshDisplay(); return; }
    const auto data = switchPayloadFor(m_switchDef, next);
    const auto handler = m_sendHandler;
    if (!handler) { m_lastError = "发送入口未就绪，状态未改变。"; refreshDisplay(); return; }
    // A warning dialog can run a nested event loop. Guard repeated clicks and
    // destruction while the synchronous, bounded admission check is running.
    QPointer<SwitchCommandWidget> self(this);
    m_submitting = true;
    const bool accepted = handler(data);
    if (!self) return;
    m_submitting = false;
    if (accepted) { m_state = next; m_lastError.clear(); }
    else m_lastError = "命令未提交发送，状态未改变。请检查连接、Break 或发送队列。";
    refreshDisplay();
}
void SwitchCommandWidget::editCommand() {
    CommandEditDialog dialog(m_switchDef, window());
    connect(&dialog, &CommandEditDialog::deleteRequested, this, [this] { confirmDelete(); });
    if (dialog.exec() != QDialog::Accepted) return;
    auto def = dialog.getDef(); def.id = m_switchDef.id;
    if (def.widgetType != static_cast<int>(CommandWidgetType::Switch)) { emit replaceRequested(this, def); return; }
    updateDef(def); emit contentChanged();
}

namespace {
CommandButton *createCommandWidget(const CommandDef &def, QWidget *parent) {
    if (def.widgetType == static_cast<int>(CommandWidgetType::Slider)) return new SliderCommandWidget(def, parent);
    if (def.widgetType == static_cast<int>(CommandWidgetType::Switch)) return new SwitchCommandWidget(def, parent);
    return new CommandButton(def, parent);
}
}

// --- PanelContentWidget ---

PanelContentWidget::PanelContentWidget(const PanelDef &def, QWidget *parent)
    : QWidget(parent),
      m_panelId(def.id),
      m_panelName(def.name),
      m_remark(def.remark),
      m_columns(qBound(1, def.columns, PanelLayout::CommandMaxColumns)),
      m_buttonWidth(qBound(100, def.buttonWidth > 0 ? def.buttonWidth : 200, 400)),
      m_buttonHeight(qBound(40, def.buttonHeight > 0 ? def.buttonHeight : 60, 200))
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("PanelContentWidget { background-color: #1a1a1a; }");
    
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    
    // Top bar
    QHBoxLayout *topLayout = new QHBoxLayout();
    QVBoxLayout *headerTextLayout = new QVBoxLayout();
    QLabel *lblName = new QLabel(def.name, this);lblName->setTextFormat(Qt::PlainText);lblName->setWordWrap(true);
    lblName->setObjectName("headerName");
    lblName->setStyleSheet("font-size: 16px; color: white;");
    QLabel *lblSub = new QLabel("左键发送/切换，右键打开菜单，长按右键编辑。", this);
    lblSub->setObjectName("headerSub");
    lblSub->setStyleSheet("color: #888888; font-size: 12px;");
    headerTextLayout->addWidget(lblName);
    headerTextLayout->addWidget(lblSub);
    

    QPushButton *btnSettings = new QPushButton("面板设置", this);btnSettings->setObjectName("commandPanelSettings");btnSettings->setToolTip("修改当前面板名称、说明、布局列数及命令按钮尺寸。");
    QPushButton *btnAddCmd = new QPushButton("新增命令", this);
    btnAddCmd->setObjectName("commandAddCommand");btnAddCmd->setToolTip("新增按钮、滑条或开关命令；编辑并保存后加入当前面板。");

    topLayout->addLayout(headerTextLayout);
    topLayout->addStretch();

    auto btnLayout = new QPushButton(QStringLiteral("编辑布局"), this);
    btnLayout->setObjectName("commandEditLayout");
    btnLayout->setToolTip(QStringLiteral("设置自动排列或自定义分行，滑条独占一行。"));
    connect(btnLayout, &QPushButton::clicked, this, &PanelContentWidget::editLayout);
    topLayout->addWidget(btnSettings);
    topLayout->addWidget(btnLayout);
    topLayout->addWidget(btnAddCmd);
    
    connect(btnSettings, &QPushButton::clicked, this, &PanelContentWidget::onSettingsClicked);
    
    mainLayout->addLayout(topLayout);
    
    // Scroll area for commands
    scrollContainer = new QWidget(this);
    scrollContainer->setStyleSheet("background-color: transparent;");
    
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setObjectName("commandPanelScroll"); scroll->viewport()->setObjectName("commandPanelViewport");
    scroll->setWidgetResizable(true);
    scroll->setWidget(scrollContainer);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    
    mainLayout->addWidget(scroll);
    

    connect(btnAddCmd, &QPushButton::clicked, this, &PanelContentWidget::onAddCommand);


    for (const auto &cmd : def.commands) appendCommandWidget(cmd);
    m_layoutRows = def.layoutRows;
    rebuildLayout();
    applyTheme(m_isDark);
}

PanelDef PanelContentWidget::getDef() const {
    PanelDef def;
    def.id = m_panelId;
    def.name = m_panelName;
    def.remark = m_remark;
    def.columns = m_columns;
    def.buttonWidth = m_buttonWidth;
    def.buttonHeight = m_buttonHeight;
    def.layoutRows = m_layoutRows;
    for (CommandButton *btn : m_buttons) {
        def.commands.append(btn->getDef());
    }
    return def;
}


void PanelContentWidget::onSettingsClicked() {
    QDialog dlg(this);
    dlg.setObjectName("commandPanelSettingsDialog");PanelUi::windowControls(&dlg);
    dlg.setWindowTitle("面板设置");
    dlg.setMinimumSize(380, 250);
    dlg.resize(540, 520);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    
    QString style = "QDialog { font-size: 14px; } QLabel { font-size: 14px; } QSpinBox { font-size: 14px; padding: 4px; } QPushButton { font-size: 14px; padding: 6px; border-radius: 4px; min-width: 60px; }";
    if (isDarkTheme()) {
        dlg.setStyleSheet(style + "QDialog { background-color: #1a1a1a; color: #dddddd; } QLabel { color: #dddddd; } QSpinBox { background-color: #333333; color: #dddddd; border: 1px solid #555555; } QPushButton { background-color: #333333; color: #dddddd; border: 1px solid #555555; } QPushButton:hover { background-color: #444444; }");
    } else {
        dlg.setStyleSheet(style);
    }
    
    QFormLayout *formLayout = new QFormLayout(&dlg);
    formLayout->setSpacing(15);
    formLayout->setContentsMargins(20, 20, 20, 20);
    auto editPanelName=new QLineEdit(m_panelName,&dlg);editPanelName->setObjectName("commandPanelNameEdit");editPanelName->setMaxLength(1024);formLayout->addRow("面板名称:",editPanelName);
    auto editDescription=new QTextEdit(&dlg);editDescription->setObjectName("commandPanelDescriptionEdit");editDescription->setAcceptRichText(false);editDescription->setPlaceholderText("请输入该命令面板的说明信息。这些信息将保存在面板配置中，不参与命令发送。");editDescription->setPlainText(m_remark);formLayout->addRow("面板说明:",editDescription);
    
    QSpinBox *spinCols = new QSpinBox(&dlg);
    spinCols->setObjectName("commandPanelColumns");
    spinCols->setRange(1, PanelLayout::CommandMaxColumns);
    spinCols->setValue(m_columns);
    formLayout->addRow("布局列数 (1~10):", spinCols);

    QSpinBox *spinButtonWidth = new QSpinBox(&dlg);
    spinButtonWidth->setRange(100, 400);
    spinButtonWidth->setValue(qBound(100, m_buttonWidth, 400));
    formLayout->addRow("按钮宽度 (100~400):", spinButtonWidth);

    QSpinBox *spinButtonHeight = new QSpinBox(&dlg);
    spinButtonHeight->setRange(40, 200);
    spinButtonHeight->setValue(qBound(40, m_buttonHeight, 200));
    formLayout->addRow("按钮高度 (40~200):", spinButtonHeight);

    QPushButton *btnReset = new QPushButton("恢复默认布局", &dlg);
    btnReset->setToolTip("列数恢复为 3，按钮宽度恢复为 200，高度恢复为 60");
    formLayout->addRow(btnReset);

    QDialogButtonBox *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    formLayout->addRow(btnBox);
    
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, [&]{if(editPanelName->text().trimmed().isEmpty()){QMessageBox::warning(&dlg,"名称不能为空","请输入面板名称。");return;}dlg.accept();});
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(btnReset, &QPushButton::clicked, &dlg, [spinCols, spinButtonWidth, spinButtonHeight]() {
        spinCols->setValue(3);
        spinButtonWidth->setValue(200);
        spinButtonHeight->setValue(60);
    });

    if (dlg.exec() == QDialog::Accepted) {
        m_panelName=editPanelName->text().trimmed();m_remark=editDescription->toPlainText();if(auto header=findChild<QLabel*>("headerName"))header->setText(m_panelName);
        m_columns = spinCols->value();
        m_buttonWidth = spinButtonWidth->value();
        m_buttonHeight = spinButtonHeight->value();
        rebuildLayout(true);
        emit contentChanged();
    }
}

void PanelContentWidget::onViewRemark() {
    RemarkDialog dlg(m_remark, this);
    if (dlg.exec() == QDialog::Accepted) {
        setRemark(dlg.getRemark());
    }
}

void PanelContentWidget::connectCommandWidget(CommandButton *btn)
{
    if (!btn) return;
    btn->applyTheme(m_isDark);
    if (auto toggle = qobject_cast<SwitchCommandWidget *>(btn)) toggle->setSendHandler(m_switchSendHandler);
    connect(btn, &CommandButton::sendRequested, this, &PanelContentWidget::sendRequested);
    connect(btn, &CommandButton::deleteRequested, this, &PanelContentWidget::onCommandDeleted);
    connect(btn, &CommandButton::replaceRequested, this, &PanelContentWidget::onCommandReplaced);
    connect(btn, &CommandButton::contentChanged, this, &PanelContentWidget::contentChanged);
    connect(btn, &CommandButton::dragStarted, this, &PanelContentWidget::onDragStarted);
    connect(btn, &CommandButton::dragMoved, this, &PanelContentWidget::onDragMoved);
    connect(btn, &CommandButton::dragReleased, this, &PanelContentWidget::onDragReleased);
    connect(btn, &CommandButton::commandMenuRequested, this, &PanelContentWidget::onCommandMenuRequested);
}

void PanelContentWidget::onCommandMenuRequested(CommandButton *btn, const QPoint &globalPos)
{
    const int index = m_buttons.indexOf(btn);
    if (index < 0) return;
    QMenu *menu = new QMenu(this);
    menu->setObjectName("commandItemMenu");
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    const auto definition=btn->getDef();
    if(auto toggle=qobject_cast<SwitchCommandWidget*>(btn)){
        auto toggleAction=menu->addAction("切换并发送："+switchStateLabel(definition,1-toggle->currentState()).left(48).replace("&","&&"));toggleAction->setObjectName("commandMenuToggle");toggleAction->setEnabled(!definition.switchNeedsRepair);connect(toggleAction,&QAction::triggered,toggle,[toggle]{toggle->sendOnce();});
        for(int state=0;state<2;++state){auto action=menu->addAction(QString("发送状态 %1：%2%3").arg(state).arg(switchStateLabel(definition,state).left(48).replace("&","&&"),state==toggle->currentState()?"（当前）":""));action->setObjectName("commandMenuState"+QString::number(state));action->setEnabled(!definition.switchNeedsRepair);connect(action,&QAction::triggered,toggle,[toggle,state]{toggle->sendState(state);});}
    }else if(auto slider=qobject_cast<SliderCommandWidget*>(btn)){
        auto current=menu->addAction(listSlider(definition)?"发送当前编号对应值":"发送当前滑条值");current->setObjectName("commandMenuSendOnce");connect(current,&QAction::triggered,slider,&SliderCommandWidget::sendOnce);
        auto defaults=menu->addAction(listSlider(definition)?"恢复默认编号并发送":"恢复默认值并发送");defaults->setObjectName("commandMenuSendDefault");connect(defaults,&QAction::triggered,slider,&SliderCommandWidget::sendDefaultValue);
        if(listSlider(definition)){auto list=menu->addMenu("选择列表项并发送");list->setObjectName("commandMenuListValues");list->setStyleSheet("QMenu {menu-scrollable:1;}");for(int i=0;i<definition.sliderValues.size();++i){auto value=definition.sliderValues[i];value.replace('\n'," ").replace('\r'," ");if(value.size()>48)value=value.left(48)+"…";auto action=list->addAction(QString("%1：%2").arg(i).arg(value.replace("&","&&")));action->setObjectName("commandMenuListValue"+QString::number(i));connect(action,&QAction::triggered,slider,[slider,i]{slider->sendListItem(i);});}}
    }else{
        auto once=menu->addAction("发送一次");once->setObjectName("commandMenuSendOnce");connect(once,&QAction::triggered,btn,&CommandButton::sendOnce);
        if(definition.intervalMs>0){auto start=menu->addAction("开始定时发送");start->setObjectName("commandMenuStartTimed");start->setEnabled(!btn->timedSending());connect(start,&QAction::triggered,btn,[btn]{btn->setTimedSending(true);});auto stop=menu->addAction("停止定时发送");stop->setObjectName("commandMenuStopTimed");stop->setEnabled(btn->timedSending());connect(stop,&QAction::triggered,btn,[btn]{btn->setTimedSending(false);});}
    }
    menu->addSeparator();
    auto edit=menu->addAction("编辑命令");edit->setObjectName("commandMenuEdit");connect(edit,&QAction::triggered,btn,&CommandButton::openEditor);
    QAction *add = menu->addAction(QStringLiteral("新增命令"));
    QAction *copy = menu->addAction(QStringLiteral("复制命令"));
    menu->addSeparator();
    QAction *previous = menu->addAction(QStringLiteral("命令位置前移"));
    QAction *next = menu->addAction(QStringLiteral("命令位置后移"));
    previous->setEnabled(!m_layoutRows && index > 0);
    next->setEnabled(!m_layoutRows && index + 1 < m_buttons.size());
    menu->addSeparator();
    QAction *remove = menu->addAction(QStringLiteral("删除命令"));
    connect(add, &QAction::triggered, this, &PanelContentWidget::onAddCommand);
    connect(copy, &QAction::triggered, btn, [this, btn]() {
        const int currentIndex = m_buttons.indexOf(btn);
        if (currentIndex < 0) return;
        CommandDef copyDef = btn->getDef();
        QString baseName = copyDef.name;
        baseName.remove(QRegularExpression(QStringLiteral("_[0-9]+$")));
        int suffix = 1;
        QString candidate;
        bool exists;
        do {
            candidate = baseName + QStringLiteral("_%1").arg(suffix++);
            exists = false;
            for (CommandButton *other : m_buttons) {
                if (other->getDef().name == candidate) { exists = true; break; }
            }
        } while (exists);
        copyDef.name = candidate;
        copyDef.id = QUuid::createUuid().toString();
        // Duplicating an active command must not start a second transmitter.
        copyDef.autoSend = false;
        appendCommandWidget(copyDef);
        m_buttons.move(m_buttons.size() - 1, currentIndex + 1);
        rebuildLayout();
        emit contentChanged();
    });
    const auto moveCommand = [this, btn](int delta) {
        const int current = m_buttons.indexOf(btn);
        const int target = current + delta;
        if (current < 0 || target < 0 || target >= m_buttons.size()) return;
        m_buttons.move(current, target);
        rebuildLayout();
        emit contentChanged();
    };
    connect(previous, &QAction::triggered, btn, [moveCommand]() { moveCommand(-1); });
    connect(next, &QAction::triggered, btn, [moveCommand]() { moveCommand(1); });
    connect(remove, &QAction::triggered, btn, &CommandButton::confirmDelete);
    menu->popup(globalPos);
}

void PanelContentWidget::appendCommandWidget(CommandDef def)
{
    if (def.id.trimmed().isEmpty() || m_commandIds.contains(def.id))
        def.id = QUuid::createUuid().toString();
    m_commandIds.insert(def.id);
    auto btn = createCommandWidget(def, scrollContainer);
    connectCommandWidget(btn);
    btn->show();
    m_buttons.append(btn);
}

void PanelContentWidget::addCommand(const CommandDef &cmdDef)
{
    appendCommandWidget(cmdDef);
    rebuildLayout();
}
void PanelContentWidget::onAddCommand() {
    CommandDef def;
    def.name = "新命令";
    def.intervalMs = 0;
    def.appendIndex = 1;
    
    CommandEditDialog dlg(def, this->window());
    if (dlg.exec() == QDialog::Accepted) {
        addCommand(dlg.getDef());
        emit contentChanged();
    }
}

void PanelContentWidget::onCommandDeleted(CommandButton *btn)
{
    if (!btn) return;
    if (!m_buttons.removeOne(btn)) return;
    m_commandIds.remove(btn->getDef().id);
    btn->deleteLater();
    rebuildLayout();
    emit contentChanged();
}

void PanelContentWidget::onCommandReplaced(CommandButton *btn, const CommandDef &def)
{
    const int index = m_buttons.indexOf(btn);
    if (index < 0) return;
    CommandDef replacementDef = def;
    replacementDef.id = btn->getDef().id;
    CommandButton *replacement = createCommandWidget(replacementDef, scrollContainer);
    connectCommandWidget(replacement);
    replacement->show();
    m_buttons[index] = replacement;
    m_draggedButton = nullptr;
    btn->deleteLater();
    rebuildLayout();
    emit contentChanged();
}
QVector<PanelLayout::Item> PanelContentWidget::layoutItems() const
{
    QVector<PanelLayout::Item> items;
    items.reserve(m_buttons.size());
    for (auto button : m_buttons) {
        const auto def = button->getDef();
        items.append({def.id, def.name, qobject_cast<SliderCommandWidget *>(button) != nullptr});
    }
    return items;
}

void PanelContentWidget::editLayout()
{
    PanelLayout::edit(this, PanelLayout::EditorKind::Commands, layoutItems(), m_columns, m_layoutRows,
        [this](int columns, const PanelLayout::Layout &layout, QString &error) {
            if (m_columns == columns && m_layoutRows == layout) return true;
            const int previousColumns = m_columns;
            const auto previousLayout = m_layoutRows;
            m_columns = columns;
            m_layoutRows = layout;
            if (m_saveLayout && !m_saveLayout(error)) {
                m_columns = previousColumns;
                m_layoutRows = previousLayout;
                return false;
            }
            rebuildLayout();
            return true;
        });
}

void PanelContentWidget::rebuildLayout(bool animate)
{
    auto scroll = qobject_cast<QScrollArea *>(scrollContainer->parentWidget()->parentWidget());
    const int viewportWidth = scroll ? scroll->viewport()->width() : scrollContainer->width();
    const int availableWidth = viewportWidth < 100 ? 800 : viewportWidth;
    const int buttonWidth = m_buttonWidth;
    const int normalHeight = m_buttonHeight;
    const int spacing = 10;
    const int sliderWidth = 3 * buttonWidth + 2 * spacing;
    const int sliderHeight = 108;
    const auto items = layoutItems();
    const auto rows = PanelLayout::arrange(items, m_columns, m_layoutRows.value_or(PanelLayout::Rows{}));
    if (m_layoutRows) m_layoutRows = rows;
    if (scroll) scroll->setHorizontalScrollBarPolicy(m_layoutRows ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

    QHash<QString, CommandButton *> buttons;
    for (int i = 0; i < items.size(); ++i) buttons.insert(items[i].id, m_buttons[i]);

    const int visibleColumns = m_layoutRows ? m_columns
        : qMax(1, (availableWidth - spacing) / (buttonWidth + spacing));
    const auto place = [&](CommandButton *button, const QRect &target) {
        if (button == m_draggedButton) return;
        if (animate && button->pos() != target.topLeft()) {
            auto animation = new QPropertyAnimation(button, "pos", button);
            animation->setDuration(220);
            animation->setEndValue(target.topLeft());
            animation->start(QAbstractAnimation::DeleteWhenStopped);
            button->resize(target.size());
        } else {
            button->setGeometry(target);
        }
    };

    int y = spacing;
    int minWidth = m_layoutRows ? 0 : sliderWidth + 2 * spacing;
    for (const auto &row : rows) {
        auto first = buttons.value(row.first());
        if (qobject_cast<SliderCommandWidget *>(first)) {
            place(first, QRect(spacing, y, sliderWidth, sliderHeight));
            minWidth = qMax(minWidth, sliderWidth + 2 * spacing);
            y += sliderHeight + spacing;
            continue;
        }
        for (int i = 0; i < row.size(); ++i) {
            const int x = spacing + (i % visibleColumns) * (buttonWidth + spacing);
            const int top = y + (i / visibleColumns) * (normalHeight + spacing);
            place(buttons.value(row[i]), QRect(x, top, buttonWidth, normalHeight));
        }
        minWidth = qMax(minWidth, qMin(visibleColumns, row.size()) * (buttonWidth + spacing) + spacing);
        y += ((row.size() + visibleColumns - 1) / visibleColumns) * (normalHeight + spacing);
    }
    scrollContainer->setMinimumSize(m_layoutRows ? minWidth : qMax(availableWidth, minWidth), y + spacing);
}

void PanelContentWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this]() { rebuildLayout(); });
}


void PanelContentWidget::applyTheme(bool isDark) {
    m_isDark = isDark;
    // These controls must not retain the initial dark palette while their
    // ancestor dialog is being restored or repolished with the saved theme.
    const QString actionStyle = QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: 1px solid %3; padding: 6px; border-radius: 3px; font-weight: bold; }"
        "QPushButton:hover { background-color: %4; }"
        "QPushButton:pressed { background-color: %5; }")
        .arg(isDark ? "#333333" : "#ffffff", isDark ? "#dddddd" : "#333333",
             isDark ? "#555555" : "#cccccc", isDark ? "#444444" : "#e0e0e0",
             isDark ? "#222222" : "#d0d0d0");
    for (const auto name : {"commandPanelSettings", "commandAddCommand", "commandEditLayout"})
        if (auto button = findChild<QPushButton *>(name)) button->setStyleSheet(actionStyle);
    if (auto scroll = findChild<QScrollArea *>("commandPanelScroll"))
        scroll->viewport()->setStyleSheet(QString("QWidget#commandPanelViewport { background-color: %1; }").arg(isDark ? "#1a1a1a" : "#f0f0f0"));
    if (isDark) {
        setStyleSheet("PanelContentWidget { background-color: #1a1a1a; }");
        QLabel *lblName = findChild<QLabel*>("headerName");
        if(lblName) lblName->setStyleSheet("font-size: 22px; font-weight:bold; color: white;");
        QLabel *lblSub = findChild<QLabel*>("headerSub");
        if(lblSub) lblSub->setStyleSheet("color: #888888; font-size: 12px;");
    } else {
        setStyleSheet("PanelContentWidget { background-color: #f0f0f0; }");
        QLabel *lblName = findChild<QLabel*>("headerName");
        if(lblName) lblName->setStyleSheet("font-size: 22px; font-weight:bold; color: #333333;");
        QLabel *lblSub = findChild<QLabel*>("headerSub");
        if(lblSub) lblSub->setStyleSheet("color: #666666; font-size: 12px;");
    }
    for (auto button : m_buttons) button->applyTheme(isDark);
}

void PanelContentWidget::setSwitchSendHandler(const std::function<bool(const QByteArray &)> &handler) {
    m_switchSendHandler = handler;
    for (auto button : m_buttons) if (auto toggle = qobject_cast<SwitchCommandWidget *>(button)) toggle->setSendHandler(handler);
}
void PanelContentWidget::forceStopAllTimers() {
    for (CommandButton *btn : m_buttons) {
        btn->forceStopTimer();
    }
}

void PanelContentWidget::onDragStarted(CommandButton *btn) {
    if (m_layoutRows) return;
    m_draggedButton = btn;
    m_dragOffset = btn->mapFromGlobal(QCursor::pos());
    btn->raise();
    m_dragTargetIndex = m_buttons.indexOf(btn);
    rebuildLayout(true);
}

void PanelContentWidget::onDragMoved(CommandButton *btn, const QPoint &globalPos) {
    if (btn != m_draggedButton) return;

    QPoint parentPos = scrollContainer->mapFromGlobal(globalPos);
    btn->move(parentPos - m_dragOffset);

    const bool draggingSlider = qobject_cast<SliderCommandWidget*>(btn) != nullptr;
    if (!draggingSlider) {
        QPoint center = btn->geometry().center();
        int newTargetIndex = -1;
        int minDistance = 999999;

        for (int i = 0; i < m_buttons.size(); ++i) {
            if (m_buttons[i] == btn) continue;
            int dist = (m_buttons[i]->geometry().center() - center).manhattanLength();
            if (dist < minDistance) {
                minDistance = dist;
                newTargetIndex = i;
            }
        }

        if (newTargetIndex != -1 && newTargetIndex != m_dragTargetIndex) {
            m_buttons.removeOne(btn);
            m_buttons.insert(newTargetIndex, btn);
            m_dragTargetIndex = newTargetIndex;
            rebuildLayout(true);
        }
        return;
    }

    // A slider occupies a complete row. Its insertion point is restricted to
    // the boundaries between logical rows of ordinary buttons and sliders.
    QList<CommandButton*> remaining = m_buttons;
    remaining.removeOne(btn);
    const int availableWidth = scrollContainer->parentWidget() ? scrollContainer->parentWidget()->width() : scrollContainer->width();
    const int normalCols = qMax(1, m_columns > 0 ? m_columns : 3);

    struct RowInfo { int startIndex; int endIndex; int centerY; };
    QList<RowInfo> rows;
    int index = 0;
    while (index < remaining.size()) {
        const int rowStart = index;
        CommandButton *first = remaining.at(index);
        if (qobject_cast<SliderCommandWidget*>(first)) {
            rows.append({rowStart, rowStart + 1, first->geometry().center().y()});
            ++index;
            continue;
        }

        int count = 0;
        while (index < remaining.size() && count < normalCols
               && !qobject_cast<SliderCommandWidget*>(remaining.at(index))) {
            ++index;
            ++count;
        }
        CommandButton *rowFirst = remaining.at(rowStart);
        CommandButton *rowLast = remaining.at(index - 1);
        rows.append({rowStart, index, (rowFirst->geometry().center().y() + rowLast->geometry().center().y()) / 2});
    }

    const int draggedCenterY = btn->geometry().center().y();
    int insertionIndex = remaining.size();
    if (!rows.isEmpty()) {
        insertionIndex = rows.first().startIndex;
        for (int i = 0; i < rows.size(); ++i) {
            const int nextY = (i + 1 < rows.size()) ? rows.at(i + 1).centerY : INT_MAX;
            const int boundaryY = nextY == INT_MAX ? INT_MAX : (rows.at(i).centerY + nextY) / 2;
            if (draggedCenterY < boundaryY) {
                insertionIndex = rows.at(i).startIndex;
                break;
            }
            insertionIndex = rows.at(i).endIndex;
        }
    }

    if (insertionIndex < 0) insertionIndex = 0;
    if (insertionIndex > remaining.size()) insertionIndex = remaining.size();
    const int currentIndex = m_buttons.indexOf(btn);
    if (currentIndex != insertionIndex) {
        m_buttons.removeOne(btn);
        m_buttons.insert(insertionIndex, btn);
        m_dragTargetIndex = insertionIndex;
        rebuildLayout(true);
    }
}
void PanelContentWidget::onDragReleased(CommandButton *btn) {
    if (btn != m_draggedButton) return;
    
    m_draggedButton = nullptr;
    m_dragTargetIndex = -1;
    rebuildLayout(true);
    emit contentChanged();
}

// --- CommandPanelWindow ---

CommandPanelWindow::CommandPanelWindow(QWidget *parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle("命令交互面板");
    resize(950, 600);
    PanelUi::windowControls(this);

    

    QHBoxLayout *mainLayout = new QHBoxLayout(this);

    // Left pane
    QVBoxLayout *leftLayout = new QVBoxLayout();
    listPanels = new QListWidget(this);
    listPanels->setObjectName("commandPanelList");
    listPanels->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(listPanels, &QListWidget::customContextMenuRequested, this, &CommandPanelWindow::onListContextMenu);
    leftLayout->addWidget(listPanels, 1);

    QHBoxLayout *leftBtnsLayout1 = new QHBoxLayout();
    QPushButton *btnAddPanel = new QPushButton("新增面板", this);btnAddPanel->setObjectName("commandAddPanel");btnAddPanel->setToolTip("新增命令面板，同名时自动添加 _1、_2 等后缀。");
    QPushButton *btnDelPanel = new QPushButton("删除面板", this);btnDelPanel->setToolTip("删除选中面板及全部命令，需要确认，不向设备发送命令。");
    btnAddPanel->setAutoDefault(false);
    btnDelPanel->setAutoDefault(false);
    leftBtnsLayout1->addWidget(btnAddPanel);
    leftBtnsLayout1->addWidget(btnDelPanel);
    
    QHBoxLayout *leftBtnsLayout2 = new QHBoxLayout();
    QPushButton *btnImport = new QPushButton("导入面板", this);btnImport->setToolTip("从 JSON 文件导入命令面板，可选择追加或覆盖。");
    QPushButton *btnExport = new QPushButton("导出所有", this);btnExport->setObjectName("commandExportAll");btnExport->setToolTip("将全部命令面板和命令配置导出为一个 JSON 文件。");
    btnImport->setAutoDefault(false);
    btnExport->setAutoDefault(false);
    leftBtnsLayout2->addWidget(btnImport);
    leftBtnsLayout2->addWidget(btnExport);

    leftLayout->addLayout(leftBtnsLayout1);
    leftLayout->addLayout(leftBtnsLayout2);
    QPushButton *btnHelp = new QPushButton("使用说明", this);
    btnHelp->setToolTip("查看按钮、滑条、开关命令和命令面板的使用说明。");
    btnHelp->setAutoDefault(false);
    btnHelp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    leftLayout->addWidget(btnHelp);

    mainLayout->addLayout(leftLayout, 1);

    // Right pane
    stackPanels = new QStackedWidget(this);
    rightContentStack = new QStackedWidget(this);
    rightContentStack->addWidget(stackPanels);

    emptyStateWidget = new QWidget(rightContentStack);
    QVBoxLayout *emptyStateLayout = new QVBoxLayout(emptyStateWidget);
    emptyStateLayout->setContentsMargins(24, 24, 24, 24);
    emptyStateLabel = new QLabel(QString::fromUtf8("欢迎使用命令交互面板\n当前还没有命令面板。\n点击左下角“新增面板”创建面板，\n或点击“导入面板”加载已有的配置文件。"), emptyStateWidget);
    emptyStateLabel->setAlignment(Qt::AlignCenter);
    emptyStateLabel->setWordWrap(true);
    emptyStateLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    emptyStateLabel->setStyleSheet(QStringLiteral("font-size: 16px; color: #dddddd;"));
    emptyStateLayout->addStretch(1);
    emptyStateLayout->addWidget(emptyStateLabel, 0, Qt::AlignCenter);
    emptyStateLayout->addStretch(1);
    rightContentStack->addWidget(emptyStateWidget);
    auto rightLayout = new QVBoxLayout;
    auto recoveryRow = new QHBoxLayout;
    configStatusLabel = new QLabel(this); configStatusLabel->setObjectName("commandConfigStatus");
    configStatusLabel->setWordWrap(true); configStatusLabel->setTextFormat(Qt::PlainText);
    configStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    btnRepairConfig = new QPushButton("一键修复", this); btnRepairConfig->setObjectName("commandConfigRepair");
    btnRepairConfig->setAutoDefault(false);
    btnRepairConfig->setToolTip("先完整备份原配置，再修复可识别的问题；无法修复时尝试最近有效备份。缺失的命令内容需手动补全。");
    connect(btnRepairConfig, &QPushButton::clicked, this, &CommandPanelWindow::onRepairConfig);
    recoveryRow->addWidget(configStatusLabel, 1); recoveryRow->addWidget(btnRepairConfig);
    rightLayout->addLayout(recoveryRow); rightLayout->addWidget(rightContentStack, 1);
    mainLayout->addLayout(rightLayout, 4);

    connect(listPanels, &QListWidget::currentRowChanged, this, &CommandPanelWindow::onPanelSelectionChanged);
    connect(listPanels, &QListWidget::itemDoubleClicked, this, &CommandPanelWindow::onPanelDoubleClicked);
    connect(btnAddPanel, &QPushButton::clicked, this, &CommandPanelWindow::onAddPanel);
    connect(btnDelPanel, &QPushButton::clicked, this, &CommandPanelWindow::onRemovePanel);
    connect(btnImport, &QPushButton::clicked, this, &CommandPanelWindow::onImportPanels);
    connect(btnExport, &QPushButton::clicked, this, &CommandPanelWindow::onExportPanels);
    connect(btnHelp, &QPushButton::clicked, this, [this]() {
        QDialog helpDialog(this);
        helpDialog.setWindowTitle(QStringLiteral("命令交互面板帮助"));
        helpDialog.setWindowFlags((helpDialog.windowFlags() & ~Qt::WindowContextHelpButtonHint)
                                 | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
        helpDialog.resize(720, 560);
        helpDialog.setMinimumSize(400, 300);
        QVBoxLayout *helpLayout = new QVBoxLayout(&helpDialog);
        QTextBrowser *helpText = new QTextBrowser(&helpDialog);
        helpText->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        helpText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        helpText->setLineWrapMode(QTextEdit::WidgetWidth);
        helpText->setPlainText(QString::fromUtf8(
            "【命令交互面板使用指南】\n\n"
            "一、基础操作\n"
            "1. 发送命令：未启用定时的按钮命令，左键点击发送一次。\n"
            "2. 定时发送：编辑按钮命令并设置定时后，左键点击会立即发送一次，再按周期循环，再次点击停止提交新命令；已排队的字节仍可能发出。与数据收发区的定时发送不同，后者首次在一个周期结束后发送。\n"
            "3. 编辑命令：长按右键约 0.7 秒进入按钮、滑条或开关命令编辑，也可短按右键打开菜单后选择“编辑命令”。可修改名称、命令内容、追加方式、颜色、说明等。鼠标悬停仅显示说明，未填写时显示“暂无说明”，说明不参与发送。\n"
            "悬停说明按当前屏幕空间自动换行，长串字符也会折行；过长时显示前部预览并提示右键编辑查看完整内容。说明始终完整保存，编辑窗口内可滚动查看全部。\n"
            "短按右键打开操作菜单，前部按类型提供发送：普通命令发送一次；定时命令发送一次、开始/停止定时（单次不切换定时状态）；开关切换发送或指定状态发送；滑条发送当前值、恢复默认值并发送，列表可选择编号对应值发送。显式发送当前/同一状态允许再次发送；开关只有发送入口接纳后才更新显示。菜单还可编辑、新增、复制、前移、后移、删除。复制自动添加 _1、_2 等后缀，不自动启动发送。打开菜单或编辑窗口本身不发送。\n"
            "4. 删除命令：在编辑窗口点击左下角“删除”，确认后移除该命令。\n"
            "5. 调整顺序：在按钮或开关命令表面按住左键拖动可调整顺序，拖动完成不发送；滑条命令按整行移动，只能放在某一行的开头，不会拆散普通按钮行。\n\n"
            "二、按钮命令\n"
            "1. 命令：填写实际发送的主体内容。命令/前缀和附加命令输入框随内容自动增高，最多展示 10 行（含自动换行），超过后才在框内滚动；删减内容会自动收回。\n"
            "2. 附加命令：填写需要紧接在命令主体后发送的内容。\n"
            "3. 追加：可选择无追加、回车换行、换行回车、回车、换行或自定义追加内容。\n"
            "4. 实际发送顺序为：命令 + 附加命令 + 追加。\n\n"
            "三、滑条命令\n"
            "1. 命令前缀：例如输入 SET VALUE=，数值 20 对应 SET VALUE=20。文本模式可用 {value} 指定插入位置；未写占位符时接在前缀后。滑条没有附加命令字段。\n"
            "2. 实际发送顺序为：命令前缀 + 滑条值 + 追加。\n"
            "3. 滑条模式：常规模式可设置最小值、最大值、步进、默认值和小数位；列表模式按自定义列表选择参数。\n"
            "列表模式中，编号自动从 0 开始、步进始终为 1；每项独立一行，可添加、删除、上移和下移，编号自动更新。最多 256 项，超过 10 项在列表内部滚动。\n"
            "顶部默认值下拉框选择已有项；移动列表时保留默认参数，删除默认项时选择邻近项。外部滑条及右侧输入框显示编号，实际发送编号对应的内容，不发送编号本身。\n"
            "4. 发送方式：选择“松开滑条发送”时，松开滑条发送一次；选择“滑动发送”时，拖动过程中数值变化会按约 50ms 的间隔发送。\n"
            "5. 右侧数值框：修改后按 Enter 或移开焦点提交，上下箭头也可改变数值；按范围和步进归一化，值实际变化时发送。重复确认相同值通常不再发送，需要重发时使用右键菜单的发送功能。\n"
            "6. 滑条命令默认独占一行，控件中会显示名称、滑条、当前数值、范围和实际命令预览。\n\n"
            "四、开关命令\n"
            "1. 与普通按钮同尺寸、同布局，左侧显示名称和状态，右侧为滑块式开关：状态 0 滑块在左，状态 1 在右。每个状态单独设置命令、状态名称和按钮颜色；默认状态名称为关闭/开启，也可改为模式 A/B 等。状态名称和共用说明可任意输入，不发送。\n"
            "2. 点击切换到另一状态，发送该状态的命令 + 追加。HEX 和追加方式共用；HEX 只限制命令与自定义追加，自动大写并按字节空格分隔。命令输入最多展示 10 行，超过后内部滚动。\n"
            "3. 只有成功提交到当前接口的发送队列后才切换。未连接、Break 占用、文件发送冲突或队列拒绝时保持原状态，请检查底部提示；不自动重试或定时轮换。状态仅表示软件请求，不代表设备已确认执行。\n"
            "4. 默认状态在顶部选择，保存默认状态、不保存运行状态；启动或重新载入按默认状态展示。启动、重连、导入、切换面板或主题不会自动发送。两个状态均可设透明色以适配主题。\n\n"
            "五、换行与 HEX 格式\n"
            "1. 文本命令以 UTF-8 发送，不受主界面接收框的 UTF-8/GBK 选项影响。在命令、附加命令或开关命令输入框中，普通 Enter 保存并关闭编辑；Ctrl+Enter 或 Alt+Enter 插入实际 LF 换行。直接键入反斜杠形式的 \\r\\n 会按可见字符发送，不自动转义；实际 CR/LF 可用追加选项或 HEX 0D 0A 指定。\n"
            "2. HEX 格式：勾选时按 UTF-8 字节转换已有文本，例如文本 12 转为 31 32；之后输入 AA 表示字节 AA。取消勾选会按 UTF-8 解码；开关命令及列表参数不能无损还原时会保留 HEX 并提示。普通命令中的任意二进制字节建议保持 HEX，避免解码成乱码。\n"
            "3. HEX 模式支持直接粘贴多行字节（带空格、换行或制表符），也支持 5A5A5A 这样的连续数据，自动大写并按字节加空格；空白不作为命令发送。支持 Ctrl+V、右键粘贴、复制、剪切和撤销。\n"
            "4. 仅接受 0-9、A-F 和分隔空白，不支持 0x 前缀、地址或注释；非法粘贴保留原内容。每个字节必须完整两位，保存时检查；需要发送换行字节时请输入 0D 0A 或 0A。\n\n"
            "常规滑条即使勾选 HEX，变化数值仍是十进制文本，不会自动编码为 int/float 二进制。例如 HEX 前缀 AA、数值 20（小数位 0），发送 AA 32 30。需要指定离散二进制参数时，可用列表模式填写实际字节，不能仅勾选 HEX 就发送原生浮点二进制。\n\n"
            "列表模式的 HEX 参数只接受十六进制字符和空白，并自动转为大写；保存时每个字节必须完整。发送 AA 55 表示两个字节，不是文本 AA55，也不是列表编号。切换格式时同步转换列表以保留字节；不能无损转为 UTF-8 时保留 HEX 并提示。\n\n"
            "按钮颜色：颜色选择框使用中文，拾取屏幕颜色下方的“使用透明按钮”可清除自定义颜色，恢复主题适配的默认透明样式；取消颜色选择不改变原颜色，保存命令后生效。\n\n"
            "六、配置管理与导入导出\n"
            "1. 自动保存：新增、编辑、删除、排序命令，修改布局列数、面板名称和说明后，配置会自动保存到程序配置文件。面板名称和说明统一在右上角“面板设置”中编辑。\n"
            "重新打开软件时，会恢复上次选中的命令面板；只有没有记忆记录或对应面板已不存在时，才打开第一个面板。改名或调整顺序不影响记忆，没有面板时显示欢迎提示。\n"
            "2. 保存内容：包括所有面板、面板顺序、面板名称和说明、布局列数、命令顺序、按钮/滑条/开关类型、命令内容、滑条模式、列表内容与顺序、默认编号、常规滑条参数、开关两组命令/状态名称/颜色与默认状态、HEX、追加、颜色、说明和定时设置。软件设置菜单中的整体配置导入导出同样保留这些内容；旧滑条配置默认为常规模式，旧命令和面板的备注自动作为说明读取。\n"
            "3. 导出配置：左下角“导出所有”或列表右键菜单“导出所有面板”可导出全部面板，默认 CmdConfig.json；右键某个面板并选择“导出当前面板”可单独导出该面板，默认文件名为“当前面板名称_CmdConfig.json”。空白处右键同样可新增、导入或导出全部；单个面板操作置灰，且不改变原选择。\n"
            "右键选择“复制面板”会把完整副本插入原面板下一行，名称追加 _1；重名时用 _2、_3。命令与布局保留，但副本不会自动启动定时发送。\n"
            "4. 导入配置：左下角“导入面板”选择 JSON 文件后，可选择“追加”或“覆盖”。追加保留当前配置并添加导入面板；覆盖用文件中的面板替换当前全部面板，请先导出备份；取消不修改配置。\n"
            "导入或重新打开软件不会自动启动定时发送，需手动点击开始。配置损坏或被其他实例修改时，自动保存暂停并保留原文件，不以空列表覆盖。可先导出仍可读取的面板；窗口提示“一键修复”时，可点击修复，先备份原文件，再尝试兼容修复或恢复有效备份。旧开关正文缺失时保留为“待补全 · 禁止发送”，需编辑补全两个状态后保存，无法凭空恢复丢失正文。也可使用“导入面板 → 覆盖”恢复已备份配置。\n"
            "5. 备份范围：独立 JSON 包含面板、命令及布局，不包含设备连接参数、自定义数据协议库和上次选择等全局设置；整体迁移请使用软件“设置”菜单的导出配置 / 导入配置。定时周期、开关默认状态和滑条默认值会保存，但不会将运行状态恢复为自动发送任务。\n"));
        helpLayout->addWidget(helpText, 1);
        QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &helpDialog);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        connect(buttons, &QDialogButtonBox::rejected, &helpDialog, &QDialog::reject);
        helpLayout->addWidget(buttons);
        if (m_isDark) {
            helpDialog.setStyleSheet("QDialog { background-color: #1e1e1e; } QTextBrowser { background-color: #252526; color: #dddddd; border: 1px solid #555555; padding: 10px; font-size: 14px; } QPushButton { padding: 6px 20px; }");
        } else {
            helpDialog.setStyleSheet("QDialog { background-color: #f0f0f0; } QTextBrowser { background-color: #ffffff; color: #333333; border: 1px solid #cccccc; padding: 10px; font-size: 14px; } QPushButton { padding: 6px 20px; }");
        }
        helpDialog.exec();
    });

    

    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(dataDir);
    if (!dir.exists()) dir.mkpath(".");
    defaultConfigPath = dir.filePath("cmd_panels.json");

    loadConfig(defaultConfigPath);

    const auto theme = qApp->property("waveLinkDarkTheme");
    applyTheme(theme.isValid() ? theme.toBool() : true);
    qApp->installEventFilter(this);
    updateEmptyState();
}

CommandPanelWindow::~CommandPanelWindow() {
    saveConfig(defaultConfigPath);
}

bool CommandPanelWindow::eventFilter(QObject *object, QEvent *event) {
    if (object == qApp && event->type() == QEvent::DynamicPropertyChange &&
        static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "waveLinkDarkTheme")
        applyTheme(qApp->property("waveLinkDarkTheme").toBool());
    return QDialog::eventFilter(object, event);
}


void CommandPanelWindow::updateEmptyState()
{
    if (!rightContentStack || !emptyStateWidget || !stackPanels) return;
    if (emptyStateLabel) emptyStateLabel->setText(m_allowAutoSave
        ? "欢迎使用命令交互面板\n当前还没有命令面板。\n点击左下角“新增面板”创建面板，\n或点击“导入面板”加载已有的配置文件。"
        : "命令配置未能安全读取，原文件已保留。\n点击上方“一键修复”尝试恢复面板，\n或使用“导入 → 覆盖”恢复已导出的配置。");
    int pending = 0;
    for (int i = 0; i < stackPanels->count(); ++i)
        if (auto panel = qobject_cast<PanelContentWidget *>(stackPanels->widget(i)))
            for (const auto &command : panel->getDef().commands) if (command.switchNeedsRepair) ++pending;
    if (configStatusLabel) {
        QString message = !m_allowAutoSave ? m_configSaveError : pending ? QString("面板已恢复；%1 条开关命令待补全，禁止发送。请右键编辑并保存两个状态的命令。").arg(pending) : QString();
        if (message.isEmpty() && !m_recoveryMessage.isEmpty()) message = m_recoveryMessage.section('\n', 0, 0) + " 详情见悬停说明。";
        configStatusLabel->setText(message); configStatusLabel->setToolTip(m_recoveryMessage);
        configStatusLabel->setVisible(!message.isEmpty()); btnRepairConfig->setVisible(!m_allowAutoSave);
    }
    rightContentStack->setCurrentWidget(stackPanels->count() == 0 ? emptyStateWidget : stackPanels);
}
void CommandPanelWindow::applyTheme(bool isDark) {
    m_isDark = isDark;
    if (configStatusLabel) configStatusLabel->setStyleSheet(isDark ? "color: #ffb86c;" : "color: #9a4d00;");
    if (emptyStateLabel) emptyStateLabel->setStyleSheet(isDark ? "font-size: 16px; color: #dddddd;" : "font-size: 16px; color: #333333;");
    if (isDark) {
        setStyleSheet(
            "QDialog { background-color: #1a1a1a; color: #dddddd; }"
            "QLabel { font-size: 13px; }"
            "QPushButton { background-color: #333333; color: #dddddd; border: 1px solid #555555; padding: 6px; border-radius: 3px; font-weight: bold; }"
            "QPushButton:hover { background-color: #444444; }"
            "QListWidget { background-color: #1e1e1e; color: #dddddd; border: none; outline: 0; }"
            "QListWidget::item { padding-left: 10px; border-bottom: 1px solid #2a2a2a; }"
            "QListWidget::item:selected { background-color: #00D26A; color: white; }"
            "QScrollArea { background-color: transparent; border: none; }"
        );
    } else {
        setStyleSheet(
            "QDialog { background-color: #f0f0f0; color: #333333; }"
            "QLabel { font-size: 13px; }"
            "QPushButton { background-color: #ffffff; color: #333333; border: 1px solid #cccccc; padding: 6px; border-radius: 3px; font-weight: bold; }"
            "QPushButton:hover { background-color: #e0e0e0; }"
            "QListWidget { background-color: #ffffff; color: #333333; border: none; outline: 0; }"
            "QListWidget::item { padding-left: 10px; border-bottom: 1px solid #eeeeee; }"
            "QListWidget::item:selected { background-color: #00D26A; color: white; }"
            "QScrollArea { background-color: transparent; border: none; }"
        );
    }
    setStyleSheet(styleSheet() + commandToolTipStyle(isDark) + PanelUi::listStyle(isDark));
    for (int i = 0; i < stackPanels->count(); ++i) {
        PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(stackPanels->widget(i));
        if (panel) panel->applyTheme(isDark);
    }
}
void CommandPanelWindow::forceStopAllTimers() {
    for (int i = 0; i < stackPanels->count(); ++i) {
        PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(stackPanels->widget(i));
        if (panel) panel->forceStopAllTimers();
    }
}

void CommandPanelWindow::onAddPanel() {
    QInputDialog dialog(this);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setWindowTitle("新增面板");
    dialog.setLabelText("面板名称:");
    dialog.setTextValue("");
    dialog.setStyleSheet("QInputDialog { min-width: 300px; } QLabel { min-width: 250px; }");
    dialog.setMinimumWidth(350);
    dialog.resize(350, 150);
    if (dialog.exec() == QDialog::Accepted) {
        QString text = dialog.textValue().trimmed();
        if (!text.isEmpty()) {
            const QString selectedId = selectedPanelId();
            PanelDef def;
            def.id = QUuid::createUuid().toString();
            QStringList names;for(int i=0;i<listPanels->count();++i)names<<listPanels->item(i)->text();
            def.name = PanelUi::uniqueName(text.trimmed(),names);
            addPanelToUI(def);
            restorePanelSelection(selectedId);
            onContentChanged();
        }
    }
}

void CommandPanelWindow::onRemovePanel() {
    int row = listPanels->currentRow();
    if (row >= 0 && row < stackPanels->count()) {
        if (QMessageBox::question(this, "删除面板", "确定要删除此面板及所有命令吗？") == QMessageBox::Yes) {
            const QString selectedId = selectedPanelId();
            {
                // List and content indices temporarily differ during removal.
                // Do not remember that intermediate selection as another panel.
                QSignalBlocker blocker(listPanels);
                delete listPanels->takeItem(row);
                QWidget *panel = stackPanels->widget(row);
                stackPanels->removeWidget(panel);
                panel->deleteLater();
            }
            restorePanelSelection(selectedId);
            onContentChanged();
        }
    }
}

void CommandPanelWindow::onImportPanels() {
    QString path = QFileDialog::getOpenFileName(this, "导入配置", "", "JSON (*.json)");
    if (path.isEmpty()) return;

    QDialog modeDialog(this);
    modeDialog.setWindowTitle(QString::fromUtf8("导入方式"));
    modeDialog.setModal(true);
    modeDialog.setFixedWidth(400);

    QVBoxLayout *modeLayout = new QVBoxLayout(&modeDialog);
    modeLayout->setContentsMargins(24, 20, 24, 20);
    modeLayout->setSpacing(16);

    QLabel *modeTitle = new QLabel(QString::fromUtf8("请选择导入配置的处理方式："), &modeDialog);
    modeTitle->setWordWrap(true);
    modeTitle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    modeLayout->addWidget(modeTitle);

    QLabel *modeInfo = new QLabel(QString::fromUtf8("追加：保留当前配置并添加导入的面板；\n覆盖：清空当前配置后导入文件内容。"), &modeDialog);
    modeInfo->setWordWrap(true);
    modeInfo->setMinimumWidth(540);
    modeInfo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    modeLayout->addWidget(modeInfo);

    QHBoxLayout *modeButtons = new QHBoxLayout();
    modeButtons->addStretch();
    QPushButton *appendButton = new QPushButton(QString::fromUtf8("追加"), &modeDialog);
    QPushButton *replaceButton = new QPushButton(QString::fromUtf8("覆盖"), &modeDialog);
    QPushButton *cancelButton = new QPushButton(QString::fromUtf8("取消"), &modeDialog);
    for (QPushButton *button : {appendButton, replaceButton, cancelButton}) {
        button->setMinimumSize(96, 36);
        modeButtons->addWidget(button);
    }
    modeLayout->addLayout(modeButtons);

    connect(appendButton, &QPushButton::clicked, &modeDialog, [&modeDialog]() {
        modeDialog.done(1);
    });
    connect(replaceButton, &QPushButton::clicked, &modeDialog, [&modeDialog]() {
        modeDialog.done(2);
    });
    connect(cancelButton, &QPushButton::clicked, &modeDialog, [&modeDialog]() {
        modeDialog.done(0);
    });

    const int result = modeDialog.exec();
    bool imported = false;
    if (result == 1) {
        if (!m_allowAutoSave) { QMessageBox::warning(this, "无法追加", "当前配置未能读取或已被其他实例修改，不能安全追加。请先修复配置，或使用覆盖导入恢复。原文件未改变。"); return; }
        imported = loadConfig(path, true);
    } else if (result == 2) {
        imported = loadConfig(path, false);
    }
    if (imported) {
        if (!m_allowAutoSave) {
            // An explicit replace import may recover the library, but retain
            // the unreadable/conflicting original first for manual recovery.
            QFile original(defaultConfigPath);
            const bool exists = original.exists();
            if (exists && (!original.open(QIODevice::ReadOnly) ||
                !QFile::copy(defaultConfigPath, defaultConfigPath + ".before-restore-" + QUuid::createUuid().toString(QUuid::WithoutBraces)))) {
                QMessageBox::warning(this, "恢复未保存", "无法备份原命令配置，已取消覆盖。可先导出当前导入的面板，再处理原文件的权限或占用。"); return;
            }
            m_knownConfigBytes = exists ? original.readAll() : QByteArray();
            m_knownConfigExists = exists; m_allowAutoSave = true; m_saveFailureReported = false;
        }
        onContentChanged();
        if (!m_allowAutoSave || !m_configSaveError.isEmpty()) return;
        QMessageBox::information(this, QString::fromUtf8("导入成功"),
                                 QString::fromUtf8("配置已成功导入：\n%1").arg(path));
    }
}

void CommandPanelWindow::onExportPanels() {
    QString path = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出"), "CmdConfig.json", "JSON (*.json)");
    if (!path.isEmpty() && saveConfig(path)) {
        QMessageBox::information(this, QString::fromUtf8("导出成功"),
                                 QString::fromUtf8("所有面板已成功导出：\n%1").arg(path));
    }
}

void CommandPanelWindow::onPanelSelectionChanged(int row) {
    QSettings settings("WaveLink", "WaveLink");
    if (row >= 0 && row < stackPanels->count()) {
        stackPanels->setCurrentIndex(row);
        // IDs survive renames and reordering; names and row numbers do not.
        settings.setValue(LastCommandPanelKey, selectedPanelId());
        settings.sync();
    } else if (listPanels->count() == 0) {
        settings.remove(LastCommandPanelKey);
        settings.sync();
    }
    updateEmptyState();
}

QString CommandPanelWindow::selectedPanelId() const {
    const auto item = listPanels->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void CommandPanelWindow::restorePanelSelection(const QString &panelId) {
    int row = listPanels->count() > 0 ? 0 : -1;
    if (!panelId.isEmpty()) {
        for (int i = 0; i < listPanels->count(); ++i) {
            if (listPanels->item(i)->data(Qt::UserRole).toString() == panelId) { row = i; break; }
        }
    }
    {
        QSignalBlocker blocker(listPanels);
        listPanels->setCurrentRow(row);
    }
    // Explicitly synchronize even when the row number has not changed (reload
    // or reordering may have replaced the content at that same row).
    onPanelSelectionChanged(row);
}
void CommandPanelWindow::onContentChanged() {
    for(int i=0;i<stackPanels->count()&&i<listPanels->count();++i)if(auto panel=qobject_cast<PanelContentWidget*>(stackPanels->widget(i)))listPanels->item(i)->setText(panel->getDef().name);
    if (!saveConfig(defaultConfigPath) && !m_saveFailureReported) {
        m_saveFailureReported = true;
        QMessageBox::warning(this, "命令配置未保存", m_configSaveError + "\n\n原配置未覆盖。请先导出当前面板备份，再重新打开软件或恢复有效配置。");
    }
    updateEmptyState();
}

void CommandPanelWindow::onPanelDoubleClicked(QListWidgetItem *item) {
    if (!item) return;
    int row = listPanels->row(item);
    
    QInputDialog dialog(this);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setWindowTitle("重命名面板");
    dialog.setLabelText("面板名称:");
    dialog.setTextValue(item->text());
    dialog.setStyleSheet("QInputDialog { min-width: 300px; } QLabel { min-width: 250px; }");
    dialog.setMinimumWidth(350);
    dialog.resize(350, 150);
    if (dialog.exec() == QDialog::Accepted) {
        QString newName = dialog.textValue();
        if (!newName.isEmpty()) {
            item->setText(newName);
            PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(stackPanels->widget(row));
            if (panel) {
                panel->setPanelName(newName);
            }
        }
    }
}

void CommandPanelWindow::setSwitchSendHandler(const std::function<bool(const QByteArray &)> &handler) {
    m_switchSendHandler = handler;
    for (int i = 0; i < stackPanels->count(); ++i)
        if (auto panel = qobject_cast<PanelContentWidget *>(stackPanels->widget(i))) panel->setSwitchSendHandler(handler);
}

void CommandPanelWindow::addPanelToUI(const PanelDef &panelDef, int index) {
    PanelContentWidget *panel = new PanelContentWidget(panelDef, this);
    panel->setSwitchSendHandler(m_switchSendHandler);
    panel->m_saveLayout = [this](QString &error) {
        if (saveConfig(defaultConfigPath)) return true;
        error = m_configSaveError;
        return false;
    };
    connect(panel, &PanelContentWidget::sendRequested, this, &CommandPanelWindow::sendDataRequested);
    connect(panel, &PanelContentWidget::contentChanged, this, &CommandPanelWindow::onContentChanged);
    
    panel->applyTheme(m_isDark);
    const int position = index < 0 ? stackPanels->count() : qBound(0, index, stackPanels->count());
    QSignalBlocker blocker(listPanels);
    stackPanels->insertWidget(position, panel);
    QListWidgetItem *item = new QListWidgetItem(panelDef.name);
    item->setData(Qt::UserRole, panelDef.id);
    listPanels->insertItem(position, item);
    item->setSizeHint(QSize(0, 44));
    updateEmptyState();
}

void CommandPanelWindow::clearUI() {
    listPanels->clear();
    while (stackPanels->count() > 0) {
        QWidget *w = stackPanels->widget(0);
        stackPanels->removeWidget(w);
        w->deleteLater();
    }
    updateEmptyState();
}

bool CommandPanelWindow::loadConfig(const QString &filePath, bool append) {
    QFile f(filePath);
    const bool internal = filePath == defaultConfigPath;
    if (!f.exists() && internal) {
        if (QFile::exists(defaultConfigPath + ".bak")) {
            m_allowAutoSave = false; m_configSaveError = "命令配置文件缺失，已找到备份。请点击“一键修复”恢复。"; updateEmptyState(); return false;
        }
        m_knownConfigExists = false; m_knownConfigBytes.clear(); m_allowAutoSave = true;
        return false;
    }
    if (internal) m_allowAutoSave = false;
    if (!f.open(QIODevice::ReadOnly)) {
        if (internal) { m_configSaveError = "无法读取命令配置：" + f.errorString(); updateEmptyState(); return false; }
        QMessageBox::warning(this, QString::fromUtf8("导入失败"),
                             QString::fromUtf8("无法打开配置文件：\n%1\n\n%2")
                                 .arg(filePath, f.errorString()));
        return false;
    }
    if (f.size() > CommandPanelConfig::MaxBytes) {
        m_configSaveError = "命令配置超过 16 MB，未加载。";
        if (internal) updateEmptyState(); else QMessageBox::warning(this, "导入失败", m_configSaveError);
        return false;
    }
    QByteArray bytes = f.readAll(); f.close();
    if (internal) {
        QJsonArray prepared; QString error; QStringList changes;
        if (!CommandPanelConfig::decode(bytes, prepared, error, changes)) {
            m_configSaveError = error; updateEmptyState(); return false;
        }
        if (!changes.isEmpty()) {
            QByteArray repaired;
            if (!CommandPanelConfig::repairFile(filePath, bytes, false, repaired, m_recoveryMessage, error)) {
                m_configSaveError = "旧版配置兼容时无法保存备份：" + error; updateEmptyState(); return false;
            }
            bytes = repaired;
        }
    }
    const bool loaded = loadConfigFromString(QString::fromUtf8(bytes), append);
    if (internal && loaded) {
        m_allowAutoSave = true; m_knownConfigExists = true; m_knownConfigBytes = bytes;
        m_configSaveError.clear(); m_saveFailureReported = false;
        updateEmptyState();
    }
    return loaded;
}
bool CommandPanelWindow::loadConfigFromString(const QString &jsonStr, bool append) {
    QJsonArray normalized; QString error; QStringList changes;
    if (!CommandPanelConfig::decode(jsonStr.toUtf8(), normalized, error, changes)) {
        QMessageBox::warning(this, "导入失败", error + "\n\n当前面板未改变。");
        return false;
    }
    // Validate new list settings BEFORE replacing/appending any panels. A bad
    // imported list must neither silently truncate entries nor erase the
    // user's existing working commands.
    for (const auto &panel : normalized) for (const auto &command : panel.toObject()["commands"].toArray()) {
        const auto object = command.toObject(); CommandDef list; list.isHex = object["isHex"].toBool(); list.widgetType = object["widgetType"].toInt(0); QString error;
        if (!readSliderList(object, list, error) || !CommandSwitchConfig::validate(object, error)) {
            QMessageBox::warning(this, "导入失败", QString("命令“%1”的参数无效：\n%2\n\n当前面板未改变。").arg(object["name"].toString(), error)); return false;
        }
    }
    const QString rememberedId = QSettings("WaveLink", "WaveLink").value(LastCommandPanelKey).toString();
    QSignalBlocker selectionBlocker(listPanels);
    if (!append) clearUI();
    QSet<QString> panelIds;
    for (int i = 0; i < listPanels->count(); ++i) panelIds.insert(listPanels->item(i)->data(Qt::UserRole).toString());
    QJsonArray arr = normalized;
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr[i].toObject();
        PanelDef def;
            
            QString baseName = obj["name"].toString();
            QString finalName = baseName;
            if (append) {
                int suffix = 1;
                bool exists = true;
                while (exists) {
                    exists = false;
                    for (int k = 0; k < stackPanels->count(); ++k) {
                        PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(stackPanels->widget(k));
                        if (panel && panel->getDef().name == finalName) {
                            exists = true;
                            finalName = baseName + "_" + QString::number(suffix++);
                            break;
                        }
                    }
                }
                def.id = QUuid::createUuid().toString();
            } else {
                def.id = obj["id"].toString();
            }
            // Older/imported libraries can lack IDs or contain duplicates.
            // Assign a stable identity before exposing any selection.
            if (def.id.trimmed().isEmpty() || panelIds.contains(def.id)) def.id = QUuid::createUuid().toString();
            panelIds.insert(def.id);
            def.name = finalName;
        def.remark = obj["remark"].toString();
        if (obj.contains("columns")) {
            def.columns = obj["columns"].toInt();
        } else {
            def.columns = 3;
        }
        def.buttonWidth = obj.contains("buttonWidth")
            ? obj["buttonWidth"].toInt()
            : 200;
        def.buttonHeight = obj.contains("buttonHeight")
            ? obj["buttonHeight"].toInt()
            : 60;
        
        QJsonArray cmds = obj["commands"].toArray();
            for (int j = 0; j < cmds.size(); ++j) {
                QJsonObject cmdObj = cmds[j].toObject();
                CommandDef cmdDef;
                cmdDef.id = cmdObj["id"].toString();
                cmdDef.name = cmdObj["name"].toString();
                cmdDef.payload = cmdObj["payload"].toString();
                cmdDef.extraPayload = cmdObj["extraPayload"].toString();
                cmdDef.isHex = cmdObj["isHex"].toBool();
                // Timer-running state is not permission to send on import or
                // startup. Retain the interval; starting always requires a click.
                cmdDef.autoSend = false;
                cmdDef.intervalMs = cmdObj["intervalMs"].toInt();
                cmdDef.appendIndex = cmdObj["appendIndex"].toInt();
        
    cmdDef.btnColor = cmdObj["btnColor"].toString();
            cmdDef.note = cmdObj["note"].toString();
            cmdDef.widgetType = cmdObj["widgetType"].toInt(static_cast<int>(CommandWidgetType::Button));
            cmdDef.minValue = cmdObj["minValue"].toDouble(0.0);
            cmdDef.maxValue = cmdObj["maxValue"].toDouble(100.0);
            cmdDef.stepValue = cmdObj["stepValue"].toDouble(1.0);
            cmdDef.defaultValue = cmdObj["defaultValue"].toDouble(0.0);
            cmdDef.decimals = cmdObj["decimals"].toInt(0);
            cmdDef.sliderSendMode = cmdObj["sliderSendMode"].toInt(0);
            QString listError; readSliderList(cmdObj, cmdDef, listError);
            cmdDef.customAppendPayload = cmdObj["customAppendPayload"].toString();
            readSwitchConfig(cmdObj, cmdDef);
            def.commands.append(cmdDef);
        }
        CommandPanelConfig::readLayout(obj, def.layoutRows, error);
        addPanelToUI(def);
    }
    
    selectionBlocker.unblock();
    restorePanelSelection(rememberedId);
    return true;
}

void CommandPanelWindow::reloadConfig() {
    loadConfig(defaultConfigPath);
}

void CommandPanelWindow::onRepairConfig() {
    QByteArray bytes, repaired; QString report, error;
    const bool exists = QFile::exists(defaultConfigPath);
    if ((exists && !CommandPanelConfig::read(defaultConfigPath, bytes, error)) ||
        !CommandPanelConfig::repairFile(defaultConfigPath, bytes, true, repaired, report, error, exists)) {
        QMessageBox::warning(this, "修复未完成", error + "\n原配置未覆盖。"); return;
    }
    m_recoveryMessage = report;
    if (loadConfig(defaultConfigPath)) QMessageBox::information(this, "配置修复完成", report);
}

bool CommandPanelWindow::saveConfig(const QString &filePath) {
    const bool internal = filePath == defaultConfigPath;
    if (internal && !m_allowAutoSave) {
        m_configSaveError = "命令配置未能安全加载或已发生冲突，自动保存已暂停。"; return false;
    }
    QLockFile lock(defaultConfigPath + ".lock");
    if (internal) {
        if (!lock.tryLock(0)) { m_configSaveError = "命令配置正被其他实例保存，请稍后重试。"; return false; }
        QFile current(defaultConfigPath);
        if (current.exists() != m_knownConfigExists || (current.exists() &&
            (!current.open(QIODevice::ReadOnly) || current.readAll() != m_knownConfigBytes))) {
            m_allowAutoSave = false;
            m_configSaveError = "命令配置已被其他实例修改，已停止覆盖旧快照。"; return false;
        }
    }
    QJsonArray arr;
    for (int i = 0; i < stackPanels->count(); ++i) {
        PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(stackPanels->widget(i));
        if (panel) {
            PanelDef pDef = panel->getDef();
            QJsonObject pObj;
            pObj["id"] = pDef.id;
        pObj["name"] = pDef.name;
        pObj["remark"] = pDef.remark;
        pObj["columns"] = pDef.columns;
        PanelLayout::write(pObj, pDef.layoutRows);
        pObj["buttonWidth"] = pDef.buttonWidth;
        pObj["buttonHeight"] = pDef.buttonHeight;
        
        QJsonArray cmds;
            for (const CommandDef &cmdDef : pDef.commands) {
                QJsonObject cmdObj;
                cmdObj["id"] = cmdDef.id;
                cmdObj["name"] = cmdDef.name;
                cmdObj["payload"] = cmdDef.payload;
                cmdObj["extraPayload"] = cmdDef.extraPayload;
                cmdObj["isHex"] = cmdDef.isHex;
                cmdObj["autoSend"] = cmdDef.autoSend;
                cmdObj["intervalMs"] = cmdDef.intervalMs;
                cmdObj["appendIndex"] = cmdDef.appendIndex;
                cmdObj["btnColor"] = cmdDef.btnColor;
                cmdObj["note"] = cmdDef.note;
                cmdObj["widgetType"] = cmdDef.widgetType;
                cmdObj["minValue"] = cmdDef.minValue;
                cmdObj["maxValue"] = cmdDef.maxValue;
                cmdObj["stepValue"] = cmdDef.stepValue;
                cmdObj["defaultValue"] = cmdDef.defaultValue;
                cmdObj["decimals"] = cmdDef.decimals;
                cmdObj["sliderSendMode"] = cmdDef.sliderSendMode;
                writeSliderList(cmdObj, cmdDef);
                writeSwitchConfig(cmdObj, cmdDef);
                cmdObj["customAppendPayload"] = cmdDef.customAppendPayload;
                cmds.append(cmdObj);
            }
            pObj["commands"] = cmds;
            arr.append(pObj);
        }
    }
    
    const QByteArray data = QJsonDocument(arr).toJson(QJsonDocument::Indented);
    QJsonArray checked; QString error; QStringList changes;
    if (!CommandPanelConfig::decode(data, checked, error, changes)) { m_configSaveError = error; return false; }
    if (internal && m_knownConfigExists && data != m_knownConfigBytes) {
        QJsonArray previous; QStringList previousChanges; QString previousError;
        if (CommandPanelConfig::decode(m_knownConfigBytes, previous, previousError, previousChanges) &&
            !CommandPanelConfig::write(defaultConfigPath + ".bak", m_knownConfigBytes, error)) {
            m_configSaveError = "无法保存命令配置备份：" + error; return false;
        }
    }
    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (internal) m_configSaveError = f.errorString();
        if (filePath != defaultConfigPath) {
            QMessageBox::warning(this, QString::fromUtf8("保存失败"),
                                 QString::fromUtf8("无法写入配置文件：\n%1\n\n%2")
                                     .arg(filePath, f.errorString()));
        }
        return false;
    }
    if (f.write(data) != data.size()) {
        if (internal) m_configSaveError = f.errorString();
        if (filePath != defaultConfigPath) {
        QMessageBox::warning(this, QString::fromUtf8("保存失败"),
                             QString::fromUtf8("写入配置文件失败：\n%1\n\n%2")
                                 .arg(filePath, f.errorString()));
        }
        return false;
    }
    if (!f.commit()) {
        if (internal) m_configSaveError = f.errorString();
        if (filePath != defaultConfigPath) QMessageBox::warning(this, QString::fromUtf8("保存失败"), f.errorString());
        return false;
    }
    if (internal) { m_knownConfigExists = true; m_knownConfigBytes = data; m_configSaveError.clear(); m_saveFailureReported = false; }
    return true;
}

void CommandPanelWindow::onCopyPanel(int row) {
    auto source = qobject_cast<PanelContentWidget *>(stackPanels->widget(row));
    if (!source || row < 0 || row >= listPanels->count()) return;
    PanelDef copy = source->getDef(); const QString base = copy.name;
    QSet<QString> names;
    for (int i = 0; i < listPanels->count(); ++i) names.insert(listPanels->item(i)->text());
    int suffix = 1;
    do { copy.name = base + "_" + QString::number(suffix++); } while (names.contains(copy.name));
    copy.id = QUuid::createUuid().toString();
    QHash<QString, QString> newIds;
    for (auto &command : copy.commands) {
        const auto oldId = command.id;
        command.id = QUuid::createUuid().toString();
        newIds.insert(oldId, command.id);
        // CommandButton starts a timer if autoSend is true in its constructor.
        // A copied panel must never unexpectedly start sending to a device.
        command.autoSend = false;
    }
    PanelLayout::remap(copy.layoutRows, newIds);
    const int oldSelection = listPanels->currentRow();
    addPanelToUI(copy, row + 1);
    if (!saveConfig(defaultConfigPath)) {
        // Preserve the original panel, its running tasks and its saved file.
        auto copiedWidget = stackPanels->widget(row + 1); stackPanels->removeWidget(copiedWidget); copiedWidget->deleteLater();
        delete listPanels->takeItem(row + 1); listPanels->setCurrentRow(oldSelection); updateEmptyState();
        QMessageBox::warning(this, "复制面板失败", "无法保存副本，已取消复制。原有面板和配置未改变。\n请检查配置文件夹权限或磁盘空间。");
        return;
    }
    listPanels->setCurrentRow(row + 1);
}
void CommandPanelWindow::onListContextMenu(const QPoint &pos) {
    QListWidgetItem *item = listPanels->itemAt(pos);
    const QString targetId = item ? item->data(Qt::UserRole).toString() : QString();
    const int targetRow = item ? listPanels->row(item) : -1;
    if (item) listPanels->setCurrentRow(targetRow);
    const bool hasTarget = item != nullptr;
    
    QMenu menu(this);
    menu.setObjectName("commandPanelListMenu");
    QAction *actAdd = menu.addAction("新增面板"); actAdd->setObjectName("commandPanelMenuAdd");
    QAction *actImport = menu.addAction("导入面板"); actImport->setObjectName("commandPanelMenuImport");
    menu.addSeparator();
    QAction *actSettings=menu.addAction("面板设置");actSettings->setObjectName("commandPanelMenuSettings");
    QAction *actRename = menu.addAction("重命名"); actRename->setObjectName("commandPanelMenuRename");
    QAction *actCopy = menu.addAction("复制面板"); actCopy->setObjectName("commandPanelMenuCopy");
    QAction *actDel = menu.addAction("删除面板"); actDel->setObjectName("commandPanelMenuDelete");
    QAction *actViewRemark = menu.addAction("查看说明"); actViewRemark->setObjectName("commandPanelMenuRemark");
    menu.addSeparator();
    QAction *actExportCurrent = menu.addAction("导出当前面板"); actExportCurrent->setObjectName("commandPanelMenuExportCurrent");
    QAction *actExportAll = menu.addAction("导出所有面板"); actExportAll->setObjectName("commandPanelMenuExportAll");
    actExportAll->setEnabled(listPanels->count() > 0);
    for (auto action : {actSettings, actRename, actCopy, actDel, actViewRemark, actExportCurrent}) action->setEnabled(hasTarget);
    menu.addSeparator();
    QAction *actUp = menu.addAction("上移"); actUp->setObjectName("commandPanelMenuUp");
    QAction *actDown = menu.addAction("下移"); actDown->setObjectName("commandPanelMenuDown");
    actUp->setEnabled(hasTarget && targetRow > 0);
    actDown->setEnabled(hasTarget && targetRow + 1 < listPanels->count());
    
    QAction *res = menu.exec(listPanels->viewport()->mapToGlobal(pos));
    if (!res) return;
    if (res == actAdd) { onAddPanel(); return; }
    if (res == actImport) { onImportPanels(); return; }
    if (res == actExportAll) { onExportPanels(); return; }
    if (!hasTarget) return;
    // Resolve the clicked identity again after the nested menu loop. Blank
    // space never borrows the old selection as a destructive-action target.
    int row = -1;
    for (int i = 0; i < listPanels->count(); ++i)
        if (listPanels->item(i)->data(Qt::UserRole).toString() == targetId) { row = i; break; }
    if (row < 0 || row >= stackPanels->count()) return;
    item = listPanels->item(row);
    
    if(res==actSettings){
        if(auto panel=qobject_cast<PanelContentWidget*>(stackPanels->widget(row)))panel->openPanelSettings();
    } else if (res == actRename) {
        onPanelDoubleClicked(item);
    } else if (res == actCopy) {
        onCopyPanel(row);
    } else if (res == actDel) {
        listPanels->setCurrentRow(row);
        onRemovePanel();
    } else if (res == actViewRemark) {
        PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(stackPanels->widget(row));
        if (panel) {
            RemarkDialog dlg(panel->getRemark(), this);
            if (dlg.exec() == QDialog::Accepted) {
                panel->setRemark(dlg.getRemark());
            }
        }
    } else if (res == actExportCurrent) {
        PanelContentWidget *panel = qobject_cast<PanelContentWidget*>(stackPanels->widget(row));
        if (panel) {
            QString defaultName = panel->getDef().name + "_CmdConfig.json";
            QString path = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出当前面板配置"), defaultName, "JSON (*.json)");
            if (!path.isEmpty()) {
                QJsonArray arr;
                PanelDef pDef = panel->getDef();
                QJsonObject pObj;
                pObj["id"] = pDef.id;
                pObj["name"] = pDef.name;
                pObj["remark"] = pDef.remark;
                pObj["columns"] = pDef.columns;
                PanelLayout::write(pObj, pDef.layoutRows);
                pObj["buttonWidth"] = pDef.buttonWidth;
                pObj["buttonHeight"] = pDef.buttonHeight;
                
                QJsonArray cmds;
                for (const CommandDef &cmdDef : pDef.commands) {
                    QJsonObject cmdObj;
                    cmdObj["id"] = cmdDef.id;
                    cmdObj["name"] = cmdDef.name;
                    cmdObj["payload"] = cmdDef.payload;
                    cmdObj["extraPayload"] = cmdDef.extraPayload;
                    cmdObj["isHex"] = cmdDef.isHex;
                    cmdObj["autoSend"] = cmdDef.autoSend;
                    cmdObj["intervalMs"] = cmdDef.intervalMs;
                    cmdObj["appendIndex"] = cmdDef.appendIndex;
                    cmdObj["btnColor"] = cmdDef.btnColor;
                    cmdObj["note"] = cmdDef.note;
                cmdObj["widgetType"] = cmdDef.widgetType;
                cmdObj["minValue"] = cmdDef.minValue;
                cmdObj["maxValue"] = cmdDef.maxValue;
                cmdObj["stepValue"] = cmdDef.stepValue;
                cmdObj["defaultValue"] = cmdDef.defaultValue;
                cmdObj["decimals"] = cmdDef.decimals;
                cmdObj["sliderSendMode"] = cmdDef.sliderSendMode;
                writeSliderList(cmdObj, cmdDef);
                writeSwitchConfig(cmdObj, cmdDef);
                cmdObj["customAppendPayload"] = cmdDef.customAppendPayload;
                    cmds.append(cmdObj);
                }
                pObj["commands"] = cmds;
                arr.append(pObj);
                
                QFile f(path);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QMessageBox::warning(this, QString::fromUtf8("导出失败"),
                                         QString::fromUtf8("无法写入配置文件：\n%1\n\n%2")
                                             .arg(path, f.errorString()));
                } else {
                    const QJsonDocument doc(arr);
                    const QByteArray data = doc.toJson(QJsonDocument::Indented);
                    if (f.write(data) != data.size()) {
                        QMessageBox::warning(this, QString::fromUtf8("导出失败"),
                                             QString::fromUtf8("写入配置文件失败：\n%1\n\n%2")
                                                 .arg(path, f.errorString()));
                    } else {
                        QMessageBox::information(this, QString::fromUtf8("导出成功"),
                                                 QString::fromUtf8("当前面板已成功导出：\n%1").arg(path));
                    }
                }
            }
        }
    } else if (res == actUp) {
        if (row > 0) {
            const QString movedId = item->data(Qt::UserRole).toString();
            {
                QSignalBlocker blocker(listPanels);
                listPanels->takeItem(row);
                listPanels->insertItem(row - 1, item);
                QWidget *w = stackPanels->widget(row);
                stackPanels->removeWidget(w);
                stackPanels->insertWidget(row - 1, w);
            }
            restorePanelSelection(movedId);
            onContentChanged();
        }
    } else if (res == actDown) {
        if (row < listPanels->count() - 1) {
            const QString movedId = item->data(Qt::UserRole).toString();
            {
                QSignalBlocker blocker(listPanels);
                listPanels->takeItem(row);
                listPanels->insertItem(row + 1, item);
                QWidget *w = stackPanels->widget(row);
                stackPanels->removeWidget(w);
                stackPanels->insertWidget(row + 1, w);
            }
            restorePanelSelection(movedId);
            onContentChanged();
        }
    }
}
