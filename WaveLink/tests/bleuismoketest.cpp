#include "mainwindow.h"
#include "thememanager.h"
#include "testregistrysandbox.h"
#include "fakeserialbackend.h"
#include <QApplication>
#include <QFontDatabase>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QUuid>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QPushButton>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QMimeData>
#include <QJsonDocument>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QToolButton>
#include <QMessageBox>
#include <QGroupBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include "ble/bleprofilestore.h"
#include <iostream>

#define VERIFY(condition) do { if (!(condition)) { std::cerr << "UI failure at line " << __LINE__ << ": " #condition << "\n"; return 1; } } while (0)
static bool until(std::function<bool()> condition, int timeout = 5000)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(3); }
    return condition();
}
class UnexpectedMessageTrap : public QObject {
public:
    bool encountered = false;
    bool eventFilter(QObject *object, QEvent *event) override {
        auto box = qobject_cast<QMessageBox *>(object);
        if (!box || event->type() != QEvent::Show) return false;
        // Qt 5's Windows system-menu code cannot show native QMessageBox on
        // the offscreen platform. Report unexpected application errors rather
        // than crashing inside Qt and losing the real failure reason.
        encountered = true;
        std::cerr << "Unexpected application message: " << box->windowTitle().toUtf8().constData() << " / " << box->text().toUtf8().constData() << "\n";
        QTimer::singleShot(0, box, &QDialog::reject); return true;
    }
};

static int testSerialBreak(QApplication &app)
{
    QSettings config("WaveLink", "WaveLink");
    // Even legacy/imported keys claiming Break is on must never restore it.
    config.setValue("Serial/BreakEnabled", true);
    config.setValue("chkSerialBreak", true);
    config.remove("Serial/BreakDurationMs"); config.sync();
    auto handler = new TestSerialHandler(new FakePort);
    {
        MainWindow window(nullptr, handler); window.show(); app.processEvents();
        auto hardwarePoll = window.findChild<QTimer *>("serialHardwareCheckTimer"); VERIFY(hardwarePoll); hardwarePoll->stop();
        auto interfaces = window.findChild<QComboBox *>("comboInterface");
        auto ports = window.findChild<QComboBox *>("comboPorts");
        auto open = window.findChild<QPushButton *>("btnOpenClose");
        auto hold = window.findChild<QCheckBox *>("chkSerialBreak");
        auto duration = window.findChild<QSpinBox *>("spinSerialBreakMs");
        auto pulse = window.findChild<QPushButton *>("btnSerialBreak");
        auto send = window.findChild<QPushButton *>("btnSend");
        auto file = window.findChild<QPushButton *>("btnSendFile");
        auto group = window.findChild<QGroupBox *>("groupDeviceConnection");
        VERIFY(interfaces && ports && open && hold && duration && pulse && send && file && group);
        VERIFY(!hold->isChecked() && !hold->isEnabled() && !pulse->isEnabled() && duration->value() == 250);
        VERIFY(hold->toolTip().contains(QStringLiteral("默认释放")) && duration->toolTip() == pulse->toolTip());
        VERIFY(hold->mapTo(group, QPoint()).y() > window.findChild<QCheckBox *>("chkDtr")->mapTo(group, QPoint()).y());
        VERIFY(hold->mapTo(group, QPoint()).y() < open->mapTo(group, QPoint()).y());
        for (int i = 1; i < interfaces->count(); ++i) { interfaces->setCurrentIndex(i); VERIFY(!hold->isVisible() && !pulse->isVisible()); }
        interfaces->setCurrentIndex(0);
        ports->clear(); ports->addItem("FAKE_SOFTWARE_ONLY", "FAKE_SOFTWARE_ONLY");
        VERIFY(until([&] { return open->isEnabled(); })); // Wait for the asynchronous protocol acknowledgement.
        open->click(); VERIFY(until([&] { return hold->isEnabled(); }));
        VERIFY(handler->breakAssertions == 0 && handler->breakReleases == 1 && !hold->isChecked() && send->isEnabled());
        hold->click(); VERIFY(!send->isEnabled() && !file->isEnabled()); // Immediate interlock, before worker acknowledgement.
        VERIFY(until([&] { return hold->isChecked() && pulse->text() == QStringLiteral("释放") && pulse->isEnabled(); }));
        VERIFY(handler->breakLineState && !duration->isEnabled());
        VERIFY(QMetaObject::invokeMethod(&window, "sendPanelData", Q_ARG(QByteArray, QByteArray("blocked")), Q_ARG(bool, false)));
        VERIFY(QMetaObject::invokeMethod(&window, "onStartSendFile"));
        VERIFY(handler->getTxCount() == 0);
        pulse->click(); VERIFY(until([&] { return !hold->isChecked() && hold->isEnabled() && send->isEnabled(); }));
        VERIFY(!handler->breakLineState && file->isEnabled());
        VERIFY(QMetaObject::invokeMethod(&window, "sendPanelData", Q_ARG(QByteArray, QByteArray("hello")), Q_ARG(bool, false)));
        VERIFY(until([&] { return handler->getTxCount() == 5; }));
        duration->setValue(250); pulse->click();
        VERIFY(until([&] { return pulse->text() == QStringLiteral("释放") && !hold->isEnabled(); }));
        VERIFY(!hold->isChecked() && !send->isEnabled());
        VERIFY(until([&] { return pulse->text() == QStringLiteral("发送") && hold->isEnabled() && send->isEnabled(); }));
        VERIFY(!handler->breakLineState && handler->getTxCount() == 5);
        duration->setValue(345); VERIFY(config.value("Serial/BreakDurationMs").toInt() == 345);

        // Close and reopen from a continuous hold: no saved assertion/replay.
        hold->click(); VERIFY(until([&] { return pulse->text() == QStringLiteral("释放"); }));
        open->click(); VERIFY(until([&] { return !handler->breakLineState.load(); }));
        VERIFY(!hold->isChecked() && !hold->isEnabled());
        VERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("持续保持中")));
        const int assertions = handler->breakAssertions;
        open->click(); VERIFY(until([&] { return hold->isEnabled(); }));
        VERIFY(!hold->isChecked() && handler->breakAssertions == assertions);

        // Configuration and communication errors use copyable themed dialogs.
        QTimer dialogs; dialogs.setInterval(5);
        int warnings = 0, fatal = 0; bool copyable = true;
        QObject::connect(&dialogs, &QTimer::timeout, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (dialog->windowTitle() == QStringLiteral("串口 Break 控制提示")) ++warnings;
            else if (dialog->windowTitle() == QStringLiteral("串口通信异常")) ++fatal;
            else return;
            auto details = dialog->findChild<QPlainTextEdit *>();
            QPushButton *copy = nullptr;
            for (auto button : dialog->findChildren<QPushButton *>()) if (button->text().contains(QStringLiteral("复制"))) copy = button;
            copyable = copyable && details && details->isReadOnly() && copy;
            if (copy && details) { copy->click(); copyable = copyable && QApplication::clipboard()->text() == details->toPlainText(); }
            dialog->reject();
        }); dialogs.start();
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->breakEnableError = QSerialPort::UnsupportedOperationError; }, Qt::BlockingQueuedConnection));
        hold->click(); VERIFY(until([&] { return warnings == 1 && send->isEnabled(); }));
        VERIFY(!hold->isChecked() && !hold->isEnabled() && !pulse->isEnabled() && !handler->breakLineState);
        VERIFY(open->text().contains(QStringLiteral("关闭串口")) && copyable);
        open->click();
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->breakEnableError = QSerialPort::NoError; }, Qt::BlockingQueuedConnection));
        open->click(); VERIFY(until([&] { return hold->isEnabled(); }));
        hold->click(); VERIFY(until([&] { return pulse->text() == QStringLiteral("释放"); }));
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->breakReleaseError = QSerialPort::UnknownError; }, Qt::BlockingQueuedConnection));
        pulse->click(); VERIFY(until([&] { return fatal == 1 && interfaces->isEnabled(); }));
        VERIFY(!hold->isChecked() && !hold->isEnabled() && copyable);
        dialogs.stop();
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->breakReleaseError = QSerialPort::NoError; }, Qt::BlockingQueuedConnection));
        interfaces->setCurrentIndex(interfaces->findData(3)); VERIFY(until([&] { return open->isEnabled(); })); open->click();
        VERIFY(QMetaObject::invokeMethod(&window, "handleSerialBreakChanged", Q_ARG(quint64, 0), Q_ARG(bool, true),
            Q_ARG(bool, false), Q_ARG(bool, true), Q_ARG(QString, QString("stale Break warning"))));
        VERIFY(!hold->isChecked() && !hold->isVisible() && send->isEnabled());
        open->click(); interfaces->setCurrentIndex(0);
        ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); app.processEvents();
        window.grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("serial-break-dark.png"));
        ThemeManager::applyTheme(&app, ThemeManager::LightTheme); app.processEvents();
        window.grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("serial-break-light.png"));
        ThemeManager::applyTheme(&app, ThemeManager::DarkTheme);
        VERIFY(until([&] { return open->isEnabled(); }));
        open->click(); VERIFY(until([&] { return hold->isEnabled(); }));
        hold->click(); VERIFY(until([&] { return pulse->text() == QStringLiteral("释放"); }));
        window.close(); VERIFY(until([&] { return !handler->breakLineState.load(); }));
    }
    // A new window must still start released, but may remember pulse duration.
    {
        MainWindow restart(nullptr, new TestSerialHandler(new FakePort));
        auto hold = restart.findChild<QCheckBox *>("chkSerialBreak");
        auto duration = restart.findChild<QSpinBox *>("spinSerialBreakMs");
        VERIFY(hold && duration && !hold->isChecked() && !hold->isEnabled() && duration->value() == 345);
        restart.close();
    }
    config.remove("Serial/BreakEnabled"); config.remove("chkSerialBreak");
    config.setValue("Serial/DTR", false); config.setValue("Serial/RTS", false);
    config.sync();
    return 0;
}

static int testHardwareFlow(QApplication &app, const QString &directory)
{
    QSettings config("WaveLink", "WaveLink");
    config.remove("Serial/FlowControl"); config.sync();
    auto handler = new TestSerialHandler(new FakePort);
    handler->ctsState = 0;
    {
        MainWindow window(nullptr, handler); window.show(); app.processEvents();
        auto poll = window.findChild<QTimer *>("serialHardwareCheckTimer"); VERIFY(poll); poll->stop();
        auto interfaces = window.findChild<QComboBox *>("comboInterface");
        auto ports = window.findChild<QComboBox *>("comboPorts");
        auto open = window.findChild<QPushButton *>("btnOpenClose");
        auto flow = window.findChild<QComboBox *>("comboFlowControl");
        auto rts = window.findChild<QCheckBox *>("chkRts");
        auto dtr = window.findChild<QCheckBox *>("chkDtr");
        auto hold = window.findChild<QCheckBox *>("chkSerialBreak");
        auto send = window.findChild<QPushButton *>("btnSend");
        auto file = window.findChild<QPushButton *>("btnSendFile");
        auto cancel = window.findChild<QPushButton *>("btnCancelSerialSend");
        auto stopFile = window.findChild<QPushButton *>("btnStopSendFile");
        auto status = window.findChild<QLabel *>("lblSerialSendStatus");
        auto progress = window.findChild<QProgressBar *>("progressBarFile");
        auto path = window.findChild<QLineEdit *>("txtSendFilePath");
        VERIFY(interfaces && ports && open && flow && rts && dtr && hold && send && file && cancel && stopFile && status && progress && path);
        QTimer dialogs; dialogs.setInterval(5);
        int cancelledDialogs = 0, openErrors = 0; bool copyable = true;
        QObject::connect(&dialogs, &QTimer::timeout, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            if (dialog->windowTitle() == QStringLiteral("发送已取消，串口已关闭")) ++cancelledDialogs;
            else if (dialog->windowTitle() == QStringLiteral("串口打开失败")) ++openErrors;
            else return;
            auto details = dialog->findChild<QPlainTextEdit *>(); QPushButton *copy = nullptr;
            for (auto b : dialog->findChildren<QPushButton *>()) if (b->text().contains(QStringLiteral("复制"))) copy = b;
            copyable = copyable && details && details->isReadOnly() && copy;
            if (details && copy) { copy->click(); copyable = copyable && QApplication::clipboard()->text() == details->toPlainText(); }
            dialog->reject();
        }); dialogs.start();
        VERIFY(flow->count() == 2 && flow->currentData().toInt() == 0 && !cancel->isVisible());
        VERIFY(flow->toolTip().contains(QStringLiteral("RTS/CTS")));
        rts->setChecked(true); flow->setCurrentIndex(1);
        VERIFY(!rts->isEnabled() && !rts->isChecked() && rts->text().contains(QStringLiteral("自动")));
        VERIFY(config.value("Serial/FlowControl").toInt() == 1 && config.value("Serial/RTS").toBool());
        flow->setCurrentIndex(0); VERIFY(rts->isChecked() && rts->isEnabled()); flow->setCurrentIndex(1);
        ports->clear(); ports->addItem("FAKE_SOFTWARE_ONLY", "FAKE_SOFTWARE_ONLY");
        open->click(); VERIFY(until([&] { return hold->isEnabled(); }));
        VERIFY(!flow->isEnabled() && !rts->isEnabled() && dtr->isEnabled() && cancel->isVisible());
        dtr->click(); VERIFY(until([&] { return dtr->isEnabled(); }));
        VERIFY(config.value("Serial/RTS").toBool()); // Automatic RTS must not erase the manual preference.
        VERIFY(QMetaObject::invokeMethod(&window, "sendPanelData", Q_ARG(QByteArray, QByteArray("wait")), Q_ARG(bool, false)));
        VERIFY(until([&] { return status->text().contains(QStringLiteral("等待 CTS")) && cancel->isEnabled(); }));
        VERIFY(handler->pendingSendBytes() == 4 && handler->getTxCount() == 0 && status->text().contains(QStringLiteral("4 B")));
        cancel->click(); VERIFY(until([&] { return !cancel->isEnabled() && send->isEnabled() && handler->pendingSendBytes() == 0; }));
        VERIFY(cancelledDialogs == 1 && interfaces->isEnabled() && copyable);
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->ctsState = 1; }, Qt::BlockingQueuedConnection));
        QElapsedTimer elapsed; elapsed.start(); VERIFY(until([&] { return elapsed.elapsed() >= 250; }));
        VERIFY(handler->getTxCount() == 0); // CTS returning must not replay cancelled commands.
        open->click(); VERIFY(until([&] { return hold->isEnabled(); }));

        QFile source(QDir(directory).filePath("serial-flow-file.bin")); VERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray content = QByteArray("SERIAL_FLOW_\x11\x13\x00", 15).repeated(850);
        VERIFY(source.write(content) == content.size()); source.close(); path->setText(source.fileName());
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->ctsState = 0; }, Qt::BlockingQueuedConnection));
        file->click(); VERIFY(until([&] { return handler->pendingSendBytes() > 0 && status->text().contains(QStringLiteral("等待 CTS")); }));
        VERIFY(handler->pendingSendBytes() <= 4096 && progress->value() == 0 && !send->isEnabled() && !hold->isEnabled());
        elapsed.restart(); VERIFY(until([&] { return elapsed.elapsed() >= 250; }));
        VERIFY(handler->pendingSendBytes() <= 4096 && handler->getTxCount() == 0 && progress->value() != progress->maximum());
        VERIFY(QMetaObject::invokeMethod(&window, "sendPanelData", Q_ARG(QByteArray, QByteArray("must-not-mix")), Q_ARG(bool, false)));
        stopFile->click(); VERIFY(until([&] { return file->isEnabled() && handler->pendingSendBytes() == 0; }));
        VERIFY(cancelledDialogs == 2 && interfaces->isEnabled());
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->ctsState = 1; }, Qt::BlockingQueuedConnection));
        open->click(); VERIFY(until([&] { return hold->isEnabled(); }));
        file->click(); VERIFY(until([&] { return file->isEnabled() && progress->value() == progress->maximum(); }));
        bool exact = false;
        VERIFY(QMetaObject::invokeMethod(handler, [&] { exact = handler->fake->written == content; }, Qt::BlockingQueuedConnection));
        VERIFY(exact && handler->getTxCount() == content.size()); // Binary control bytes were not consumed as XON/XOFF.

        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->fake->holdWrites = true; }, Qt::BlockingQueuedConnection));
        file->click(); VERIFY(until([&] { return handler->pendingSendBytes() == 4096; }));
        bool nativePending = false;
        VERIFY(QMetaObject::invokeMethod(handler, [&] { nativePending = handler->fake->pendingBytes == 4096; }, Qt::BlockingQueuedConnection));
        VERIFY(nativePending && handler->getTxCount() == content.size());
        VERIFY(progress->value() == 0 && !file->isEnabled());
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->ctsState = 0; }, Qt::BlockingQueuedConnection));
        VERIFY(until([&] { return status->text().contains(QStringLiteral("等待 CTS")); }));
        stopFile->click(); VERIFY(until([&] { return cancelledDialogs == 3 && interfaces->isEnabled(); }));
        VERIFY(handler->pendingSendBytes() == 0 && flow->isEnabled() && !hold->isChecked() && copyable && progress->value() != progress->maximum());
        VERIFY(QMetaObject::invokeMethod(handler, [&] { handler->fake->holdWrites = false; handler->ctsState = 1; handler->flowError = QSerialPort::UnsupportedOperationError; }, Qt::BlockingQueuedConnection));
        open->click(); VERIFY(until([&] { return openErrors == 1 && interfaces->isEnabled(); }));
        VERIFY(flow->currentData().toInt() == 1 && !hold->isEnabled() && copyable); // No silent fallback.
        dialogs.stop();
        ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); app.processEvents();
        window.grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("serial-hardware-flow-dark.png"));
        ThemeManager::applyTheme(&app, ThemeManager::LightTheme); app.processEvents();
        window.grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("serial-hardware-flow-light.png"));
        ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); window.close();
    }
    {
        MainWindow restart(nullptr, new TestSerialHandler(new FakePort));
        auto flow = restart.findChild<QComboBox *>("comboFlowControl");
        auto rts = restart.findChild<QCheckBox *>("chkRts");
        VERIFY(flow && rts && flow->currentData().toInt() == 1 && !rts->isEnabled());
        flow->setCurrentIndex(0); VERIFY(rts->isEnabled() && rts->isChecked());
        restart.close();
    }
    config.setValue("Serial/FlowControl", 0); config.setValue("Serial/DTR", false); config.setValue("Serial/RTS", false); config.sync();
    return 0;
}

int main(int argc, char **argv)
{
    TestRegistrySandbox registry;
    VERIFY(registry.ready()); // Do not start the app unless isolation succeeded.
    QApplication app(argc, argv);
    // The protocol library uses AppDataLocation, independently of QSettings.
    // Give this fixture its own library so real or previous test selections
    // cannot override the legacy settings prepared below.
    app.setApplicationName("WaveLinkBleUi-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    struct ClipboardRestore {
        QMimeData *saved = new QMimeData;
        ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
        ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
    } clipboardRestore;
    UnexpectedMessageTrap unexpected; app.installEventFilter(&unexpected);
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) QApplication::setFont(QFont(families.first(), 9));
    QTemporaryDir settingsDirectory;
    VERIFY(settingsDirectory.isValid());
    QStandardPaths::setTestModeEnabled(true);
    QSettings advanced("WaveLink", "AdvancedApp"); advanced.setValue("InterfaceType", 0);
    QSettings config("WaveLink", "WaveLink"); config.setValue("comboProtocol", 1);
    // Recovery is scheduled during construction, so isolate its disk path
    // BEFORE creating any window, not just before the recording test.
    config.setValue("editCacheDir", settingsDirectory.path());
    const QJsonObject fakeProfile{{"schemaVersion", 2}, {"rxService", "FFE0"}, {"rxUuid", "FFE1"}, {"txService", "FFE0"}, {"txUuid", "FFE1"}, {"stream", true}};
    QString profileError; VERIFY(BleProfileStore(config).saveTemplate("Fake UART fixture", fakeProfile, false, profileError));
    config.sync(); VERIFY(registry.containsApplicationSettings());
    VERIFY(testSerialBreak(app) == 0);
    VERIFY(testHardwareFlow(app, settingsDirectory.path()) == 0);
    MainWindow window;
    window.show(); app.processEvents();
    auto interfaces = window.findChild<QComboBox *>("comboInterface");
    auto open = window.findChild<QPushButton *>("btnOpenClose");
    VERIFY(interfaces && open && interfaces->count() == 6);
    auto dtr = window.findChild<QCheckBox *>("chkDtr");
    auto rts = window.findChild<QCheckBox *>("chkRts");
    VERIFY(dtr && rts && dtr->isVisible() && rts->isVisible());
    VERIFY(!dtr->isChecked() && !rts->isChecked() && dtr->isEnabled() && rts->isEnabled());
    auto groupConnection = window.findChild<QGroupBox *>("groupDeviceConnection");
    VERIFY(groupConnection && dtr->mapTo(groupConnection, QPoint()).y() < open->mapTo(groupConnection, QPoint()).y());
    // Unknown-state UI must not silently change a saved false preference into
    // true when the user changes the other line (Qt treats half-checked as on).
    { const QSignalBlocker blocker(dtr); dtr->setCheckState(Qt::PartiallyChecked); }
    rts->setChecked(true);
    VERIFY(!config.value("Serial/DTR").toBool() && config.value("Serial/RTS").toBool());
    { const QSignalBlocker blocker(dtr); dtr->setTristate(false); dtr->setChecked(false); }
    dtr->setChecked(true); rts->setChecked(true);
    VERIFY(config.value("Serial/DTR").toBool() && config.value("Serial/RTS").toBool());
    rts->setChecked(false);
    VERIFY(config.value("Serial/DTR").toBool() && !config.value("Serial/RTS").toBool());
    // Offline changes must never call QSerialPort or produce error dialogs.
    ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); groupConnection->repaint();
    groupConnection->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("serial-control-lines-dark.png"));
    ThemeManager::applyTheme(&app, ThemeManager::LightTheme); groupConnection->repaint();
    groupConnection->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("serial-control-lines-light.png"));
    ThemeManager::applyTheme(&app, ThemeManager::DarkTheme);
    for (int i = 1; i < interfaces->count(); ++i) {
        interfaces->setCurrentIndex(i);
        VERIFY(!dtr->isVisible() && !rts->isVisible());
    }
    interfaces->setCurrentIndex(0);
    VERIFY(dtr->isVisible() && rts->isVisible() && dtr->isChecked() && !rts->isChecked());
    const QVector<int> order{0, 1, 2, 5, 4, 3}; for (int i = 0; i < order.size(); ++i) VERIFY(interfaces->itemData(i).toInt() == order[i]);
    QComboBox *protocol = nullptr;
    QPushButton *record = nullptr, *file = nullptr, *scan = nullptr;
    for (auto combo : window.findChildren<QComboBox *>()) if (combo->findText("Raw (文本/Hex)") >= 0) protocol = combo;
    for (auto button : window.findChildren<QPushButton *>()) {
        if (button->text().contains(QStringLiteral("开始录制"))) record = button;
        if (button->text() == QStringLiteral("发送文件")) file = button;
        if (button->text() == QStringLiteral("扫描…")) scan = button;
    }
    VERIFY(protocol && record && file && scan);
    VERIFY(until([&] { return protocol->isEnabled() && open->isEnabled(); }));
    protocol->setCurrentIndex(1);
    VERIFY(until([&] { return config.value("comboProtocol").toInt() == 1 && protocol->isEnabled() && open->isEnabled(); }));
    interfaces->setCurrentIndex(interfaces->findData(4));
    // Protocol choice is now shared across all interfaces, including BLE.
    VERIFY(until([&] { return protocol->currentIndex() == 1 && protocol->isEnabled() && open->isEnabled(); }));
    VERIFY(!record->isEnabled() && !file->isEnabled());
    interfaces->setCurrentIndex(0);
    VERIFY(until([&] { return protocol->currentIndex() == 1 && protocol->isEnabled() && record->isEnabled() && file->isEnabled(); }));
    interfaces->setCurrentIndex(interfaces->findData(4));
    VERIFY(until([&] { return protocol->isEnabled() && open->isEnabled(); }));
    // The following BLE cases intentionally exercise Raw (not recordable),
    // so choose it explicitly instead of assuming a per-interface default.
    protocol->setCurrentIndex(0);
    VERIFY(until([&] { return protocol->currentIndex() == 0 && protocol->isEnabled() && open->isEnabled(); }));
    QTimer automation;
    int fileErrorsShown = 0;
    bool fileErrorCopyable = false;
    int profileAction = 0; bool summaryCorrect = false, readOnlyCorrect = false, diagnosticsCorrect = false, templateSafe = false;
    bool customTemplateSafe = false, invalidWriteBlocked = false, checkDuplicate = false, duplicateSafe = false;
    bool reconnectConfirmed = false;
    int configurationDialogs = 0;
    bool gattControlsVisible = false;
    bool advancedVisible = false, advancedConfirmation = false, cancelAdvanced = false, advancedCancelled = false, advancedClickScheduled = false;
    int sendStoppedShown = 0; bool sendStoppedCopyable = false;
    bool profileThemesCaptured = false, configThemesCaptured = false, advancedThemesCaptured = false;
    bool automationBusy = false;
    automation.setInterval(10);
    QObject::connect(&automation, &QTimer::timeout, [&] {
        if (automationBusy) return;
        QScopedValueRollback<bool> automationGuard(automationBusy, true);
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (dialog->windowTitle() == QStringLiteral("重新连接并核对配置？")) {
            if (auto yes = dialog->findChild<QPushButton *>("bleConfirmYes")) { reconnectConfirmed = true; yes->click(); }
            return;
        }
        if (dialog->windowTitle() == QStringLiteral("确认高级发送参数？")) {
            const auto yes = dialog->findChild<QPushButton *>("bleConfirmYes");
            auto buttons = dialog->findChild<QDialogButtonBox *>();
            advancedConfirmation = yes && buttons && buttons->button(QDialogButtonBox::No)->isDefault();
            if (cancelAdvanced) { advancedCancelled = true; dialog->reject(); }
            else if (yes) yes->click();
            return;
        }
        if (dialog->windowTitle() == QStringLiteral("BLE 收发配置与模板")) {
            auto summary = dialog->findChild<QPlainTextEdit *>("bleProfileSummary");
            auto field = dialog->findChild<QLineEdit *>("bleNextRxUuid");
            auto save = dialog->findChild<QPushButton *>("bleSaveNextProfile");
            summaryCorrect = summary && summary->isReadOnly() && summary->toPlainText().contains(QStringLiteral("当前已确认配置"));
            if (!profileThemesCaptured) {
                profileThemesCaptured = true;
                dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-profile-dark.png"));
                ThemeManager::applyTheme(&app, ThemeManager::LightTheme); dialog->repaint();
                dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-profile-light.png"));
                ThemeManager::applyTheme(&app, ThemeManager::DarkTheme);
            }
            auto packet = dialog->findChild<QSpinBox *>("blePacketSize"); auto interval = dialog->findChild<QSpinBox *>("bleSendInterval");
            auto toggle = dialog->findChild<QToolButton *>("bleAdvancedToggle"); auto live = dialog->findChild<QPushButton *>("bleApplySendSettings");
            if (profileAction == 5 || profileAction == 6 || profileAction == 7) {
                if (profileAction == 7 && advancedCancelled) { dialog->reject(); return; }
                if (!advancedClickScheduled && packet && interval && toggle && live && live->isEnabled()) {
                    toggle->setChecked(true); packet->setValue(profileAction == 5 ? 20 : 244); interval->setValue(profileAction == 5 ? 40 : 0);
                    if (auto scroll = dialog->findChild<QScrollArea *>()) scroll->ensureWidgetVisible(live);
                    advancedVisible = packet->isVisible() && interval->isVisible() && packet->minimum() == 1 && packet->maximum() == 512 && interval->minimum() == 0 && interval->maximum() == 1000;
                    if (!advancedThemesCaptured) {
                        advancedThemesCaptured = true;
                        dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-advanced-dark.png"));
                        ThemeManager::applyTheme(&app, ThemeManager::LightTheme); dialog->repaint();
                        dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-advanced-light.png"));
                        ThemeManager::applyTheme(&app, ThemeManager::DarkTheme);
                    }
                    advancedClickScheduled = true; QTimer::singleShot(0, live, &QPushButton::click);
                }
                return;
            }
            if (profileAction == 1 && field && save) { field->setText("FFE2"); save->click(); return; }
            if (profileAction == 4) {
                for (auto button : dialog->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("保存并重新连接…")) { QTimer::singleShot(0, button, &QPushButton::click); return; }
            }
            if (profileAction == 2) readOnlyCorrect = field && field->isReadOnly() && save && !save->isEnabled() && packet && !packet->isEnabled() && interval && !interval->isEnabled() && live && !live->isEnabled();
            dialog->reject(); return;
        }
        if (dialog->windowTitle() == QStringLiteral("BLE 连接诊断")) {
            auto text = dialog->findChild<QPlainTextEdit *>("bleDiagnosticText");
            auto identity = dialog->findChild<QCheckBox *>("bleDiagnosticIdentity");
            const auto device = QJsonDocument::fromJson(QSettings("WaveLink", "WaveLink").value("Ble/LastDevice").toByteArray()).object();
            diagnosticsCorrect = text && identity && text->isReadOnly() && !identity->isChecked() &&
                text->toPlainText().contains(QStringLiteral("[已脱敏]")) && !text->toPlainText().contains(device.value("name").toString()) &&
                !text->toPlainText().contains(QCoreApplication::applicationDirPath());
            for (auto button : dialog->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("复制全部信息")) button->click();
            diagnosticsCorrect = diagnosticsCorrect && QApplication::clipboard()->text() == text->toPlainText();
            identity->setChecked(true);
            diagnosticsCorrect = diagnosticsCorrect && text->toPlainText().contains(device.value("name").toString());
            dialog->reject(); return;
        }
        if (dialog->windowTitle() == QStringLiteral("BLE 文件发送已停止")) {
            ++fileErrorsShown;
            auto text = dialog->findChild<QPlainTextEdit *>();
            fileErrorCopyable = text && text->isReadOnly() && text->toPlainText().contains(QStringLiteral("不会主动断开"));
            dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-file-error-dark.png"));
            ThemeManager::applyTheme(&app, ThemeManager::LightTheme); dialog->repaint();
            dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-file-error-light.png"));
            ThemeManager::applyTheme(&app, ThemeManager::DarkTheme);
            dialog->accept(); return;
        }
        if (dialog->windowTitle() == QStringLiteral("BLE 发送已安全停止")) {
            ++sendStoppedShown; auto text = dialog->findChild<QPlainTextEdit *>();
            sendStoppedCopyable = text && text->isReadOnly() && text->toPlainText().contains(QStringLiteral("接收和录制保持运行"));
            for (auto button : dialog->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("复制全部信息")) button->click();
            sendStoppedCopyable = sendStoppedCopyable && QApplication::clipboard()->text() == text->toPlainText();
            dialog->accept(); return;
        }
        if (dialog->windowTitle() == QStringLiteral("选择 BLE 设备")) {
            auto table = dialog->findChild<QTableWidget *>();
            if (!table || table->rowCount() == 0) return;
            table->selectRow(0);
            dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-scan-dark.png"));
            for (auto button : dialog->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("选择设备") && button->isEnabled()) { button->click(); return; }
        } else if (dialog->windowTitle() == QStringLiteral("BLE 快速收发配置")) {
            auto rx = dialog->findChild<QComboBox *>("bleReceiveCharacteristic");
            auto tx = dialog->findChild<QComboBox *>("bleTransmitCharacteristic");
            auto stream = dialog->findChild<QCheckBox *>("bleStreamConfirmation");
            if (!rx || !rx->isEnabled()) return;
            ++configurationDialogs;
            if (auto advanced = dialog->findChild<QToolButton *>("bleGattAdvancedToggle")) advanced->setChecked(true);
            if (checkDuplicate) { duplicateSafe = rx->currentIndex() == 0 && tx->currentIndex() == 0; }
            auto templates = dialog->findChild<QComboBox *>("bleTemplateChoice");
            if (templates) {
                templates->setCurrentIndex(2); QMetaObject::invokeMethod(templates, "activated", Q_ARG(int, 2));
                templateSafe = rx->currentIndex() == 0 && tx->currentIndex() == 0 && !stream->isChecked();
                templates->setCurrentIndex(1); QMetaObject::invokeMethod(templates, "activated", Q_ARG(int, 1));
                templates->setCurrentIndex(3); QMetaObject::invokeMethod(templates, "activated", Q_ARG(int, 3));
                if (!checkDuplicate) customTemplateSafe = rx->currentIndex() == 1 && tx->currentIndex() == 1 && !stream->isChecked();
                else duplicateSafe = duplicateSafe && rx->currentIndex() == 0 && tx->currentIndex() == 0;
            }
            rx->setCurrentIndex(1); tx->setCurrentIndex(1); stream->setChecked(true);
            auto mode = dialog->findChild<QComboBox *>("bleWriteMode");
            mode->setCurrentIndex(1);
            for (auto button : dialog->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("应用并开始调试")) invalidWriteBlocked = !button->isEnabled();
            mode->setCurrentIndex(0);
            gattControlsVisible = rx->isVisible() && tx->isVisible() && mode->isVisible() && stream->isVisible();
            if (auto scroll = dialog->findChild<QScrollArea *>("bleGattScroll")) scroll->ensureWidgetVisible(stream);
            if (!configThemesCaptured) {
                configThemesCaptured = true;
                dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-config-dark.png"));
                ThemeManager::applyTheme(&app, ThemeManager::LightTheme); dialog->repaint();
                dialog->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("ble-config-light.png"));
                ThemeManager::applyTheme(&app, ThemeManager::DarkTheme);
            }
            for (auto button : dialog->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("应用并开始调试") && button->isEnabled()) { button->click(); return; }
        }
    });
    automation.start();
    // Test fail-safe if a modal dialog does not reach the expected outcome.
    QTimer safety; safety.setSingleShot(true);
    QObject::connect(&safety, &QTimer::timeout, [&] { if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
    safety.start(30000); // Extended scenarios include several explicit confirmation dialogs.
    scan->click();
    open->click();
    auto client = window.findChild<BleHostClient *>(); VERIFY(client);
    VERIFY(until([&] { return client->state() == BleHostClient::State::Ready; }));
    VERIFY(open->text() == QStringLiteral("断开 BLE 连接") && !interfaces->isEnabled());
    VERIFY(!record->isEnabled() && file->isEnabled());
    VERIFY(templateSafe && customTemplateSafe && invalidWriteBlocked && gattControlsVisible);
    // Successful local configuration + unchanged discovery bypasses the UI,
    // but still awaits the helper's real subscription acknowledgement.
    const int beforeShortcut = configurationDialogs;
    open->click(); VERIFY(until([&] { return client->state() == BleHostClient::State::Idle && open->isEnabled(); }));
    open->click(); VERIFY(until([&] { return client->state() == BleHostClient::State::Ready; }));
    VERIFY(configurationDialogs == beforeShortcut && client->txCount() == 0 && !client->sendBusy());
    auto profilesButton = window.findChild<QPushButton *>("bleProfilesButton");
    auto diagnosticButton = window.findChild<QPushButton *>("bleDiagnosticsButton");
    VERIFY(profilesButton && diagnosticButton);
    profilesButton->click(); VERIFY(summaryCorrect && client->state() == BleHostClient::State::Ready);
    diagnosticButton->click(); VERIFY(diagnosticsCorrect && client->state() == BleHostClient::State::Ready);
    profileAction = 1; profilesButton->click(); profileAction = 0;
    VERIFY(client->state() == BleHostClient::State::Ready); // Next config must NOT hot-replace GATT.
    { QSettings saved("WaveLink", "WaveLink"); BleProfileStore store(saved); QString error;
      const auto device = QJsonDocument::fromJson(saved.value("Ble/LastDevice").toByteArray()).object();
      VERIFY(store.deviceProfile(device, error).value("rxUuid").toString() == BleProfileStore::normalizeUuid("FFE2")); }
    profileAction = 5; advancedClickScheduled = false; profilesButton->click(); profileAction = 0;
    VERIFY(advancedVisible && client->sendSettings()["intervalMs"] == 40 && client->sendSettings()["effectivePacket"] == 20);
    profileAction = 7; cancelAdvanced = true; advancedClickScheduled = false; profilesButton->click(); profileAction = 0; cancelAdvanced = false;
    VERIFY(advancedConfirmation && advancedCancelled && client->sendSettings()["intervalMs"] == 40);
    profileAction = 6; advancedClickScheduled = false; profilesButton->click(); profileAction = 0;
    VERIFY(advancedConfirmation && client->sendSettings()["packetSize"] == 244 && client->sendSettings()["effectivePacket"] == 20 && client->sendSettings()["intervalMs"] == 0);
    { QSettings saved("WaveLink", "WaveLink"); BleProfileStore store(saved); QString error;
      const auto device = QJsonDocument::fromJson(saved.value("Ble/LastDevice").toByteArray()).object(); const auto p = store.deviceProfile(device, error);
      VERIFY(p["schemaVersion"] == 3 && p["packetSize"] == 244 && p["intervalMs"] == 0 && p["rxUuid"].toString() == BleProfileStore::normalizeUuid("FFE2") && !p.contains("advancedConfirmed")); }
    const auto keys = QSettings("WaveLink", "WaveLink").allKeys();
    VERIFY(keys.contains("Ble/LastDevice"));
    VERIFY(std::any_of(keys.begin(), keys.end(), [](const QString &key) { return key.startsWith("Ble/Profiles/"); }));
    // Explicitly confirmed reconnect must rediscover/reconfirm, never restart
    // recording or replay a previous send queue.
    profileAction = 4; profilesButton->click(); profileAction = 0;
    VERIFY(until([&] { return client->state() == BleHostClient::State::Ready; }));
    VERIFY(reconnectConfirmed && client->txCount() == 0 && protocol->isEnabled());
    VERIFY(client->sendData(QByteArray(51, 'a')));
    profileAction = 2; profilesButton->click(); profileAction = 0;
    VERIFY(readOnlyCorrect);
    VERIFY(until([&] { return client->txCount() == 51; }));
    VERIFY(until([&] { return !client->sendBusy() && protocol->isEnabled(); }));
    // Use fake echo notifications through the real GUI -> worker -> recorder.
    window.findChild<QLineEdit *>("editCacheDir")->setText(settingsDirectory.path());
    protocol->setCurrentIndex(1); VERIFY(until([&] { return record->isEnabled() && protocol->isEnabled(); }));
    record->click(); VERIFY(!protocol->isEnabled());
    profileAction = 2; profilesButton->click(); profileAction = 0;
    VERIFY(readOnlyCorrect && client->state() == BleHostClient::State::Ready && !protocol->isEnabled());
    auto sendPath = window.findChild<QLineEdit *>("txtSendFilePath");
    auto stopFile = window.findChild<QPushButton *>("btnStopSendFile");
    auto progress = window.findChild<QProgressBar *>("progressBarFile");
    auto eta = window.findChild<QLabel *>("lblFileSendEta");
    auto etaTimer = window.findChild<QTimer *>("fileSendEtaTimer");
    VERIFY(sendPath && stopFile && progress && eta && etaTimer && eta->text() == "00:00:00");
    sendPath->setText("fake:hang-open"); file->click();
    VERIFY(progress->maximum() == 0 && stopFile->isEnabled() && !file->isEnabled());
    VERIFY(eta->text() == "--:--:--" && etaTimer->isActive());
    QElapsedTimer cancelFile; cancelFile.start(); stopFile->click(); VERIFY(cancelFile.elapsed() < 250);
    VERIFY(until([&] { return !client->fileSendBusy() && file->isEnabled(); }));
    VERIFY(eta->text() == "00:00:00" && !etaTimer->isActive());
    VERIFY(client->state() == BleHostClient::State::Ready && !protocol->isEnabled());
    sendPath->setText(settingsDirectory.filePath("missing.bin")); file->click();
    VERIFY(until([&] { return fileErrorsShown == 1 && !client->fileSendBusy(); }));
    VERIFY(eta->text() == "00:00:00" && !etaTimer->isActive());
    VERIFY(fileErrorCopyable && client->state() == BleHostClient::State::Ready && !protocol->isEnabled());
    VERIFY(client->sendData("1,")); VERIFY(until([&] { return client->txCount() == 53; }));
    VERIFY(client->sendData("2\n3,4\n")); VERIFY(until([&] { return client->txCount() == 59; }));
    VERIFY(until([&] {
        for (auto plot : window.findChildren<QCustomPlot *>()) {
            if (plot->graphCount() >= 2 && plot->graph(0)->data()->size() >= 2 && plot->graph(1)->data()->size() >= 2 &&
                plot->graph(0)->data()->constBegin()->value == 1 && plot->graph(1)->data()->constBegin()->value == 2) return true;
        }
        return false;
    }));
    record->click(); VERIFY(until([&] { return protocol->isEnabled(); }));
    QDir cache(settingsDirectory.path() + "/WaveLink_Cache");
    const auto csvFiles = cache.entryList({"*.csv"}, QDir::Files); VERIFY(csvFiles.size() == 1);
    QFile csv(cache.filePath(csvFiles.first())); VERIFY(csv.open(QIODevice::ReadOnly));
    const auto csvData = csv.readAll(); VERIFY(csvData.contains(",0,1.000000,2.000000") && csvData.contains(",1,3.000000,4.000000"));
    QComboBox *recordingMode = nullptr;
    for (auto combo : window.findChildren<QComboBox *>()) if (combo->currentText().contains("CSV") && combo != protocol) recordingMode = combo;
    VERIFY(recordingMode); recordingMode->setCurrentIndex(1);
    record->click(); VERIFY(client->sendData("5,6\n")); VERIFY(until([&] { return client->txCount() == 63; }));
    // Disconnect while recording: flush tail, restore buttons, no fake success.
    open->click(); VERIFY(until([&] { return client->state() == BleHostClient::State::Idle && interfaces->isEnabled(); }));
    VERIFY(until([&] { return record->text() == QStringLiteral("开始录制"); }));
    VERIFY(record->text() == QStringLiteral("开始录制") && protocol->isEnabled());
    const auto wlrFiles = cache.entryList({"*.wlr"}, QDir::Files); VERIFY(wlrFiles.size() == 1);
    QString recordingError; int recordedFrames = 0;
    VERIFY(WlrRecordingWriter::visitFiles({cache.filePath(wlrFiles.first())}, [&](qint64 time, const QVector<double> &values, qint64) {
        if (time > 1700000000000000000LL && values == QVector<double>({5, 6})) ++recordedFrames;
        return true;
    }, recordingError)); VERIFY(recordedFrames == 1);
    VERIFY(cache.entryList({"*.part"}, QDir::Files).isEmpty());
    window.close(); // Save settings with BLE selected; preserve prior non-BLE protocol.
    MainWindow restored;
    auto restoredDtr = restored.findChild<QCheckBox *>("chkDtr");
    auto restoredRts = restored.findChild<QCheckBox *>("chkRts");
    VERIFY(restoredDtr && restoredRts && restoredDtr->isChecked() && !restoredRts->isChecked());
    auto restoredInterface = restored.findChild<QComboBox *>("comboInterface"); VERIFY(restoredInterface->currentData().toInt() == 4);
    for (auto combo : restored.findChildren<QComboBox *>()) if (combo->findText("Raw (文本/Hex)") >= 0) VERIFY(combo->currentIndex() == 1);
    restoredInterface->setCurrentIndex(0);
    for (auto combo : restored.findChildren<QComboBox *>()) if (combo->findText("Raw (文本/Hex)") >= 0) VERIFY(combo->currentIndex() == 1);
    auto restoredOpen = restored.findChild<QPushButton *>("btnOpenClose");
    restoredInterface->setCurrentIndex(restoredInterface->findData(3)); VERIFY(until([&] { return restoredOpen->isEnabled(); })); restoredOpen->click();
    VERIFY(restoredOpen->text().contains(QStringLiteral("停止 Demo")) && !restoredInterface->isEnabled());
    VERIFY(QMetaObject::invokeMethod(&restored, "handleSerialControlLinesFinished", Q_ARG(quint64, 0),
        Q_ARG(int, 0), Q_ARG(int, 1), Q_ARG(QString, QString("stale serial warning"))));
    VERIFY(restored.findChild<QCheckBox *>("chkDtr")->isChecked() && !restored.findChild<QCheckBox *>("chkRts")->isChecked());
    QElapsedTimer demo; demo.start(); VERIFY(until([&] { return demo.elapsed() >= 150; }));
    restoredOpen->click(); VERIFY(restoredInterface->isEnabled());
    // Existing TCP client routing still sends to a real local socket.
    QTcpServer loopback; VERIFY(loopback.listen(QHostAddress::LocalHost, 0));
    restoredInterface->setCurrentIndex(1);
    restored.findChild<QLineEdit *>("editIP")->setText("127.0.0.1");
    restored.findChild<QSpinBox *>("spinPort")->setValue(loopback.serverPort());
    VERIFY(until([&] { return restoredOpen->isEnabled(); }));
    restoredOpen->click();
    VERIFY(until([&] { return restoredOpen->text().contains(QStringLiteral("断开网络")); }));
    VERIFY(until([&] { return loopback.hasPendingConnections(); }));
    auto socket = loopback.nextPendingConnection(); VERIFY(socket);
    VERIFY(QMetaObject::invokeMethod(&restored, "sendPanelData", Q_ARG(QByteArray, QByteArray("hello")), Q_ARG(bool, false)));
    VERIFY(until([&] { return socket->bytesAvailable() == 5; })); VERIFY(socket->readAll() == "hello");
    restoredOpen->click(); VERIFY(restoredInterface->isEnabled());
    ThemeManager::applyTheme(&app, ThemeManager::LightTheme); restored.resize(750, 1400); restored.show(); app.processEvents();
    ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); restored.resize(1200, 800); app.processEvents();
    restored.close();
    config.setValue("Ble/LastDevice", QJsonDocument(QJsonObject{{"name", "Fake BLE"}, {"address", "7"}, {"addressType", 0}}).toJson());
    VERIFY(BleProfileStore(config).saveDeviceProfile({{"address", "7"}, {"addressType", 0}}, fakeProfile, profileError));
    checkDuplicate = true;
    MainWindow closing; closing.show(); app.processEvents();
    auto closingInterface = closing.findChild<QComboBox *>("comboInterface"); closingInterface->setCurrentIndex(closingInterface->findData(4));
    closing.findChild<QLineEdit *>("editCacheDir")->setText(settingsDirectory.path());
    VERIFY(until([&] { return closing.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    closing.findChild<QPushButton *>("btnOpenClose")->click();
    auto closingClient = closing.findChild<BleHostClient *>();
    VERIFY(until([&] { return closingClient->state() == BleHostClient::State::Ready; }));
    VERIFY(duplicateSafe); // Duplicate UUIDs must never silently select one handle.
    QPushButton *closingRecord = nullptr;
    for (auto button : closing.findChildren<QPushButton *>()) if (button->text().contains(QStringLiteral("开始录制"))) closingRecord = button;
    VERIFY(closingRecord && closingRecord->isEnabled()); closingRecord->click();
    VERIFY(closingClient->sendData("7,8\n")); VERIFY(until([&] { return closingClient->txCount() == 4; }));
    closing.close(); VERIFY(until([&] { return !closing.isVisible(); }));
    VERIFY(cache.entryList({"*.part"}, QDir::Files).isEmpty());
    // Native capability reduction is simulated at the process boundary, with
    // the actual main window/worker/CSV writer still running. No radio access.
    checkDuplicate = false;
    config.setValue("Ble/LastDevice", QJsonDocument(QJsonObject{{"name", "Fake changing capability"}, {"address", "11"}, {"addressType", 0}}).toJson());
    VERIFY(BleProfileStore(config).saveDeviceProfile({{"address", "11"}, {"addressType", 0}}, fakeProfile, profileError));
    config.setValue("comboRecordingMode", 0); config.sync();
    MainWindow capabilityWindow; capabilityWindow.show(); app.processEvents();
    auto capabilityInterface = capabilityWindow.findChild<QComboBox *>("comboInterface"); capabilityInterface->setCurrentIndex(capabilityInterface->findData(4));
    VERIFY(until([&] { return capabilityWindow.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    capabilityWindow.findChild<QPushButton *>("btnOpenClose")->click();
    auto capabilityClient = capabilityWindow.findChild<BleHostClient *>();
    VERIFY(until([&] { return capabilityClient->state() == BleHostClient::State::Ready; }));
    profileAction = 6; advancedClickScheduled = false; capabilityWindow.findChild<QPushButton *>("bleProfilesButton")->click(); profileAction = 0;
    if (capabilityClient->sendSettings()["effectivePacket"] != 244) std::cerr << "Capability apply: " << QJsonDocument(capabilityClient->diagnostics()).toJson().constData() << "\n";
    VERIFY(capabilityClient->sendSettings()["effectivePacket"] == 244);
    QPushButton *capabilityRecord = nullptr, *capabilityFile = nullptr; QComboBox *capabilityProtocol = nullptr;
    for (auto button : capabilityWindow.findChildren<QPushButton *>()) {
        if (button->text().contains(QStringLiteral("开始录制"))) capabilityRecord = button;
        if (button->text() == QStringLiteral("发送文件")) capabilityFile = button;
    }
    for (auto combo : capabilityWindow.findChildren<QComboBox *>()) {
        if (combo->findText("Raw (文本/Hex)") >= 0) capabilityProtocol = combo;
        if (combo->currentText().contains("WLR")) combo->setCurrentIndex(0);
    }
    VERIFY(capabilityRecord && capabilityFile && capabilityProtocol);
    VERIFY(until([&] { return capabilityProtocol->isEnabled() && !capabilityClient->sendBusy(); }));
    // Reset the fake host's initial "preabc" bytes before numeric recording.
    // Each switch must finish before requesting the next one; a disabled UI
    // intentionally rejects changes while the worker acknowledges a rule.
    capabilityProtocol->setCurrentIndex(0);
    VERIFY(until([&] { return capabilityProtocol->isEnabled(); }));
    capabilityProtocol->setCurrentIndex(0);
    VERIFY(until([&] { return capabilityProtocol->isEnabled() && capabilityProtocol->currentIndex() == 0; }));
    capabilityProtocol->setCurrentIndex(1);
    VERIFY(until([&] { return capabilityProtocol->isEnabled() && capabilityRecord->isEnabled(); }));
    const int beforeCapCsv = cache.entryList({"*.csv"}, QDir::Files).size();
    QFile capSource(settingsDirectory.filePath("capability.bin")); VERIFY(capSource.open(QIODevice::WriteOnly));
    VERIFY(capSource.write(QByteArray("1,2\n").repeated(80)) == 320); capSource.close();
    capabilityRecord->click(); VERIFY(!capabilityProtocol->isEnabled());
    capabilityWindow.findChild<QLineEdit *>("txtSendFilePath")->setText(capSource.fileName()); capabilityFile->click();
    VERIFY(until([&] { return sendStoppedShown == 1 && capabilityClient->txCount() == 244 && !capabilityClient->sendBusy(); }));
    VERIFY(sendStoppedCopyable && capabilityClient->state() == BleHostClient::State::Ready && capabilityClient->canReceive() && !capabilityClient->canSend());
    VERIFY(!capabilityProtocol->isEnabled() && capabilityRecord->text().contains(QStringLiteral("停止")) && !capabilityFile->isEnabled());
    capabilityRecord->click(); VERIFY(until([&] { return capabilityProtocol->isEnabled(); }));
    const auto capCsv = cache.entryList({"*.csv"}, QDir::Files, QDir::Time); VERIFY(capCsv.size() == beforeCapCsv + 1);
    QFile capRecording(cache.filePath(capCsv.first())); VERIFY(capRecording.open(QIODevice::ReadOnly));
    VERIFY(capRecording.readAll().count("1.000000,2.000000") == 61);
    capabilityWindow.close(); VERIFY(cache.entryList({"*.part"}, QDir::Files).isEmpty());
    VERIFY(!unexpected.encountered);
    std::cout << "BLE UI selection, subscription, controls, profile persistence, restart and protocol restoration passed.\n";
    return 0;
}
