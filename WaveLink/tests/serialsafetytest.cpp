#include "serialhandler.h"
#include "fakeserialbackend.h"
#include "serialcontrolstate_win.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>

#define VERIFY(condition) do { if (!(condition)) { QTextStream(stderr) << "Serial safety failure at line " << __LINE__ << ": " #condition << Qt::endl; return 1; } } while (0)

static bool until(std::function<bool()> condition, int timeout = 1000)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return condition();
}

static int testBreak()
{
    auto port = new FakePort;
    TestSerialHandler handler(port);
    bool active = false, pulse = false, supported = false;
    int reports = 0, warnings = 0, fatal = 0, rejected = 0, code = 0;
    QObject::connect(&handler, &SerialHandler::breakStateChanged,
        [&](quint64, bool a, bool p, bool s, const QString &warning) {
            active = a; pulse = p; supported = s; ++reports; if (!warning.isEmpty()) ++warnings;
        });
    QObject::connect(&handler, &SerialHandler::errorOccurred, [&](const QString &, int error) { ++fatal; code = error; });
    QObject::connect(&handler, &SerialHandler::sendRejected, [&](quint64, const QString &) { ++rejected; });
    handler.setBreakState(1, true, 20);
    VERIFY(reports == 0 && handler.breakAssertions == 0);
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 1, false, false);
    VERIFY(reports == 1 && supported && !active && !pulse && handler.breakReleases == 1 && handler.breakAssertions == 0);
    handler.setBreakState(2, true, 20); // Stale or other-session commands are ignored.
    VERIFY(reports == 1 && handler.breakAssertions == 0);
    handler.setBreakState(1, true, -1); handler.setBreakState(1, true, 5001);
    VERIFY(warnings == 2 && !active && handler.breakAssertions == 0);
    port->pendingBytes = 1; handler.setBreakState(1, true, 0); port->pendingBytes = 0;
    VERIFY(warnings == 3 && !active && handler.breakAssertions == 0);

    QTemporaryDir dir; VERIFY(dir.isValid());
    bool recordingStarted = false;
    QObject::connect(&handler, &SerialHandler::recordingStartFinished, [&](bool ok, const QString &) { recordingStarted = ok; });
    const QString csv = dir.filePath("break-recording.csv");
    handler.startRecording(csv, 250); VERIFY(recordingStarted);
    handler.setBreakState(1, true, 0); VERIFY(active && !pulse && handler.breakLineState);
    handler.sendData("blocked"); VERIFY(rejected == 1 && handler.getTxCount() == 0 && port->written.isEmpty());
    QVector<double> frame{12.5, -3.0};
    VERIFY(QMetaObject::invokeMethod(&handler, "writeLogData", Qt::DirectConnection, Q_ARG(QVector<double>, frame)));
    handler.setBreakState(1, false, 0); VERIFY(!active && !pulse && !handler.breakLineState);
    handler.sendData("normal"); VERIFY(handler.getTxCount() == 6 && port->written == "normal");
    VERIFY(QMetaObject::invokeMethod(&handler, "writeLogData", Qt::DirectConnection, Q_ARG(QVector<double>, frame)));
    handler.stopRecording(); QFile log(csv); VERIFY(log.open(QIODevice::ReadOnly));
    VERIFY(log.readAll().count("12.500000,-3.000000") == 2); // Receiving/recording continued during Break.
    handler.setBreakState(1, true, 25); VERIFY(active && pulse);
    handler.sendData("never replayed"); VERIFY(rejected == 2 && handler.getTxCount() == 6);
    VERIFY(until([&] { return !active; })); VERIFY(!handler.breakLineState && handler.getTxCount() == 6);
    handler.setBreakState(1, true, 30); handler.setBreakState(1, false, 0);
    const int afterManual = reports;
    QElapsedTimer elapsed; elapsed.start(); VERIFY(until([&] { return elapsed.elapsed() >= 60; }));
    VERIFY(reports == afterManual && !active); // Manual release cancels the pulse timer.
    handler.setBreakState(1, true, 30); handler.closePort(); VERIFY(!port->isOpen() && !handler.breakLineState);
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 2, false, false);
    VERIFY(!active && !pulse && supported);
    handler.setBreakState(1, true, 0); VERIFY(!active);
    handler.setBreakState(2, true, 0); elapsed.restart();
    VERIFY(until([&] { return elapsed.elapsed() >= 60; })); VERIFY(active && !pulse); // Old timer must not release a fresh hold.
    handler.closePort(); VERIFY(!handler.breakLineState);

    // Unsupported startup release never asserts Break or disables ordinary TX.
    handler.breakReleaseError = QSerialPort::UnsupportedOperationError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 3, false, false);
    VERIFY(!active && !supported && port->isOpen() && fatal == 0);
    const int asserts = handler.breakAssertions;
    handler.setBreakState(3, true, 0); VERIFY(handler.breakAssertions == asserts);
    handler.sendData("ok"); VERIFY(port->written.endsWith("ok"));
    handler.closePort(); handler.breakReleaseError = QSerialPort::NoError;
    handler.breakEnableError = QSerialPort::UnsupportedOperationError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 4, false, false);
    handler.setBreakState(4, true, 0);
    VERIFY(!active && !supported && !handler.breakLineState && port->isOpen() && fatal == 0);
    handler.sendData("rollback-ok"); VERIFY(port->written.endsWith("rollback-ok"));

    // Failed release stops recording and closes the port, not a false "ready".
    handler.closePort(); handler.breakEnableError = QSerialPort::NoError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 5, false, false);
    const QString failedCsv = dir.filePath("failed-release.csv");
    handler.startRecording(failedCsv, 250); handler.setBreakState(5, true, 0);
    handler.breakReleaseError = QSerialPort::UnknownError; handler.setBreakState(5, false, 0);
    VERIFY(!port->isOpen() && fatal == 1 && code == QSerialPort::UnknownError && !QFile::exists(failedCsv + ".part"));
    const qint64 tx = handler.getTxCount(); handler.sendData("closed"); VERIFY(handler.getTxCount() == tx);
    handler.breakReleaseError = QSerialPort::NoError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 6, false, false);
    handler.breakEnableError = QSerialPort::ResourceError; handler.failedAfterChange = true;
    handler.setBreakState(6, true, 0); VERIFY(!port->isOpen() && fatal == 2 && code == QSerialPort::ResourceError);
    handler.failedAfterChange = false; handler.breakEnableError = QSerialPort::UnknownError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 7, false, false);
    handler.breakReleaseError = QSerialPort::UnknownError;
    handler.setBreakState(7, true, 0); VERIFY(!port->isOpen() && fatal == 3);

    // Actual worker-thread pulse: the release timer must not block the worker.
    handler.breakEnableError = handler.breakReleaseError = QSerialPort::NoError;
    QThread worker; handler.moveToThread(&worker); worker.start();
    VERIFY(QMetaObject::invokeMethod(&handler, "openPort", Qt::QueuedConnection,
        Q_ARG(QString, QString("FAKE_SOFTWARE_ONLY")), Q_ARG(int, 115200), Q_ARG(int, 8),
        Q_ARG(QString, QString("None")), Q_ARG(QString, QString("1")), Q_ARG(quint64, 8),
        Q_ARG(bool, false), Q_ARG(bool, false), Q_ARG(int, 0)));
    VERIFY(QMetaObject::invokeMethod(&handler, "setBreakState", Qt::QueuedConnection,
        Q_ARG(quint64, 8), Q_ARG(bool, true), Q_ARG(int, 100)));
    std::atomic<bool> workerResponsive{false};
    VERIFY(QMetaObject::invokeMethod(&handler, [&] { workerResponsive = handler.breakLineState.load(); }, Qt::QueuedConnection));
    VERIFY(until([&] { return workerResponsive.load(); }));
    VERIFY(until([&] { return !handler.breakLineState.load(); }));
    auto mainThread = QCoreApplication::instance()->thread();
    VERIFY(QMetaObject::invokeMethod(&handler, [&] { handler.closePort(); handler.moveToThread(mainThread); }, Qt::BlockingQueuedConnection));
    worker.quit(); VERIFY(worker.wait(1000));
    return 0;
}

static int testHardwareFlow()
{
    auto port = new FakePort;
    TestSerialHandler handler(port);
    int opened = 0, failed = 0, fatal = 0, rejected = 0, rts = 0, cts = -1;
    bool cancelled = false, disconnected = false;
    QObject::connect(&handler, &SerialHandler::portOpenFinished, [&](quint64, bool ok, const QString &, int) { if (ok) ++opened; else ++failed; });
    QObject::connect(&handler, &SerialHandler::errorOccurred, [&](const QString &, int) { ++fatal; });
    QObject::connect(&handler, &SerialHandler::sendRejected, [&](quint64, const QString &) { ++rejected; });
    QObject::connect(&handler, &SerialHandler::controlLinesFinished, [&](quint64, int, int rs, const QString &) { rts = rs; });
    QObject::connect(&handler, &SerialHandler::sendStatusChanged, [&](quint64, qint64, int state) { cts = state; });
    QObject::connect(&handler, &SerialHandler::sendCancellationFinished, [&](quint64, bool closed) { cancelled = true; disconnected = closed; });
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 1, false, true, 0);
    VERIFY(opened == 1 && port->flowControl() == QSerialPort::NoFlowControl && rts == 1 && handler.calls.size() == 2);
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 2, false, true, 1);
    VERIFY(opened == 2 && port->flowControl() == QSerialPort::HardwareControl && rts == -2 && handler.calls.size() == 3);
    handler.setControlLines(2, true, false);
    VERIFY(handler.calls.size() == 4 && handler.calls.last() == qMakePair(true, true) && rts == -2);
    handler.ctsState = 0;
    handler.sendData("waiting"); VERIFY(cts == 0 && handler.pendingSendBytes() == 7 && port->written.isEmpty());
    VERIFY(port->pendingBytes == 7 && handler.getTxCount() == 0); // The driver owns the CTS wait, not software polling.
    handler.setBreakState(2, true, 25); VERIFY(handler.breakAssertions == 0); // CTS wait is still pending TX.
    handler.ctsState = 1;
    VERIFY(until([&] { return handler.pendingSendBytes() == 0; }));
    VERIFY(port->written == "waiting" && cts == 1 && fatal == 0);

    for (int i = 0; i < SerialHandler::SendRequestLimit; ++i) VERIFY(handler.enqueueData(2, "x"));
    VERIFY(!handler.enqueueData(2, "too-many") && handler.pendingSendBytes() == SerialHandler::SendRequestLimit);
    handler.cancelPendingSend(2); QCoreApplication::processEvents();
    VERIFY(port->isOpen() && handler.pendingSendBytes() == 0 && port->written == "waiting");

    // Bound calls queued from another thread BEFORE the worker processes them.
    handler.ctsState = 0;
    const QByteArray full(SerialHandler::SendQueueLimit, 'A');
    VERIFY(handler.enqueueData(2, full)); VERIFY(!handler.enqueueData(2, "overflow") && rejected == 2);
    VERIFY(handler.pendingSendBytes() == SerialHandler::SendQueueLimit);
    handler.cancelPendingSend(2); VERIFY(cancelled && !disconnected && port->isOpen() && handler.pendingSendBytes() == 0);
    QCoreApplication::processEvents();
    handler.ctsState = 1; QElapsedTimer elapsed; elapsed.start();
    VERIFY(until([&] { return elapsed.elapsed() >= 230; })); VERIFY(port->written == "waiting"); // No stale replay.

    QTemporaryDir dir; VERIFY(dir.isValid());
    const auto csv = dir.filePath("cts-recording.csv"); handler.startRecording(csv, 250);
    handler.ctsState = 0; VERIFY(handler.enqueueData(2, "cancelled"));
    const QVector<double> frame{42.0};
    VERIFY(QMetaObject::invokeMethod(&handler, "writeLogData", Qt::DirectConnection, Q_ARG(QVector<double>, frame)));
    cancelled = false; handler.cancelPendingSend(2); VERIFY(cancelled && !disconnected && port->isOpen());
    VERIFY(QFile::exists(csv + ".part"));
    VERIFY(QMetaObject::invokeMethod(&handler, "writeLogData", Qt::DirectConnection, Q_ARG(QVector<double>, frame)));
    handler.stopRecording(); QFile log(csv); VERIFY(log.open(QIODevice::ReadOnly)); VERIFY(log.readAll().count("42.000000") == 2);

    // A write already in the driver must be cancelled by closing, not by
    // leaving an aborted Windows OVERLAPPED operation reusable.
    handler.ctsState = 1; port->holdWrites = true; handler.sendData("in-driver");
    VERIFY(handler.pendingSendBytes() == 9 && port->bytesToWrite() == 9);
    cancelled = false; handler.cancelPendingSend(2);
    VERIFY(cancelled && disconnected && !port->isOpen() && handler.pendingSendBytes() == 0 && fatal == 0);
    port->holdWrites = false;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 3, false, true, 0);
    VERIFY(rts == 1 && port->flowControl() == QSerialPort::NoFlowControl);
    const auto beforeStale = port->written;
    VERIFY(!handler.enqueueData(2, "stale")); handler.cancelPendingSend(2); VERIFY(port->isOpen() && port->written == beforeStale);
    // Old GUI calls cannot consume reservations or emit bytes in a new session.
    VERIFY(handler.enqueueData(3, "old-session")); handler.closePort();
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 4, false, false, 1);
    VERIFY(handler.enqueueData(4, "new-session"));
    VERIFY(until([&] { return handler.pendingSendBytes() == 0; }));
    VERIFY(!port->written.contains("old-session") && port->written.endsWith("new-session"));
    handler.ctsError = QSerialPort::UnsupportedOperationError;
    handler.sendData("unknown-pin"); VERIFY(port->isOpen() && cts == -1 && fatal == 0);
    handler.ctsError = QSerialPort::ResourceError; handler.sendData("bad-device");
    VERIFY(!port->isOpen() && fatal == 1);
    handler.ctsError = QSerialPort::NoError; handler.flowError = QSerialPort::UnsupportedOperationError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 5, false, false, 1);
    VERIFY(!port->isOpen() && failed == 1); // Never silently fall back to no flow.
    handler.flowError = QSerialPort::NoError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 6, false, false, 2);
    VERIFY(!port->isOpen() && failed == 2); // Do not accidentally enable XON/XOFF for binary data.
    return 0;
}

class CloseCheckedPort : public FakePort {
public:
    std::function<void()> beforeClose;
    void close() override { if (beforeClose) beforeClose(); FakePort::close(); }
};
class CombinedControlsHandler : public TestSerialHandler {
public:
    explicit CombinedControlsHandler(CloseCheckedPort *port) : TestSerialHandler(port) {}
    QSerialPort::SerialPortError combinedError = QSerialPort::NoError;
    bool dtr = true, rts = true, automaticRts = false;
    int pairCalls = 0, pairWrites = 0;
protected:
    bool supportsCombinedControlLines() const override { return true; }
    bool applyControlLinesTogether(bool nextDtr, bool nextRts, bool hardware,
                                  QSerialPort::SerialPortError &error, QString &message) override {
        ++pairCalls; error = combinedError; message.clear();
        if (error != QSerialPort::NoError) { message = "Injected combined-control failure"; return false; }
        if (dtr != nextDtr || automaticRts != hardware || (!hardware && rts != nextRts)) ++pairWrites;
        dtr = nextDtr; automaticRts = hardware; if (!hardware) rts = nextRts;
        return true;
    }
};
static int testControlLifecycle()
{
#ifdef Q_OS_WIN
    // Check the exact DCB transformation used by the real Windows backend.
    // All non-control-line settings must survive, and unchanged states must
    // avoid a redundant native write. This does not measure physical pulses.
    for (bool dtr : {false, true}) for (bool rts : {false, true}) for (bool hardware : {false, true}) {
        DCB state{}; state.DCBlength = sizeof(state); state.BaudRate = 1000000;
        state.ByteSize = 8; state.Parity = EVENPARITY; state.StopBits = TWOSTOPBITS;
        state.fBinary = TRUE; state.fOutxCtsFlow = hardware; state.fOutX = FALSE; state.fInX = FALSE;
        state.fDtrControl = DTR_CONTROL_HANDSHAKE; state.fRtsControl = RTS_CONTROL_TOGGLE;
        VERIFY(SerialControlState::update(state, dtr, rts, hardware));
        VERIFY(state.fDtrControl == DWORD(dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE));
        VERIFY(state.fRtsControl == DWORD(hardware ? RTS_CONTROL_HANDSHAKE : rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE));
        VERIFY(state.BaudRate == 1000000 && state.ByteSize == 8 && state.Parity == EVENPARITY && state.StopBits == TWOSTOPBITS);
        VERIFY(state.fBinary && state.fOutxCtsFlow == hardware && !state.fOutX && !state.fInX);
        VERIFY(!SerialControlState::update(state, dtr, rts, hardware));
    }
#endif
    auto port = new CloseCheckedPort; CombinedControlsHandler handler(port);
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    VERIFY(!port->settingsRestoredOnClose());
    QT_WARNING_POP
    int opened = 0, failed = 0, fatal = 0, ds = -1, rs = -1, warnings = 0;
    bool readyAfterControls = true, releasedBeforeClose = true;
    QStringList signalOrder;
    QObject::connect(&handler, &SerialHandler::portOpenFinished, [&](quint64, bool ok, const QString &, int) {
        if (ok) { ++opened; readyAfterControls = readyAfterControls && !handler.dtr && !handler.rts && !handler.breakLineState; signalOrder << "ready"; }
        else ++failed;
    });
    QObject::connect(&handler, &SerialHandler::controlLinesFinished, [&](quint64, int dtr, int rts, const QString &warning) {
        ds = dtr; rs = rts; if (!warning.isEmpty()) ++warnings; signalOrder << "controls";
    });
    QObject::connect(&handler, &SerialHandler::breakStateChanged, [&](quint64, bool, bool, bool, const QString &) { signalOrder << "break"; });
    QObject::connect(&handler, &SerialHandler::errorOccurred, [&](const QString &, int) { ++fatal; });
    port->beforeClose = [&] { releasedBeforeClose = releasedBeforeClose && !handler.dtr && !handler.rts; };
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 1, false, false);
    VERIFY(opened == 1 && readyAfterControls && ds == 0 && rs == 0 && handler.pairCalls == 1 && handler.pairWrites == 1);
    VERIFY(signalOrder == QStringList({"ready", "controls", "break"}) && handler.calls.isEmpty());
    VERIFY(port->written.isEmpty() && handler.getTxCount() == 0 && handler.breakAssertions == 0);
    handler.setControlLines(1, false, false); VERIFY(handler.pairCalls == 1);
    handler.dtr = handler.rts = true; // Simulate driver-side state changing.
    handler.closePort(); VERIFY(releasedBeforeClose && handler.pairCalls == 2 && !port->isOpen());
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 2, false, false);
    const int beforeStale = handler.pairCalls;
    handler.setControlLines(1, true, true); VERIFY(handler.pairCalls == beforeStale && !handler.dtr && !handler.rts);
    handler.setControlLines(2, true, true); VERIFY(handler.pairCalls == beforeStale + 1 && ds == 1 && rs == 1);
    handler.setControlLines(2, false, false); VERIFY(handler.pairCalls == beforeStale + 2 && ds == 0 && rs == 0);
    handler.closePort(); VERIFY(releasedBeforeClose);
    port->beforeClose = {}; // Unsupported states below intentionally stay unknown.

    handler.combinedError = QSerialPort::UnsupportedOperationError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 3, false, false);
    VERIFY(port->isOpen() && warnings == 1 && ds == -1 && rs == -1 && handler.calls.isEmpty());
    handler.closePort();
    const int beforeFailure = opened;
    handler.combinedError = QSerialPort::ResourceError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 4, false, false);
    VERIFY(!port->isOpen() && opened == beforeFailure && failed == 1 && fatal == 0);
    handler.combinedError = QSerialPort::NoError;
    handler.breakReleaseError = QSerialPort::ResourceError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 5, false, false);
    VERIFY(!port->isOpen() && opened == beforeFailure && failed == 2 && fatal == 0);
    handler.breakReleaseError = QSerialPort::NoError;

    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 6, false, false, 1);
    VERIFY(port->isOpen() && handler.automaticRts && ds == 0 && rs == -2 && handler.calls.isEmpty());
    handler.setControlLines(6, true, false); VERIFY(handler.dtr && handler.automaticRts && ds == 1 && rs == -2);
    handler.setControlLines(6, false, false); handler.closePort();
    VERIFY(fatal == 0 && handler.breakAssertions == 0 && port->written.isEmpty());
    return 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    VERIFY(testControlLifecycle() == 0);
    VERIFY(testBreak() == 0);
    VERIFY(testHardwareFlow() == 0);
    auto port = new FakePort;
    TestSerialHandler handler(port);
    int opened = 0, fatal = 0, completed = 0, warnings = 0;
    int dtr = -2, rts = -2, fatalCode = 0;
    QObject::connect(&handler, &SerialHandler::portOpenFinished, [&](quint64, bool success, const QString &, int) { if (success) ++opened; });
    QObject::connect(&handler, &SerialHandler::errorOccurred, [&](const QString &, int code) { ++fatal; fatalCode = code; });
    QObject::connect(&handler, &SerialHandler::controlLinesFinished, [&](quint64, int ds, int rs, const QString &warning) {
        ++completed; dtr = ds; rts = rs; if (!warning.isEmpty()) ++warnings;
    });

    handler.setControlLines(1, true, true);
    VERIFY(handler.calls.isEmpty() && completed == 0 && fatal == 0);
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 1, false, false);
    VERIFY(opened == 1 && fatal == 0 && completed == 1 && warnings == 0);
    VERIFY(dtr == 0 && rts == 0 && handler.calls.size() == 2);
    VERIFY(port->flowControl() == QSerialPort::NoFlowControl);
    handler.setControlLines(1, true, false);
    VERIFY(handler.calls.size() == 3 && handler.calls.last() == qMakePair(true, true));
    handler.setControlLines(1, true, true);
    VERIFY(handler.calls.size() == 4 && handler.calls.last() == qMakePair(false, true) && dtr == 1 && rts == 1);
    handler.setControlLines(1, true, true);
    VERIFY(handler.calls.size() == 4); // Never pulse unchanged lines.
    handler.closePort();
    handler.setControlLines(1, false, false);
    VERIFY(handler.calls.size() == 4 && fatal == 0);
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 2, true, true);
    VERIFY(handler.calls.size() == 6 && dtr == 1 && rts == 1);
    handler.setControlLines(1, false, false); // Delayed old-session request.
    VERIFY(handler.calls.size() == 6 && dtr == 1 && rts == 1);

    handler.dtrError = QSerialPort::UnsupportedOperationError;
    handler.setControlLines(2, false, true);
    VERIFY(port->isOpen() && fatal == 0 && warnings == 1 && dtr == -1 && rts == 1);
    handler.setControlLines(2, false, false);
    VERIFY(handler.calls.size() == 8 && warnings == 1 && dtr == -1 && rts == 0);

    // Nonfatal unsupported-line warnings must not stop an ongoing recording.
    QTemporaryDir dir; VERIFY(dir.isValid());
    bool recordingStarted = false;
    QObject::connect(&handler, &SerialHandler::recordingStartFinished, [&](bool success, const QString &) { recordingStarted = success; });
    const QString csv = dir.filePath("control-test.csv");
    handler.startRecording(csv, 250);
    VERIFY(recordingStarted && QFile::exists(csv + ".part"));
    handler.setControlLines(2, true, false);
    VERIFY(port->isOpen() && fatal == 0 && warnings == 2 && QFile::exists(csv + ".part"));
    handler.stopRecording();
    handler.closePort();
    handler.rtsError = QSerialPort::UnknownError;
    handler.openPort("FAKE_SOFTWARE_ONLY", 115200, 8, "None", "1", 3, true, true);
    VERIFY(port->isOpen() && dtr == -1 && rts == -1 && fatal == 0 && warnings == 3);

    // Disconnect errors still close safely and are not swallowed by warnings.
    handler.dtrError = QSerialPort::ResourceError;
    handler.failedAfterChange = true; // Subsequent unsupported error must not mask disconnect.
    const int beforeFatal = handler.calls.size();
    handler.setControlLines(3, false, true);
    VERIFY(!port->isOpen() && fatal == 1 && fatalCode == QSerialPort::ResourceError);
    VERIFY(handler.calls.size() == beforeFatal + 1);
    handler.setControlLines(3, true, false);
    VERIFY(handler.calls.size() == beforeFatal + 1);

    // Exercise the exact queued open/set signatures used by the GUI.
    handler.dtrError = handler.rtsError = QSerialPort::NoError;
    handler.failedAfterChange = false;
    QThread worker;
    handler.moveToThread(&worker);
    worker.start();
    VERIFY(QMetaObject::invokeMethod(&handler, "openPort", Qt::QueuedConnection,
        Q_ARG(QString, QString("FAKE_SOFTWARE_ONLY")), Q_ARG(int, 115200), Q_ARG(int, 8),
        Q_ARG(QString, QString("None")), Q_ARG(QString, QString("1")), Q_ARG(quint64, 4),
        Q_ARG(bool, false), Q_ARG(bool, true), Q_ARG(int, 0)));
    VERIFY(QMetaObject::invokeMethod(&handler, "setControlLines", Qt::QueuedConnection,
        Q_ARG(quint64, 4), Q_ARG(bool, true), Q_ARG(bool, false)));
    QThread *mainThread = app.thread();
    // A queued barrier is safe here because all device operations are fake.
    bool barrier = QMetaObject::invokeMethod(&handler, [&]() { handler.closePort(); handler.moveToThread(mainThread); }, Qt::BlockingQueuedConnection);
    worker.quit(); worker.wait();
    VERIFY(barrier && opened == 4 && dtr == 1 && rts == 0 && fatal == 1);
    QTextStream(stdout) << "Serial hardware flow control, bounded TX queues, CTS waits/cancellation, DTR/RTS, Break defaults/pulses, stale requests, unsupported drivers, recording continuity and disconnect recovery passed (no hardware access)." << Qt::endl;
    return 0;
}
