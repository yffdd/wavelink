#include "ble/blestreamworker.h"
#include <QCoreApplication>
#include <QThread>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QtEndian>
#include <functional>
#include <iostream>

#define VERIFY(c) do { if (!(c)) { std::cerr << "BLE worker failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(std::function<bool()> condition)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < 10000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    return condition();
}
struct Fixture {
    QThread thread; BleStreamWorker *worker = new BleStreamWorker;
    Fixture() { worker->moveToThread(&thread); QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater); thread.start(); }
    QString stop() { QString error; QMetaObject::invokeMethod(worker, "stopRecordingNow", Qt::BlockingQueuedConnection, Q_RETURN_ARG(QString, error)); return error; }
    ~Fixture() { worker->endSession(); stop(); thread.quit(); thread.wait(); }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); QTemporaryDir dir; VERIFY(dir.isValid()); Fixture f;
    int starts = 0, failures = 0, pipelineFailures = 0; bool asynchronouslyStopped = false;
    QObject::connect(f.worker, &BleStreamWorker::pipelineError, &app, [&](quint64, const QString &) { ++pipelineFailures; });
    QObject::connect(f.worker, &BleStreamWorker::recordingStopped, &app, [&](quint64 request, const QString &error, qint64 bytes) {
        if (request == 6 && error.isEmpty() && bytes > 0) asynchronouslyStopped = true;
    });
    QObject::connect(f.worker, &BleStreamWorker::recordingStartFinished, &app, [&](quint64, quint64, bool ok, const QString &) { ok ? ++starts : ++failures; });
    QObject::connect(f.worker, &BleStreamWorker::recordingError, &app, [&](quint64, quint64, const QString &) { ++failures; });
    const qint64 t = 1791370000000000000LL;
    f.worker->beginSession(1, 1, false);
    f.worker->startRecording(dir.filePath("split.csv"), 250, 1);
    VERIFY(until([&] { return starts == 1; }));
    VERIFY(f.worker->enqueueData(1, "1,", t));
    VERIFY(f.worker->enqueueData(1, "2\n3,4\n5,", t + 123456700));
    VERIFY(f.worker->enqueueData(1, "6\n", t + 223456700));
    VERIFY(f.stop().isEmpty());
    QFile csv(dir.filePath("split.csv")); VERIFY(csv.open(QIODevice::ReadOnly)); const auto text = csv.readAll();
    VERIFY(text.contains("1791370000.123456700,0,1.000000,2.000000"));
    VERIFY(text.contains("1791370000.123456700,1,3.000000,4.000000"));
    VERIFY(text.contains("1791370000.223456700,2,5.000000,6.000000"));
    quint64 dropped = 0; auto frames = f.worker->takePlotFrames(100, dropped);
    VERIFY(frames.size() == 3 && frames[2] == QVector<double>({5, 6}));
    VERIFY(!f.worker->enqueueData(999, "999\n", t));
    VERIFY(f.worker->enqueueData(1, "7,8\n", t)); f.stop();
    f.worker->setProtocol(0, false);
    VERIFY(f.worker->takePlotFrames(100, dropped).isEmpty()); // No old-protocol plot frames.
    f.worker->setProtocol(1, false);
    f.worker->startRecording(dir.filePath("full.wlr"), 250, 2); VERIFY(until([&] { return starts == 2; }));
    QByteArray burst;
    for (int i = 0; i < 18000; ++i) burst += QByteArray::number(i) + ",2\n";
    VERIFY(f.worker->enqueueData(1, burst, t + 300000000));
    // No GUI consumes display frames: recording must still keep all frames.
    f.worker->stopRecording(6); VERIFY(until([&] { return asynchronouslyStopped; }));
    frames = f.worker->takePlotFrames(10000, dropped);
    VERIFY(frames.size() == 8192 && dropped == 18000 - 8192);
    f.worker->endSession(); VERIFY(f.stop().isEmpty());
    int count = 0; QString error;
    VERIFY(WlrRecordingWriter::visitFiles({dir.filePath("full.wlr")}, [&](qint64 timestamp, const QVector<double> &values, qint64) {
        if (timestamp != t + 300000000 || values != QVector<double>({double(count), 2})) return false;
        ++count; return true;
    }, error)); VERIFY(count == 18000);
    VERIFY(!QFile::exists(dir.filePath("full.wlr.part")));
    VERIFY(!f.worker->enqueueData(1, "999\n", t));
    // JustFloat tail and payload can both cross BLE notification boundaries.
    for (bool bigEndian : {false, true}) {
        f.worker->beginSession(bigEndian ? 3 : 2, 2, bigEndian);
        const quint64 session = bigEndian ? 3 : 2;
        QByteArray packet(8, '\0'); const float value = 12.5f;
        if (bigEndian) qToBigEndian<float>(value, reinterpret_cast<uchar *>(packet.data()));
        else memcpy(packet.data(), &value, 4);
        // Tail follows the explicitly configured endian, just like payload.
        packet.replace(4, 4, QByteArray::fromHex(bigEndian ? "7f800000" : "0000807f"));
        packet += packet; // Existing JustFloat parser synchronizes on two frames.
        VERIFY(f.worker->enqueueData(session, packet.left(5), t));
        VERIFY(f.worker->enqueueData(session, packet.mid(5), t + 400000000));
        VERIFY(f.stop().isEmpty()); frames = f.worker->takePlotFrames(100, dropped);
        VERIFY(frames.size() == 1 && frames[0] == QVector<double>({12.5}));
    }
    f.worker->beginSession(4, 1, false);
    f.worker->startRecording(dir.filePath("shape.csv"), 250, 3); VERIFY(until([&] { return starts == 3; }));
    VERIFY(f.worker->enqueueData(4, "1,2\n3\n4,5\n", t));
    VERIFY(until([&] { return f.worker->skippedRecordingFrames() == 1; })); VERIFY(f.stop().isEmpty());
    VERIFY(failures == 0 && QFile::exists(dir.filePath("shape.csv")) && f.worker->recordedFrames() == 2);
    // A buffered WLR write/final rename failure must be visible even on stop.
    f.worker->startRecording(dir.filePath("final.wlr"), 250, 4); VERIFY(until([&] { return starts == 4; }));
    VERIFY(f.worker->enqueueData(4, "1,2\n", t));
    VERIFY(QDir().mkdir(dir.filePath("final.wlr")));
    VERIFY(!f.stop().isEmpty()); VERIFY(QFile::exists(dir.filePath("final.wlr.part")));
    f.worker->startRecording(dir.filePath("missing/sub/invalid.csv"), 250, 5);
    VERIFY(!f.stop().isEmpty()); VERIFY(until([&] { return failures >= 2; }));
    // AMU-4 uses an internal buffer and must also report incomplete-frame overflow.
    f.worker->beginSession(5, 3, false);
    VERIFY(f.worker->enqueueData(5, "0:1\r", t));
    VERIFY(f.worker->enqueueData(5, "\n1:2\r\n2:3\r\n", t + 1));
    VERIFY(f.stop().isEmpty()); frames = f.worker->takePlotFrames(100, dropped);
    VERIFY(frames.size() == 1 && frames[0] == QVector<double>({1, 2, 3}));
    VERIFY(f.worker->enqueueData(5, QByteArray(1024 * 1024 + 1, 'x'), t));
    f.stop(); VERIFY(until([&] { return pipelineFailures == 1; }));
    f.worker->beginSession(6, 1, false);
    VERIFY(f.worker->enqueueData(6, QByteArray(1024 * 1024 + 1, 'x'), t));
    f.stop(); VERIFY(until([&] { return pipelineFailures == 2; }));
    f.worker->beginSession(7, 1, false);
    VERIFY(f.worker->enqueueData(7, QByteArray("1,").repeated(256) + "1\n", t));
    f.stop(); VERIFY(until([&] { return pipelineFailures == 3; }));
    // Fill a stopped worker's mailbox deterministically to verify the bound.
    BleStreamWorker bounded;
    bounded.beginSession(1, 0, false);
    VERIFY(bounded.enqueueData(1, QByteArray(4 * 1024 * 1024, 'x'), t));
    VERIFY(!bounded.enqueueData(1, "x", t));
    bounded.endSession(); VERIFY(!bounded.enqueueData(1, "x", t));
    // Sequence-verified multichannel load with no display consumption. Back
    // pressure is paced by the test producer, not silently dropped by the app.
    f.worker->beginSession(8, 1, false);
    f.worker->startRecording(dir.filePath("stress.wlr"), 1000, 8);
    VERIFY(until([&] { return starts == 5; }));
    const int totalFrames = 100000, channels = 64;
    QElapsedTimer stress; stress.start();
    for (int begin = 0; begin < totalFrames; begin += 128) {
        QByteArray batch; const int end = qMin(begin + 128, totalFrames);
        for (int i = begin; i < end; ++i) {
            for (int ch = 0; ch < channels; ++ch) { if (ch) batch += ','; batch += QByteArray::number(i + ch); }
            batch += '\n';
        }
        VERIFY(f.worker->enqueueData(8, batch, t + begin));
        VERIFY(until([&] { return f.worker->parsedFrames() >= quint64(end); }));
    }
    VERIFY(f.stop().isEmpty() && f.worker->recordedFrames() == quint64(totalFrames));
    const quint64 lifetimeDropped = f.worker->displayDropped();
    frames = f.worker->takePlotFrames(10000, dropped);
    // The 8 MiB bound may reduce the 8192 frame-count ceiling for 64 channels.
    VERIFY(!frames.isEmpty() && frames.size() <= 8192 && dropped == quint64(totalFrames - frames.size()));
    VERIFY(lifetimeDropped == dropped && f.worker->displayDropped() == dropped);
    count = 0;
    VERIFY(WlrRecordingWriter::visitFiles({dir.filePath("stress.wlr")}, [&](qint64 timestamp, const QVector<double> &values, qint64) {
        if (timestamp != t + (count / 128) * 128 || values.size() != channels) return false;
        for (int ch = 0; ch < channels; ++ch) if (values[ch] != count + ch) return false;
        ++count; return true;
    }, error)); VERIFY(count == totalFrames);
    std::cout << "100000 frames x 64 channels WLR sequence/timestamp verification passed in " << stress.elapsed() << " ms; display-only drops=" << dropped << ".\n";
    f.worker->beginSession(9, 1, false);
    f.worker->startRecording(dir.filePath("stress.csv"), 1000, 9);
    VERIFY(until([&] { return starts == 6; }));
    QByteArray csvBurst;
    for (int i = 0; i < 10000; ++i) {
        for (int ch = 0; ch < 8; ++ch) { if (ch) csvBurst += ','; csvBurst += QByteArray::number(i + ch); }
        csvBurst += '\n';
    }
    VERIFY(f.worker->enqueueData(9, csvBurst, t));
    VERIFY(f.stop().isEmpty() && f.worker->recordedFrames() == 10000);
    QFile stressCsv(dir.filePath("stress.csv")); VERIFY(stressCsv.open(QIODevice::ReadOnly));
    VERIFY(stressCsv.readLine().startsWith(QByteArray("\xEF\xBB\xBF", 3) + "UnixTime(s;FS="));
    for (int i = 0; i < 10000; ++i) {
        const auto columns = stressCsv.readLine().trimmed().split(',');
        VERIFY(columns.size() == 10 && columns[0] == "1791370000.000000000" && columns[1].toInt() == i);
        for (int ch = 0; ch < 8; ++ch) VERIFY(columns[ch + 2].toDouble() == i + ch);
    }
    VERIFY(stressCsv.atEnd());
    std::cout << "10000 frames x 8 channels CSV sequence/value/timestamp verification passed.\n";
    std::cout << "BLE worker: split protocols, original timestamps, CSV/WLR full-rate recording, disconnect drain, bounded input and write/finalization failures passed.\n";
    return 0;
}
