#include "commandpanelwindow.h"
#include "testregistrysandbox.h"
#include "ProtocolConfigTransfer.h"
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QFileInfo>
#include <QHeaderView>
#include <QScrollBar>
#include <QElapsedTimer>
#include <QThread>
#include <QInputMethodEvent>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QSettings>
#include <QMimeData>
#include <QTextCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QTextBlock>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QScrollArea>
#include <QtMath>
#include <functional>
#include <iostream>
#define VERIFY(c) do { if (!(c)) { std::cerr << "Command slider failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(const std::function<bool()> &condition, int timeout = 1000) {
    QElapsedTimer clock; clock.start(); while (!condition() && clock.elapsed() < timeout) { QApplication::processEvents(); QThread::msleep(1); } return condition();
}
static QString warning(const std::function<void()> &action) {
    // Qt 5.14 Windows QMessageBox's native Show handler is incompatible with
    // offscreen. Keep the real application decision, skip only that handler.
    class Messages : public QObject {
    public: QString text;
    protected: bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) if (auto box = qobject_cast<QMessageBox *>(object)) {
            text = box->text(); QTimer::singleShot(0, box, &QDialog::accept); return true;
        }
        return false;
    }
    } messages;
    qApp->installEventFilter(&messages); action(); qApp->removeEventFilter(&messages); return messages.text;
}
static bool colorAction(CommandEditDialog &editor, const std::function<bool(QColorDialog *)> &action) {
    bool okay = false; QTimer::singleShot(0, [&] {
        auto color = editor.findChild<QColorDialog *>("commandColorDialog");
        if (color) { okay = action(color); if (color->isVisible()) color->reject(); }
    });
    editor.findChild<QPushButton *>("commandColorPicker")->click(); return okay;
}
static PanelContentWidget *firstPanel(CommandPanelWindow &window) {
    for (auto panel : window.findChildren<PanelContentWidget *>())
        if (auto stack = qobject_cast<QStackedWidget *>(panel->parentWidget())) if (stack->indexOf(panel) >= 0) return panel;
    return nullptr; // Ignore old panels awaiting deleteLater after import.
}
static int commandEditorHeightTests(int argc, char **argv) {
    auto settle = [] { for (int i = 0; i < 12; ++i) QApplication::processEvents(); };
    auto lines = [](QTextEdit *edit) {
        edit->document()->documentLayout()->documentSize(); int count = 0;
        for (auto block = edit->document()->begin(); block.isValid(); block = block.next()) if (block.layout()) count += block.layout()->lineCount();
        return count;
    };
    auto content = [](int count) { QStringList text; for (int i = 0; i < count; ++i) text << QString("command %1").arg(i); return text.join('\n'); };
    CommandDef def; def.payload = content(10); def.extraPayload = content(10);
    CommandEditDialog editor(def); editor.resize(700, 760); editor.show(); settle();
    auto payload = editor.findChild<QTextEdit *>("commandPayload"), extra = editor.findChild<QTextEdit *>("commandExtraPayload");
    auto scroll = editor.findChild<QScrollArea *>("commandEditScroll");
    VERIFY(payload && extra && scroll);
    for (auto input : {payload, extra}) {
        VERIFY(lines(input) == 10 && input->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
        VERIFY(input->viewport()->height() >= qCeil(input->document()->documentLayout()->documentSize().height()));
    }
    VERIFY(scroll->verticalScrollBar()->maximum() > 0 && editor.findChild<QPushButton *>("commandSave")->isVisible());
    for (auto input : {payload, extra}) {
        int previous = 0, capped = 0;
        for (int count : {1, 3, 4, 10, 11, 30}) {
            input->setPlainText(content(count)); settle(); VERIFY(lines(input) == count);
            if (count <= 10) {
                VERIFY(input->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && input->verticalScrollBar()->maximum() == 0);
                VERIFY(input->viewport()->height() >= qCeil(input->document()->documentLayout()->documentSize().height()));
                if (count > 3) VERIFY(input->height() > previous);
            } else {
                VERIFY(input->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded && input->verticalScrollBar()->maximum() > 0);
                if (count == 11) capped = input->height(); else VERIFY(input->height() == capped);
            }
            VERIFY(input->toPlainText() == content(count)); previous = input->height();
        }
        input->clear(); settle(); VERIFY(input->height() < capped && input->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    }
    // Automatic wrapping is visual content too, not just explicit newlines.
    const QString wrapped = QString("AA ").repeated(170);
    payload->setPlainText(wrapped); extra->setPlainText(wrapped); editor.resize(600, 760); settle();
    VERIFY(lines(payload) > 3); const int narrowHeight = payload->height();
    editor.resize(1200, 760); settle(); VERIFY(payload->height() < narrowHeight && payload->toPlainText() == wrapped);
    editor.resize(700, 760); payload->setPlainText(content(10)); settle(); const int smallFont = payload->height();
    auto font = payload->font(); font.setPointSize(14); payload->setFont(font); settle();
    VERIFY(payload->height() > smallFont && lines(payload) == 10 && payload->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    if (argc > 1) {
        VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); scroll->ensureWidgetVisible(payload);
        editor.grab().save(QString::fromLocal8Bit(argv[1]) + "/command-editor-ten-lines.png");
    }
    auto hex = editor.findChild<QCheckBox *>("commandHex"); hex->setChecked(true);
    payload->setPlainText(QString("aa").repeated(500)); extra->setPlainText(QString("bb").repeated(500)); settle();
    for (auto input : {payload, extra}) VERIFY(input->document()->blockCount() == 1 && lines(input) > 10
        && input->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded && input->verticalScrollBar()->maximum() > 0);
    VERIFY(editor.getDef().payload == QString::fromLatin1(QByteArray(500, char(0xaa)).toHex(' ').toUpper()));
    payload->setPlainText("AA"); extra->clear(); settle();
    VERIFY(payload->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && extra->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    return 0;
}
static int commandHexInputTests(int argc, char **argv) {
    // Clipboard contents belong to the user, even if an assertion fails.
    struct ClipboardRestore {
        QMimeData *saved = new QMimeData;
        ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
        ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
    } clipboard;
    const QString multiline =
        "5a 5a 5a 5a 40 00 02 00 30 00 00 00 00 00 00 00\r\n"
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00\r\n"
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00\n"
        "00 00 00 00 00 00 00 00 00 00 00 00 23 a8 0d 0a";
    const QByteArray bytes = QByteArray::fromHex("5A5A5A5A4000020030") + QByteArray(51, '\0') + QByteArray::fromHex("23A80D0A");
    const auto spaced = QString::fromLatin1(bytes.toHex(' ').toUpper());
    CommandDef def; def.isHex = true; def.payload = "aa55"; def.extraPayload = "0d\n0a"; def.appendIndex = 5; def.customAppendPayload = "feef";
    CommandEditDialog editor(def); editor.show(); QApplication::processEvents();
    auto payload = editor.findChild<QTextEdit *>("commandPayload");
    auto extra = editor.findChild<QTextEdit *>("commandExtraPayload"), append = editor.findChild<QTextEdit *>("commandCustomAppend");
    VERIFY(payload && extra && append && payload->toPlainText() == "AA 55" && extra->toPlainText() == "0D 0A" && append->toPlainText() == "FE EF");
    auto key = [](QWidget *target, int code, Qt::KeyboardModifiers modifiers, const QString &text = {}) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers, text); QApplication::sendEvent(target, &event);
    };
    for (auto input : {payload, extra, append}) {
        input->setFocus(); key(input, Qt::Key_A, Qt::ControlModifier, QString(QChar(1)));
        QApplication::clipboard()->setText(multiline);
        key(input, Qt::Key_V, Qt::ControlModifier, QString(QChar(22)));
        VERIFY(input->toPlainText() == spaced && input->textCursor().position() == spaced.size());
        input->selectAll(); QApplication::clipboard()->setText("5a5A5a");
        key(input, Qt::Key_Insert, Qt::ShiftModifier); VERIFY(input->toPlainText() == "5A 5A 5A");
        key(input, Qt::Key_Z, Qt::ControlModifier, QString(QChar(26))); VERIFY(input->toPlainText() == spaced);
        key(input, Qt::Key_Y, Qt::ControlModifier, QString(QChar(25))); VERIFY(input->toPlainText() == "5A 5A 5A");
        input->selectAll(); QApplication::clipboard()->setText("ab\tcd\nEF\r\n00"); input->paste();
        VERIFY(input->toPlainText() == "AB CD EF 00");
        auto menu = input->createStandardContextMenu(); bool pasted = false;
        input->selectAll(); QApplication::clipboard()->setText("5a5a5a");
        for (auto action : menu->actions()) if (action->text().startsWith(QTextEdit::tr("&Paste"))) { action->trigger(); pasted = true; break; }
        delete menu; VERIFY(pasted && input->toPlainText() == "5A 5A 5A");
        for (const QString invalid : {QString("aa 中文 55"), QString("0xAA"), QString("G1"), QString("address: 5A")}) {
            input->selectAll(); QApplication::clipboard()->setText(invalid); input->paste(); VERIFY(input->toPlainText() == "5A 5A 5A");
        }
        QInputMethodEvent chinese; chinese.setCommitString("中文"); QApplication::sendEvent(input, &chinese); VERIFY(input->toPlainText() == "5A 5A 5A");
        key(input, Qt::Key_G, Qt::NoModifier, "G"); VERIFY(input->toPlainText() == "5A 5A 5A");
        key(input, Qt::Key_At, Qt::ControlModifier | Qt::AltModifier, "@"); VERIFY(input->toPlainText() == "5A 5A 5A");
        input->setPlainText("AA BB CC"); auto cursor = input->textCursor(); cursor.setPosition(3); input->setTextCursor(cursor);
        key(input, Qt::Key_0, Qt::NoModifier, "0"); key(input, Qt::Key_D, Qt::NoModifier, "d");
        VERIFY(input->toPlainText() == "AA 0D BB CC" && input->textCursor().position() == 5);
        key(input, Qt::Key_Backspace, Qt::NoModifier); VERIFY(input->toPlainText() == "AA 0B BC C");
        input->undo(); VERIFY(input->toPlainText() == "AA 0D BB CC");
        cursor = input->textCursor(); cursor.setPosition(2); input->setTextCursor(cursor);
        key(input, Qt::Key_Delete, Qt::NoModifier); VERIFY(input->toPlainText() == "AA DB BC C");
        input->undo(); input->selectAll(); key(input, Qt::Key_C, Qt::ControlModifier, QString(QChar(3)));
        VERIFY(QApplication::clipboard()->text() == "AA 0D BB CC");
        key(input, Qt::Key_X, Qt::ControlModifier, QString(QChar(24))); VERIFY(input->toPlainText().isEmpty()); input->undo();
        VERIFY(input->toPlainText() == "AA 0D BB CC");
        input->selectAll(); QApplication::clipboard()->setText(multiline); input->paste();
        VERIFY(input->toPlainText() == spaced);
    }
    if (argc > 1) { VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); editor.grab().save(QString::fromLocal8Bit(argv[1]) + "/command-hex-multiline.png"); }
    auto save = editor.findChild<QPushButton *>("commandSave");
    payload->setPlainText("A"); VERIFY(warning([&] { save->click(); }).contains("完整字节") && editor.isVisible());
    payload->setPlainText(spaced); extra->setPlainText("F"); VERIFY(warning([&] { save->click(); }).contains("完整字节") && editor.isVisible());
    extra->setPlainText(spaced); append->setPlainText("B"); VERIFY(warning([&] { save->click(); }).contains("完整字节") && editor.isVisible());
    append->setPlainText(spaced); save->click(); VERIFY(editor.result() == QDialog::Accepted);
    const auto saved = editor.getDef(); VERIFY(saved.payload == spaced && saved.extraPayload == spaced && saved.customAppendPayload == spaced);
    CommandButton command(saved); QByteArray sent; QObject::connect(&command, &CommandButton::sendRequested, [&](const QByteArray &v, bool) { sent = v; });
    command.sendDataPacket(false); VERIFY(sent == bytes.repeated(3) && sent.size() == 192);
    auto single = saved; single.extraPayload.clear(); single.appendIndex = 0; command.updateDef(single);
    command.sendDataPacket(false); VERIFY(sent == bytes && sent.size() == 64);
    CommandEditDialog reopened(saved); VERIFY(reopened.findChild<QTextEdit *>("commandPayload")->toPlainText() == spaced);
    CommandPanelWindow window; const QJsonArray library{QJsonObject{{"name", "HEX 粘贴测试"}, {"commands", QJsonArray{}}}};
    VERIFY(window.loadConfigFromString(QJsonDocument(library).toJson())); firstPanel(window)->addCommand(saved);
    VERIFY(QMetaObject::invokeMethod(firstPanel(window), "contentChanged", Qt::DirectConnection));
    CommandPanelWindow restarted; const auto restored = firstPanel(restarted)->getDef().commands[0];
    VERIFY(restored.isHex && restored.payload == spaced && restored.extraPayload == spaced && restored.customAppendPayload == spaced);
    CommandDef text; text.payload = "Hi"; CommandEditDialog plain(text); auto edit = plain.findChild<QTextEdit *>("commandPayload");
    auto hex = plain.findChild<QCheckBox *>("commandHex"); hex->setChecked(true); VERIFY(edit->toPlainText() == "48 69");
    edit->selectAll(); QApplication::clipboard()->setText("4142"); edit->paste(); hex->setChecked(false); VERIFY(edit->toPlainText() == "AB");
    edit->selectAll(); QApplication::clipboard()->setText("中文\n正常文本"); edit->paste(); VERIFY(edit->toPlainText() == "中文\n正常文本");
    return 0;
}
int main(int argc, char **argv) {
    TestRegistrySandbox registry; VERIFY(registry.ready()); QApplication app(argc, argv);
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).value(0), 9));
    QStandardPaths::setTestModeEnabled(true); app.setOrganizationName("WaveLinkTests"); app.setApplicationName("CommandSliderTests");
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); VERIFY(QDir().mkpath(base));
    QTemporaryDir dir(base + "/slider-XXXXXX"); VERIFY(dir.isValid());
    app.setApplicationName("CommandSliderTests/" + QFileInfo(dir.path()).fileName());
    VERIFY(QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)) == QDir::cleanPath(dir.path()));
    VERIFY(commandEditorHeightTests(argc, argv) == 0);
    VERIFY(commandHexInputTests(argc, argv) == 0);
    CommandDef list; list.name = "列表参数"; list.widgetType = 1; list.sliderMode = 1;
    list.payload = "SET="; list.sliderValues = QStringList{"LOW", "MID", "HIGH"}; list.defaultListIndex = 1; list.appendIndex = 1;
    {
        SliderCommandWidget slider(list); QList<QByteArray> sent; QList<bool> automatic;
        QObject::connect(&slider, &CommandButton::sendRequested, [&](const QByteArray &v, bool a) { sent << v; automatic << a; });
        auto control = slider.findChild<QSlider *>("sliderCommandControl"); auto value = slider.findChild<QDoubleSpinBox *>("sliderCommandValue");
        VERIFY(control && value && control->maximum() == 2 && control->value() == 1 && control->singleStep() == 1 && control->pageStep() == 1);
        VERIFY(value->minimum() == 0 && value->maximum() == 2 && value->value() == 1 && value->decimals() == 0 && value->singleStep() == 1 && sent.isEmpty());
        VERIFY(slider.findChild<QLabel *>("sliderCommandPreview")->text().contains("SET=MID\\r\\n"));
        control->setValue(2); VERIFY(sent.isEmpty()); QMetaObject::invokeMethod(control, "sliderReleased", Qt::DirectConnection);
        VERIFY(sent == QList<QByteArray>({"SET=HIGH\r\n"}) && !automatic[0] && value->value() == 2);
        value->setValue(0); VERIFY(sent.last() == "SET=LOW\r\n" && control->value() == 0);
        // Editing/reloading modes and defaults never sends a command.
        auto changed = list; changed.defaultListIndex = 2; slider.updateDef(changed); VERIFY(sent.size() == 2 && control->value() == 2);
        changed.sliderValues = QStringList{"ONLY"}; changed.defaultListIndex = 0; slider.updateDef(changed);
        VERIFY(control->maximum() == 0 && value->maximum() == 0 && value->value() == 0 && sent.size() == 2);
        slider.sendDataPacket(false); VERIFY(sent.last() == "SET=ONLY\r\n");
        changed.sliderValues.clear(); slider.updateDef(changed); slider.sendDataPacket(false); VERIFY(sent.size() == 3 && !control->isEnabled());
        changed = list; changed.sliderSendMode = 1; slider.updateDef(changed); control->setSliderDown(true); control->setValue(2);
        VERIFY(until([&] { return sent.size() == 4; }) && automatic.last()); slider.forceStopTimer(); control->setSliderDown(false);
        auto regular = list; regular.sliderMode = 0; regular.minValue = -10; regular.maxValue = 10; regular.stepValue = .5; regular.decimals = 1; regular.defaultValue = 2.5;
        slider.updateDef(regular); VERIFY(control->maximum() == 10000 && value->value() == 2.5 && value->decimals() == 1 && sent.size() == 4);
        value->setValue(3.5); VERIFY(sent.last() == "SET=3.5\r\n");
    }
    {
        auto binary = list; binary.isHex = true; binary.payload = "FE"; binary.sliderValues = QStringList{"00", "AA 55", "0d 0a"}; binary.defaultListIndex = 1;
        SliderCommandWidget slider(binary); QList<QByteArray> sent; QObject::connect(&slider, &CommandButton::sendRequested, [&](const QByteArray &v, bool) { sent << v; });
        slider.sendDataPacket(false); VERIFY(sent == QList<QByteArray>({QByteArray::fromHex("FEAA550D0A")}));
        auto placeholder = binary; placeholder.payload = "AA{value}55"; placeholder.appendIndex = 5; placeholder.customAppendPayload = "EE";
        slider.updateDef(placeholder); slider.sendDataPacket(false); VERIFY(sent.last() == QByteArray::fromHex("AAAA5555EE"));
        auto invalid = binary; invalid.sliderValues[0] = "GG"; slider.updateDef(invalid); slider.sendDataPacket(false); VERIFY(sent.size() == 2);
        // Keep the existing regular HEX slider's numeric-as-ASCII behavior.
        auto regular = binary; regular.sliderMode = 0; regular.defaultValue = 10; regular.payload = "AA"; regular.appendIndex = 0;
        slider.updateDef(regular); slider.sendDataPacket(false); VERIFY(sent.last() == QByteArray::fromHex("AA3130"));
    }
    CommandDef configured;
    {
        CommandEditDialog editor(list); editor.show(); QApplication::processEvents();
        auto mode = editor.findChild<QComboBox *>("commandSliderMode"); auto table = editor.findChild<QTableWidget *>("commandSliderList");
        auto defaults = editor.findChild<QComboBox *>("commandListDefault"); auto add = editor.findChild<QPushButton *>("commandListAdd");
        auto remove = editor.findChild<QPushButton *>("commandListDelete"); auto up = editor.findChild<QPushButton *>("commandListUp"); auto down = editor.findChild<QPushButton *>("commandListDown");
        VERIFY(mode && table && defaults && mode->currentIndex() == 1 && table->rowCount() == 3 && defaults->currentIndex() == 1);
        VERIFY(editor.findChild<QWidget *>("commandRegularSliderOptions")->isHidden() && !editor.findChild<QWidget *>("commandListSliderOptions")->isHidden());
        for (int i = 0; i < 3; ++i) VERIFY(table->item(i, 0)->text() == QString::number(i) && !(table->item(i, 0)->flags() & Qt::ItemIsEditable));
        table->selectRow(1); up->click(); VERIFY(table->item(0, 1)->text() == "MID" && defaults->currentIndex() == 0);
        down->click(); VERIFY(table->item(1, 1)->text() == "MID" && defaults->currentIndex() == 1);
        table->selectRow(0); remove->click(); VERIFY(table->rowCount() == 2 && defaults->currentIndex() == 0 && defaults->currentText().contains("MID"));
        table->item(0, 1)->setText("MEDIUM"); VERIFY(defaults->currentText().contains("MEDIUM") && editor.getDef().sliderValues[0] == "MEDIUM");
        mode->setCurrentIndex(0); VERIFY(!editor.findChild<QWidget *>("commandRegularSliderOptions")->isHidden());
        mode->setCurrentIndex(1); VERIFY(table->rowCount() == 2 && table->item(0, 1)->text() == "MEDIUM");
        while (table->rowCount() < 10) add->click(); const int height = table->height();
        VERIFY(table->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff); add->click(); QApplication::processEvents();
        VERIFY(table->height() == height && table->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded && table->verticalScrollBar()->maximum() > 0);
        QApplication::processEvents();
        VERIFY(add->mapTo(&editor, QPoint()).y() >= table->mapTo(&editor, QPoint()).y() + table->height());
        while (table->rowCount() < 256) add->click(); VERIFY(!add->isEnabled()); add->click(); VERIFY(table->rowCount() == 256);
        for (int i = 0; i < 256; ++i) VERIFY(table->item(i, 0)->text() == QString::number(i));
        VERIFY(defaults->count() == 256 && defaults->currentText().contains("MEDIUM"));
        defaults->setCurrentIndex(255); table->selectRow(255); remove->click(); VERIFY(defaults->currentIndex() == 254 && add->isEnabled());
        table->item(0, 1)->setText("");
        VERIFY(warning([&] { editor.findChild<QPushButton *>("commandSave")->click(); }).contains("不能为空") && editor.isVisible());
        table->item(0, 1)->setText("MEDIUM"); configured = editor.getDef();
        VERIFY(configured.sliderMode == 1 && configured.sliderValues.size() == 255 && configured.defaultListIndex == 254);
        if (argc > 1) { VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); editor.grab().save(QString::fromLocal8Bit(argv[1]) + "/command-list-editor.png"); }
        editor.findChild<QPushButton *>("commandSave")->click(); VERIFY(editor.result() == QDialog::Accepted);
    }
    {
        CommandEditDialog editor(list); editor.show(); QApplication::processEvents();
        auto table = editor.findChild<QTableWidget *>("commandSliderList"); auto hex = editor.findChild<QCheckBox *>("commandHex");
        hex->setChecked(true); VERIFY(table->item(0, 1)->text() == "4C 4F 57"); hex->setChecked(false); VERIFY(table->item(0, 1)->text() == "LOW");
        hex->setChecked(true); table->item(0, 1)->setText("ab cd"); VERIFY(table->item(0, 1)->text() == "AB CD");
        table->editItem(table->item(0, 1)); QApplication::processEvents(); auto edit = table->findChild<QLineEdit *>("commandListValueEditor"); VERIFY(edit && edit->validator());
        edit->clear(); edit->insert("aa 55"); VERIFY(edit->text() == "AA 55"); edit->insert("G中文!"); VERIFY(edit->text() == "AA 55");
        QInputMethodEvent input; input.setCommitString("中文"); QApplication::sendEvent(edit, &input); VERIFY(edit->text() == "AA 55");
        table->setFocus(); QApplication::processEvents();
        table->item(0, 1)->setText("A"); VERIFY(warning([&] { editor.findChild<QPushButton *>("commandSave")->click(); }).contains("完整的 HEX"));
        table->item(0, 1)->setText("FF"); VERIFY(warning([&] { hex->setChecked(false); }).contains("UTF-8") && hex->isChecked() && table->item(0, 1)->text() == "FF");
        table->item(0, 1)->setText("30"); hex->setChecked(false); VERIFY(table->item(0, 1)->text() == "0");
        while (table->rowCount()) { table->selectRow(0); editor.findChild<QPushButton *>("commandListDelete")->click(); }
        VERIFY(warning([&] { editor.findChild<QPushButton *>("commandSave")->click(); }).contains("至少需要一项"));
        editor.findChild<QComboBox *>("commandWidgetType")->setCurrentIndex(0); // An inactive empty list must not break a saved button command.
        editor.findChild<QPushButton *>("commandSave")->click(); VERIFY(editor.result() == QDialog::Accepted);
        editor.close();
    }
    {
        auto colorDef = list; colorDef.btnColor = "#123456"; CommandEditDialog editor(colorDef); editor.show(); QApplication::processEvents();
        VERIFY(colorAction(editor, [&](QColorDialog *color) {
            auto picker = color->findChild<QPushButton *>("commandColorScreenPicker"), transparent = color->findChild<QPushButton *>("commandColorTransparent");
            if (!picker || !transparent || picker->text() != "拾取屏幕颜色" || transparent->text() != "使用透明按钮") return false;
            if (transparent->mapTo(color, QPoint()).y() <= picker->mapTo(color, QPoint()).y() + picker->height()) return false;
            if (!QCoreApplication::translate("QColorDialog", "Cursor at %1, %2\nPress ESC to cancel").contains("按 ESC 取消拾取")) return false;
            for (auto b : color->findChildren<QAbstractButton *>()) if (b->text().contains("Pick") || b->text().contains("Cancel") || b->text() == "OK") return false;
            picker->setEnabled(false); if (transparent->isEnabled()) return false; picker->setEnabled(true);
            if (argc > 1) color->grab().save(QString::fromLocal8Bit(argv[1]) + "/command-color-chinese.png");
            transparent->click(); return true;
        }));
        VERIFY(editor.getDef().btnColor.isEmpty());
        VERIFY(colorAction(editor, [](QColorDialog *color) { color->setCurrentColor(QColor("#eeddcc")); color->accept(); return true; }));
        VERIFY(editor.getDef().btnColor == "#eeddcc");
        VERIFY(colorAction(editor, [](QColorDialog *color) { color->setCurrentColor(Qt::red); color->reject(); return true; }));
        VERIFY(editor.getDef().btnColor == "#eeddcc");
        VERIFY(QCoreApplication::translate("QColorDialog", "Cursor at %1, %2\nPress ESC to cancel").startsWith("Cursor at"));
        CommandButton button(editor.getDef()); auto reset = editor.getDef(); reset.btnColor.clear(); button.updateDef(reset); VERIFY(button.styleSheet().contains("background-color: transparent"));
    }
    {
        CommandPanelWindow window;
        const QJsonArray empty{QJsonObject{{"id", "panel-list"}, {"name", "列表保存测试"}, {"commands", QJsonArray{}}}};
        VERIFY(window.loadConfigFromString(QJsonDocument(empty).toJson())); firstPanel(window)->addCommand(configured);
        VERIFY(QMetaObject::invokeMethod(firstPanel(window), "contentChanged", Qt::DirectConnection));
        window.forceStopAllTimers();
        QFile file(dir.filePath("cmd_panels.json")); VERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll(); file.close();
        CommandPanelWindow imported; auto loaded = firstPanel(imported)->getDef().commands[0];
        VERIFY(loaded.sliderMode == 1 && loaded.sliderValues == configured.sliderValues && loaded.defaultListIndex == 254);
        auto panels = QJsonDocument::fromJson(bytes).array(); auto obj = panels[0].toObject(); auto commands = obj["commands"].toArray(); auto command = commands[0].toObject();
        command["sliderMode"] = 0; command.remove("sliderValues"); command.remove("defaultListIndex"); commands[0] = command; obj["commands"] = commands; panels[0] = obj;
        CommandPanelWindow legacy; VERIFY(legacy.loadConfigFromString(QJsonDocument(panels).toJson())); VERIFY(firstPanel(legacy)->getDef().commands[0].sliderMode == 0);
        const auto before = firstPanel(imported)->getDef().commands[0].sliderValues;
        command["sliderMode"] = 1; command["sliderValues"] = QJsonArray{QString("AA"), QString("GG")}; command["isHex"] = true; commands[0] = command; obj["commands"] = commands; panels[0] = obj;
        bool accepted = true; VERIFY(warning([&] { accepted = imported.loadConfigFromString(QJsonDocument(panels).toJson()); }).contains("HEX") && !accepted);
        VERIFY(firstPanel(imported)->getDef().commands[0].sliderValues == before);
        QString error; DataProtocols::ProtocolProfileStore store(dir.filePath("protocols.json")); VERIFY(store.load(error));
        QSettings main("WaveLink", "WaveLink"); main.setValue("sliderImportSentinel", "original"); main.sync();
        QFile retained(dir.filePath("cmd_panels.json")); VERIFY(retained.open(QIODevice::ReadOnly)); const auto originalFile = retained.readAll(); retained.close();
        QVariantMap global{{"CommandPanel/json", QString::fromUtf8(QJsonDocument(panels).toJson())}, {"WaveLink/sliderImportSentinel", "changed"}};
        VERIFY(!DataProtocols::ProtocolConfigTransfer::apply(global, store, -1, false, 1, retained.fileName(), error) && error.contains("HEX"));
        main.sync(); VERIFY(main.value("sliderImportSentinel").toString() == "original");
        VERIFY(retained.open(QIODevice::ReadOnly) && retained.readAll() == originalFile); retained.close();
        QJsonArray oversized; for (int i = 0; i < 257; ++i) oversized.append("00"); command["sliderValues"] = oversized; commands[0] = command; obj["commands"] = commands; panels[0] = obj;
        VERIFY(warning([&] { accepted = imported.loadConfigFromString(QJsonDocument(panels).toJson()); }).contains("256") && !accepted);
        VERIFY(firstPanel(imported)->getDef().commands[0].sliderValues == before);
        window.forceStopAllTimers(); imported.forceStopAllTimers(); legacy.forceStopAllTimers();
    }
    {
        const auto configPath = dir.filePath("cmd_panels.json"); QFile config(configPath);
        const QByteArray damaged("{\"panels\": [ incomplete but recoverable content");
        VERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate) && config.write(damaged) == damaged.size()); config.close();
        // Startup now reports repairable configuration failures in a persistent
        // banner, not a modal "import failed" dialog before the window opens.
        bool recoveryVisible = false;
        VERIFY(warning([&] {
            CommandPanelWindow failedLoad;
            auto status = failedLoad.findChild<QLabel *>("commandConfigStatus");
            auto repair = failedLoad.findChild<QPushButton *>("commandConfigRepair");
            recoveryVisible = status && status->text().contains("JSON") && repair && !repair->isHidden();
        }).isEmpty() && recoveryVisible);
        VERIFY(config.open(QIODevice::ReadOnly) && config.readAll() == damaged); config.close();
        const QJsonArray library{QJsonObject{{"id", "protected-panel"}, {"name", "安全命令"}, {"commands", QJsonArray{
            QJsonObject{{"id", "timer"}, {"name", "定时"}, {"payload", "DO NOT AUTO SEND"}, {"intervalMs", 1}, {"autoSend", true}}}}}};
        const auto bytes = QJsonDocument(library).toJson();
        VERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate) && config.write(bytes) == bytes.size()); config.close();
        CommandPanelWindow first, second; int unexpected = 0;
        QObject::connect(&first, &CommandPanelWindow::sendDataRequested, [&](const QByteArray &, bool) { ++unexpected; });
        QElapsedTimer elapsed; elapsed.start(); VERIFY(until([&] { return elapsed.elapsed() >= 20; }));
        VERIFY(unexpected == 0 && !firstPanel(first)->getDef().commands[0].autoSend);
        for (auto timer : first.findChildren<QTimer *>()) VERIFY(!timer->isActive());
        firstPanel(first)->setRemark("第一个实例的修改");
        VERIFY(warning([&] { firstPanel(second)->setRemark("不得覆盖较新配置"); }).contains("其他实例"));
        VERIFY(config.open(QIODevice::ReadOnly)); const auto persisted = QJsonDocument::fromJson(config.readAll()).array(); config.close();
        VERIFY(persisted[0].toObject()["remark"].toString() == "第一个实例的修改");
    }
    std::cout << "Multiline/contiguous HEX paste, editing/undo, exact byte sends, slider/color validation, safe imports and command-save protection passed.\n";
}
