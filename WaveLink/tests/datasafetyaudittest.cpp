#include "csvrecordingwriter.h"
#include "wlrrecordingwriter.h"
#include "recordingchannelnames.h"
#include "waveimportbudget.h"
#include "ffthelper.h"
#include "networkhandler.h"
#include "serialhandler.h"
#include "CustomProtocolParser.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QBuffer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <functional>
#include <iostream>
#define VERIFY(c) do { if (!(c)) { std::cerr << "Data safety audit failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(const std::function<bool()> &condition, int timeout = 15000) {
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return condition();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QTemporaryDir dir; VERIFY(dir.isValid());
    qRegisterMetaType<QVector<double>>("QVector<double>");
    for (const auto &label : {QString("UnixTime"), QString("HostReceiveTime")}) {
        for (const auto &metadata : {QString("FS=250"), QString("Hint=250"), QString("FS=1.255e2"), QString("Hint=1.255e2")}) {
            double rate = -1;
            VERIFY(RecordingChannels::parseReceiveTimeColumn(label + "(s;" + metadata + ')', rate));
            VERIFY(rate == metadata.mid(metadata.indexOf('=') + 1).toDouble());
        }
        double rate = -1;
        VERIFY(RecordingChannels::parseReceiveTimeColumn(label + "(s)", rate) && rate == 0);
        VERIFY(RecordingChannels::parseReceiveTimeColumn(label + "(s;FS=0)", rate) && rate == 0);
    }
    double notRate = 0;
    VERIFY(!RecordingChannels::parseReceiveTimeColumn("Time(s)", notRate));
    VERIFY(RecordingChannels::csvReceiveTimeColumn(250) == "UnixTime(s;FS=250)");
    const QVector<double> precise{1e-12, -4e-7, 1.2345678901234567, 1.234567890123456e15, double(float(3.3))};
    for (double value : precise) VERIFY(RecordingChannels::csvNumber(value).toDouble() == value);
    CsvRecordingWriter csv; WlrRecordingWriter wlr; QString error;
    const auto csvPath = dir.filePath("precision.csv"), wlrPath = dir.filePath("precision.wlr");
    VERIFY(csv.start(csvPath, error, 250) && csv.append(precise, error, 1700000000000000000LL) && csv.finish(error));
    VERIFY(wlr.start(wlrPath, error, 250) && wlr.append(precise, error, 1700000000000000000LL) && wlr.finish(error));
    QFile file(csvPath); VERIFY(file.open(QIODevice::ReadOnly)); file.readLine(); const auto cells = file.readLine().trimmed().split(',');
    for (int i = 0; i < precise.size(); ++i) VERIFY(cells[i + 2].toDouble() == precise[i]);
    QByteArray converted; QBuffer output(&converted); output.open(QIODevice::WriteOnly);
    VERIFY(WlrRecordingWriter::exportToCsv({wlrPath}, &output, error));
    const auto fastCells = converted.split('\n')[1].split(',');
    for (int i = 0; i < precise.size(); ++i) VERIFY(fastCells[i + 2].toDouble() == precise[i]);
    {
        CsvRecordingWriter live; const auto path = dir.filePath("live.csv");
        VERIFY(live.start(path, error) && live.append({1}, error));
        const QFileInfoList active{QFileInfo(path + ".part"), QFileInfo(path + ".part.lock")};
        VERIFY(!RecordingChannels::cacheFilesIdle(active) && !RecordingChannels::removeCacheFile(active[0]));
        VERIFY(RecordingChannels::removeCacheFile(active[1]) && QFile::exists(path + ".part.lock"));
        VERIFY(live.finish(error));
    }
    for (int available : {64, 128, 150, 250, 256, 1023}) {
        const int count = FftHelper::powerOfTwoFloor(available);
        VERIFY(count <= available && count * 2 > available && (count & (count - 1)) == 0);
        std::vector<std::complex<double>> data(count);
        for (int i = 0; i < count; ++i) data[i] = {std::sin(2 * M_PI * 5 * i / count), 0};
        FftHelper::computeFFT(data); VERIFY(std::abs(std::abs(data[5]) * 2 / count - 1) < 1e-12);
    }
    VERIFY(WaveImportBudget::fits(4, 2, 96) && !WaveImportBudget::fits(5, 2, 96));
    VERIFY(!WaveImportBudget::fits(1000000000000LL, 256, 256LL * 1024 * 1024));
    VERIFY(WaveImportBudget::bytesForAvailableMemory(1024ULL * 1024 * 1024) == 128LL * 1024 * 1024);
    // The same custom decoder feeds recordings on live transports. Names and
    // complete-group values must survive WLR and CSV conversion unchanged.
    {
        DataProtocols::ProtocolProfile profile;
        VERIFY(DataProtocols::ProtocolProfileValidator::profile(DataProtocols::ProtocolProfileValidator::keyValueTemplate(true), profile, error));
        SerialHandler keySource; DataProtocols::ProtocolSelection selection; selection.kind = "custom"; selection.id = profile.id;
        keySource.applyProtocol(1, selection, profile);
        const auto path = dir.filePath("keyvalue.wlr"); keySource.startRecording(path, 250);
        auto decoder = keySource.findChild<DataProtocols::CustomProtocolParser *>(); VERIFY(decoder);
        const QByteArray frame("2:33\r\n0:11\r\n1:22\r\n");
        for (int i = 0; i < frame.size();) {
            const int end = frame.indexOf("\r\n", i) + 2; VERIFY(end > i);
            const auto line = frame.mid(i, end - i); VERIFY(decoder->processData(line.constData(), line.size()) == line.size()); i = end;
        }
        VERIFY(keySource.finishRecording().isEmpty()); QStringList names; int frames = 0;
        VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64, const QVector<double> &v, qint64) { ++frames; return v == QVector<double>({11, 22, 33}); }, error, nullptr, nullptr, &names));
        VERIFY(frames == 1 && names == QStringList({"CBF", "IR", "RED"}));
        QByteArray csv; QBuffer output(&csv); output.open(QIODevice::WriteOnly);
        VERIFY(WlrRecordingWriter::exportToCsv({path}, &output, error) && csv.startsWith(RecordingChannels::csvUtf8Bom() + "UnixTime(s;FS=250),SampleIndex,CBF,IR,RED\n"));
    }
    AMU4Parser amu; QVector<QVector<double>> parsed;
    QObject::connect(&amu, &ProtocolParser::channelsUpdated, [&](const QVector<double> &v) { parsed << v; });
    const QByteArray old("0:1\r\n1:2\r\n"); amu.processData(old.constData(), old.size()); amu.reset();
    const QByteArray fresh("2:3\r\n"); amu.processData(fresh.constData(), fresh.size()); VERIFY(parsed.isEmpty());
    const QByteArray rest("0:4\r\n1:5\r\n"); amu.processData(rest.constData(), rest.size()); VERIFY(parsed == QVector<QVector<double>>({{4, 5, 3}}));
    // Force a final-rename failure without touching any real recording. Every
    // backend must report it and retain the complete temporary file.
    const auto blocked = dir.filePath("blocked.csv"); VERIFY(QDir().mkpath(blocked));
    SerialHandler serial; serial.startRecording(blocked, 250);
    VERIFY(QMetaObject::invokeMethod(&serial, "writeLogData", Q_ARG(QVector<double>, QVector<double>({1, 2}))));
    VERIFY(!serial.finishRecording().isEmpty() && QFile::exists(blocked + ".part") && QFileInfo(blocked).isDir());
    NetworkHandler finishNetwork; const auto netBlocked = dir.filePath("blocked-net.csv"); VERIFY(QDir().mkpath(netBlocked));
    finishNetwork.startRecording(netBlocked, 250);
    VERIFY(QMetaObject::invokeMethod(&finishNetwork, "writeLogData", Q_ARG(QVector<double>, QVector<double>({1, 2}))));
    VERIFY(!finishNetwork.finishRecording().isEmpty() && QFile::exists(netBlocked + ".part"));
    // Localhost only: no device access, BLE scan or external network traffic.
    QTcpServer peer; VERIFY(peer.listen(QHostAddress::LocalHost, 0));
    NetworkHandler network; network.setProtocol(1); int errors = 0;
    QObject::connect(&network, &NetworkHandler::errorOccurred, [&](const QString &) { ++errors; });
    VERIFY(!network.enqueueData("disconnected"));
    VERIFY(network.startConnection(1, "127.0.0.1", peer.serverPort()) && until([&] { return peer.hasPendingConnections(); }));
    auto remote = peer.nextPendingConnection(); VERIFY(remote && network.sendReady());
    const auto burstFile = dir.filePath("burst.wlr"); network.startRecording(burstFile, 250);
    QByteArray burst; const int frames = 300000; burst.reserve(frames * 4);
    for (int i = 0; i < frames; ++i) burst += "1,2\n";
    VERIFY(burst.size() > 1024 * 1024 && remote->write(burst) == burst.size());
    VERIFY(until([&] { return network.getLoggedBytes() >= 36LL + frames * 36LL; }));
    VERIFY(network.finishRecording().isEmpty() && errors == 0 && network.getRxCount() == burst.size());
    int recorded = 0;
    VERIFY(WlrRecordingWriter::visitFiles({burstFile}, [&](qint64, const QVector<double> &v, qint64) { ++recorded; return v == QVector<double>({1, 2}); }, error) && recorded == frames);
    VERIFY(!network.enqueueData(QByteArray(256 * 1024 + 1, 'X')));
    for (int i = 0; i < 4; ++i) VERIFY(network.enqueueData(QByteArray(65536, 'X')));
    VERIFY(network.pendingSendBytes() == 256 * 1024 && !network.enqueueData("overflow"));
    VERIFY(until([&] { return network.pendingSendBytes() == 0 && remote->bytesAvailable() == 256 * 1024; }));
    VERIFY(remote->readAll() == QByteArray(256 * 1024, 'X') && network.getTxCount() == 256 * 1024);
    VERIFY(network.enqueueData("old-session")); network.stopConnection();
    QCoreApplication::processEvents(); VERIFY(!network.sendReady() && network.pendingSendBytes() == 0);
    network.resetCounters(); VERIFY(network.getRxCount() == 0 && network.getTxCount() == 0);
    std::cout << "CSV/WLR precision, valid FFT windows, import budgets, parser reset, finalization errors, large TCP bursts, bounded TX and counters passed.\n";
    return 0;
}
