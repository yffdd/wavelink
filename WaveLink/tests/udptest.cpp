#include "networkhandler.h"
#include "ProtocolSampleGenerator.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QBuffer>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <functional>
#include <iostream>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "UDP failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(const std::function<bool()> &condition, int timeout = 4000) {
    QElapsedTimer t; t.start();
    while (!condition() && t.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return condition();
}
static void pump() { QElapsedTimer t; t.start(); until([&] { return t.elapsed() >= 30; }); }
static QUdpSocket *activeSocket(NetworkHandler &source) {
    for (auto socket : source.findChildren<QUdpSocket *>()) if (socket->state() == QAbstractSocket::BoundState) return socket;
    return nullptr;
}
static QVector<QVector<double>> frames(NetworkHandler &source) { quint64 dropped = 0; return source.takePlotFrames(4096, dropped); }
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QTemporaryDir directory; VERIFY(directory.isValid());
    struct RestoreProxy { QNetworkProxy previous = QNetworkProxy::applicationProxy(); ~RestoreProxy() { QNetworkProxy::setApplicationProxy(previous); } } restore;
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 9));
    QUdpSocket peer, other; peer.setProxy(QNetworkProxy::NoProxy); other.setProxy(QNetworkProxy::NoProxy);
    VERIFY(peer.bind(QHostAddress(QHostAddress::LocalHost), 0) && other.bind(QHostAddress(QHostAddress::LocalHost), 0));
    NetworkHandler source; QString error; int failures = 0, errors = 0, actualPort = 0; QByteArray received;
    QObject::connect(&source, &NetworkHandler::connectionAttemptFinished, [&](bool ok, const QString &message) { if (!ok) { ++failures; error = message; } });
    QObject::connect(&source, &NetworkHandler::errorOccurred, [&](const QString &message) { ++errors; error = message; });
    QObject::connect(&source, &NetworkHandler::localEndpointChanged, [&](const QString &, int port) { actualPort = port; });
    QObject::connect(&source, &NetworkHandler::dataReceived, [&](const QByteArray &data) { received += data; });
    source.setProtocol(0);
    for (const auto local : {QString("0.0.0.0"), QString("127.0.0.1")}) {
        VERIFY(source.startUdp(local, 0, "", 0, true) && source.isConnected() && !source.sendReady());
        auto socket = activeSocket(source); VERIFY(socket && socket->localAddress() == QHostAddress(local) && socket->localPort() == actualPort);
        received.clear(); VERIFY(peer.writeDatagram("RAW", QHostAddress::LocalHost, quint16(actualPort)) == 3);
        VERIFY(until([&] { return received == "RAW"; }) && !source.enqueueData("receive-only")); source.stopConnection();
    }
    // One write is one datagram. Preserve bytes, source port, ordering of queued
    // local writes, counters and the explicit receive-source filter.
    VERIFY(source.startUdp("127.0.0.1", 0, "127.0.0.1", peer.localPort(), true) && source.sendReady());
    auto socket = activeSocket(source); VERIFY(socket); const auto port = socket->localPort();
    const QByteArray binary = QByteArray::fromHex("AA550040FF000D0A"); const auto txBefore = source.getTxCount();
    VERIFY(source.enqueueData(binary) && source.enqueueData("SECOND"));
    VERIFY(until([&] { return source.pendingSendBytes() == 0 && peer.hasPendingDatagrams(); }));
    QByteArray data; data.resize(65535); QHostAddress sender; quint16 senderPort = 0;
    auto count = peer.readDatagram(data.data(), data.size(), &sender, &senderPort); data.resize(int(count));
    VERIFY(data == binary && sender == QHostAddress::LocalHost && senderPort == port);
    VERIFY(until([&] { return peer.hasPendingDatagrams(); })); data.resize(65535); count = peer.readDatagram(data.data(), data.size()); data.resize(int(count));
    VERIFY(data == "SECOND" && source.getTxCount() - txBefore == binary.size() + 6);
    VERIFY(!source.enqueueData(QByteArray(65508, 'x')) && !source.enqueueData({}));
    received.clear(); const auto rxBefore = source.getRxCount();
    VERIFY(other.writeDatagram("BLOCKED", QHostAddress::LocalHost, port) == 7); pump();
    VERIFY(received.isEmpty() && source.getRxCount() == rxBefore);
    VERIFY(peer.writeDatagram(binary, QHostAddress::LocalHost, port) == binary.size());
    VERIFY(until([&] { return received == binary; }));
    VERIFY(source.enqueueData("CANCELLED")); source.stopConnection(); pump();
    VERIFY(!source.isConnected() && !source.sendReady() && source.pendingSendBytes() == 0 && !peer.hasPendingDatagrams());
    // Filter can be turned off without changing the fixed outbound target.
    VERIFY(source.startUdp("127.0.0.1", 0, "127.0.0.1", peer.localPort(), false));
    received.clear(); VERIFY(other.writeDatagram("OTHER", QHostAddress::LocalHost, quint16(actualPort)) == 5);
    VERIFY(until([&] { return received == "OTHER"; })); source.stopConnection();
    // Never assemble a truncated payload with the next datagram. A complete
    // packet can contain multiple frames. Both recording formats share this path.
    source.setProtocol(1);
    VERIFY(source.startUdp("127.0.0.1", 0, "", 0, true));
    for (const auto extension : {QString("csv"), QString("wlr")}) {
        frames(source); const auto path = directory.filePath("udp." + extension); source.startRecording(path, 250);
        const auto rxStart = source.getRxCount();
        VERIFY(peer.writeDatagram("1,2", QHostAddress::LocalHost, quint16(actualPort)) == 3);
        VERIFY(until([&] { return source.getRxCount() == rxStart + 3; }) && frames(source).isEmpty());
        VERIFY(peer.writeDatagram("3,4\n5,6\n", QHostAddress::LocalHost, quint16(actualPort)) == 8);
        VERIFY(until([&] { return source.getRxCount() == rxStart + 11; }));
        VERIFY(frames(source) == QVector<QVector<double>>({{3, 4}, {5, 6}}));
        VERIFY(source.finishRecording().isEmpty());
        if (extension == "csv") {
            QFile file(path); VERIFY(file.open(QIODevice::ReadOnly)); const auto rows = file.readAll().trimmed().split('\n');
            VERIFY(rows.size() == 3 && rows[0].contains("UnixTime(s;FS=250),SampleIndex"));
            VERIFY(rows[1].split(',')[2].toDouble() == 3 && rows[1].split(',')[3].toDouble() == 4);
            VERIFY(rows[1].split(',')[0].toDouble() > 1e9);
        } else {
            int recorded = 0; VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64 time, const QVector<double> &v, qint64) {
                ++recorded; return time > 1000000000000000000LL && (v == QVector<double>({3, 4}) || v == QVector<double>({5, 6}));
            }, error) && recorded == 2);
        }
    }
    // JustFloat channel-count stabilization must survive datagram boundaries.
    source.setProtocol(2); const auto sample = QByteArray::fromHex("0000803F000000400000807F");
    auto rx = source.getRxCount();
    VERIFY(peer.writeDatagram(sample, QHostAddress::LocalHost, quint16(actualPort)) == sample.size());
    VERIFY(until([&] { return source.getRxCount() == rx + sample.size(); }) && frames(source).isEmpty());
    VERIFY(peer.writeDatagram(sample, QHostAddress::LocalHost, quint16(actualPort)) == sample.size());
    VERIFY(until([&] { return source.getRxCount() == rx + sample.size() * 2; }));
    VERIFY(frames(source) == QVector<QVector<double>>({{1, 2}})); source.stopConnection();
    // Live tail-only fixed frames confirm across complete packets from a
    // single sender. A sender change or partial datagram restarts confirmation.
    const auto tailOnly = QJsonDocument::fromJson(R"({
      "id":"a9441d8a-cd7b-4f70-9c0a-34cf95c64ca0","revision":1,"name":"UDP fixed","type":"binary","byteOrder":"little",
      "frame":{"mode":"fixed","headerHex":"","tailHex":"0000807F","sizeBytes":12,"payloadOffset":0,"payloadBytes":8,"maxFrameBytes":4096},
      "channels":[{"id":"a","name":"A","offset":0,"type":"float32"},{"id":"b","name":"B","offset":4,"type":"float32"}]
    })").object();
    ProtocolProfile fixedProfile;
    VERIFY(ProtocolProfileValidator::profile(tailOnly, fixedProfile, error));
    ProtocolSelection fixedSelection;
    fixedSelection.kind = "custom";
    fixedSelection.id = fixedProfile.id;
    source.applyProtocol(1, fixedSelection, fixedProfile);
    VERIFY(source.startUdp("127.0.0.1", 0, "", 0, true));
    const auto receivePacket = [&](QUdpSocket &socket, const QByteArray &packet) {
        const auto before = source.getRxCount();
        return socket.writeDatagram(packet, QHostAddress::LocalHost, quint16(actualPort)) == packet.size()
            && until([&] { return source.getRxCount() == before + packet.size(); });
    };
    VERIFY(receivePacket(peer, sample) && frames(source).isEmpty());
    VERIFY(receivePacket(peer, sample) && frames(source).isEmpty());
    VERIFY(receivePacket(other, sample) && frames(source).isEmpty());
    VERIFY(receivePacket(other, sample.chopped(1)) && frames(source).isEmpty());
    VERIFY(receivePacket(other, sample) && frames(source).isEmpty());
    VERIFY(receivePacket(other, sample) && frames(source).isEmpty());
    VERIFY(receivePacket(other, sample) && frames(source) == QVector<QVector<double>>({{1, 2}}));
    source.stopConnection();
    // Custom dynamic arrays with CRC, all channels and names preserved to WLR.
    auto definition = QJsonDocument::fromJson(R"({"id":"539eeaa8-bd2c-4d61-898e-9784ef5fe835","revision":1,"name":"UDP array","type":"binary","byteOrder":"little",
      "array":{"type":"float32","minChannels":1,"maxChannels":256},"channels":[],
      "frame":{"mode":"length","headerHex":"AA55","tailHex":"0D0A","payloadOffset":4,"payloadEndOffset":4,"minFrameBytes":12,"maxFrameBytes":4096,
      "lengthField":{"offset":2,"type":"uint16","byteOrder":"little","meaning":"payload-bytes","unitBytes":1,"adjustment":8}},
      "checksum":{"algorithm":"crc16-modbus","range":{"offset":0,"endOffset":4},"field":{"from":"end","offset":4,"type":"uint16","byteOrder":"little"}}})").object();
    definition.insert("channels", QJsonArray{QJsonObject{{"id", "temperature"}, {"name", "温度"}, {"index", 0}, {"scale", 1}, {"bias", 0}}});
    ProtocolProfile profile; VERIFY(ProtocolProfileValidator::profile(definition, profile, error));
    ProtocolSelection selection; selection.kind = "custom"; selection.id = profile.id; source.applyProtocol(1, selection, profile);
    VERIFY(source.startUdp("127.0.0.1", 0, "", 0, true)); QByteArray first, second;
    VERIFY(ProtocolSampleGenerator::generate(profile, first, error, 2) && ProtocolSampleGenerator::generate(profile, second, error, 4));
    const auto path = directory.filePath("array.wlr"); source.startRecording(path, 250); rx = source.getRxCount();
    VERIFY(peer.writeDatagram(first + second, QHostAddress::LocalHost, quint16(actualPort)) == first.size() + second.size());
    VERIFY(until([&] { return source.getRxCount() == rx + first.size() + second.size(); }));
    const auto decoded = frames(source); VERIFY(decoded.size() == 2 && decoded[0].size() == 2 && decoded[1].size() == 4);
    VERIFY(source.skippedRecordingFrames() == 1 && source.finishRecording().isEmpty()); int records = 0; QStringList names;
    VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64, const QVector<double> &v, qint64) { return v == decoded[records++]; }, error, nullptr, nullptr, &names));
    VERIFY(records == 1 && names.mid(0, 4) == QStringList({"温度", "Ch2", "Ch3", "Ch4"}));
    QByteArray exported; QBuffer csvOutput(&exported); VERIFY(csvOutput.open(QIODevice::WriteOnly));
    VERIFY(WlrRecordingWriter::exportToCsv({path}, &csvOutput, error) && exported.contains(QString("温度,Ch2\n").toUtf8()));
    // A key-value group must be wholly in a packet, not mixed across senders.
    VERIFY(ProtocolProfileValidator::profile(ProtocolProfileValidator::keyValueTemplate(), profile, error));
    selection.id = profile.id; source.applyProtocol(2, selection, profile); rx = source.getRxCount();
    VERIFY(peer.writeDatagram("0:1\r\n", QHostAddress::LocalHost, quint16(actualPort)) == 5);
    VERIFY(until([&] { return source.getRxCount() == rx + 5; }));
    VERIFY(peer.writeDatagram("1:2\r\n2:3\r\n", QHostAddress::LocalHost, quint16(actualPort)) == 10);
    VERIFY(until([&] { return source.getRxCount() == rx + 15; }) && frames(source).isEmpty());
    VERIFY(peer.writeDatagram("0:1\r\n1:2\r\n2:3\r\n", QHostAddress::LocalHost, quint16(actualPort)) == 15);
    VERIFY(until([&] { return source.getRxCount() == rx + 30; }) && frames(source) == QVector<QVector<double>>({{1, 2, 3}}));
    source.stopConnection();
    // Real conflicts/validation, no fallback to wildcard or random port.
    VERIFY(!source.startUdp("127.0.0.1", peer.localPort(), "", 0, false) && error.contains("监听"));
    VERIFY(!source.startUdp("not-an-ip", 0, "", 0, false));
    VERIFY(!source.startUdp("127.0.0.1", 65536, "", 0, false));
    VERIFY(!source.startUdp("127.0.0.1", 0, "127.0.0.1", 0, false));
    VERIFY(!source.startUdp("127.0.0.1", 0, "", 12345, false));
    VERIFY(!source.startUdp("127.0.0.1", 0, "0.0.0.0", 12345, false));
    VERIFY(!source.startUdp("127.0.0.1", 0, "239.1.1.1", 12345, false));
    VERIFY(!source.startUdp("127.0.0.1", 0, "255.255.255.255", 12345, false));
    VERIFY(!source.startUdp("127.0.0.1", 0, "::1", 12345, false));
    VERIFY(!source.isConnected() && !source.sendReady() && failures == 9 && errors == 0);
    // Real queued/meta-object signature, fixed local port and immediate reuse.
    bool opened = false; QUdpSocket reserve; reserve.setProxy(QNetworkProxy::NoProxy);
    VERIFY(reserve.bind(QHostAddress(QHostAddress::LocalHost), 0)); const auto fixed = reserve.localPort(); reserve.close();
    VERIFY(QMetaObject::invokeMethod(&source, "startUdp", Qt::DirectConnection, Q_RETURN_ARG(bool, opened),
        Q_ARG(QString, QString("127.0.0.1")), Q_ARG(int, fixed), Q_ARG(QString, QString("")), Q_ARG(int, 0), Q_ARG(bool, true)) && opened);
    VERIFY(activeSocket(source)->localPort() == fixed); source.stopConnection();
    VERIFY(source.startUdp("127.0.0.1", fixed, "", 0, true)); source.stopConnection();
    QUdpSocket v6; v6.setProxy(QNetworkProxy::NoProxy);
    if (v6.bind(QHostAddress(QHostAddress::LocalHostIPv6), 0)) {
        VERIFY(source.startUdp("::1", 0, "::1", v6.localPort(), true));
        VERIFY(source.enqueueData("IPV6") && until([&] { return v6.hasPendingDatagrams(); })); source.stopConnection();
    }
    std::cout << "UDP bind/filter, raw and binary sends, independent packets, JustFloat, custom CRC arrays/key-value, CSV/WLR recording, validation, cleanup and IPv6 passed (loopback only).\n";
    return 0;
}
