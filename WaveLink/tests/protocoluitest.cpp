#include "protocolmanagerdialog.h"
#include "protocoleditorwidget.h"
#include "testregistrysandbox.h"
#include "sidebarmenutesthelper.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QPushButton>
#include <QListWidget>
#include <QLineEdit>
#include <QTableWidget>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPalette>
#include <iostream>
#include <functional>
#include <cmath>
#include <cstring>
#include <limits>
#include <QTimer>
#include <QMessageBox>
#include <QDir>
#include <QPixmap>
#include <QFontDatabase>
#include <QSpinBox>
#include <QSplitter>
#include <QScrollArea>
#include <QScrollBar>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QShortcut>
#include <QClipboard>
#include <QMimeData>
#include <QInputMethodEvent>
#include <QAbstractItemDelegate>
#include <QMenu>
#include <QAction>
#include <QHeaderView>
#include <QFontMetrics>
#include <QStyledItemDelegate>
#include <QStandardItemModel>
#include <QCheckBox>
#include <QTextBrowser>
#include <QWheelEvent>
#include "ProtocolChecksum.h"
#include "ProtocolParserFactory.h"
#include "ProtocolParser.h"
#include <memory>
#include "protocolsampleedit.h"
#include "ProtocolSampleGenerator.h"
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Protocol UI failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(std::function<bool()> condition) {
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < 10000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return condition();
}
static bool checkHexInput(QLineEdit *edit) {
    if (!edit || !edit->validator()) return false;
    edit->clear(); edit->insert("a");
    if (edit->text() != "A" || edit->cursorPosition() != 1) return false;
    edit->insert("f 0d 0a");
    if (edit->text() != "AF 0D 0A") return false;
    edit->setCursorPosition(1); edit->insert("b");
    if (edit->text() != "ABF 0D 0A" || edit->cursorPosition() != 2) return false;
    const QString original = edit->text(); edit->insert("G中文!");
    if (edit->text() != original) return false;
    QInputMethodEvent input; input.setCommitString("中文"); QApplication::sendEvent(edit, &input);
    if (edit->text() != original) return false;
    // Preserve clipboard contents even if a test assertion fails.
    struct ClipboardRestore {
        QMimeData *saved = new QMimeData;
        ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
        ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
    } restore;
    edit->selectAll(); QApplication::clipboard()->setText("aa 55 0d 0a");
    QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier); QApplication::sendEvent(edit, &paste);
    if (edit->text() != "AA 55 0D 0A") return false;
    edit->undo(); if (edit->text() != original) return false;
    edit->redo(); if (edit->text() != "AA 55 0D 0A") return false;
    edit->selectAll(); QApplication::clipboard()->setText("aa 中文 55");
    if (!QMetaObject::invokeMethod(edit, "paste", Qt::DirectConnection)) return false;
    if (edit->text() != "AA 55 0D 0A") return false;
    edit->selectAll(); QApplication::clipboard()->setText("0xAA"); QApplication::sendEvent(edit, &paste);
    if (edit->text() != "AA 55 0D 0A") return false;
    edit->selectAll(); QApplication::clipboard()->setText(QString(QChar(0xfb00))); QApplication::sendEvent(edit, &paste);
    if (edit->text() != "AA 55 0D 0A") return false; // Unicode ligature is not ASCII HEX.
    // Exercise the actual native context-menu connection to the paste slot.
    edit->selectAll(); QApplication::clipboard()->setText("ab cd");
    auto menu = edit->createStandardContextMenu(); bool menuPasted = false;
    for (auto action : menu->actions()) if (action->text().startsWith(QLineEdit::tr("&Paste"))) { action->trigger(); menuPasted = true; break; }
    delete menu;
    if (!menuPasted || edit->text() != "AB CD") return false;
    edit->undo(); if (edit->text() != "AA 55 0D 0A") return false;
    edit->redo(); if (edit->text() != "AB CD") return false;
    edit->clear();
    QKeyEvent key(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, "d"); QApplication::sendEvent(edit, &key);
    if (edit->text() != "D") return false;
    edit->undo(); if (!edit->text().isEmpty()) return false;
    edit->redo(); if (edit->text() != "D") return false;
    QApplication::clipboard()->setText("ef"); QApplication::sendEvent(edit, &paste);
    if (edit->text() != "DEF") return false;
    edit->undo(); if (edit->text() != "D") return false;
    edit->redo(); if (edit->text() != "DEF") return false;
    QInputMethodEvent hexCommit; hexCommit.setCommitString("ab"); QApplication::sendEvent(edit, &hexCommit);
    return edit->text() == "DEFAB";
}
static bool createProtocol(ProtocolManagerDialog &manager, int index, bool accept = true, const char *buttonName = "protocolNew", bool cbfTemplate = false) {
    bool chosen = false;
    QTimer driver; driver.setSingleShot(true);
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolNewDialog") {
            auto type = widget->findChild<QComboBox *>("protocolNewType");
            chosen = type && type->count() == 5 && type->itemText(0) == "文本分隔类型协议" && type->itemText(1) == "二进制固定长度类型协议" && type->itemText(2) == "二进制长度字段类型协议" && type->itemText(3) == "二进制首尾分隔类型协议" && type->itemText(4) == "多通道键值协议";
            if (chosen) type->setCurrentIndex(type->findData(index));
            if (chosen && index == 3) {
                auto templates = widget->findChild<QComboBox *>("protocolNewTemplate"); chosen = templates && templates->isVisible();
                if (templates) templates->setCurrentIndex(cbfTemplate ? 1 : 0);
            }
            if (accept && chosen) static_cast<QDialog *>(widget)->accept(); else static_cast<QDialog *>(widget)->reject();
        }
    });
    driver.start(0); manager.findChild<QPushButton *>(buttonName)->click(); return chosen;
}
static int delimitedUiTests(const QString &path, const QString &artifacts) {
    QString error; ProtocolManagerDialog manager(path); manager.resize(1500, 980); manager.show();
    VERIFY(createProtocol(manager, 4));
    auto list = manager.findChild<QListWidget *>("protocolList"); auto editor = manager.findChild<ProtocolEditorWidget *>();
    auto type = editor->findChild<QComboBox *>("protocolType");
    auto head = editor->findChild<QLineEdit *>("protocol_headerHex"), tail = editor->findChild<QLineEdit *>("protocol_tailHex");
    auto summary = editor->findChild<QLabel *>("protocolFrameSummary");
    auto input = manager.findChild<QPlainTextEdit *>("protocolSample"); auto count = manager.findChild<QSpinBox *>("protocolSampleArrayCount");
    auto test = manager.findChild<QPushButton *>("protocolTest"), generate = manager.findChild<QPushButton *>("protocolGenerateSample");
    auto results = manager.findChild<QTableWidget *>("protocolResults"); auto diagnostics = manager.findChild<QPlainTextEdit *>("protocolDiagnostics");
    VERIFY(list && editor && type && head && tail && summary && input && count && test && generate && results && diagnostics);
    VERIFY(list->currentItem()->text() == "二进制首尾分隔协议" && type->currentData().toString() == "delimiter");
    VERIFY(head->text().isEmpty() && tail->text() == "0000807F" && editor->isDynamicArrayProtocol());
    for (const auto key : {"sizeBytes", "lengthType", "lengthMeaning", "lengthOffset", "binaryLayout", "headerAlgorithm"})
        VERIFY(editor->findChild<QWidget *>(QString("protocol_") + key)->isHidden());
    VERIFY(summary->text().contains("N×4") && summary->text().contains("仅帧尾") && !summary->text().contains("配置冲突"));
    count->setValue(10); generate->click();
    QByteArray bytes; VERIFY(ProtocolProfileValidator::parseHex(input->toPlainText(), bytes) && bytes.size() == 44);
    for (int chunk = 0; chunk < 3; ++chunk) {
        manager.findChild<QComboBox *>("protocolChunkMode")->setCurrentIndex(chunk); test->click();
        VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 10 && results->item(9, 3)->text() == "10" && diagnostics->toPlainText().isEmpty());
    }
    tail->clear(); ProtocolProfile p; VERIFY(!editor->profile(p, error) && summary->text().contains("不能同时为空"));
    head->setText("AA55"); VERIFY(editor->profile(p, error) && summary->text().contains("仅帧头") && summary->text().contains("最后一帧暂存"));
    generate->click(); test->click(); VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 10 && diagnostics->toPlainText().isEmpty());
    VERIFY(manager.findChild<QLabel *>("protocolStats")->text().contains("正常边界"));
    tail->setText("0D0A"); VERIFY(editor->profile(p, error) && summary->text().contains("首尾配对"));
    generate->click(); test->click(); VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 10 && diagnostics->toPlainText().isEmpty());
    // Per-channel overrides are optional and never limit dynamic count.
    manager.findChild<QPushButton *>("protocolAddField")->click();
    auto fields = editor->findChild<QTableWidget *>("protocolFields"); fields->item(0, 1)->setText("温度"); fields->item(0, 10)->setText("首尾分隔备注");
    manager.findChild<QPushButton *>("protocolSave")->click();
    ProtocolProfileStore stored(path); VERIFY(stored.load(error) && stored.snapshot().profiles.first().isDelimiterFramed());
    VERIFY(stored.snapshot().profiles.first().definition.value("channels").toArray()[0].toObject().value("note") == QJsonValue("首尾分隔备注"));
    if (!artifacts.isEmpty()) { QApplication::processEvents(); manager.grab().save(artifacts + "/protocol-delimited-array.png"); }
    manager.close(); return 0;
}
static bool exportDefaultName(const QString &expected, const std::function<void()> &action) {
    QString actual; QElapsedTimer deadline; deadline.start(); QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (widget->isVisible()) {
            if (auto picker = qobject_cast<QFileDialog *>(widget)) {
                actual = QFileInfo(picker->selectedFiles().value(0)).fileName();
                picker->reject();
            } else if (deadline.elapsed() > 8000) {
                if (auto dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
            }
        }
    });
    driver.start(10); action();
    if (actual != expected) std::cerr << "Export filename: " << actual.toStdString() << " expected " << expected.toStdString() << "\n";
    return actual == expected;
}
static int channelFieldInputTests(const QString &path, const QString &artifacts) {
    struct ClipboardRestore {
        QMimeData *saved = new QMimeData;
        ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
        ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
    } clipboardRestore;
    QString error; ProtocolProfile fixed, output; VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(true), fixed, error));
    ProtocolEditorWidget editor; editor.setProfile(fixed); editor.resize(1450, 1050); editor.show(); QCoreApplication::processEvents();
    auto table = editor.findChild<QTableWidget *>("protocolFields"); VERIFY(table && table->columnCount() == 11 && table->horizontalHeaderItem(10)->text() == "备注");
    VERIFY(table->item(0, 10) && table->item(0, 10)->text().isEmpty());
    QStyleOptionViewItem option;
    for (int col : {3, 6, 7, 8, 9}) {
        const auto index = table->model()->index(0, col); const auto original = index.data(Qt::EditRole).toString();
        auto delegate = table->itemDelegateForColumn(col); VERIFY(delegate);
        std::unique_ptr<QWidget> cellEditor(delegate->createEditor(table->viewport(), option, index));
        auto line = qobject_cast<QLineEdit *>(cellEditor.get()); VERIFY(line && line->validator()); delegate->setEditorData(line, index);
        line->selectAll(); line->insert(col == 3 ? "6" : "-1.25"); VERIFY(line->hasAcceptableInput());
        delegate->setModelData(line, table->model(), index); VERIFY(index.data().toString() == (col == 3 ? "6" : "-1.25"));
        const QString valid = line->text();
        for (const QString &invalid : {QString("中文"), QString("1.2中文"), QString("NaN"), QString("Inf"), QString("1,2"), QString("0xFF"), QString("１２３")}) {
            line->selectAll(); QApplication::clipboard()->setText(invalid);
            QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier); QApplication::sendEvent(line, &paste);
            VERIFY(line->text() == valid);
            line->selectAll(); QInputMethodEvent input; input.setCommitString(invalid); QApplication::sendEvent(line, &input); VERIFY(line->text() == valid);
        }
        line->selectAll(); QKeyEvent chinese(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, "中"); QApplication::sendEvent(line, &chinese); VERIFY(line->text() == valid);
        if (col == 3) {
            for (const QString &invalid : {QString("2.5"), QString("-1"), QString("1e2")}) { line->selectAll(); line->insert(invalid); VERIFY(line->text() == valid); }
        } else {
            for (const QString &number : {QString(".5"), QString("1."), QString("+0.5"), QString("6.02e2"), QString("-1.2E-3")}) {
                line->selectAll(); line->insert(number); VERIFY(line->text() == number && line->hasAcceptableInput());
                delegate->setModelData(line, table->model(), index); VERIFY(index.data().toString() == number);
            }
            const auto stored = index.data().toString(); line->selectAll(); line->insert("1e999"); VERIFY(!line->hasAcceptableInput());
            delegate->setModelData(line, table->model(), index); VERIFY(index.data().toString() == stored);
        }
        const auto beforeEmpty = index.data().toString(); line->clear(); delegate->setModelData(line, table->model(), index);
        VERIFY(col >= 8 ? line->hasAcceptableInput() && index.data().toString().isEmpty() : !line->hasAcceptableInput() && index.data().toString() == beforeEmpty);
        table->model()->setData(index, original);
    }
    const QString note = "电极 A1 / μV，±0.5% #2（备用）", unit = "µV/通道-A_1%";
    for (int col : {2, 10}) {
        const auto index = table->model()->index(0, col); auto delegate = table->itemDelegate(index);
        std::unique_ptr<QWidget> cellEditor(delegate->createEditor(table->viewport(), option, index)); auto line = qobject_cast<QLineEdit *>(cellEditor.get()); VERIFY(line && !line->validator());
        delegate->setEditorData(line, index); line->selectAll(); QInputMethodEvent input; input.setCommitString(col == 2 ? unit : note); QApplication::sendEvent(line, &input);
        delegate->setModelData(line, table->model(), index); VERIFY(index.data().toString() == (col == 2 ? unit : note));
    }
    VERIFY(table->item(0, 10)->toolTip() == note && editor.profile(output, error));
    VERIFY(output.definition["channels"].toArray()[0].toObject()["note"].toString() == note);
    editor.findChild<QPushButton *>("protocolFieldDown")->setFocus(); table->setCurrentCell(0, 10); editor.findChild<QPushButton *>("protocolFieldDown")->click();
    VERIFY(table->item(1, 10)->text() == note && table->item(0, 10)->text().isEmpty());
    table->setCurrentCell(1, 10); editor.findChild<QPushButton *>("protocolFieldUp")->click(); VERIFY(table->item(0, 10)->text() == note);
    editor.findChild<QPushButton *>("protocolAddField")->click(); VERIFY(table->item(table->rowCount() - 1, 10)->text().isEmpty());
    editor.setProfile(output);
    for (const auto &definition : {ProtocolEditorWidget::example(false), ProtocolEditorWidget::example(true), ProtocolEditorWidget::lengthExample(), ProtocolEditorWidget::dynamicArrayExample(), ProtocolProfileValidator::keyValueTemplate()}) {
        ProtocolProfile profile; VERIFY(ProtocolProfileValidator::profile(definition, profile, error)); editor.setProfile(profile);
        if (table->rowCount() == 0) editor.findChild<QPushButton *>("protocolAddField")->click();
        VERIFY(table->item(0, 10)->text().isEmpty()); table->item(0, 10)->setText(note); table->item(0, 2)->setText(unit);
        if (profile.isKeyValue()) {
            const auto index = table->model()->index(0, 3); auto delegate = table->itemDelegateForColumn(3);
            std::unique_ptr<QWidget> cellEditor(delegate->createEditor(table->viewport(), option, index)); auto line = qobject_cast<QLineEdit *>(cellEditor.get()); VERIFY(line && !line->validator());
            delegate->setEditorData(line, index); line->selectAll(); QInputMethodEvent input; input.setCommitString("温度_A-1"); QApplication::sendEvent(line, &input);
            delegate->setModelData(line, table->model(), index); VERIFY(table->item(0, 3)->text() == "温度_A-1");
        }
        VERIFY(editor.profile(output, error));
        ProtocolProfileStore store(path); VERIFY(store.load(error)); auto snapshot = store.snapshot(); snapshot.profiles = {output}; VERIFY(store.commit(snapshot, error));
        ProtocolProfileStore reopened(path); VERIFY(reopened.load(error)); editor.setProfile(reopened.snapshot().profiles[0]); VERIFY(table->item(0, 10)->text() == note && table->item(0, 2)->text() == unit);
        const auto sharePath = path + ".export.json"; VERIFY(reopened.exportProfiles(sharePath, {output.id}, error)); QByteArray bytes; VERIFY(ProtocolProfileStore::readFile(sharePath, bytes, error));
        ProtocolImportPreview imported; VERIFY(ProtocolProfileStore::prepareImport({}, bytes, false, ProtocolImportMode::Append, false, imported, error));
        VERIFY(imported.snapshot.profiles[0].definition["channels"].toArray()[0].toObject()["note"].toString() == note);
        table->item(0, 10)->setText(""); VERIFY(editor.profile(output, error) && !output.definition["channels"].toArray()[0].toObject().contains("note"));
    }
    if (!artifacts.isEmpty()) { editor.setProfile(fixed); table->item(0, 10)->setText(note); table->horizontalScrollBar()->setValue(table->horizontalScrollBar()->maximum()); QCoreApplication::processEvents(); editor.grab().save(artifacts + "/protocol-channel-notes.png"); }
    editor.close(); return 0;
}
static int diagnosticPreviewTests(const QString &path, const QString &artifacts) {
    QString error; ProtocolProfile binary, text, array, keys;
    VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(true), binary, error));
    VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(false), text, error));
    VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::dynamicArrayExample(), array, error));
    VERIFY(ProtocolProfileValidator::profile(ProtocolProfileValidator::keyValueTemplate(), keys, error));
    ProtocolProfileStore store(path); VERIFY(store.load(error)); auto snapshot = store.snapshot(); snapshot.profiles = {binary, text, array, keys}; VERIFY(store.commit(snapshot, error));
    ProtocolManagerDialog manager(path, nullptr, binary.id); manager.resize(1450, 950); manager.show(); QCoreApplication::processEvents();
    auto protocol = manager.findChild<QComboBox *>("protocolPreviewProtocol"), mode = manager.findChild<QComboBox *>("protocolInputMode"), chunks = manager.findChild<QComboBox *>("protocolChunkMode");
    auto input = manager.findChild<QPlainTextEdit *>("protocolSample"), diagnostics = manager.findChild<QPlainTextEdit *>("protocolDiagnostics");
    auto test = manager.findChild<QPushButton *>("protocolTest"); auto results = manager.findChild<QTableWidget *>("protocolResults"); auto stats = manager.findChild<QLabel *>("protocolStats");
    VERIFY(protocol && mode && chunks && input && diagnostics && test && results && stats);
    auto run = [&](const ProtocolProfile &profile, const QByteArray &bytes) {
        protocol->setCurrentIndex(protocol->findData(profile.id));
        const bool binaryInput = profile.definition["type"] == QJsonValue("binary"); mode->setCurrentIndex(binaryInput ? 1 : 0);
        input->setPlainText(ProtocolSampleEdit::displayBytes(bytes, binaryInput, true, binaryInput));
        test->click(); return until([&] { return test->isEnabled(); });
    };
    QByteArray good; VERIFY(ProtocolSampleGenerator::generate(binary, good, error) && good.size() == 11);
    auto damaged = good; damaged[10] = 0; QString first;
    for (int packetMode : {0, 1, 2}) {
        chunks->setCurrentIndex(packetMode); VERIFY(run(binary, damaged + good));
        VERIFY(results->rowCount() == 2 && stats->text().contains("存在异常"));
        const auto message = diagnostics->toPlainText();
        VERIFY(message.contains("字节偏移 10") && message.contains("帧尾不匹配") && message.contains("0D 0A") && message.contains("0D 00") && message.contains("附近输入"));
        QCoreApplication::processEvents();
        VERIFY(stats->height() >= stats->heightForWidth(stats->width()));
        if (first.isEmpty()) first = message; else VERIFY(message == first);
    }
    if (!artifacts.isEmpty()) manager.grab().save(artifacts + "/protocol-error-diagnostics.png");
    VERIFY(run(binary, good) && results->rowCount() == 2 && diagnostics->toPlainText().isEmpty() && stats->text().contains("未发现异常"));
    damaged = good; damaged[8] = char(quint8(damaged[8]) ^ 1);
    VERIFY(run(binary, damaged) && diagnostics->toPlainText().contains("数据校验失败") && diagnostics->toPlainText().contains("预期"));
    damaged = good; damaged[0] = 0;
    VERIFY(run(binary, damaged) && diagnostics->toPlainText().contains("未找到帧头"));
    VERIFY(run(binary, good.left(8)) && diagnostics->toPlainText().contains("字节数量不足"));
    VERIFY(run(text, "DATA:25.34;3.30\r\n") && diagnostics->toPlainText().contains("分隔符"));
    VERIFY(run(text, "DATA:25.34,3.30\n") && diagnostics->toPlainText().contains("结束符缺失"));
    VERIFY(run(array, QByteArray::fromHex("AA55FFFF")) && diagnostics->toPlainText().contains("长度字段错误"));
    VERIFY(run(keys, "0:1\r\n1:2\r\n") && diagnostics->toPlainText().contains("仍缺键 “2”"));
    damaged = good; damaged[10] = 0; chunks->setCurrentIndex(0);
    VERIFY(run(binary, damaged.repeated(120)) && diagnostics->toPlainText().count("【解析错误】") == 100 && stats->text().contains("拒绝 120 帧"));
    VERIFY(run(binary, good) && diagnostics->toPlainText().isEmpty()); manager.close(); return 0;
}
int main(int argc, char **argv) {
    TestRegistrySandbox registry; VERIFY(registry.ready()); QApplication app(argc, argv);
    app.setAttribute(Qt::AA_DontUseNativeDialogs);
    // Offscreen Qt does not enumerate native Windows fonts automatically.
    const int fontId = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (fontId >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).value(0), 9));
    QTemporaryDir temporary; VERIFY(temporary.isValid()); QString error;
    {
        ProtocolSampleEdit input; input.show(); input.setFocus(); input.setHexMode(true);
        auto key = [&input](int code, const QString &text = QString(), Qt::KeyboardModifiers modifiers = Qt::NoModifier) { QKeyEvent event(QEvent::KeyPress, code, modifiers, text); QApplication::sendEvent(&input, &event); };
        key(Qt::Key_A, "aa550d0a"); VERIFY(input.toPlainText() == "AA 55 0D 0A");
        key(Qt::Key_G, "G中文"); VERIFY(input.toPlainText() == "AA 55 0D 0A");
        QInputMethodEvent invalid; invalid.setCommitString("中文"); QApplication::sendEvent(&input, &invalid); VERIFY(input.toPlainText() == "AA 55 0D 0A");
        key(Qt::Key_Backspace); VERIFY(input.toPlainText() == "AA 55 0D 0"); key(Qt::Key_A, "a"); VERIFY(input.toPlainText() == "AA 55 0D 0A");
        input.setSpacedHex(false); VERIFY(input.toPlainText() == "AA550D0A"); input.setSpacedHex(true); VERIFY(input.toPlainText() == "AA 55 0D 0A");
        auto cursor = input.textCursor(); cursor.setPosition(3); input.setTextCursor(cursor); key(Qt::Key_B, "b"); VERIFY(input.toPlainText() == "AA B5 50 D0 A");
        key(Qt::Key_Z, {}, Qt::ControlModifier); VERIFY(input.toPlainText() == "AA 55 0D 0A"); key(Qt::Key_Y, {}, Qt::ControlModifier); VERIFY(input.toPlainText() == "AA B5 50 D0 A");
        struct ClipboardRestore {
            QMimeData *saved = new QMimeData;
            ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
            ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
        } restore;
        input.selectAll(); QApplication::clipboard()->setText("aB cd 0d0a"); key(Qt::Key_V, {}, Qt::ControlModifier); VERIFY(input.toPlainText() == "AB CD 0D 0A");
        input.selectAll(); QApplication::clipboard()->setText("AA GG"); key(Qt::Key_V, {}, Qt::ControlModifier); VERIFY(input.toPlainText() == "AB CD 0D 0A");
        key(Qt::Key_Z, {}, Qt::ControlModifier); VERIFY(input.toPlainText() == "AA B5 50 D0 A"); key(Qt::Key_Y, {}, Qt::ControlModifier); VERIFY(input.toPlainText() == "AB CD 0D 0A");
        QByteArray all; for (int i = 0; i < 256; ++i) all.append(char(i)); QByteArray decoded; QString error;
        VERIFY(ProtocolSampleEdit::textBytes(ProtocolSampleEdit::displayBytes(all, false, false), decoded, error) && decoded == all);
        const auto escapedAll = ProtocolSampleEdit::displayBytes(all, false, false, true);
        VERIFY(escapedAll.size() == 4 * 256 && escapedAll.startsWith("\\x00\\x01") && escapedAll.endsWith("\\xFF"));
        VERIFY(ProtocolSampleEdit::textBytes(escapedAll, decoded, error) && decoded == all);
        VERIFY(ProtocolSampleEdit::displayBytes(all, true, true, true) == ProtocolSampleEdit::displayBytes(all, true, true));
        VERIFY(ProtocolSampleEdit::textBytes("中文\\r\\n\\x00\\\\x41", decoded, error) && decoded == QString("中文").toUtf8() + QByteArray("\r\n\0\\x41", 7));
        VERIFY(!ProtocolSampleEdit::textBytes("\\xG1", decoded, error));
        const QString binaryAsText = "\\xAAU\\x01\\x00\\x00\\x00\\x00@\\x00\\x00@@\\x00\\x00\\x80@\\x00\\r\\n";
        const auto originalBytes = QByteArray::fromHex("AA550100000000400000404000008040000D0A");
        VERIFY(ProtocolSampleEdit::textBytes(binaryAsText, decoded, error) && decoded == originalBytes);
        VERIFY(ProtocolSampleEdit::displayBytes(originalBytes, false, false) == binaryAsText); // '@' is 40 and 'U' is 55, not extra bytes.
        const QString binaryEscaped = "\\xAA\\x55\\x01\\x00\\x00\\x00\\x00\\x40\\x00\\x00\\x40\\x40\\x00\\x00\\x80\\x40\\x00\\x0D\\x0A";
        VERIFY(ProtocolSampleEdit::displayBytes(originalBytes, false, false, true) == binaryEscaped);
        VERIFY(ProtocolSampleEdit::textBytes(binaryEscaped, decoded, error) && decoded == originalBytes);
        VERIFY(ProtocolSampleEdit::displayBytes("DATA:1,2\r\n", false, false) == "DATA:1,2\\r\\n");
    }
    ProtocolEditorWidget editor;
    {
        ProtocolEditorWidget settings;
        for (const auto &definition : {ProtocolEditorWidget::example(false), ProtocolEditorWidget::example(true), ProtocolEditorWidget::lengthExample(), ProtocolEditorWidget::dynamicArrayExample(), ProtocolProfileValidator::keyValueTemplate()}) {
            ProtocolProfile seed, saved; VERIFY(ProtocolProfileValidator::profile(definition, seed, error)); settings.setProfile(seed);
            auto mode = settings.findChild<QComboBox *>("protocol_recordingValueMode");
            VERIFY(mode && mode->count() == 2 && mode->currentData().toString() == "raw" && mode->currentText() == "原始数值（默认）");
            VERIFY(!mode->toolTip().isEmpty() && mode->itemText(1) == "换算后数值");
            int edits = 0; auto connection = QObject::connect(&settings, &ProtocolEditorWidget::edited, [&] { ++edits; });
            mode->setCurrentIndex(mode->findData("converted"));
            VERIFY(edits > 0 && settings.profile(saved, error) && saved.recordsConvertedValues());
            VERIFY(saved.definition.value("channels") == seed.definition.value("channels"));
            settings.setProfile(saved); VERIFY(mode->currentData().toString() == "converted");
            ProtocolEditorWidget reopened; reopened.setProfile(saved);
            VERIFY(reopened.findChild<QComboBox *>("protocol_recordingValueMode")->currentData().toString() == "converted");
            mode->setCurrentIndex(mode->findData("raw")); VERIFY(settings.profile(saved, error) && !saved.recordsConvertedValues());
            settings.resize(900, 1400); settings.show(); QApplication::processEvents();
            auto table = settings.findChild<QTableWidget *>("protocolFields");
            VERIFY(mode->mapTo(&settings, QPoint()).y() < table->mapTo(&settings, QPoint()).y());
            QObject::disconnect(connection);
        }
    }
    {
        ProtocolEditorWidget boundaries; ProtocolProfile seed, output;
        VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(true), seed, error)); boundaries.setProfile(seed);
        auto headPreset = boundaries.findChild<QComboBox *>("protocolHeaderPreset"), tailPreset = boundaries.findChild<QComboBox *>("protocolTailPreset");
        auto head = boundaries.findChild<QLineEdit *>("protocol_headerHex"), tail = boundaries.findChild<QLineEdit *>("protocol_tailHex");
        auto summary = boundaries.findChild<QLabel *>("protocolFrameSummary"); auto type = boundaries.findChild<QComboBox *>("protocolType");
        VERIFY(headPreset && tailPreset && head && tail && summary && headPreset->count() == 4 && tailPreset->count() == 6);
        VERIFY(headPreset->currentData().toString() == "AA55" && tailPreset->currentData().toString() == "0D0A");
        VERIFY(!headPreset->toolTip().isEmpty() && !tailPreset->toolTip().isEmpty() && tailPreset->toolTip().contains("不代替完整协议配置"));
        VERIFY(boundaries.findChild<QWidget *>("protocol_headerHexRow") == head->parentWidget());
        VERIFY(boundaries.findChild<QWidget *>("protocol_tailHexRow") == tail->parentWidget());
        for (auto combo : {headPreset, tailPreset}) for (int index = 0; index < combo->count() - 1; ++index) {
            combo->setCurrentIndex(index); const auto hex = combo->itemData(index).toString();
            VERIFY((combo == headPreset ? head : tail)->text() == hex && combo->currentData().toString() == hex);
            VERIFY(!combo->itemData(index, Qt::ToolTipRole).toString().isEmpty());
        }
        // Choosing Custom is only a view mode: it neither clears the current
        // bytes nor marks the saved protocol dirty.
        int edits = 0; QObject::connect(&boundaries, &ProtocolEditorWidget::edited, [&] { ++edits; });
        const auto retained = tail->text(); tailPreset->setCurrentIndex(tailPreset->findData("custom"));
        VERIFY(tail->text() == retained && edits == 0);
        head->clear(); head->insert("aa 55"); VERIFY(head->text() == "AA 55" && headPreset->currentData().toString() == "AA55");
        tail->clear(); tail->insert("0d 0a"); VERIFY(tail->text() == "0D 0A" && tailPreset->currentData().toString() == "0D0A");
        head->setText("C0FF"); tail->setText("F00D");
        VERIFY(headPreset->currentData().toString() == "custom" && tailPreset->currentData().toString() == "custom");
        VERIFY(boundaries.profile(output, error)); const auto customDefinition = output.definition;
        boundaries.setProfile(seed); boundaries.setProfile(output); ProtocolProfile roundTrip;
        VERIFY(boundaries.profile(roundTrip, error) && roundTrip.definition == customDefinition);
        VERIFY(head->text() == "C0FF" && tail->text() == "F00D" && headPreset->currentData().toString() == "custom" && tailPreset->currentData().toString() == "custom");
        ProtocolSnapshot snapshot; snapshot.profiles = {roundTrip}; snapshot.shared.kind = "custom"; snapshot.shared.id = roundTrip.id;
        const auto bytes = ProtocolProfileValidator::encode(snapshot, false, error); ProtocolSnapshot imported;
        VERIFY(!bytes.isEmpty() && ProtocolProfileValidator::decode(bytes, false, imported, error));
        VERIFY(imported.profiles[0].definition == customDefinition && !bytes.contains("Preset"));
        boundaries.setProfile(imported.profiles[0]); VERIFY(headPreset->currentData().toString() == "custom" && tailPreset->currentData().toString() == "custom");
        head->setText("A"); VERIFY(headPreset->currentData().toString() == "custom" && !boundaries.profile(output, error));
        head->setText("00"); VERIFY(headPreset->currentData().toString() == "custom" && head->text() != headPreset->itemData(headPreset->findData("")).toString());
        headPreset->setCurrentIndex(headPreset->findData("")); tailPreset->setCurrentIndex(tailPreset->findData(""));
        VERIFY(head->text().isEmpty() && tail->text().isEmpty() && !boundaries.profile(output, error));
        VERIFY(summary->styleSheet().contains("#E85C5C") && summary->text().contains("不能同时为空"));
        tailPreset->setCurrentIndex(tailPreset->findData("0D0A")); VERIFY(boundaries.profile(output, error));
        VERIFY(headPreset->currentData().toString().isEmpty() && !summary->styleSheet().contains("#E85C5C"));
        boundaries.setProfile(output); VERIFY(head->text().isEmpty() && tail->text() == "0D0A");
        // Existing JustFloat-compatible fixed profiles auto-match the preset,
        // retaining all ten fields and their original 44-byte layout.
        auto eeg = seed.definition; eeg.insert("checksum", QJsonObject{{"algorithm", "none"}});
        eeg.insert("frame", QJsonObject{{"mode", "fixed"}, {"headerHex", ""}, {"tailHex", "0000807F"}, {"sizeBytes", 44}, {"payloadOffset", 0}, {"payloadBytes", 40}, {"maxFrameBytes", 4096}});
        QJsonArray channels; for (int i = 0; i < 10; ++i) channels.append(QJsonObject{{"id", QString("c%1").arg(i)}, {"name", QString("Ch%1").arg(i + 1)}, {"offset", i * 4}, {"type", "float32"}, {"scale", 1}, {"bias", 0}}); eeg.insert("channels", channels);
        VERIFY(ProtocolProfileValidator::profile(eeg, output, error)); boundaries.setProfile(output);
        VERIFY(headPreset->currentData().toString().isEmpty() && tailPreset->currentData().toString() == "0000807F");
        VERIFY(tailPreset->currentText() == "JustFloat 小端（0000807F）");
        VERIFY(boundaries.profile(roundTrip, error) && roundTrip.definition == output.definition);
        for (const auto &fixture : {ProtocolEditorWidget::lengthExample(), ProtocolEditorWidget::dynamicArrayExample()}) {
            VERIFY(ProtocolProfileValidator::profile(fixture, seed, error)); boundaries.setProfile(seed);
            auto model = qobject_cast<QStandardItemModel *>(headPreset->model()); VERIFY(model && !model->item(headPreset->findData(""))->isEnabled());
            VERIFY(boundaries.findChild<QLabel *>("protocol_label_headerHex")->text().contains("必填"));
            const auto unchanged = head->text(); edits = 0;
            headPreset->setCurrentIndex(headPreset->findData("")); VERIFY(head->text() == unchanged && headPreset->currentData().toString() == "AA55" && edits == 0);
            tailPreset->setCurrentIndex(tailPreset->findData("")); VERIFY(tail->text().isEmpty() && boundaries.profile(output, error));
            if (seed.isDynamicArray()) {
                VERIFY(output.definition["frame"].toObject()["payloadEndOffset"] == QJsonValue(0));
                VERIFY(output.definition["frame"].toObject()["minFrameBytes"] == QJsonValue(8));
            }
            head->clear(); VERIFY(!boundaries.profile(output, error) && summary->styleSheet().contains("#E85C5C"));
            headPreset->setCurrentIndex(headPreset->findData("55AA")); VERIFY(head->text() == "55AA" && boundaries.profile(output, error));
            type->setCurrentIndex(1); VERIFY(model->item(headPreset->findData(""))->isEnabled());
        }
    }
    {
        ProtocolEditorWidget standard; ProtocolProfile fixed, output; QByteArray sample;
        VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(true), fixed, error)); standard.setProfile(fixed);
        standard.findChild<QSpinBox *>("protocol_sizeBytes")->setValue(12);
        VERIFY(standard.profile(output, error)); const auto check = output.definition["checksum"].toObject();
        VERIFY(check["range"].toObject()["offset"] == QJsonValue(0) && check["field"].toObject()["from"] == QJsonValue("end") && check["field"].toObject()["offset"] == QJsonValue(3));
        auto summary = standard.findChild<QLabel *>("protocolFrameSummary"); VERIFY(summary->text().startsWith("帧结构："));
        VERIFY(summary->text().indexOf("其他/保留（1）") < summary->text().indexOf("数据校验（1）"));
        VERIFY(ProtocolSampleGenerator::generate(output, sample, error)); sample[8] = char(0x71); sample[9] = char(ProtocolChecksum("sum8").calculate(sample.constData(), 9));
        std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(output, nullptr, error)));
        VERIFY(parser && parser->processData(sample.constData(), sample.size()) == sample.size() && parser->stats().validFrames == 1);
        sample[8] = char(0x72); parser.reset(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(output, nullptr, error))); parser->processData(sample.constData(), sample.size());
        VERIFY(parser->stats().validFrames == 0 && parser->stats().checksumFailures > 0);
        standard.findChild<QSpinBox *>("protocol_sizeBytes")->setValue(8);
        VERIFY(!standard.profile(output, error) && summary->text().contains("配置冲突") && summary->styleSheet().contains("#E85C5C"));
        standard.findChild<QSpinBox *>("protocol_sizeBytes")->setValue(11); VERIFY(standard.profile(output, error) && !summary->styleSheet().contains("#E85C5C"));
        standard.findChild<QLineEdit *>("protocol_tailHex")->clear(); standard.findChild<QSpinBox *>("protocol_sizeBytes")->setValue(9);
        VERIFY(standard.profile(output, error) && output.definition["checksum"].toObject()["field"].toObject()["offset"] == QJsonValue(1));
        VERIFY(ProtocolSampleGenerator::generate(output, sample, error)); parser.reset(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(output, nullptr, error))); parser->processData(sample.constData(), sample.size()); VERIFY(parser->stats().validFrames == 1);
        standard.findChild<QLineEdit *>("protocol_headerHex")->clear();
        VERIFY(!standard.profile(output, error) && error.contains("不能同时为空"));
        VERIFY(summary->text().contains("不能同时为空") && summary->styleSheet().contains("#E85C5C"));
        standard.findChild<QLineEdit *>("protocol_headerHex")->setText("AA55");
        VERIFY(standard.profile(output, error) && !summary->styleSheet().contains("#E85C5C"));
        auto variable = ProtocolEditorWidget::lengthExample(); ProtocolProfile v; VERIFY(ProtocolProfileValidator::profile(variable, v, error)); standard.setProfile(v);
        standard.findChild<QSpinBox *>("protocol_minFrameBytes")->setValue(13); VERIFY(standard.profile(output, error));
        VERIFY(output.definition["frame"].toObject()["lengthField"].toObject()["adjustment"] == QJsonValue(7));
        for (auto key : {"lengthAdjustment", "lengthUnit", "checkStart", "checkLength", "checkOffset", "checkRangeMode", "checkFrom", "checkEndOffset", "checkByteOrder"}) VERIFY(standard.findChild<QWidget *>(QString("protocol_") + key)->isHidden());
    }
    {
        ProtocolEditorWidget arrays; ProtocolProfile seed, saved; QByteArray packet;
        VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::dynamicArrayExample(), seed, error));
        arrays.setProfile(seed); VERIFY(arrays.profile(saved, error) && saved.definition == seed.definition);
        auto combo = [&arrays](const char *key, const char *choice) { auto c = arrays.findChild<QComboBox *>(QString("protocol_") + key); c->setCurrentIndex(c->findData(choice)); };
        combo("arrayType", "int16"); combo("lengthMeaning", "elements"); combo("algorithm", "crc16-modbus"); combo("headerAlgorithm", "crc8-smbus");
        arrays.findChild<QSpinBox *>("protocol_prefixBytes")->setValue(3);
        arrays.findChild<QSpinBox *>("protocol_trailerBytes")->setValue(2);
        VERIFY(arrays.profile(saved, error));
        const auto frame = saved.definition["frame"].toObject(), length = frame["lengthField"].toObject();
        VERIFY(frame["payloadOffset"] == QJsonValue(8) && frame["payloadEndOffset"] == QJsonValue(6));
        VERIFY(length["unitBytes"] == QJsonValue(2) && length["adjustment"] == QJsonValue(14));
        VERIFY(arrays.findChild<QSpinBox *>("protocol_payloadOffset")->isHidden() && arrays.findChild<QSpinBox *>("protocol_checkStart")->isHidden());
        VERIFY(ProtocolSampleGenerator::generate(saved, packet, error, 13));
        arrays.findChild<QPushButton *>("protocolAddField")->click(); auto table = arrays.findChild<QTableWidget *>("protocolFields");
        VERIFY(table->rowCount() == 1 && table->item(0, 3)->text() == "0"); table->item(0, 1)->setText("温度");
        VERIFY(arrays.profile(saved, error) && saved.outputChannelNames(13)[0] == "温度" && saved.outputChannelNames(13)[12] == "Ch13");
        arrays.findChild<QCheckBox *>("protocol_autoFrameLayout")->setChecked(false);
        VERIFY(!arrays.findChild<QSpinBox *>("protocol_payloadOffset")->isHidden() && arrays.findChild<QSpinBox *>("protocol_payloadOffset")->isEnabled());
        VERIFY(arrays.profile(saved, error)); arrays.setProfile(saved); ProtocolProfile roundTrip;
        VERIFY(arrays.profile(roundTrip, error) && roundTrip.definition == saved.definition);
    }
    auto editorType = editor.findChild<QComboBox *>("protocolType");
    VERIFY(editorType && editorType->count() == 5);
    for (int i = 0; i < 5; ++i) VERIFY(editorType->itemText(i) == ProtocolEditorWidget::protocolTypeNames()[i]);
    VERIFY(!editor.findChild<QComboBox *>("protocol_frameMode"));
    auto basicForm = qobject_cast<QFormLayout *>(editor.findChild<QLineEdit *>("protocolName")->parentWidget()->layout());
    auto titleField = editor.findChild<QLineEdit *>("protocolName");
    VERIFY(titleField->font().pointSizeF() >= editor.font().pointSizeF() + 2 && titleField->font().weight() >= QFont::DemiBold);
    const QFont editorFont = editor.font(); QFont largerFont = editorFont; largerFont.setPointSizeF(editorFont.pointSizeF() + 2);
    editor.setFont(largerFont); QCoreApplication::processEvents();
    VERIFY(titleField->font().pointSizeF() >= largerFont.pointSizeF() + 2);
    editor.setFont(editorFont); QCoreApplication::processEvents();
    VERIFY(titleField->font().pointSizeF() == editorFont.pointSizeF() + 2);
    VERIFY(basicForm && qobject_cast<QLabel *>(basicForm->labelForField(editor.findChild<QLineEdit *>("protocolName")))->text() == "协议名称");
    int nameRow, typeRow, descriptionRow; QFormLayout::ItemRole role;
    basicForm->getWidgetPosition(editor.findChild<QLineEdit *>("protocolName"), &nameRow, &role);
    basicForm->getWidgetPosition(editorType, &typeRow, &role);
    basicForm->getWidgetPosition(editor.findChild<QPlainTextEdit *>("protocolDescription"), &descriptionRow, &role);
    VERIFY(nameRow < typeRow && typeRow < descriptionRow);
    // Hover help must be attached to the captions themselves, including
    // inactive binary/CRC rows kept outside the compact form layout.
    const auto captions = editor.findChildren<QLabel *>(); int explained = 0;
    for (auto caption : captions) if (caption->objectName().startsWith("protocol_label_")) {
        VERIFY(!caption->toolTip().isEmpty()); ++explained;
    }
    VERIFY(explained == 48);
    auto textForm = qobject_cast<QFormLayout *>(editor.findChild<QLineEdit *>("protocol_prefix")->parentWidget()->layout());
    int previousRow = -1;
    for (auto key : {"prefix", "suffix", "delimiterMode", "delimiter", "terminatorHex"}) {
        int row; textForm->getWidgetPosition(editor.findChild<QWidget *>(QString("protocol_") + (QString::fromLatin1(key) == "terminatorHex" ? "terminatorRow" : key)), &row, &role);
        VERIFY(row > previousRow); previousRow = row;
    }
    for (auto key : {"prefix", "suffix"}) VERIFY(editor.findChild<QLabel *>(QString("protocol_label_") + key)->text().endsWith("（可空）"));
    for (auto key : {"terminatorHex", "headerHex", "tailHex"}) VERIFY(checkHexInput(editor.findChild<QLineEdit *>(QString("protocol_") + key)));
    // Ordinary textual prefixes remain textual, not constrained to HEX.
    auto prefix = editor.findChild<QLineEdit *>("protocol_prefix"); prefix->insert("中文DATA:"); VERIFY(prefix->text() == "中文DATA:");
    for (bool binary : {false, true}) {
        ProtocolProfile source, roundTrip;
        VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(binary), source, error));
        editor.setProfile(source); VERIFY(editor.profile(roundTrip, error));
        VERIFY(editorType->currentIndex() == (binary ? 1 : 0));
        VERIFY(roundTrip.definition == source.definition);
    }
    ProtocolProfile lengthProfile, lengthRoundTrip;
    VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::lengthExample(), lengthProfile, error));
    editor.setProfile(lengthProfile); VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition == lengthProfile.definition);
    auto frameSummary = editor.findChild<QLabel *>("protocolFrameSummary"); VERIFY(frameSummary && !frameSummary->isHidden());
    VERIFY(frameSummary->textInteractionFlags().testFlag(Qt::TextSelectableByMouse) && frameSummary->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));
    VERIFY(frameSummary->text().startsWith("帧结构：") && frameSummary->text().contains("长度（1）")
        && frameSummary->text().contains("帧尾（2）") && frameSummary->text().contains("T − 12"));
    auto tail = editor.findChild<QLineEdit *>("protocol_tailHex"); tail->setText("0D");
    VERIFY(frameSummary->text().contains("帧尾（1）") && frameSummary->text().contains("T − 11"));
    editor.setProfile(lengthProfile);
    VERIFY(editorType->currentIndex() == 2);
    auto lengthType = editor.findChild<QComboBox *>("protocol_lengthType");
    VERIFY(lengthType && !editor.findChild<QComboBox *>("protocol_lengthByteOrder"));
    auto wide = lengthProfile.definition; auto wideFrame = wide.value("frame").toObject(), wideLength = wideFrame.value("lengthField").toObject();
    wideLength.insert("type", "uint16"); wideLength.insert("byteOrder", "big"); wideLength.insert("adjustment", 10);
    wideFrame.insert("lengthField", wideLength); wideFrame.insert("payloadOffset", 4); wideFrame.insert("minFrameBytes", 16); wide.insert("frame", wideFrame);
    wide.insert("byteOrder", "big");
    wide.insert("checksum", QJsonObject{{"algorithm", "crc32-iso-hdlc"}, {"range", QJsonObject{{"offset", 0}, {"endOffset", 6}}},
        {"field", QJsonObject{{"from", "end"}, {"offset", 6}, {"type", "uint32"}, {"byteOrder", "big"}}}});
    VERIFY(ProtocolProfileValidator::profile(wide, lengthProfile, error)); editor.setProfile(lengthProfile);
    VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition == lengthProfile.definition);
    VERIFY(editor.findChild<QComboBox *>("protocol_checkByteOrder")->isEnabled());
    VERIFY(editor.findChild<QLabel *>("protocolLegacyLengthHint")->isHidden());
    auto defaultOrder = editor.findChild<QComboBox *>("protocol_byteOrder"); defaultOrder->setCurrentIndex(defaultOrder->findData("little"));
    VERIFY(editor.findChild<QLabel *>("protocolLegacyLengthHint")->isHidden());
    VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition.value("frame").toObject().value("lengthField").toObject().value("byteOrder") == QJsonValue("little"));
    defaultOrder->setCurrentIndex(defaultOrder->findData("big"));
    VERIFY(editor.findChild<QLabel *>("protocolChecksumSummary")->text().contains("04C11DB7"));
    auto algorithm = editor.findChild<QComboBox *>("protocol_algorithm");
    algorithm->setCurrentIndex(algorithm->findData("none"));
    for (auto key : {"checkStart", "checkRangeMode", "checkLength", "checkEndOffset", "checkFrom", "checkOffset", "checkByteOrder"}) {
        auto field = editor.findChild<QWidget *>(QString("protocol_") + key); VERIFY(field && field->isHidden());
        auto form = qobject_cast<QFormLayout *>(field->parentWidget()->layout());
        VERIFY(form && !form->labelForField(field)); // No empty hidden rows.
        VERIFY(editor.findChild<QLabel *>(QString("protocol_label_") + key)->isHidden());
    }
    VERIFY(editor.findChild<QLabel *>("protocolChecksumSummary")->isHidden());
    VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition.value("checksum").toObject().value("algorithm") == QJsonValue("none"));
    algorithm->setCurrentIndex(algorithm->findData("crc32-iso-hdlc"));
    VERIFY(editor.findChild<QWidget *>("protocol_checkOffset")->isHidden());
    VERIFY(editor.findChild<QWidget *>("protocol_checkByteOrder")->isHidden());
    VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition == lengthProfile.definition);
    // Returning to an old fixed scheme must not retain hidden length rules.
    ProtocolProfile fixed; VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(true), fixed, error)); editor.setProfile(fixed);
    VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition == fixed.definition);
    editor.resize(900, 1100); editor.show(); QCoreApplication::processEvents();
    auto conditions = editor.findChild<QTableWidget *>("protocolConditions");
    for (int col = 0; col < 2; ++col) VERIFY(!conditions->horizontalHeaderItem(col)->toolTip().isEmpty());
    auto fieldsTable = editor.findChild<QTableWidget *>("protocolFields");
    for (int col = 0; col < fieldsTable->columnCount(); ++col) VERIFY(!fieldsTable->horizontalHeaderItem(col)->toolTip().isEmpty());
    VERIFY(editor.findChild<QLabel *>("protocolConditionsHint")->text().contains("命令码"));
    VERIFY(editor.findChild<QLabel *>("protocolChannelsHint")->text().contains("默认"));
    auto headersFit = [](QTableWidget *table) {
        QFontMetrics metrics(table->horizontalHeader()->font());
        for (int col = 1; col < table->columnCount(); ++col) if (!table->isColumnHidden(col))
            if (table->columnWidth(col) < metrics.horizontalAdvance(table->horizontalHeaderItem(col)->text()) + 16) return false;
        return true;
    };
    VERIFY(headersFit(fieldsTable));
    const QFont normalFont = editor.font(); auto largeFont = normalFont; largeFont.setPointSize(14);
    editor.setFont(largeFont); QCoreApplication::processEvents(); VERIFY(headersFit(fieldsTable));
    editor.setFont(normalFont); QCoreApplication::processEvents();
    // Displaying '默认' is presentation only; the stored empty override must
    // still inherit the protocol byte order, including after an explicit edit.
    auto choiceDelegate = fieldsTable->itemDelegateForColumn(5);
    const auto orderIndex = fieldsTable->model()->index(0, 5);
    auto orderChoice = qobject_cast<QComboBox *>(choiceDelegate->createEditor(fieldsTable, QStyleOptionViewItem(), orderIndex));
    VERIFY(orderChoice); choiceDelegate->setEditorData(orderChoice, orderIndex); VERIFY(orderChoice->currentText() == "默认" && orderChoice->currentData().toString().isEmpty());
    orderChoice->setCurrentIndex(2); choiceDelegate->setModelData(orderChoice, fieldsTable->model(), orderIndex);
    VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition.value("channels").toArray()[0].toObject().value("byteOrder") == QJsonValue("big"));
    orderChoice->setCurrentIndex(0); choiceDelegate->setModelData(orderChoice, fieldsTable->model(), orderIndex);
    VERIFY(editor.profile(lengthRoundTrip, error) && lengthRoundTrip.definition == fixed.definition); delete orderChoice;
    editor.findChild<QPushButton *>("protocolAddField")->click(); VERIFY(fieldsTable->item(2, 1)->text() == "Ch3"); editor.setProfile(fixed);
    editor.findChild<QPushButton *>("protocolAddField")->click(); VERIFY(fieldsTable->item(2, 3)->text() == "6");
    fieldsTable->selectRow(2); editor.findChild<QPushButton *>("protocolFieldUp")->click();
    VERIFY(fieldsTable->item(0, 3)->text() == "0" && fieldsTable->item(1, 3)->text() == "2" && fieldsTable->item(2, 3)->text() == "6");
    fieldsTable->item(0, 4)->setText("float64"); VERIFY(fieldsTable->item(1, 3)->text() == "8" && fieldsTable->item(2, 3)->text() == "12");
    fieldsTable->selectRow(1); editor.findChild<QPushButton *>("protocolFieldDown")->click();
    VERIFY(fieldsTable->item(2, 3)->text() == "12");
    fieldsTable->selectRow(0); editor.findChild<QPushButton *>("protocolDeleteField")->click();
    VERIFY(fieldsTable->item(0, 3)->text() == "0" && fieldsTable->item(1, 3)->text() == "4");
    for (int i = 0; i < 25; ++i) editor.findChild<QPushButton *>("protocolAddField")->click();
    QCoreApplication::processEvents();
    VERIFY(fieldsTable->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && fieldsTable->height() > 27 * fieldsTable->rowHeight(0));
    VERIFY(fieldsTable->visualItemRect(fieldsTable->item(26, 1)).bottom() <= fieldsTable->viewport()->height()); editor.setProfile(fixed);
    conditions->insertRow(0); conditions->setItem(0, 0, new QTableWidgetItem("0")); conditions->setItem(0, 1, new QTableWidgetItem("AA55"));
    conditions->editItem(conditions->item(0, 1)); QCoreApplication::processEvents();
    auto hexCell = conditions->findChild<QLineEdit *>("protocolConditionHexEditor"); VERIFY(checkHexInput(hexCell));
    hexCell->clear(); hexCell->insert("aa55"); editor.findChild<QLineEdit *>("protocolName")->setFocus(); QCoreApplication::processEvents();
    VERIFY(conditions->item(0, 1)->text() == "AA55"); VERIFY(editor.profile(lengthRoundTrip, error));
    editor.setProfile(fixed); // Remove the temporary condition without changing the fixture.
    auto ending = editor.findChild<QLineEdit *>("protocol_terminatorHex");
    ProtocolProfile textRequired; VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(false), textRequired, error));
    editor.setProfile(textRequired); ending->clear(); ending->insert("d"); VERIFY(ending->text() == "D" && !editor.profile(lengthRoundTrip, error));
    ending->clear(); VERIFY(!editor.profile(lengthRoundTrip, error) && error.contains("请选择或输入结束符")); // Required even though empty is editable.
    VERIFY(!editor.findChild<QLabel *>("protocolTerminatorHint")->isHidden());
    {
        auto presets = editor.findChild<QComboBox *>("protocolTerminatorPreset"); auto hint = editor.findChild<QLabel *>("protocolTerminatorHint");
        VERIFY(presets && presets->count() == 8 && presets->findData("") == -1 && presets->findText("无") == -1);
        for (int index = 0; index < presets->count() - 1; ++index) {
            presets->setCurrentIndex(index); const auto hex = presets->itemData(index).toString();
            VERIFY(ending->text() == hex && hint->isHidden()); ProtocolProfile output; VERIFY(editor.profile(output, error));
            VERIFY(output.definition["frame"].toObject()["terminatorHex"].toString() == hex);
            QByteArray sample; VERIFY(ProtocolSampleGenerator::generate(output, sample, error) && sample.endsWith(QByteArray::fromHex(hex.toLatin1())));
            // Every preset handles split delimiters, half packets and glued
            // frames identically through the same live parser implementation.
            for (int chunk : {1, 3, sample.size() * 2}) {
                std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(output, nullptr, error))); VERIFY(parser);
                const auto input = sample + sample; QByteArray buffered;
                for (int pos = 0; pos < input.size(); pos += chunk) { buffered += input.mid(pos, chunk); const int consumed = parser->processData(buffered.constData(), buffered.size()); VERIFY(consumed >= 0 && consumed <= buffered.size()); buffered.remove(0, consumed); }
                VERIFY(parser->stats().validFrames == 2 && parser->stats().rejectedFrames == 0 && buffered.isEmpty());
            }
        }
        presets->setCurrentIndex(presets->findData("custom")); const auto unchanged = ending->text();
        VERIFY(!unchanged.isEmpty()); ending->setText("aa 55"); // Programmatic import/load is normalized when persisted too.
        VERIFY(presets->currentData().toString() == "custom" && hint->isHidden());
        ProtocolProfile custom; VERIFY(editor.profile(custom, error) && custom.definition["frame"].toObject()["terminatorHex"] == QJsonValue("AA55"));
        editor.setProfile(custom); VERIFY(ending->text() == "AA55" && presets->currentData().toString() == "custom");
        ending->clear(); ending->insert("0d 0a"); VERIFY(ending->text() == "0D 0A" && presets->currentData().toString() == "0D0A" && hint->isHidden());
        ending->clear(); ending->insert("D"); VERIFY(presets->currentData().toString() == "custom" && !hint->isHidden() && hint->text().contains("两位"));
        ending->setText(QString(34, QChar('A'))); VERIFY(!editor.profile(custom, error) && !hint->isHidden() && hint->text().contains("16 字节"));
        ending->clear(); VERIFY(!editor.profile(custom, error) && hint->text().contains("必须有结束符"));
    }
    editor.setProfile(fixed);
    algorithm->setCurrentIndex(algorithm->findData("none")); QCoreApplication::processEvents();
    auto sizeField = editor.findChild<QSpinBox *>("protocol_sizeBytes"), payloadField = editor.findChild<QSpinBox *>("protocol_payloadOffset");
    VERIFY(frameSummary->text().contains("总帧长：11 字节") && !frameSummary->text().contains("校验（"));
    sizeField->setValue(12); VERIFY(frameSummary->text().contains("总帧长：12 字节") && frameSummary->text().contains("其他/保留（2）"));
    sizeField->setValue(11);
    const int frameGap = payloadField->mapTo(&editor, QPoint()).y() - sizeField->mapTo(&editor, QPoint()).y() - sizeField->height();
    VERIFY(frameGap >= 6 && frameGap <= 12);
    auto note = editor.findChild<QLabel *>("protocolBinaryNote");
    auto checksumRule = editor.findChild<QLabel *>("protocolChecksumRule"); VERIFY(checksumRule && checksumRule->text().contains("帧起点 0"));
    const int checksumGap = note->mapTo(&editor, QPoint()).y() - checksumRule->mapTo(&editor, QPoint()).y() - checksumRule->height();
    VERIFY(checksumGap >= 6 && checksumGap <= 12);
    auto binaryForm = qobject_cast<QFormLayout *>(sizeField->parentWidget()->layout()); VERIFY(binaryForm && binaryForm->rowCount() == 8);
    VERIFY(!binaryForm->labelForField(note) && !binaryForm->labelForField(checksumRule));
    const int fixedHeight = editor.sizeHint().height();
    editorType->setCurrentIndex(2); QCoreApplication::processEvents();
    VERIFY(!editor.findChild<QWidget *>("protocol_lengthOffset")->isHidden() && sizeField->isHidden());
    VERIFY(binaryForm->rowCount() == 14); // Explanations and conditions live outside the dynamic form.
    const int variableHeight = editor.sizeHint().height();
    editorType->setCurrentIndex(1); QCoreApplication::processEvents(); VERIFY(editor.sizeHint().height() == fixedHeight);
    editorType->setCurrentIndex(0); QCoreApplication::processEvents();
    VERIFY(headersFit(fieldsTable));
    auto frames = editor.findChild<QWidget *>("protocolFrames");
    VERIFY(frames && !editor.findChild<QWidget *>("protocol_prefix")->parentWidget()->isHidden());
    VERIFY(!editor.findChild<QWidget *>("protocol_sizeBytes")->parentWidget()->isVisible());
    VERIFY(editor.sizeHint().height() < variableHeight);
    // A tall viewport must leave spare height below the entire configuration,
    // never redistribute it between rows of any form or protocol page.
    {
        QScrollArea tall; tall.setWidgetResizable(true); tall.resize(1200, 2200);
        auto compact = new ProtocolEditorWidget; tall.setWidget(compact); tall.show();
        for (const auto &fixture : {ProtocolEditorWidget::example(false), ProtocolEditorWidget::example(true), ProtocolEditorWidget::lengthExample()}) {
            ProtocolProfile source; VERIFY(ProtocolProfileValidator::profile(fixture, source, error)); compact->setProfile(source); QApplication::processEvents();
            auto name = compact->findChild<QLineEdit *>("protocolName"); auto type = compact->findChild<QComboBox *>("protocolType"); auto description = compact->findChild<QPlainTextEdit *>("protocolDescription");
            auto gap = [compact](QWidget *a, QWidget *b) { return b->mapTo(compact, QPoint()).y() - a->mapTo(compact, QPoint()).y() - a->height(); };
            VERIFY(name->mapTo(compact, QPoint()).y() <= 12);
            VERIFY(gap(name, type) >= 6 && gap(name, type) <= 12);
            VERIFY(gap(type, description) >= 6 && gap(type, description) <= 12);
            const auto keys = source.definition.value("type") == QJsonValue("text") ? QStringList{"prefix", "suffix", "delimiterMode", "delimiter", "terminatorHex"} : QStringList{"byteOrder", "headerHex", "tailHex"};
            for (int i = 1; i < keys.size(); ++i) {
                const int spacing = gap(compact->findChild<QWidget *>("protocol_" + keys[i - 1]), compact->findChild<QWidget *>("protocol_" + keys[i]));
                VERIFY(spacing >= 6 && spacing <= 12);
            }
            if (argc > 1) {
                VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1])));
                VERIFY(tall.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-tall-" + compact->findChild<QComboBox *>("protocolType")->currentData().toString() + ".png"));
            }
        }
    }
    // Linked data-region edits are bidirectional, bounded, and reversible.
    {
        auto definition = fixed.definition; definition.insert("autoChannelLayout", true);
        definition.insert("checksum", QJsonObject{{"algorithm", "none"}});
        auto frame = definition.value("frame").toObject(); frame.insert("sizeBytes", 10); definition.insert("frame", frame);
        ProtocolProfile linked, output; VERIFY(ProtocolProfileValidator::profile(definition, linked, error));
        editor.setProfile(linked);
        auto payload = editor.findChild<QSpinBox *>("protocol_payloadBytes"); auto total = editor.findChild<QSpinBox *>("protocol_sizeBytes");
        auto automatic = editor.findChild<QCheckBox *>("protocol_autoChannelLayout");
        VERIFY(payload->value() == 6 && total->value() == 10 && !(fieldsTable->item(0, 3)->flags() & Qt::ItemIsEditable));
        editor.findChild<QPushButton *>("protocolAddField")->click();
        VERIFY(fieldsTable->rowCount() == 3 && fieldsTable->item(2, 3)->text() == "6" && payload->value() == 10 && total->value() == 14);
        fieldsTable->item(0, 4)->setText("float64");
        VERIFY(fieldsTable->item(1, 3)->text() == "8" && fieldsTable->item(2, 3)->text() == "12" && payload->value() == 16 && total->value() == 20);
        payload->setValue(20); VERIFY(fieldsTable->rowCount() == 4 && fieldsTable->item(3, 3)->text() == "16" && fieldsTable->item(3, 4)->text() == "float32");
        VERIFY(fieldsTable->item(3, 6)->text() == "1" && fieldsTable->item(3, 5)->text().isEmpty() && fieldsTable->item(3, 2)->text().isEmpty());
        payload->setValue(22); VERIFY(fieldsTable->rowCount() == 4 && total->value() == 26 && editor.profile(output, error));
        VERIFY(editor.findChild<QLabel *>("protocolDataRegionHint")->text().contains("2 字节未配置/保留"));
        VERIFY(editor.profile(output, error)); const auto beforeShrink = output.definition; bool shrinkPrompt = false;
        QTimer::singleShot(0, [&] { auto dialog = editor.findChild<QDialog *>("protocolDataRegionConfirm"); shrinkPrompt = dialog && dialog->width() >= 540; if (dialog) dialog->reject(); });
        payload->setValue(12); VERIFY(shrinkPrompt && editor.profile(output, error) && output.definition == beforeShrink);
        QTimer::singleShot(0, [&] { auto dialog = editor.findChild<QDialog *>("protocolDataRegionConfirm"); shrinkPrompt = dialog != nullptr; if (dialog) dialog->accept(); });
        payload->setValue(12); VERIFY(shrinkPrompt && fieldsTable->rowCount() == 2 && payload->value() == 12 && total->value() == 16 && editor.profile(output, error));
        fieldsTable->selectRow(1); editor.findChild<QPushButton *>("protocolFieldUp")->click();
        VERIFY(fieldsTable->item(0, 4)->text() == "float32" && fieldsTable->item(1, 3)->text() == "4" && payload->value() == 12);
        const QString retainedId = fieldsTable->item(0, 0)->text();
        QTimer::singleShot(0, [&] { if (auto dialog = editor.findChild<QDialog *>("protocolDataRegionConfirm")) dialog->accept(); });
        payload->setValue(4); VERIFY(fieldsTable->rowCount() == 1 && fieldsTable->item(0, 0)->text() == retainedId && total->value() == 8);
        payload->setValue(2); VERIFY(payload->value() == 4 && fieldsTable->rowCount() == 1); // Cannot discard the last complete field.
        automatic->setChecked(false); payload->setValue(24); VERIFY(fieldsTable->rowCount() == 1 && total->value() == 8 && fieldsTable->item(0, 3)->flags().testFlag(Qt::ItemIsEditable));
        total->setValue(28); automatic->setChecked(true); VERIFY(fieldsTable->rowCount() == 6 && payload->value() == 24 && total->value() == 28 && editor.profile(output, error));
        payload->setValue(1032); VERIFY(fieldsTable->rowCount() == 256 && !editor.profile(output, error) && error.contains("256"));
        editor.setProfile(fixed); automatic->setChecked(true); // Also move a fixed checksum/range along with the data region.
        editor.findChild<QPushButton *>("protocolAddField")->click();
        VERIFY(payload->value() == 10 && total->value() == 15 && editor.findChild<QSpinBox *>("protocol_checkOffset")->value() == 3 && editor.findChild<QSpinBox *>("protocol_checkLength")->value() == 12 && editor.profile(output, error));
        const auto body = QByteArray::fromHex("E6093333534000002040");
        auto packet = QByteArray::fromHex("AA55") + body; packet.append(char(ProtocolChecksum(output.definition.value("checksum").toObject().value("algorithm").toString()).calculate(packet.constData(), packet.size()))); packet += QByteArray::fromHex("0D0A");
        std::unique_ptr<ProtocolParser> parser(ProtocolParserFactory::createCustom(output, nullptr, error)); VERIFY(parser);
        QVector<double> values; QObject::connect(parser.get(), &ProtocolParser::channelsUpdated, [&](const QVector<double> &received) { values = received; });
        VERIFY(parser->processData(packet.constData(), packet.size()) == packet.size() && values.size() == 3);
        VERIFY(std::abs(values[0] - 25.34) < 1e-10 && std::abs(values[1] - 3.3) < 1e-5 && values[2] == 2.5);
        // End-relative CRC and length formula continue working after expansion.
        auto variableDefinition = ProtocolEditorWidget::lengthExample(); variableDefinition.insert("autoChannelLayout", true);
        ProtocolProfile variableLinked; VERIFY(ProtocolProfileValidator::profile(variableDefinition, variableLinked, error)); editor.setProfile(variableLinked);
        payload->setValue(10); VERIFY(fieldsTable->rowCount() == 3 && editor.findChild<QSpinBox *>("protocol_minFrameBytes")->value() == 16);
        VERIFY(editor.findChild<QSpinBox *>("protocol_lengthAdjustment")->value() == 6 && editor.findChild<QSpinBox *>("protocol_checkOffset")->value() == 3 && editor.profile(output, error));
        packet = QByteArray::fromHex("AA550A") + body; packet.append(char(ProtocolChecksum("crc8-smbus").calculate(packet.constData(), packet.size()))); packet += QByteArray::fromHex("0D0A");
        parser.reset(ProtocolParserFactory::createCustom(output, nullptr, error)); VERIFY(parser); values.clear();
        QObject::connect(parser.get(), &ProtocolParser::channelsUpdated, [&](const QVector<double> &received) { values = received; });
        VERIFY(parser->processData(packet.constData(), packet.size()) == packet.size() && values.size() == 3 && values[2] == 2.5);
        editor.setProfile(fixed); VERIFY(!automatic->isChecked());
    }
    {
        bool helpChecked = false;
        struct ClipboardRestore {
            QMimeData *saved = new QMimeData;
            ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
            ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
        } restore;
        QTimer::singleShot(0, [&] {
            auto dialog = editor.findChild<QDialog *>("protocolChecksumHelpDialog"); if (!dialog) return;
            auto list = dialog->findChild<QListWidget *>("protocolChecksumHelpList"); auto body = dialog->findChild<QTextBrowser *>("protocolChecksumHelpText");
            helpChecked = dialog->windowFlags().testFlag(Qt::WindowMinimizeButtonHint) && dialog->windowFlags().testFlag(Qt::WindowMaximizeButtonHint)
                && dialog->isSizeGripEnabled() && list && list->count() == 8 && body;
            if (helpChecked) for (int i = 0; i < list->count(); ++i) {
                list->setCurrentRow(i); const auto id = list->currentItem()->data(Qt::UserRole).toString();
                helpChecked = helpChecked && body->toPlainText().contains("123456789") && body->toPlainText().contains("uint32_t wavelink_");
                dialog->findChild<QPushButton *>("protocolChecksumCopyCode")->click(); helpChecked = helpChecked && QApplication::clipboard()->text() == ProtocolChecksum::cExample(id);
            }
            if (argc > 1) { list->setCurrentRow(3); QApplication::processEvents(); dialog->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-checksum-help.png"); }
            dialog->accept();
        });
        editor.findChild<QPushButton *>("protocolChecksumHelp")->click(); VERIFY(helpChecked);
    }
    {
        // Converting byte offsets/array indices to text columns is a semantic
        // conversion, not just a header rename. Check all three binary forms.
        ProtocolEditorWidget converted;
        auto type = converted.findChild<QComboBox *>("protocolType");
        auto fields = converted.findChild<QTableWidget *>("protocolFields");
        auto add = converted.findChild<QPushButton *>("protocolAddField");
        auto remove = converted.findChild<QPushButton *>("protocolDeleteField");
        auto consecutive = [&] { for (int row = 0; row < fields->rowCount(); ++row) if (fields->item(row, 3)->text() != QString::number(row)) return false; return true; };
        for (auto fixture : {ProtocolEditorWidget::example(true), ProtocolEditorWidget::lengthExample(), ProtocolEditorWidget::dynamicArrayExample()}) {
            ProtocolProfile source; VERIFY(ProtocolProfileValidator::profile(fixture, source, error)); converted.setProfile(source);
            while (fields->rowCount() < 3) add->click();
            if (!source.isDynamicArray()) fields->item(0, 4)->setText("float64");
            fields->item(2, 3)->setText("17");
            QStringList fieldIds, fieldNames, scales;
            for (int row = 0; row < 3; ++row) { fieldIds << fields->item(row, 0)->text(); fieldNames << fields->item(row, 1)->text(); scales << fields->item(row, 6)->text(); }
            type->setCurrentIndex(0); VERIFY(consecutive() && fields->rowCount() == 3 && fields->horizontalHeaderItem(3)->toolTip().contains("0 对应第 1 列"));
            ProtocolProfile output; VERIFY(converted.profile(output, error));
            for (int row = 0; row < 3; ++row) {
                VERIFY(fields->item(row, 0)->text() == fieldIds[row] && fields->item(row, 1)->text() == fieldNames[row] && fields->item(row, 6)->text() == scales[row]);
                VERIFY(output.definition["channels"].toArray()[row].toObject()["column"].toInt(-1) == row);
            }
            QByteArray sample; VERIFY(ProtocolSampleGenerator::generate(output, sample, error));
            std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(output, nullptr, error))); VERIFY(parser);
            int count = 0; QObject::connect(parser.get(), &ProtocolParser::channelsUpdated, [&](const QVector<double> &values) { count = values.size(); });
            parser->processData(sample.constData(), sample.size()); VERIFY(parser->stats().validFrames == 1 && count == 3);
            add->click(); VERIFY(fields->rowCount() == 4 && consecutive());
            fields->selectRow(1); remove->click(); VERIFY(fields->rowCount() == 3 && consecutive());
            fields->selectRow(2); converted.findChild<QPushButton *>("protocolFieldUp")->click(); VERIFY(consecutive());
            fields->selectRow(0); converted.findChild<QPushButton *>("protocolFieldDown")->click(); VERIFY(consecutive());
            VERIFY(converted.profile(output, error)); converted.setProfile(output); VERIFY(consecutive());
            type->setCurrentIndex(1); int byteOffset = 0;
            for (int row = 0; row < fields->rowCount(); ++row) { VERIFY(fields->item(row, 3)->text() == QString::number(byteOffset)); byteOffset += ProtocolProfileValidator::typeBytes(fields->item(row, 4)->text()); }
            type->setCurrentIndex(0); VERIFY(consecutive());
            while (fields->rowCount()) { fields->selectRow(0); remove->click(); VERIFY(consecutive()); }
            VERIFY(converted.profile(output, error) && output.definition["channels"].toArray().isEmpty());
            add->click(); VERIFY(fields->rowCount() == 1 && fields->item(0, 3)->text() == "0");
        }
        auto sparse = ProtocolEditorWidget::example(false); auto channels = sparse["channels"].toArray(); auto second = channels[1].toObject(); second.insert("column", 2); channels[1] = second; sparse.insert("channels", channels);
        ProtocolProfile source; VERIFY(ProtocolProfileValidator::profile(sparse, source, error)); converted.setProfile(source);
        VERIFY(fields->item(1, 3)->text() == "2"); add->click(); VERIFY(fields->item(2, 3)->text() == "1"); // Never duplicates column 2.
        fields->selectRow(0); remove->click(); VERIFY(fields->item(0, 3)->text() == "2" && fields->item(1, 3)->text() == "1");
        fields->selectRow(1); converted.findChild<QPushButton *>("protocolFieldUp")->click(); VERIFY(fields->item(0, 3)->text() == "1" && fields->item(1, 3)->text() == "2");
        ProtocolProfile output; VERIFY(converted.profile(output, error)); converted.setProfile(output); add->click(); VERIFY(fields->item(2, 3)->text() == "0");
        // Explicitly editing a previously continuous mapping opts out too.
        VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(false), source, error)); converted.setProfile(source);
        fields->item(1, 3)->setText("2"); add->click(); VERIFY(fields->item(2, 3)->text() == "1");
    }
    {
        const auto selectedPath = temporary.filePath("selected-on-open.json"); ProtocolProfileStore store(selectedPath); VERIFY(store.load(error));
        ProtocolProfile text, binary, variable;
        VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(false), text, error) && ProtocolProfileValidator::profile(ProtocolEditorWidget::example(true), binary, error)
            && ProtocolProfileValidator::profile(ProtocolEditorWidget::lengthExample(), variable, error));
        ProtocolSnapshot snapshot; snapshot.profiles = {text, binary, variable}; snapshot.shared.kind = "custom"; snapshot.shared.id = binary.id; VERIFY(store.commit(snapshot, error));
        const auto before = ProtocolProfileValidator::encode(store.snapshot(), true, error);
        {
            ProtocolManagerDialog selected(selectedPath); auto list = selected.findChild<QListWidget *>("protocolList");
            VERIFY(list->currentRow() == 1 && list->currentItem()->data(Qt::UserRole).toString() == binary.id);
            VERIFY(selected.findChild<QLineEdit *>("protocolName")->text() == binary.name && selected.findChild<QComboBox *>("protocolPreviewProtocol")->currentData().toString() == binary.id);
            VERIFY(selected.findChild<QComboBox *>("protocolInputMode")->currentIndex() == 1);
            list->setCurrentRow(0); selected.close();
        }
        VERIFY(store.load(error) && ProtocolProfileValidator::encode(store.snapshot(), true, error) == before); // Browsing never applies another parser.
        ProtocolManagerDialog preferred(selectedPath, nullptr, variable.id);
        VERIFY(preferred.findChild<QListWidget *>("protocolList")->currentRow() == 2 && preferred.findChild<QComboBox *>("protocolPreviewProtocol")->currentData().toString() == variable.id);
        ProtocolManagerDialog missing(selectedPath, nullptr, "missing-id"); VERIFY(missing.findChild<QListWidget *>("protocolList")->currentRow() == 0);
        ProtocolManagerDialog empty(temporary.filePath("empty-on-open.json"), nullptr, binary.id); VERIFY(empty.findChild<QListWidget *>("protocolList")->count() == 0 && !empty.findChild<ProtocolEditorWidget *>()->isEnabled());
    }
    {
        const auto welcomePath = temporary.filePath("welcome.json"); ProtocolManagerDialog welcome(welcomePath); welcome.show(); QCoreApplication::processEvents();
        auto page = welcome.findChild<QWidget *>("protocolWelcome"); auto save = welcome.findChild<QPushButton *>("protocolSave");
        auto list = welcome.findChild<QListWidget *>("protocolList"); auto editor = welcome.findChild<ProtocolEditorWidget *>();
        VERIFY(page && page->isVisible() && editor->isHidden() && !save->isEnabled() && list->count() == 0 && !QFile::exists(welcomePath));
        VERIFY(welcome.findChild<QLabel *>("protocolWelcomeText")->text().contains("新增协议") && welcome.findChild<QPushButton *>("protocolWelcomeImport")->isEnabled());
        welcome.resize(1200, 800); QCoreApplication::processEvents();
        if (argc > 1) welcome.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-welcome.png");
        VERIFY(createProtocol(welcome, 0, false, "protocolWelcomeNew") && page->isVisible() && list->count() == 0); // Cancel keeps first-run state.
        bool importOffered = false; QTimer::singleShot(0, &welcome, [&] {
            for (auto widget : QApplication::topLevelWidgets()) if (auto picker = qobject_cast<QFileDialog *>(widget)) { importOffered = true; picker->reject(); }
        });
        welcome.findChild<QPushButton *>("protocolWelcomeImport")->click(); VERIFY(importOffered && page->isVisible() && list->count() == 0);
        VERIFY(createProtocol(welcome, 0, true, "protocolWelcomeNew") && page->isHidden() && editor->isVisible() && save->isEnabled() && list->count() == 1);
        auto presets = welcome.findChild<QComboBox *>("protocolTerminatorPreset"); VERIFY(presets->currentData().toString() == "0D0A");
        presets->setCurrentIndex(presets->findData("0A")); save->click();
        ProtocolProfileStore persisted(welcomePath); VERIFY(persisted.load(error) && persisted.snapshot().profiles.size() == 1 && persisted.snapshot().profiles[0].definition["frame"].toObject()["terminatorHex"] == QJsonValue("0A"));
        { ProtocolManagerDialog restored(welcomePath); VERIFY(restored.findChild<QWidget *>("protocolWelcome")->isHidden() && restored.findChild<QComboBox *>("protocolTerminatorPreset")->currentData().toString() == "0A"); }
        QByteArray beforeInvalid; VERIFY(ProtocolProfileStore::readFile(welcomePath, beforeInvalid, error));
        welcome.findChild<QLineEdit *>("protocol_terminatorHex")->clear(); bool saveWarned = false, testWarned = false;
        QTimer::singleShot(0, &welcome, [&] { if (auto info = welcome.findChild<QDialog *>("protocolInfoDialog")) { saveWarned = info->windowTitle() == "保存失败" && info->findChild<QPlainTextEdit *>("protocolInfoText")->toPlainText().contains("请选择或输入结束符"); info->accept(); } });
        save->click(); VERIFY(saveWarned);
        QTimer::singleShot(0, &welcome, [&] { if (auto info = welcome.findChild<QDialog *>("protocolInfoDialog")) { testWarned = info->windowTitle() == "无法解析测试" && info->findChild<QPlainTextEdit *>("protocolInfoText")->toPlainText().contains("请选择或输入结束符"); info->accept(); } });
        welcome.findChild<QPushButton *>("protocolTest")->click(); VERIFY(testWarned);
        QByteArray afterInvalid; VERIFY(ProtocolProfileStore::readFile(welcomePath, afterInvalid, error) && afterInvalid == beforeInvalid);
        presets->setCurrentIndex(presets->findData("0A")); save->click();
        QTimer::singleShot(0, &welcome, [&] { welcome.findChild<QDialog *>("protocolConfirm")->findChild<QPushButton *>("protocolDecision3")->click(); });
        welcome.findChild<QPushButton *>("protocolDelete")->click(); VERIFY(page->isVisible() && editor->isHidden() && !save->isEnabled() && list->count() == 0);
        welcome.close(); ProtocolManagerDialog reopened(welcomePath); VERIFY(!reopened.findChild<QWidget *>("protocolWelcome")->isHidden() && !reopened.findChild<QPushButton *>("protocolSave")->isEnabled());
        const auto damagedPath = temporary.filePath("welcome-damaged.json"); VERIFY(ProtocolProfileStore::writeFile(damagedPath, "{broken}", error));
        ProtocolManagerDialog damaged(damagedPath); VERIFY(damaged.findChild<QLabel *>("protocolWelcomeTitle")->text() == "协议库暂不可用"
            && !damaged.findChild<QPushButton *>("protocolWelcomeNew")->isEnabled() && !damaged.findChild<QPushButton *>("protocolWelcomeImport")->isEnabled());
        QByteArray preserved; VERIFY(ProtocolProfileStore::readFile(damagedPath, preserved, error) && preserved == "{broken}");
    }
    editor.hide();
    const auto path = temporary.filePath("protocols.json");
    ProtocolManagerDialog dialog(path); dialog.show(); QCoreApplication::processEvents();
    VERIFY(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint) && dialog.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    auto create = dialog.findChild<QPushButton *>("protocolNew"), save = dialog.findChild<QPushButton *>("protocolSave"), test = dialog.findChild<QPushButton *>("protocolTest");
    auto list = dialog.findChild<QListWidget *>("protocolList"); auto results = dialog.findChild<QTableWidget *>("protocolResults");
    VERIFY(create && save && test && list && results);
    auto generateAction = dialog.findChild<QPushButton *>("protocolGenerateSample"); VERIFY(generateAction);
    for (auto action : {generateAction, test}) {
        VERIFY(action->styleSheet().contains("background-color") && action->styleSheet().contains(":hover")
            && action->styleSheet().contains(":pressed") && action->styleSheet().contains(":disabled"));
    }
    VERIFY(generateAction->styleSheet().contains("#176ea6") && test->styleSheet().contains("#187545"));
    VERIFY(save->text() == "保存协议" && save->styleSheet().contains("background-color"));
    VERIFY(!dialog.findChild<QPushButton *>("protocolApply"));
    VERIFY(dialog.findChild<QShortcut *>("protocolSaveShortcut")->key() == QKeySequence::Save);
    auto columns = dialog.findChild<QSplitter *>("protocolColumns");
    auto leftSplit = dialog.findChild<QSplitter *>("protocolLeftSplit");
    auto editorScroll = dialog.findChild<QScrollArea *>("protocolEditorScroll");
    auto previewScroll = dialog.findChild<QScrollArea *>("protocolPreviewScroll");
    auto managementScroll = dialog.findChild<QScrollArea *>("protocolManagementScroll");
    VERIFY(columns && columns->count() == 3 && columns->orientation() == Qt::Horizontal);
    VERIFY(leftSplit && leftSplit->count() == 2 && leftSplit->orientation() == Qt::Vertical);
    VERIFY(editorScroll->mapToGlobal(QPoint()).x() > list->mapToGlobal(QPoint()).x());
    VERIFY(previewScroll->mapToGlobal(QPoint()).x() > editorScroll->mapToGlobal(QPoint()).x());
    VERIFY(managementScroll->mapToGlobal(QPoint()).y() > list->mapToGlobal(QPoint()).y());
    VERIFY(list->styleSheet().contains("::item:selected") && list->styleSheet().contains("#00D26A"));
    VERIFY(managementScroll->widget()->findChildren<QPushButton *>().size() == 6);
    VERIFY(dialog.findChild<QPushButton *>("protocolRecoverBackup")->isHidden());
    auto previewProtocol = dialog.findChild<QComboBox *>("protocolPreviewProtocol"); VERIFY(previewProtocol && previewProtocol->count() == 0 && !test->isEnabled());
    for (auto key : {"protocolPreviewProtocol", "protocolInputMode", "protocolChunkMode"}) {
        auto field = dialog.findChild<QComboBox *>(key); VERIFY(field && !field->toolTip().isEmpty());
        auto label = dialog.findChild<QLabel *>(QString(key) + "Label"); VERIFY(label && !label->toolTip().isEmpty());
    }
    auto previewForm = qobject_cast<QFormLayout *>(previewProtocol->parentWidget()->layout()->itemAt(1)->layout());
    int previewRow; previewForm->getWidgetPosition(previewProtocol, &previewRow, &role); VERIFY(previewRow == 0);
    VERIFY(!dialog.findChild<QPushButton *>("protocolLoadSample"));
    for (auto key : {"protocolSample", "protocolGenerateSample", "protocolTest", "protocolCancelTest", "protocolStats", "protocolDiagnostics"})
        VERIFY(!dialog.findChild<QWidget *>(key)->toolTip().isEmpty());
    for (int col = 0; col < results->columnCount(); ++col) VERIFY(!results->horizontalHeaderItem(col)->toolTip().isEmpty());
    VERIFY(createProtocol(dialog, 1, false) && list->count() == 0);
    VERIFY(createProtocol(dialog, 1)); VERIFY(list->count() == 1 && list->currentRow() == 0 && list->currentItem()->isSelected());
    const QString draftId = list->currentItem()->data(Qt::UserRole).toString(); VERIFY(!draftId.isEmpty());
    VERIFY(previewProtocol->currentData().toString() == draftId && previewProtocol->currentText().contains("当前编辑"));
    auto draftFields = dialog.findChild<QTableWidget *>("protocolFields"); VERIFY(draftFields->item(0, 1)->text() == "Ch1" && draftFields->item(1, 1)->text() == "Ch2");
    VERIFY(dialog.findChild<QComboBox *>("protocol_algorithm")->currentData() == QJsonValue("none").toVariant());
    VERIFY(draftFields->item(0, 6)->text() == "1" && draftFields->item(1, 6)->text() == "1");
    VERIFY(dialog.findChild<QComboBox *>("protocol_outOfRangePolicy")->currentData().toString() == "nan-channel");
    VERIFY(dialog.findChild<QCheckBox *>("protocol_autoChannelLayout")->isChecked());
    ProtocolProfileStore unsavedStore(path); VERIFY(unsavedStore.load(error) && unsavedStore.snapshot().profiles.isEmpty());
    auto liveName = dialog.findChild<QLineEdit *>("protocolName"); const QString initialDraftName = liveName->text();
    VERIFY(initialDraftName == "二进制固定长度帧协议");
    liveName->setText("正在编辑的新协议"); VERIFY(list->currentItem()->text() == liveName->text() && previewProtocol->currentText().startsWith(liveName->text()));
    liveName->clear(); VERIFY(list->currentItem()->text() == "未命名协议");
    liveName->setText(initialDraftName);
    VERIFY(dialog.findChild<QSpinBox *>("protocol_checkStart")->value() == 0 && dialog.findChild<QSpinBox *>("protocol_checkLength")->value() == 8);
    VERIFY(dialog.findChild<QPushButton *>("protocolCancelTest")->isHidden());
    VERIFY(dialog.findChild<QComboBox *>("protocolInputMode")->currentIndex() == 1);
    // Precision checks use an explicit device sample, not the generated 1/2.
    dialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText("AA 55 E6 09 33 33 53 40 0D 0A");
    test->click(); VERIFY(!previewProtocol->isEnabled() && !dialog.findChild<QPushButton *>("protocolGenerateSample")->isEnabled() && dialog.findChild<QPushButton *>("protocolCancelTest")->isEnabled());
    VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 2 && results->item(0, 1)->text() == "Ch1");
    VERIFY(results->item(1, 3)->text() == "3.3" && results->item(1, 4)->text() == "3.3");
    VERIFY(results->item(1, 3)->toolTip().contains("3.299999952316284") && results->item(1, 4)->toolTip().contains("float32"));
    if (argc > 1) {
        VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); dialog.resize(1440, 860); QCoreApplication::processEvents();
        VERIFY(dialog.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-binary-float32.png"));
    }
    VERIFY(unsavedStore.load(error) && unsavedStore.snapshot().profiles.isEmpty());
    save->click(); VERIFY(list->count() == 1 && list->currentItem()->data(Qt::UserRole) == draftId);
    ProtocolProfileStore check(path); VERIFY(check.load(error) && check.snapshot().profiles.size() == 1);
    test->click(); VERIFY(until([&] { return test->isEnabled(); }));
    VERIFY(results->rowCount() == 2 && results->item(0, 4)->text() == "2534");
    auto modes = dialog.findChild<QComboBox *>("protocolChunkMode");
    for (int mode : {1, 2}) { modes->setCurrentIndex(mode); test->click(); VERIFY(until([&] { return test->isEnabled(); })); VERIFY(results->rowCount() == 2 && results->item(1, 3)->text() == "3.3" && results->item(1, 4)->text() == "3.3"); }
    VERIFY(chooseSidebarAction(dialog, list, 0, "protocolListMenu", "protocolMenuCopy")); VERIFY(list->count() == 2);
    VERIFY(createProtocol(dialog, 0)); VERIFY(list->count() == 3 && list->currentRow() == 2 && liveName->text() == "文本分隔协议");
    VERIFY(dialog.findChild<QComboBox *>("protocolInputMode")->currentIndex() == 0);
    dialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText("DATA:25.34,3.30\\r\\n");
    save->click(); VERIFY(list->count() == 3); test->click(); VERIFY(until([&] { return test->isEnabled(); })); VERIFY(results->rowCount() == 2);
    VERIFY(results->item(1, 3)->text() == "3.3" && results->item(1, 4)->text() == "3.3");
    VERIFY(results->item(1, 3)->toolTip().contains("完整精度"));
    // Testing a different saved protocol must leave the current invalid draft,
    // editor selection and persisted library untouched; its sample follows
    // the selected test protocol, rather than keeping an unrelated old frame.
    const QString textId = list->currentItem()->data(Qt::UserRole).toString();
    VERIFY(check.load(error)); const auto beforePreview = ProtocolProfileValidator::encode(check.snapshot(), true, error);
    previewProtocol->setCurrentIndex(previewProtocol->findData(draftId));
    dialog.findChild<QComboBox *>("protocolInputMode")->setCurrentIndex(1);
    const QString binarySample = "AA 55 E6 09 33 33 53 40 0D 0A";
    dialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText(binarySample);
    auto textDelimiter = dialog.findChild<QLineEdit *>("protocol_delimiter"); textDelimiter->clear(); liveName->setText("未保存的文本草稿");
    VERIFY(previewProtocol->currentData().toString() == draftId && list->currentItem()->data(Qt::UserRole).toString() == textId);
    test->click(); VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 2 && results->item(0, 4)->text() == "2534");
    VERIFY(results->item(1, 3)->text() == "3.3" && results->item(1, 4)->text() == "3.3"); // Other saved test selection uses its own field type.
    VERIFY(dialog.findChild<QLabel *>("protocolStats")->text().contains("测试协议：" + initialDraftName));
    VERIFY(liveName->text() == "未保存的文本草稿" && textDelimiter->text().isEmpty());
    VERIFY(check.load(error) && ProtocolProfileValidator::encode(check.snapshot(), true, error) == beforePreview);
    previewProtocol->setCurrentIndex(previewProtocol->findData(textId));
    VERIFY(results->rowCount() == 0 && dialog.findChild<QPlainTextEdit *>("protocolSample")->toPlainText().isEmpty()
        && dialog.findChild<QComboBox *>("protocolInputMode")->currentIndex() == 0);
    bool invalidPreviewWarned = false;
    QTimer::singleShot(0, &dialog, [&] { if (auto info = dialog.findChild<QDialog *>("protocolInfoDialog")) { invalidPreviewWarned = info->windowTitle() == "无法解析测试"; info->accept(); } });
    test->click(); VERIFY(invalidPreviewWarned);
    textDelimiter->setText(","); dialog.findChild<QComboBox *>("protocolInputMode")->setCurrentIndex(0);
    dialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText("DATA:25.34,3.30\\r\\n");
    test->click(); VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 2 && dialog.findChild<QLabel *>("protocolStats")->text().contains("未保存的文本草稿"));
    VERIFY(check.load(error) && ProtocolProfileValidator::encode(check.snapshot(), true, error) == beforePreview);
    auto sampleInput = dialog.findChild<QPlainTextEdit *>("protocolSample");
    for (int mode : {0, 1, 2}) {
        modes->setCurrentIndex(mode); sampleInput->setPlainText("DATA:25.34,3.30,2.5,2.8\\r\\n");
        test->click(); VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 4);
        VERIFY(results->item(1, 3)->text() == "3.3" && results->item(1, 4)->text() == "3.3");
        VERIFY(results->item(2, 1)->text() == "Ch3" && results->item(2, 2)->text().isEmpty());
        VERIFY(results->item(2, 3)->text() == "2.5" && results->item(2, 4)->text() == "2.5");
        VERIFY(results->item(3, 1)->text() == "Ch4" && results->item(3, 3)->text() == "2.8");
        VERIFY(results->item(2, 5)->text() == "auto-column-2");
    }
    if (argc > 1) {
        VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); dialog.resize(1440, 860); QCoreApplication::processEvents();
        VERIFY(dialog.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-text-four-channels.png"));
    }
    sampleInput->setPlainText("DATA:0.12345678901234567,3.30\\r\\n");
    test->click(); VERIFY(until([&] { return test->isEnabled(); }) && results->rowCount() == 2);
    VERIFY(results->item(0, 3)->text().toDouble() == QString("0.12345678901234567").toDouble()); // Not a fixed decimal rounding.
    sampleInput->setPlainText("DATA:25.34,3.30\\r\\n");
    liveName->setText("中文重命名"); VERIFY(list->currentItem()->text() == "中文重命名");
    // Send the real Ctrl+S key event with focus still inside the editor.
    QApplication::setActiveWindow(&dialog); liveName->setFocus(); QCoreApplication::processEvents();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier), release(QEvent::KeyRelease, Qt::Key_S, Qt::ControlModifier);
    QApplication::sendEvent(liveName, &press); QApplication::sendEvent(liveName, &release);
    VERIFY(until([&] { return dialog.findChild<QLabel *>("protocolSaveStatus")->text().startsWith("协议已保存"); }));
    VERIFY(check.load(error) && check.snapshot().profiles.last().name == "中文重命名");
    // Cancel import and invalid documents do not mutate the file.
    auto before = ProtocolProfileValidator::encode(check.snapshot(), true, error);
    auto share = ProtocolProfileValidator::encode(check.snapshot(), false, error);
    VERIFY(dialog.importBytes(share, ProtocolImportMode::Append, false, error));
    VERIFY(!dialog.importBytes("{invalid", ProtocolImportMode::Append, true, error));
    VERIFY(check.load(error) && ProtocolProfileValidator::encode(check.snapshot(), true, error) == before);
    VERIFY(dialog.importBytes(share, ProtocolImportMode::Append, true, error) && list->count() == 6);
    VERIFY(check.load(error));
    VERIFY(check.exportProfiles(temporary.filePath("shared.json"), {}, error));
    QByteArray bytes; ProtocolSnapshot exported;
    VERIFY(ProtocolProfileStore::readFile(temporary.filePath("shared.json"), bytes, error) && ProtocolProfileValidator::decode(bytes, false, exported, error));
    VERIFY(exported.profiles.size() == 6);
    const auto afterImport = ProtocolProfileValidator::encode(check.snapshot(), true, error);
    // Switching/closing with an invalid draft must honor Cancel/Discard.
    const int current = list->currentRow();
    auto name = dialog.findChild<QLineEdit *>("protocolName"); const QString savedName = list->currentItem()->text(); name->setText("unsaved draft");
    VERIFY(list->item(current)->text() == "unsaved draft");
    QTimer::singleShot(0, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolConfirm") widget->findChild<QPushButton *>("protocolDecision0")->click(); });
    list->setCurrentRow(current == 0 ? 1 : 0); VERIFY(list->currentRow() == current && name->text() == "unsaved draft");
    QTimer::singleShot(0, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolConfirm") widget->findChild<QPushButton *>("protocolDecision2")->click(); });
    list->setCurrentRow(current == 0 ? 1 : 0); VERIFY(name->text() != "unsaved draft");
    VERIFY(list->item(current)->text() == savedName);
    VERIFY(check.load(error) && ProtocolProfileValidator::encode(check.snapshot(), true, error) == afterImport);
    list->setCurrentRow(2); // The saved text profile.
    dialog.findChild<QComboBox *>("protocolInputMode")->setCurrentIndex(0);
    dialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText(QString("DATA:1,2\\r\\n").repeated(2000));
    modes->setCurrentIndex(1); test->click();
    dialog.findChild<QPushButton *>("protocolCancelTest")->click(); VERIFY(test->isEnabled());
    test->click(); VERIFY(until([&] { return test->isEnabled(); }));
    VERIFY(results->rowCount() == 1000 && dialog.findChild<QLabel *>("protocolStats")->text().contains("有效 2000 帧"));
    dialog.resize(800, 620); QCoreApplication::processEvents();
    VERIFY(save->isVisible() && test->isVisible());
    if (argc > 1) { VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); VERIFY(dialog.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-manager-light.png")); }
    QPalette dark = app.palette(); dark.setColor(QPalette::Window, QColor("#1e1e1e")); dark.setColor(QPalette::Base, QColor("#252526")); dark.setColor(QPalette::Text, QColor("#dddddd")); dark.setColor(QPalette::WindowText, QColor("#dddddd")); app.setPalette(dark);
    QCoreApplication::processEvents(); VERIFY(dialog.findChild<QPlainTextEdit *>("protocolSample")->palette().color(QPalette::Base) == dark.color(QPalette::Base));
    if (argc > 1) VERIFY(dialog.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-manager-dark.png"));
    // Reopening restores the saved list; discard/cancel prompts are handled
    // explicitly in interactive use, not bypassed by a destructor auto-save.
    dialog.close(); ProtocolManagerDialog reopened(path); VERIFY(reopened.findChild<QListWidget *>("protocolList")->count() == 6);
    ProtocolManagerDialog advanced(temporary.filePath("length.json")); advanced.show(); QCoreApplication::processEvents();
    VERIFY(createProtocol(advanced, 2));
    VERIFY(advanced.findChild<QLineEdit *>("protocolName")->text() == "二进制长度字段帧协议");
    VERIFY(advanced.findChild<QComboBox *>("protocol_outOfRangePolicy")->currentData().toString() == "nan-channel");
    VERIFY(advanced.findChild<QCheckBox *>("protocol_autoFrameLayout")->isChecked());
    VERIFY(advanced.findChild<QTableWidget *>("protocolFields")->rowCount() == 0);
    VERIFY(advanced.findChild<QSpinBox *>("protocol_lengthAdjustment")->value() == 6);
    VERIFY(advanced.findChild<QComboBox *>("protocol_checkRangeMode")->currentData().toString() == "end" && advanced.findChild<QComboBox *>("protocol_checkFrom")->currentData().toString() == "end");
    auto advancedTest = advanced.findChild<QPushButton *>("protocolTest"); auto advancedResults = advanced.findChild<QTableWidget *>("protocolResults");
    advanced.findChild<QPushButton *>("protocolSave")->click();
    for (int mode : {0, 1, 2}) {
        advanced.findChild<QComboBox *>("protocolChunkMode")->setCurrentIndex(mode); advancedTest->click();
        VERIFY(until([&] { return advancedTest->isEnabled(); }) && advancedResults->rowCount() == 2);
        VERIFY(advancedResults->item(0, 4)->text() == "1" && advancedResults->item(0, 1)->text() == "Ch1");
        VERIFY(advancedResults->item(1, 3)->text() == "2" && advancedResults->item(1, 4)->text() == "2");
    }
    advanced.findChild<QPlainTextEdit *>("protocolSample")->setPlainText("AA 55 00 00 AA 55 08 00 00 00 80 3F 00 00 00 40 0D 0A"); advancedTest->click();
    VERIFY(until([&] { return advancedTest->isEnabled(); }) && advancedResults->rowCount() == 2);
    VERIFY(advanced.findChild<QPlainTextEdit *>("protocolDiagnostics")->toPlainText().contains("长度字段"));
    ProtocolProfileStore advancedStore(temporary.filePath("length.json")); VERIFY(advancedStore.load(error));
    VERIFY(advancedStore.snapshot().profiles[0].definition.value("frame").toObject().value("mode") == QJsonValue("length"));
    advanced.resize(1060, 780); QCoreApplication::processEvents();
    if (argc > 1) VERIFY(advanced.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-length-editor-dark.png"));
    advanced.close();
    {
        ProtocolManagerDialog generated(temporary.filePath("generated.json")); generated.show(); QApplication::processEvents();
        VERIFY(createProtocol(generated, 0)); generated.findChild<QPushButton *>("protocolSave")->click();
        VERIFY(createProtocol(generated, 2)); generated.findChild<QPushButton *>("protocolSave")->click();
        auto input = generated.findChild<QPlainTextEdit *>("protocolSample"); auto format = generated.findChild<QComboBox *>("protocolInputMode");
        auto pretty = generated.findChild<QCheckBox *>("protocolSpaceHex"); auto generate = generated.findChild<QPushButton *>("protocolGenerateSample"); auto test = generated.findChild<QPushButton *>("protocolTest");
        VERIFY(pretty && pretty->text() == "HEX 字节间空格");
        auto protocol = generated.findChild<QComboBox *>("protocolPreviewProtocol"); auto modes = generated.findChild<QComboBox *>("protocolChunkMode"); auto stats = generated.findChild<QLabel *>("protocolStats");
        const auto name = generated.findChild<QLineEdit *>("protocolName")->text(); const auto original = input->toPlainText(); QByteArray bytes;
        VERIFY(ProtocolProfileValidator::parseHex(original, bytes)); format->setCurrentIndex(0); QByteArray textBytes;
        VERIFY(!pretty->isVisible() && ProtocolSampleEdit::textBytes(input->toPlainText(), textBytes, error) && textBytes == bytes && input->toPlainText().contains("\\xAA"));
        VERIFY(input->toPlainText() == ProtocolSampleEdit::displayBytes(bytes, false, false, true));
        format->setCurrentIndex(1); VERIFY(pretty->isVisible() && input->toPlainText() == original);
        pretty->setChecked(false); VERIFY(input->toPlainText() == QString::fromLatin1(bytes.toHex().toUpper())); pretty->setChecked(true); VERIFY(input->toPlainText() == original);
        input->setPlainText("AA5"); format->setCurrentIndex(0); VERIFY(format->currentIndex() == 1 && input->toPlainText() == "AA 5");
        generate->click(); VERIFY(stats->text().contains("生成一帧") && input->toPlainText() != "AA 5");
        const auto expected = input->toPlainText(); QByteArray generatedBytes; VERIFY(ProtocolProfileValidator::parseHex(expected, generatedBytes));
        format->setCurrentIndex(0); generate->click();
        VERIFY(input->toPlainText() == ProtocolSampleEdit::displayBytes(generatedBytes, false, false, true));
        format->setCurrentIndex(1); VERIFY(input->toPlainText() == expected);
        for (int mode : {0, 1, 2}) {
            modes->setCurrentIndex(mode); VERIFY(generated.findChild<QLabel *>("protocolChunkHint")->text().contains("解析结果应相同"));
            test->click(); VERIFY(generated.findChild<QPushButton *>("protocolCancelTest")->isEnabled()); VERIFY(until([&] { return test->isEnabled(); }));
            VERIFY(generated.findChild<QTableWidget *>("protocolResults")->rowCount() == 2 && generated.findChild<QPushButton *>("protocolCancelTest")->isHidden());
            VERIFY(stats->text().contains("模拟分包：" + modes->currentText()));
            if (mode == 0) VERIFY(stats->text().contains("1 次"));
            if (mode == 1) VERIFY(stats->text().contains(QString("%1 次").arg(generatedBytes.size())) && stats->text().contains("每包 1–1 字节"));
        }
        protocol->setCurrentIndex(0); generate->click(); VERIFY(generated.findChild<QLineEdit *>("protocolName")->text() == name);
        test->click(); VERIFY(until([&] { return test->isEnabled(); }) && generated.findChild<QTableWidget *>("protocolResults")->rowCount() == 2 && stats->text().contains("测试协议：文本分隔协议"));
        // Generation with an invalid draft must preserve the previous input.
        protocol->setCurrentIndex(1); const auto beforeFailure = input->toPlainText(); generated.findChild<QLineEdit *>("protocol_headerHex")->clear(); bool refused = false;
        QTimer::singleShot(0, [&] { if (auto report = generated.findChild<QDialog *>("protocolInfoDialog")) { refused = report->windowTitle().contains("无法生成"); report->accept(); } });
        generate->click(); VERIFY(refused && input->toPlainText() == beforeFailure);
        QByteArray previousBytes; VERIFY(ProtocolProfileValidator::parseHex(beforeFailure, previousBytes));
        format->setCurrentIndex(0); VERIFY(input->toPlainText() == ProtocolSampleEdit::displayBytes(previousBytes, false, false, true));
        format->setCurrentIndex(1); VERIFY(input->toPlainText() == beforeFailure);
        generated.findChild<QLineEdit *>("protocol_headerHex")->setText("AA55");
        auto algorithm = generated.findChild<QComboBox *>("protocol_algorithm"); algorithm->setCurrentIndex(algorithm->findData("crc16-modbus"));
        VERIFY(generated.findChild<QSpinBox *>("protocol_checkStart")->value() == 0 && generated.findChild<QSpinBox *>("protocol_checkEndOffset")->value() == 4 && generated.findChild<QSpinBox *>("protocol_checkOffset")->value() == 4);
        VERIFY(generated.findChild<QSpinBox *>("protocol_minFrameBytes")->value() == 12 && generated.findChild<QSpinBox *>("protocol_lengthAdjustment")->value() == 8);
        generated.findChild<QPushButton *>("protocolSave")->click(); generate->click(); test->click(); VERIFY(until([&] { return test->isEnabled(); }) && stats->text().contains("有效 1 帧"));
        auto longer = QByteArray::fromHex("AA550C000000803F0000004000004040"); const quint32 crc = ProtocolChecksum("crc16-modbus").calculate(longer.constData(), longer.size());
        longer.append(char(crc)); longer.append(char(crc >> 8)); longer += QByteArray::fromHex("0D0A"); input->setPlainText(QString::fromLatin1(longer.toHex()));
        test->click(); VERIFY(until([&] { return test->isEnabled(); }) && stats->text().contains("有效 1 帧") && stats->text().contains("校验失败 0"));
        VERIFY(generated.findChild<QTableWidget *>("protocolResults")->rowCount() == 3); // All complete array values are decoded.
        auto count = generated.findChild<QSpinBox *>("protocolSampleArrayCount"); VERIFY(count && count->isVisible()); count->setValue(16); generate->click(); test->click();
        VERIFY(until([&] { return test->isEnabled(); }) && generated.findChild<QTableWidget *>("protocolResults")->rowCount() == 16);
        if (argc > 1) { generated.resize(1560, 1100); QApplication::processEvents(); VERIFY(generated.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-sample-generator-dark.png")); }
        generated.close();
    }
    {
        // Binary text display follows the TEST protocol even when a text
        // protocol is open in the editor, and never changes the sample bytes.
        ProtocolManagerDialog escaped(temporary.filePath("escaped-samples.json")); escaped.show(); QApplication::processEvents();
        for (int type : {0, 1, 2}) { VERIFY(createProtocol(escaped, type)); escaped.findChild<QPushButton *>("protocolSave")->click(); }
        escaped.findChild<QListWidget *>("protocolList")->setCurrentRow(0);
        auto selector = escaped.findChild<QComboBox *>("protocolPreviewProtocol");
        auto format = escaped.findChild<QComboBox *>("protocolInputMode");
        auto input = escaped.findChild<QPlainTextEdit *>("protocolSample");
        auto generate = escaped.findChild<QPushButton *>("protocolGenerateSample");
        auto test = escaped.findChild<QPushButton *>("protocolTest");
        auto stats = escaped.findChild<QLabel *>("protocolStats");
        format->setCurrentIndex(0);
        generate->click(); VERIFY(input->toPlainText().startsWith("DATA:") && input->toPlainText().endsWith("\\r\\n"));
        for (int type : {1, 2}) {
            selector->setCurrentIndex(type); VERIFY(format->currentIndex() == 1); QByteArray autoBytes;
            VERIFY(ProtocolProfileValidator::parseHex(input->toPlainText(), autoBytes) && !autoBytes.isEmpty());
            format->setCurrentIndex(0); generate->click(); QByteArray decoded;
            VERIFY(ProtocolSampleEdit::textBytes(input->toPlainText(), decoded, error));
            VERIFY(input->toPlainText() == ProtocolSampleEdit::displayBytes(decoded, false, false, true));
            VERIFY(input->toPlainText().startsWith("\\xAA\\x55") && input->toPlainText().endsWith("\\x0D\\x0A"));
            VERIFY(!input->toPlainText().contains('@') && !input->toPlainText().contains('U'));
            VERIFY(escaped.findChild<QComboBox *>("protocolType")->currentIndex() == 0);
            const auto before = input->toPlainText(); format->setCurrentIndex(1);
            QByteArray hexBytes; VERIFY(ProtocolProfileValidator::parseHex(input->toPlainText(), hexBytes) && hexBytes == decoded);
            format->setCurrentIndex(0); VERIFY(input->toPlainText() == before);
            test->click(); VERIFY(until([&] { return test->isEnabled(); }) && stats->text().contains("有效 1 帧"));
            if (argc > 1 && type == 2) {
                escaped.resize(1560, 1100); QApplication::processEvents();
                escaped.findChild<QScrollArea *>("protocolPreviewScroll")->ensureWidgetVisible(input);
                VERIFY(input->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-binary-escaped-text.png"));
            }
        }
        escaped.close();
    }
    {
        // Automatic defaults apply on protocol/type changes only. Manual
        // formatting and edited input survive ordinary config edits and save.
        ProtocolManagerDialog automatic(temporary.filePath("automatic-samples.json")); automatic.show(); QApplication::processEvents();
        VERIFY(createProtocol(automatic, 1));
        auto format = automatic.findChild<QComboBox *>("protocolInputMode"); auto input = automatic.findChild<QPlainTextEdit *>("protocolSample");
        auto type = automatic.findChild<QComboBox *>("protocolType"); auto test = automatic.findChild<QPushButton *>("protocolTest");
        auto stats = automatic.findChild<QLabel *>("protocolStats");
        VERIFY(format->currentIndex() == 1 && stats->text().contains("自动选择"));
        test->click(); VERIFY(until([&] { return test->isEnabled(); }) && stats->text().contains("有效 1 帧"));
        format->setCurrentIndex(0); const auto manual = input->toPlainText();
        automatic.findChild<QLineEdit *>("protocolName")->setText("自动格式测试");
        VERIFY(format->currentIndex() == 0 && input->toPlainText() == manual);
        automatic.findChild<QPushButton *>("protocolSave")->click(); VERIFY(format->currentIndex() == 0 && input->toPlainText() == manual);
        type->setCurrentIndex(0); VERIFY(format->currentIndex() == 0 && !input->toPlainText().isEmpty() && input->toPlainText() != manual);
        test->click(); VERIFY(until([&] { return test->isEnabled(); }) && stats->text().contains("有效 1 帧"));
        type->setCurrentIndex(1); VERIFY(format->currentIndex() == 1 && !input->toPlainText().isEmpty());
        test->click(); VERIFY(until([&] { return test->isEnabled(); }) && stats->text().contains("有效 1 帧"));
        automatic.findChild<QPushButton *>("protocolSave")->click(); automatic.close();
    }
    // Closing, invalid saves, Ctrl+S on uncommitted numeric input and deleting
    // a new draft must never silently save it or delete a different protocol.
    const QString draftPath = temporary.filePath("draft-lifecycle.json");
    ProtocolManagerDialog drafts(draftPath); drafts.show(); QCoreApplication::processEvents();
    auto draftList = drafts.findChild<QListWidget *>("protocolList");
    auto draftName = drafts.findChild<QLineEdit *>("protocolName");
    VERIFY(createProtocol(drafts, 1)); const QString draftInitialName = draftName->text();
    draftName->clear();
    QTimer::singleShot(0, &drafts, [&] { if (auto info = drafts.findChild<QDialog *>("protocolInfoDialog")) info->accept(); });
    drafts.findChild<QPushButton *>("protocolSave")->click();
    VERIFY(draftList->count() == 1 && draftList->currentItem()->text() == "未命名协议");
    ProtocolProfileStore draftDisk(draftPath); VERIFY(draftDisk.load(error) && draftDisk.snapshot().profiles.isEmpty());
    QTimer::singleShot(0, &drafts, [&] { drafts.findChild<QDialog *>("protocolConfirm")->findChild<QPushButton *>("protocolDecision0")->click(); });
    VERIFY(!drafts.close() && drafts.isVisible() && draftList->count() == 1);
    QTimer::singleShot(0, &drafts, [&] { drafts.findChild<QDialog *>("protocolConfirm")->findChild<QPushButton *>("protocolDecision2")->click(); });
    VERIFY(drafts.close() && draftList->count() == 0);
    drafts.show(); QCoreApplication::processEvents(); VERIFY(createProtocol(drafts, 1));
    auto frameSize = drafts.findChild<QSpinBox *>("protocol_sizeBytes");
    QApplication::setActiveWindow(&drafts); frameSize->setFocus();
    auto numberEdit = frameSize->findChild<QLineEdit *>(); VERIFY(numberEdit);
    numberEdit->setText("12"); VERIFY(frameSize->value() == 10); // Keyboard tracking is off.
    QApplication::sendEvent(frameSize, &press); QApplication::sendEvent(frameSize, &release);
    VERIFY(until([&] { return drafts.findChild<QLabel *>("protocolSaveStatus")->text().startsWith("协议已保存"); }));
    VERIFY(draftDisk.load(error) && draftDisk.snapshot().profiles.size() == 1);
    VERIFY(draftDisk.snapshot().profiles[0].definition.value("frame").toObject().value("sizeBytes").toInt() == 12);
    const auto savedDraftBytes = ProtocolProfileValidator::encode(draftDisk.snapshot(), true, error);
    VERIFY(createProtocol(drafts, 0) && draftList->count() == 2 && draftList->currentRow() == 1);
    QTimer::singleShot(0, &drafts, [&] { drafts.findChild<QDialog *>("protocolConfirm")->findChild<QPushButton *>("protocolDecision2")->click(); });
    drafts.findChild<QPushButton *>("protocolDelete")->click();
    VERIFY(draftList->count() == 1 && draftList->currentItem()->text() == draftInitialName);
    VERIFY(draftDisk.load(error) && ProtocolProfileValidator::encode(draftDisk.snapshot(), true, error) == savedDraftBytes);
    drafts.close();
    // Right-click copy uses the same guarded, durable operation as the button.
    const QString menuPath = temporary.filePath("menus.json");
    ProtocolManagerDialog menus(menuPath); menus.show(); QCoreApplication::processEvents();
    auto menuList = menus.findChild<QListWidget *>("protocolList");
    VERIFY(createProtocol(menus, 1)); menus.findChild<QPushButton *>("protocolSave")->click();
    VERIFY(createProtocol(menus, 0)); menus.findChild<QPushButton *>("protocolSave")->click();
    const QString originalId = menuList->item(0)->data(Qt::UserRole).toString(), originalName = menuList->item(0)->text();
    const QString lastId = menuList->item(1)->data(Qt::UserRole).toString();
    menuList->setCurrentRow(0);
    VERIFY(chooseSidebarAction(menus, menuList, 0, "protocolListMenu", "protocolMenuCopy"));
    VERIFY(menuList->count() == 3 && menuList->currentRow() == 1 && menuList->item(1)->text() == originalName + "_1");
    VERIFY(chooseSidebarAction(menus, menuList, 0, "protocolListMenu", "protocolMenuCopy"));
    VERIFY(menuList->count() == 4 && menuList->item(1)->text() == originalName + "_2" && menuList->item(2)->text() == originalName + "_1");
    VERIFY(menuList->item(0)->data(Qt::UserRole) == originalId && menuList->item(3)->data(Qt::UserRole) == lastId);
    VERIFY(chooseSidebarAction(menus, menuList, 1, "protocolListMenu", "protocolMenuCopy"));
    VERIFY(menuList->count() == 5 && menuList->item(2)->text() == originalName + "_2_1");
    // New drafts have their own selected list item. Cancelling navigation
    // keeps it; discarding removes it without modifying the saved library.
    VERIFY(createProtocol(menus, 0));
    VERIFY(menuList->count() == 6 && menuList->currentRow() == 5 && menuList->selectedItems().size() == 1);
    VERIFY(menus.findChild<QLineEdit *>("protocolName")->text() == "文本分隔协议_1");
    const QString newDraftId = menuList->currentItem()->data(Qt::UserRole).toString();
    menus.findChild<QLineEdit *>("protocolName")->setText("未保存新协议"); VERIFY(menuList->item(5)->text() == "未保存新协议");
    const QPoint sameRowPos = menuList->visualItemRect(menuList->item(2)).center();
    QTimer::singleShot(0, &menus, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolConfirm") widget->findChild<QPushButton *>("protocolDecision0")->click(); });
    VERIFY(QMetaObject::invokeMethod(menuList, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, sameRowPos)));
    VERIFY(menuList->count() == 6 && menuList->currentRow() == 5 && menuList->currentItem()->data(Qt::UserRole) == newDraftId);
    QTimer::singleShot(0, &menus, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolConfirm") widget->findChild<QPushButton *>("protocolDecision2")->click(); });
    menuList->setCurrentRow(0);
    VERIFY(menuList->count() == 5 && menuList->currentRow() == 0);
    menus.close(); ProtocolManagerDialog menusReopened(menuPath);
    auto restoredList = menusReopened.findChild<QListWidget *>("protocolList"); VERIFY(restoredList->count() == 5);
    VERIFY(restoredList->item(1)->text() == originalName + "_2" && restoredList->item(2)->text() == originalName + "_2_1");
    menusReopened.show(); QCoreApplication::processEvents();
    auto description = menusReopened.findChild<QPlainTextEdit *>("protocolDescription"); description->setPlainText("这是当前协议自己的说明，不是全局使用指南。");
    menusReopened.findChild<QPushButton *>("protocolSave")->click();
    bool descriptionMatched = false; QTimer descriptionCloser; descriptionCloser.setInterval(5);
    QObject::connect(&descriptionCloser, &QTimer::timeout, &descriptionCloser, [&] {
        if (auto info = menusReopened.findChild<QDialog *>("protocolInfoDialog")) {
            descriptionMatched = info->windowTitle().contains("协议说明") && info->findChild<QPlainTextEdit *>("protocolInfoText")->toPlainText() == description->toPlainText();
            info->accept(); descriptionCloser.stop();
        }
    });
    descriptionCloser.start(); VERIFY(chooseSidebarAction(menusReopened, restoredList, 0, "protocolListMenu", "protocolMenuDescription")); VERIFY(descriptionMatched);
    // Exercise all three actual export entry points. Selecting every row is
    // still a selected export; only the explicit All button has no suffix.
    VERIFY(exportDefaultName("WaveLink_Protocols_" + originalName + ".json", [&] { menusReopened.findChild<QPushButton *>("protocolExportSelected")->click(); }));
    bool currentExport = false;
    VERIFY(exportDefaultName("WaveLink_Protocols_" + originalName + "_2.json", [&] { currentExport = chooseSidebarAction(menusReopened, restoredList, 1, "protocolListMenu", "protocolMenuExport"); }));
    VERIFY(currentExport);
    restoredList->setCurrentRow(0); restoredList->item(1)->setSelected(true);
    VERIFY(exportDefaultName("WaveLink_Protocols_" + originalName + "_" + originalName + "_2.json", [&] { menusReopened.findChild<QPushButton *>("protocolExportSelected")->click(); }));
    restoredList->selectAll(); QStringList allIds;
    for (int i = 0; i < restoredList->count(); ++i) allIds << restoredList->item(i)->data(Qt::UserRole).toString();
    ProtocolProfileStore filenameStore(menuPath); VERIFY(filenameStore.load(error));
    VERIFY(exportDefaultName(filenameStore.defaultExportFileName(allIds), [&] { menusReopened.findChild<QPushButton *>("protocolExportSelected")->click(); }));
    VERIFY(exportDefaultName("WaveLink_Protocols.json", [&] { menusReopened.findChild<QPushButton *>("protocolExportAll")->click(); }));
    restoredList->clearSelection();
    restoredList->setCurrentRow(1); restoredList->item(2)->setSelected(true); VERIFY(restoredList->selectedItems().size() == 2);
    QTimer::singleShot(0, &menusReopened, [&] { menusReopened.findChild<QDialog *>("protocolConfirm")->findChild<QPushButton *>("protocolDecision0")->click(); });
    menusReopened.findChild<QPushButton *>("protocolDelete")->click(); VERIFY(restoredList->count() == 5);
    menusReopened.setMutationGuard([](const ProtocolSnapshot &candidate, QString &error) {
        if (candidate.profiles.size() < 4) { error = "测试：所选协议受保护，本次删除全部取消。"; return false; } return true;
    });
    bool deletionNamesClear = false;
    QTimer::singleShot(0, &menusReopened, [&] {
        auto confirm = menusReopened.findChild<QDialog *>("protocolConfirm"); auto names = confirm->findChild<QPlainTextEdit *>("protocolDeleteNames");
        deletionNamesClear = names && names->isReadOnly() && names->font().bold() && names->styleSheet().contains("2px solid") && names->toPlainText().contains(originalName + "_2") && names->toPlainText().contains(originalName + "_2_1");
        deletionNamesClear = deletionNamesClear && confirm->width() >= 560 && confirm->findChild<QPushButton *>("protocolDecision3")->text() == "删除协议" && confirm->findChild<QPushButton *>("protocolDecision0")->isDefault();
        if (argc > 1) confirm->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-delete-confirm.png");
        confirm->findChild<QPushButton *>("protocolDecision3")->click();
    });
    QTimer::singleShot(5, &menusReopened, [&] { if (auto info = menusReopened.findChild<QDialog *>("protocolInfoDialog")) info->accept(); });
    menusReopened.findChild<QPushButton *>("protocolDelete")->click(); VERIFY(restoredList->count() == 5);
    VERIFY(deletionNamesClear);
    ProtocolProfileStore protectedStore(menuPath); VERIFY(protectedStore.load(error) && protectedStore.snapshot().profiles.size() == 5);
    menusReopened.setMutationGuard({});
    QTimer::singleShot(0, &menusReopened, [&] { menusReopened.findChild<QDialog *>("protocolConfirm")->findChild<QPushButton *>("protocolDecision3")->click(); });
    menusReopened.findChild<QPushButton *>("protocolDelete")->click(); VERIFY(restoredList->count() == 3);
    VERIFY(restoredList->item(0)->data(Qt::UserRole) == originalId && restoredList->item(2)->data(Qt::UserRole) == lastId);
    menusReopened.close();
    // Recovery stays out of daily management but remains available after a
    // damaged file; preserve that damaged original before restoring a backup.
    QByteArray backupBytes; ProtocolSnapshot backupSnapshot;
    VERIFY(ProtocolProfileStore::readFile(menuPath + ".bak", backupBytes, error));
    VERIFY(ProtocolProfileValidator::decode(backupBytes, true, backupSnapshot, error));
    QFile corrupted(menuPath); VERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate)); VERIFY(corrupted.write("{broken") == 7); corrupted.close();
    ProtocolManagerDialog recovery(menuPath); recovery.show(); QCoreApplication::processEvents();
    auto recoverButton = recovery.findChild<QPushButton *>("protocolRecoverBackup"); VERIFY(recoverButton && recoverButton->isVisible());
    QTimer::singleShot(0, &recovery, [&] { recovery.findChild<QDialog *>("protocolConfirm")->findChild<QPushButton *>("protocolDecision3")->click(); });
    recoverButton->click(); VERIFY(recoverButton->isHidden());
    VERIFY(recovery.findChild<QListWidget *>("protocolList")->count() == backupSnapshot.profiles.size());
    VERIFY(QDir(temporary.path()).entryList({"menus.json.before-restore-*"}, QDir::Files).size() == 1);
    recovery.close();
    // An empty text channel table is a persisted, fully automatic protocol,
    // rather than an invalid/zero-channel decoder.
    const auto autoPath = temporary.filePath("automatic-text.json");
    ProtocolManagerDialog autoDialog(autoPath); autoDialog.show(); QCoreApplication::processEvents();
    VERIFY(createProtocol(autoDialog, 0));
    autoDialog.findChild<QTableWidget *>("protocolFields")->setRowCount(0);
    autoDialog.findChild<QPushButton *>("protocolSave")->click();
    ProtocolProfileStore autoStore(autoPath); VERIFY(autoStore.load(error) && autoStore.snapshot().profiles.size() == 1);
    VERIFY(autoStore.snapshot().profiles[0].definition.value("channels").toArray().isEmpty());
    autoDialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText("DATA:25.34,3.30,2.5,2.8\\r\\n");
    auto autoTest = autoDialog.findChild<QPushButton *>("protocolTest"); autoTest->click();
    auto autoResults = autoDialog.findChild<QTableWidget *>("protocolResults");
    VERIFY(until([&] { return autoTest->isEnabled(); }) && autoResults->rowCount() == 4);
    for (int i = 0; i < 4; ++i) VERIFY(autoResults->item(i, 1)->text() == QString("Ch%1").arg(i + 1) && autoResults->item(i, 2)->text().isEmpty());
    VERIFY(autoResults->item(1, 3)->text() == "3.3" && autoResults->item(1, 4)->text() == "3.3");
    autoDialog.close(); ProtocolManagerDialog autoReopened(autoPath);
    VERIFY(autoReopened.findChild<QTableWidget *>("protocolFields")->rowCount() == 0);
    // Float32 display must honor its source precision, including converted
    // results, WITHOUT rounding float64, constant biases or meaningful digits.
    ProtocolManagerDialog precisionDialog(temporary.filePath("binary-precision.json")); precisionDialog.show(); QCoreApplication::processEvents();
    VERIFY(createProtocol(precisionDialog, 1));
    auto precisionEditor = precisionDialog.findChild<ProtocolEditorWidget *>();
    ProtocolProfile precisionSeed; VERIFY(precisionEditor->profile(precisionSeed, error));
    auto precisionTest = precisionDialog.findChild<QPushButton *>("protocolTest");
    auto precisionResults = precisionDialog.findChild<QTableWidget *>("protocolResults");
    auto runPrecision = [&](double value, bool single, double scale, double bias, bool big = false) {
        const int width = single ? 4 : 8; quint64 bits = 0;
        const double rawValue = single ? double(static_cast<float>(value)) : value;
        if (single) { const float f = static_cast<float>(value); quint32 word; std::memcpy(&word, &f, 4); bits = word; }
        else std::memcpy(&bits, &value, 8);
        QByteArray packet;
        for (int i = 0; i < width; ++i) packet.append(char(bits >> (8 * (big ? width - 1 - i : i))));
        auto definition = precisionSeed.definition; definition.remove("checksum");
        definition.insert("byteOrder", big ? "big" : "little");
        definition.insert("frame", QJsonObject{{"mode", "fixed"}, {"headerHex", ""}, {"tailHex", "0D0A"}, {"sizeBytes", width + 2}, {"payloadOffset", 0}, {"payloadBytes", width}, {"maxFrameBytes", 4096}});
        packet += QByteArray::fromHex("0D0A");
        definition.insert("channels", QJsonArray{QJsonObject{{"id", "precision"}, {"name", "Ch1"}, {"type", single ? "float32" : "float64"}, {"offset", 0}, {"scale", scale}, {"bias", bias}}});
        ProtocolProfile p; if (!ProtocolProfileValidator::profile(definition, p, error)) return false;
        precisionEditor->setProfile(p);
        precisionDialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText(QString::fromLatin1(packet.toHex(' ').toUpper()));
        precisionTest->click();
        return until([&] { return precisionTest->isEnabled(); }) && precisionResults->rowCount() == 1
            && precisionResults->item(0, 3)->toolTip().startsWith("完整精度：" + QString::number(rawValue, 'g', 17))
            && precisionResults->item(0, 4)->toolTip().startsWith("完整精度：" + QString::number(rawValue * scale + bias, 'g', 17));
    };
    for (bool big : {false, true}) {
        VERIFY(runPrecision(3.3, true, 1, 0, big));
        VERIFY(precisionResults->item(0, 3)->text() == "3.3" && precisionResults->item(0, 4)->text() == "3.3");
        VERIFY(runPrecision(3.3, true, 10, 0, big) && precisionResults->item(0, 4)->text().toDouble() == 33);
        VERIFY(runPrecision(3.3, true, .1, 0, big) && precisionResults->item(0, 4)->text() == "0.33");
        VERIFY(runPrecision(3.3, true, -10, 100, big) && precisionResults->item(0, 4)->text().toDouble() == 67);
        VERIFY(runPrecision(-3.3, true, 1, 0, big) && precisionResults->item(0, 3)->text() == "-3.3");
    }
    VERIFY(runPrecision(12345678, true, 1, 0) && precisionResults->item(0, 3)->text().toDouble() == 12345678); // A fixed 7-digit limit would lose information.
    for (bool single : {false, true}) for (double value : {10., 20., 100., -10., 1000., 1000000., .00001, -.000001}) {
        VERIFY(runPrecision(value, single, 1, 0));
        const auto displayed = precisionResults->item(0, 3)->text();
        VERIFY(!displayed.contains('e', Qt::CaseInsensitive) && displayed.toDouble() == value);
        VERIFY(precisionResults->item(0, 4)->text() == displayed);
    }
    VERIFY(runPrecision(1, true, 10, 0) && precisionResults->item(0, 4)->text() == "10");
    VERIFY(runPrecision(1e20, true, 1, 0) && precisionResults->item(0, 3)->text().contains('e', Qt::CaseInsensitive));
    VERIFY(runPrecision(1e-20, true, 1, 0) && precisionResults->item(0, 3)->text().contains('e', Qt::CaseInsensitive));
    // User's complete JustFloat-style, tail-only fixed frame: Ch10 is "10",
    // not "1e+01". Preserve the complete values in the tooltip as before.
    {
        auto definition = precisionSeed.definition; definition.remove("checksum");
        definition.insert("frame", QJsonObject{{"mode", "fixed"}, {"headerHex", ""}, {"tailHex", "0000807F"},
            {"sizeBytes", 44}, {"payloadOffset", 0}, {"payloadBytes", 40}, {"maxFrameBytes", 4096}});
        QJsonArray channels;
        for (int i = 0; i < 10; ++i) channels.append(QJsonObject{{"id", QString("c%1").arg(i)},
            {"name", QString("Ch%1").arg(i + 1)}, {"type", "float32"}, {"offset", i * 4}, {"scale", 1}, {"bias", 0}});
        definition.insert("channels", channels);
        ProtocolProfile p; VERIFY(ProtocolProfileValidator::profile(definition, p, error)); precisionEditor->setProfile(p);
        precisionDialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText(
            "00 00 80 3F 00 00 00 40 00 00 40 40 00 00 80 40 00 00 A0 40 00 00 C0 40 00 00 E0 40 00 00 00 41 00 00 10 41 00 00 20 41 00 00 80 7F");
        precisionTest->click(); VERIFY(until([&] { return precisionTest->isEnabled(); }) && precisionResults->rowCount() == 10);
        for (int i = 0; i < 10; ++i) VERIFY(precisionResults->item(i, 3)->text() == QString::number(i + 1)
            && precisionResults->item(i, 4)->text() == QString::number(i + 1));
    }
    for (float value : {std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min(), 0.f}) {
        VERIFY(runPrecision(value, true, 1, 0));
        VERIFY(static_cast<float>(precisionResults->item(0, 3)->text().toDouble()) == value);
    }
    const double preciseBias = QString("0.12345678901234567").toDouble();
    VERIFY(runPrecision(3.3, true, 0, preciseBias) && precisionResults->item(0, 4)->text().toDouble() == preciseBias);
    VERIFY(runPrecision(1e-20, true, 1, preciseBias) && precisionResults->item(0, 4)->text().toDouble() == preciseBias);
    VERIFY(runPrecision(preciseBias, false, 1, 0, true) && precisionResults->item(0, 3)->text().toDouble() == preciseBias && precisionResults->item(0, 4)->text().toDouble() == preciseBias);
    precisionDialog.findChild<QPushButton *>("protocolSave")->click(); precisionDialog.close();
    ProtocolManagerDialog rangeDialog(temporary.filePath("range-policy.json")); rangeDialog.show();
    VERIFY(createProtocol(rangeDialog, 0));
    auto rangeFields = rangeDialog.findChild<QTableWidget *>("protocolFields"); rangeFields->item(0, 9)->setText("1");
    auto rangePolicy = rangeDialog.findChild<QComboBox *>("protocol_outOfRangePolicy"); VERIFY(rangePolicy && rangePolicy->currentData().toString() == "reject-frame");
    VERIFY(rangePolicy->itemData(0).toString() == "nan-channel" && rangePolicy->itemData(1).toString() == "reject-frame");
    VERIFY(rangePolicy->itemText(0).contains("NaN") && rangePolicy->itemText(1) == "拒绝整帧");
    rangePolicy->setCurrentIndex(rangePolicy->findData("nan-channel"));
    rangeDialog.findChild<QPlainTextEdit *>("protocolSample")->setPlainText("DATA:2,3\\r\\nDATA:1,4\\r\\nDATA:NaN,5\\r\\n");
    auto rangeTest = rangeDialog.findChild<QPushButton *>("protocolTest"); rangeTest->click(); VERIFY(until([&] { return rangeTest->isEnabled(); }));
    auto rangeResults = rangeDialog.findChild<QTableWidget *>("protocolResults");
    VERIFY(rangeResults->rowCount() == 6 && rangeResults->item(0, 3)->text() == "2" && rangeResults->item(0, 4)->text() == "NaN");
    VERIFY(rangeResults->item(1, 4)->text() == "3" && rangeResults->item(2, 4)->text() == "1" && rangeResults->item(0, 4)->toolTip().contains("超出"));
    VERIFY(rangeResults->item(4, 3)->text() == "NaN" && rangeResults->item(4, 4)->text() == "NaN");
    VERIFY(rangeResults->item(4, 4)->toolTip().contains("设备发送") && rangeResults->item(5, 4)->text() == "5");
    VERIFY(rangeDialog.findChild<QLabel *>("protocolStats")->text().contains("超限标记 1"));
    rangeDialog.findChild<QPushButton *>("protocolSave")->click();
    ProtocolProfileStore rangeStore(temporary.filePath("range-policy.json")); VERIFY(rangeStore.load(error));
    VERIFY(rangeStore.snapshot().profiles[0].definition["outOfRangePolicy"] == QJsonValue("nan-channel"));
    rangeDialog.resize(1440, 860); QCoreApplication::processEvents();
    if (argc > 1) rangeDialog.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-nan-policy.png");
    for (int i = 0; i < 30; ++i) rangeDialog.findChild<QPushButton *>("protocolAddField")->click();
    rangeDialog.resize(1000, 720); QCoreApplication::processEvents();
    auto rangeEditorScroll = rangeDialog.findChild<QScrollArea *>("protocolEditorScroll");
    VERIFY(rangeEditorScroll && rangeEditorScroll->verticalScrollBar()->maximum() > 0 && rangeFields->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    rangeEditorScroll->verticalScrollBar()->setValue(rangeEditorScroll->verticalScrollBar()->maximum()); QCoreApplication::processEvents();
    VERIFY(rangeFields->visualItemRect(rangeFields->item(31, 1)).bottom() <= rangeFields->viewport()->height());
    VERIFY(rangeDialog.findChild<QPushButton *>("protocolSave")->isVisible());
    rangeDialog.findChild<QPushButton *>("protocolSave")->click();
    if (argc > 1) rangeDialog.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-full-channel-table.png");
    rangeDialog.close();
    // Key/value is a general type; CBF is only an opt-in editable template.
    const auto keyPath = temporary.filePath("keyvalue-ui.json");
    ProtocolManagerDialog keys(keyPath); keys.show(); QCoreApplication::processEvents();
    VERIFY(keys.findChild<QListWidget *>("protocolList")->count() == 0);
    VERIFY(createProtocol(keys, 3));
    auto keyEditor = keys.findChild<ProtocolEditorWidget *>(); auto keyFields = keys.findChild<QTableWidget *>("protocolFields");
    VERIFY(keyEditor->isKeyValueProtocol() && !keyEditor->isBinaryProtocol() && keyFields->rowCount() == 3);
    auto keyPolicy = keys.findChild<QComboBox *>("protocol_outOfRangePolicy");
    VERIFY(keyPolicy->currentIndex() == 0 && keyPolicy->currentData().toString() == "nan-channel");
    VERIFY(keys.findChild<QPlainTextEdit *>("protocolDescription")->toPlainText().startsWith("样例：0:0\\r\\n1:1\\r\\n2:2\\r\\n"));
    VERIFY(keyFields->horizontalHeaderItem(3)->text() == "通道键" && keyFields->isColumnHidden(4) && keyFields->isColumnHidden(5));
    VERIFY(keys.findChild<QComboBox *>("protocol_delimiterMode")->isHidden() && keys.findChild<QSpinBox *>("protocol_groupTimeoutMs")->isVisible());
    VERIFY(keys.findChild<QWidget *>("protocol_devicePresetRow")->isHidden());
    keyFields->item(0, 3)->setText("温度"); keyFields->item(0, 1)->setText("温度");
    keyFields->item(1, 3)->setText("IR");
    keys.findChild<QPushButton *>("protocolAddField")->click(); VERIFY(keyFields->item(3, 3)->text() == "0");
    keyFields->selectRow(3); keys.findChild<QPushButton *>("protocolDeleteField")->click();
    keyFields->selectRow(1); keys.findChild<QPushButton *>("protocolFieldUp")->click();
    VERIFY(keyFields->item(0, 3)->text() == "IR" && keyFields->item(1, 3)->text() == "温度");
    keys.findChild<QPushButton *>("protocolGenerateSample")->click();
    auto keyTest = keys.findChild<QPushButton *>("protocolTest"); keyTest->click(); VERIFY(until([&] { return keyTest->isEnabled(); }));
    VERIFY(keys.findChild<QTableWidget *>("protocolResults")->rowCount() == 3);
    // Verify saved semantic values, not option indices, in both directions.
    keyPolicy->setCurrentIndex(keyPolicy->findData("reject-frame"));
    keys.findChild<QPushButton *>("protocolSave")->click();
    ProtocolProfileStore keyStore(keyPath); VERIFY(keyStore.load(error) && keyStore.snapshot().profiles.size() == 1);
    VERIFY(keyStore.snapshot().profiles[0].isKeyValue() && keyStore.snapshot().profiles[0].definition["channels"].toArray()[0].toObject()["key"] == QJsonValue("IR"));
    VERIFY(keyStore.snapshot().profiles[0].definition["outOfRangePolicy"] == QJsonValue("reject-frame"));
    VERIFY(createProtocol(keys, 3, true, "protocolNew", true));
    VERIFY(keyPolicy->currentIndex() == 0 && keyPolicy->currentData().toString() == "nan-channel");
    VERIFY(keys.findChild<QLineEdit *>("protocolName")->text() == "CBF/IR/RED" && keyFields->rowCount() == 3);
    VERIFY(keyFields->item(0, 1)->text() == "CBF" && keyFields->item(1, 1)->text() == "IR" && keyFields->item(2, 1)->text() == "RED");
    auto preset = keys.findChild<QCheckBox *>("protocol_devicePresetEnabled");
    VERIFY(preset->isVisible() && preset->isChecked() && keys.findChild<QSpinBox *>("protocol_devicePresetPort")->isEnabled());
    VERIFY(keys.findChild<QSpinBox *>("protocol_devicePresetPort")->value() == 4970 && keyFields->item(0, 2)->text() == "MHz");
    VERIFY(keys.findChild<QPlainTextEdit *>("protocolDescription")->toPlainText().contains("设备采样率默认31Hz，采用TCP服务端数据接口，默认端口4970。"));
    keys.findChild<QSpinBox *>("protocol_devicePresetPort")->setValue(4971);
    VERIFY(keys.findChild<QSpinBox *>("protocol_devicePresetPort")->isEnabled());
    keys.findChild<QPushButton *>("protocolSave")->click();
    VERIFY(keyStore.load(error) && keyStore.snapshot().profiles.size() == 2);
    VERIFY(keyStore.snapshot().profiles[1].definition["outOfRangePolicy"] == QJsonValue("nan-channel"));
    const auto cbfPreset = keyStore.snapshot().profiles[1].definition["devicePreset"].toObject();
    VERIFY(cbfPreset["enabled"].toBool() && cbfPreset["port"].toInt() == 4971);
    keyTest->click(); VERIFY(until([&] { return keyTest->isEnabled(); }) && keys.findChild<QTableWidget *>("protocolResults")->rowCount() == 3);
    auto cbfDefinition = keyStore.snapshot().profiles[1].definition;
    keyEditor->setProfile(keyStore.snapshot().profiles[0]); VERIFY(keyFields->item(0, 3)->text() == "IR");
    VERIFY(keyPolicy->currentIndex() == 1 && keyPolicy->currentData().toString() == "reject-frame");
    keyEditor->setProfile(keyStore.snapshot().profiles[1]); ProtocolProfile readBack;
    VERIFY(keyPolicy->currentIndex() == 0 && keyPolicy->currentData().toString() == "nan-channel");
    VERIFY(keyEditor->profile(readBack, error) && readBack.definition == cbfDefinition);
    // Repeated type switching must not retain hidden key/group/device rules.
    auto keyType = keys.findChild<QComboBox *>("protocolType"); keyType->setCurrentIndex(0);
    VERIFY(keyFields->item(0, 3)->text() == "0" && keyFields->item(1, 3)->text() == "1" && keyFields->item(2, 3)->text() == "2");
    VERIFY(keys.findChild<QSpinBox *>("protocol_groupTimeoutMs")->isHidden());
    keyType->setCurrentIndex(3); VERIFY(keyEditor->profile(readBack, error) && readBack.isKeyValue());
    keyEditor->setProfile(keyStore.snapshot().profiles[1]); keys.findChild<QPushButton *>("protocolSave")->click(); keys.close();
    {
        const auto path = temporary.filePath("template-menu.json"); ProtocolManagerDialog manager(path); manager.show();
        VERIFY(createProtocol(manager, 3)); manager.findChild<QPushButton *>("protocolSave")->click();
        auto list = manager.findChild<QListWidget *>("protocolList"); bool nestedTemplate = false;
        QTimer driver; driver.setSingleShot(true);
        QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
            auto menu = manager.findChild<QMenu *>("protocolListMenu");
            auto create = menu ? menu->findChild<QMenu *>("protocolMenuNew") : nullptr;
            auto templates = create ? create->findChild<QMenu *>("protocolMenuNewTemplates") : nullptr;
            auto cbf = templates ? templates->findChild<QAction *>("protocolMenuNewCbf") : nullptr;
            nestedTemplate = create && templates && cbf && create->actions().size() == 6 && create->actions().last()->menu() == templates
                && templates->title() == "新增模板协议" && templates->actions().size() == 1 && templates->actions().first() == cbf
                && cbf->isEnabled() && cbf->text() == "CBF/IR/RED 设备模板";
            if (nestedTemplate) cbf->trigger(); if (menu) menu->close();
        });
        driver.start(0); const QPoint pos = list->visualItemRect(list->item(0)).center();
        VERIFY(QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, pos)) && nestedTemplate);
        auto enabled = manager.findChild<QCheckBox *>("protocol_devicePresetEnabled");
        VERIFY(list->count() == 2 && list->currentItem()->text() == "CBF/IR/RED" && enabled->isChecked());
        VERIFY(manager.findChild<QTableWidget *>("protocolFields")->item(0, 2)->text() == "MHz");
        enabled->setChecked(false); manager.findChild<QPushButton *>("protocolSave")->click();
        ProtocolProfileStore saved(path); VERIFY(saved.load(error) && saved.snapshot().profiles.size() == 2);
        VERIFY(!saved.snapshot().profiles[1].definition.value("devicePreset").toObject().value("enabled").toBool()
            && saved.snapshot().profiles[1].definition.value("channels").toArray()[0].toObject().value("unit") == QJsonValue("MHz"));
        manager.close();
    }
    {
        const auto path = temporary.filePath("full-description.json"); ProtocolManagerDialog manager(path); manager.resize(1100, 720); manager.show();
        VERIFY(createProtocol(manager, 0));
        auto description = manager.findChild<QPlainTextEdit *>("protocolDescription");
        auto scroll = manager.findChild<QScrollArea *>("protocolEditorScroll");
        VERIFY(description && scroll && description->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff
            && description->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
        QString longDescription;
        for (int i = 0; i < 32; ++i) longDescription += QString("第%1行：完整展示协议说明，编辑和窗口宽度变化时自动调整高度，不使用输入框自己的滚动条。\n").arg(i + 1);
        longDescription += QString(400, 'A'); description->setPlainText(longDescription);
        VERIFY(until([&] { return description->height() > 500 && description->verticalScrollBar()->maximum() == 0 && scroll->verticalScrollBar()->maximum() > 0; }));
        QTextCursor end(description->document()); end.movePosition(QTextCursor::End);
        VERIFY(description->cursorRect(end).bottom() < description->viewport()->height());
        const int narrowHeight = description->height(); manager.resize(1800, 900);
        VERIFY(until([&] { return description->height() < narrowHeight && description->verticalScrollBar()->maximum() == 0; }));
        const int wideHeight = description->height(); const QFont originalFont = description->font();
        QFont larger = originalFont; larger.setPointSize(qMax(9, larger.pointSize()) + 4); description->setFont(larger);
        VERIFY(until([&] { return description->height() > wideHeight && description->verticalScrollBar()->maximum() == 0; }));
        description->setFont(originalFont); VERIFY(until([&] { return description->height() == wideHeight; }));
        scroll->verticalScrollBar()->setValue(0);
        const QPoint position(20, 20);
        QWheelEvent wheel(position, description->viewport()->mapToGlobal(position), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(description->viewport(), &wheel);
        VERIFY(scroll->verticalScrollBar()->value() > 0 && description->verticalScrollBar()->value() == 0);
        description->setFocus(); description->setTextCursor(end); QApplication::processEvents();
        VERIFY(until([&] { return scroll->verticalScrollBar()->value() > 0 && description->verticalScrollBar()->value() == 0; }));
        description->insertPlainText("\n新增说明"); VERIFY(until([&] { return description->verticalScrollBar()->maximum() == 0; }));
        manager.findChild<QPushButton *>("protocolSave")->click();
        ProtocolProfileStore store(path); VERIFY(store.load(error) && store.snapshot().profiles[0].description == longDescription + "\n新增说明");
        auto editor = manager.findChild<ProtocolEditorWidget *>();
        for (const auto &definition : {ProtocolEditorWidget::example(true), ProtocolEditorWidget::dynamicArrayExample(), ProtocolProfileValidator::keyValueTemplate(true)}) {
            auto fixture = definition; fixture.insert("description", longDescription); ProtocolProfile profile;
            VERIFY(ProtocolProfileValidator::profile(fixture, profile, error)); editor->setProfile(profile);
            VERIFY(until([&] { return description->height() > 500 && description->verticalScrollBar()->maximum() == 0; }));
        }
        description->setPlainText("简短说明"); VERIFY(until([&] { return description->height() < 150 && description->verticalScrollBar()->maximum() == 0; }));
        description->clear(); VERIFY(until([&] { return description->height() < 150 && description->verticalScrollBar()->maximum() == 0; }));
        editor->setProfile(store.snapshot().profiles[0]);
        VERIFY(until([&] { return description->height() > 500 && description->verticalScrollBar()->maximum() == 0; }));
        scroll->verticalScrollBar()->setValue(0); QApplication::processEvents();
        if (argc > 1) manager.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-full-description.png");
        manager.findChild<QPushButton *>("protocolSave")->click();
        manager.close();
    }
    VERIFY(delimitedUiTests(temporary.filePath("delimited-ui.json"), argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString()) == 0);
    VERIFY(channelFieldInputTests(temporary.filePath("channel-fields.json"), argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString()) == 0);
    VERIFY(diagnosticPreviewTests(temporary.filePath("diagnostic-preview.json"), argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString()) == 0);
    std::cout << "Protocol manager CRUD, preview, import/export, error diagnostics and persistence passed.\n"; return 0;
}
