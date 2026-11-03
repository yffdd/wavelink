#include "ProtocolConfigTransfer.h"
#include "ProtocolParserFactory.h"
#include "ProtocolSampleGenerator.h"
#include "ProtocolChecksum.h"
#include "serialhandler.h"
#include "networkhandler.h"
#include "ble/blestreamworker.h"
#include "fakeserialbackend.h"
#include "testregistrysandbox.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSettings>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDataStream>
#include <iostream>
#include <functional>
#include <cmath>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Protocol live failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(std::function<bool()> condition) {
    QElapsedTimer t; t.start(); while (!condition() && t.elapsed() < 7000) { QCoreApplication::processEvents(); QThread::msleep(1); } return condition();
}
static ProtocolProfile example(bool binary, bool variable = false) {
    QFile file(QStringLiteral(WAVELINK_PROTOCOL_EXAMPLE_DIR) + (variable ? "/length_temperature_voltage.json" : binary ? "/binary_temperature_voltage.json" : "/text_temperature_voltage.json"));
    QString error; ProtocolSnapshot s;
    if (!file.open(QIODevice::ReadOnly) || !ProtocolProfileValidator::decode(file.readAll(), false, s, error) || s.profiles.isEmpty()) { std::cerr << error.toUtf8().constData(); return {}; }
    return s.profiles[0];
}
static QByteArray binaryPacket(const ProtocolProfile &profile) {
    auto bytes = QByteArray::fromHex(profile.definition.value("frame").toObject().value("mode") == QJsonValue("length") ? "AA5506E60933335340000D0A" : "AA55E60933335340000D0A");
    const auto check = profile.definition.value("checksum").toObject(), range = check.value("range").toObject(), field = check.value("field").toObject();
    ProtocolChecksum calculator(check.value("algorithm").toString());
    const int start = range.value("offset").toInt(), length = range.contains("endOffset") ? bytes.size() - range.value("endOffset").toInt() - start : range.value("length").toInt();
    const int position = field.value("from") == QJsonValue("end") ? bytes.size() - field.value("offset").toInt() : field.value("offset").toInt();
    const quint32 value = calculator.calculate(bytes.constData() + start, length);
    for (int i = 0; i < calculator.width(); ++i) bytes[position + i] = char(value >> (8 * i));
    return bytes;
}
class ReceivePort : public FakePort {
public:
    void feed(const QByteArray &bytes) { incoming += bytes; emit readyRead(); }
    qint64 bytesAvailable() const override { return incoming.size() + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 count) override { const auto n = qMin<qint64>(count, incoming.size()); memcpy(data, incoming.constData(), size_t(n)); incoming.remove(0, int(n)); return n; }
private:
    QByteArray incoming;
};
template<class Worker> static QVector<QVector<double>> take(Worker &w) { quint64 dropped = 0; return w.takePlotFrames(100, dropped); }
static int testSerial(const QString &directory, const ProtocolProfile &text, const ProtocolProfile &binary) {
    auto port = new ReceivePort; TestSerialHandler worker(port); int ack = 0; bool accepted = false, opened = false;
    QObject::connect(&worker, &SerialHandler::protocolApplyFinished, [&](quint64 request, bool ok, const QString &) { ack = int(request); accepted = ok; });
    QObject::connect(&worker, &SerialHandler::portOpenFinished, [&](quint64, bool ok, const QString &, int) { opened = ok; });
    worker.openPort("SOFTWARE_ONLY", 115200, 8, "None", "1", 1, false, false); VERIFY(opened);
    ProtocolSelection selection; selection.kind = "custom"; selection.id = text.id;
    worker.applyProtocol(1, selection, text); VERIFY(accepted && ack == 1);
    port->feed("DATA:25."); VERIFY(take(worker).isEmpty());
    auto wrong = selection; wrong.id = binary.id; worker.applyProtocol(2, wrong, text); VERIFY(!accepted);
    port->feed("34,3.30\r\n"); auto frames = take(worker); VERIFY(frames.size() == 1 && frames[0] == QVector<double>({25.34, 3.3}));
    port->feed("DATA:25.34,3.30,2.5,2.8\r\n"); frames = take(worker);
    VERIFY(frames.size() == 1 && frames[0] == QVector<double>({25.34, 3.3, 2.5, 2.8}));
    worker.startRecording(directory + "/serial-four.csv", 123);
    port->feed("DATA:25.34,3.30,2.5,2.8\r\n"); worker.stopRecording(); take(worker);
    QFile fourCsv(directory + "/serial-four.csv"); VERIFY(fourCsv.open(QIODevice::ReadOnly));
    const auto fourBytes = fourCsv.readAll();
    VERIFY(fourBytes.contains("25.340000,3.300000,2.500000,2.800000") && fourBytes.contains(QStringLiteral(",温度,电压,Ch3,Ch4\n").toUtf8()));
    int recordingErrors = 0; QObject::connect(&worker, &SerialHandler::recordingError, [&](const QString &) { ++recordingErrors; });
    auto textPacket = [](int channels, int first = 1) { QStringList fields; for (int i = 0; i < channels; ++i) fields << QString::number(first + i); return "DATA:" + fields.join(',').toUtf8() + "\r\n"; };
    for (const QString extension : {"csv", "wlr"}) {
        port->feed(textPacket(17)); take(worker); // Display before recording must not establish its width.
        const auto path = directory + "/locked-shape." + extension; worker.startRecording(path, 123);
        VERIFY(worker.skippedRecordingFrames() == 0);
        for (int count : {12, 17, 11, 12}) port->feed(textPacket(count));
        VERIFY(worker.skippedRecordingFrames() == 2 && recordingErrors == 0);
        auto shown = take(worker); VERIFY(shown.size() == 4 && shown[0].size() == 12 && shown[1].size() == 17 && shown[2].size() == 11 && shown[3].size() == 12);
        worker.stopRecording(); VERIFY(worker.skippedRecordingFrames() == 0);
        if (extension == "wlr") { int saved = 0; QString issue;
            VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64, const QVector<double> &v, qint64) { ++saved; return v.size() == 12 && v.first() == 1 && v.last() == 12; }, issue) && saved == 2);
        } else { QFile file(path); VERIFY(file.open(QIODevice::ReadOnly)); const auto rows = file.readAll().trimmed().split('\n');
            VERIFY(rows.size() == 3 && rows[1].split(',').size() == 14 && rows[2].split(',').size() == 14); }
        // Every recording learns its own FIRST frame, never the protocol row
        // count or the preceding recording's width; counts do not leak either.
        const auto restarted = directory + "/locked-restart." + extension; worker.startRecording(restarted, 123);
        for (int count : {17, 12, 17}) port->feed(textPacket(count));
        VERIFY(worker.skippedRecordingFrames() == 1 && recordingErrors == 0); worker.stopRecording(); take(worker);
        if (extension == "wlr") { int saved = 0; QString issue;
            VERIFY(WlrRecordingWriter::visitFiles({restarted}, [&](qint64, const QVector<double> &v, qint64) { ++saved; return v.size() == 17; }, issue) && saved == 2);
        }
    }
    worker.startRecording(directory + "/serial.csv", 123);
    worker.applyProtocol(3, ProtocolSelection::fromLegacy(0), {}); VERIFY(!accepted);
    port->feed("DATA:1,2\r\nDATA:3,4\r\n"); worker.stopRecording();
    QFile csv(directory + "/serial.csv"); VERIFY(csv.open(QIODevice::ReadOnly)); const auto lines = csv.readAll(); VERIFY(lines.contains("1.000000,2.000000") && lines.contains("3.000000,4.000000"));
    selection.id = binary.id; worker.applyProtocol(4, selection, binary); VERIFY(accepted);
    worker.startRecording(directory + "/serial.wlr", 123);
    const auto packet = binaryPacket(binary);
    for (char byte : packet) port->feed(QByteArray(1, byte)); worker.stopRecording();
    int count = 0; QString error; QStringList names;
    VERIFY(WlrRecordingWriter::visitFiles({directory + "/serial.wlr"}, [&](qint64 timestamp, const QVector<double> &values, qint64) {
        ++count; return timestamp > 0 && values.size() == 2 && values[0] == 2534 && std::abs(values[1] - 3.3) < 1e-5;
    }, error, nullptr, nullptr, &names)); VERIFY(count == 1 && names == binary.outputChannelNames());
    auto nanDefinition = text.definition; auto nanFields = nanDefinition["channels"].toArray(); auto nanField = nanFields[0].toObject();
    nanField.insert("max", 0); nanFields[0] = nanField; nanDefinition.insert("channels", nanFields); nanDefinition.insert("outOfRangePolicy", "nan-channel");
    ProtocolProfile nanText; VERIFY(ProtocolProfileValidator::profile(nanDefinition, nanText, error)); selection.id = nanText.id;
    worker.applyProtocol(5, selection, nanText); VERIFY(accepted);
    for (const QString extension : {QString("csv"), QString("wlr")}) {
        const auto path = directory + "/serial-nan." + extension; worker.startRecording(path, 123);
        port->feed("DATA:1,2\r\nDATA:0,3\r\n"); worker.stopRecording(); const auto nanFrames = take(worker);
        VERIFY(nanFrames.size() == 2 && std::isnan(nanFrames[0][0]) && nanFrames[0][1] == 2 && nanFrames[1][0] == 0 && nanFrames[1][1] == 3);
        if (extension == "csv") { QFile file(path); VERIFY(file.open(QIODevice::ReadOnly) && file.readAll().contains(",1.000000,2.000000")); }
        else { int rows = 0; VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64, const QVector<double> &v, qint64) { ++rows; return rows == 1 ? v == QVector<double>({1, 2}) : v == QVector<double>({0, 3}); }, error)); VERIFY(rows == 2); }
    }
    worker.closePort(); return 0;
}
static int testNetwork(const QString &directory, const ProtocolProfile &profile, bool clientMode) {
    NetworkHandler worker; ProtocolSelection selection; selection.kind = "custom"; selection.id = profile.id;
    bool accepted = false; QObject::connect(&worker, &NetworkHandler::protocolApplyFinished, [&](quint64, bool ok, const QString &) { accepted = ok; });
    worker.applyProtocol(1, selection, profile); VERIFY(accepted);
    QTcpServer peerServer; QTcpSocket peerClient; QTcpSocket *peer = nullptr;
    if (clientMode) {
        VERIFY(peerServer.listen(QHostAddress::LocalHost, 0)); VERIFY(worker.startConnection(1, "127.0.0.1", peerServer.serverPort()));
        VERIFY(until([&] { return peerServer.hasPendingConnections(); })); peer = peerServer.nextPendingConnection();
    } else {
        VERIFY(peerServer.listen(QHostAddress::LocalHost, 0)); const int port = peerServer.serverPort(); peerServer.close();
        VERIFY(worker.startConnection(2, "127.0.0.1", port)); peerClient.connectToHost(QHostAddress::LocalHost, port); peer = &peerClient;
        VERIFY(until([&] { return peerClient.state() == QAbstractSocket::ConnectedState && worker.isConnected(); }));
    }
    int reads = 0; QObject::connect(&worker, &NetworkHandler::dataReceived, [&](const QByteArray &) { ++reads; });
    const bool binary = profile.definition.value("type") == QJsonValue("binary");
    const auto payload = binary ? binaryPacket(profile) : QByteArray("DATA:25.34,3.30\r\n");
    peer->write(payload.left(8)); peer->flush(); VERIFY(until([&] { return reads >= 1; })); VERIFY(take(worker).isEmpty());
    auto wrong = selection; wrong.id = "00000000-0000-4000-8000-000000000000"; worker.applyProtocol(2, wrong, profile); VERIFY(!accepted);
    peer->write(payload.mid(8)); peer->flush(); VERIFY(until([&] { return reads >= 2; }));
    auto frames = take(worker); VERIFY(frames.size() == 1 && std::abs(frames[0][0] - 25.34) < 1e-9 && std::abs(frames[0][1] - 3.3) < 1e-5);
    const auto path = directory + (clientMode ? "/client.wlr" : "/server.wlr"); worker.startRecording(path, 250);
    worker.applyProtocol(3, ProtocolSelection::fromLegacy(0), {}); VERIFY(!accepted);
    peer->write(binary ? QByteArray::fromHex("AA5500") + payload.repeated(2) : QByteArray("DATA:1,2\r\nDATA:3,4\r\n")); peer->flush(); VERIFY(until([&] { return reads >= 3; })); worker.stopRecording();
    int count = 0; QString error;
    VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64 timestamp, const QVector<double> &values, qint64) { ++count; return timestamp > 0 && (binary ? values[0] == 2534 && std::abs(values[1] - 3.3) < 1e-5 : values == QVector<double>({double(count * 2 - 1), double(count * 2)})); }, error)); VERIFY(count == 2);
    if (!binary) {
        const auto fourPath = directory + (clientMode ? "/client-four.wlr" : "/server-four.wlr");
        worker.startRecording(fourPath, 250); const int previousReads = reads;
        peer->write("DATA:25.34,3.30,2.5,2.8\r\n"); peer->flush(); VERIFY(until([&] { return reads > previousReads; })); worker.stopRecording();
        frames = take(worker); VERIFY(frames.size() == 3 && frames.last() == QVector<double>({25.34, 3.3, 2.5, 2.8}));
        count = 0; VERIFY(WlrRecordingWriter::visitFiles({fourPath}, [&](qint64 stamp, const QVector<double> &v, qint64) { ++count; return stamp > 0 && v == QVector<double>({25.34, 3.3, 2.5, 2.8}); }, error)); VERIFY(count == 1);
    }
    QStringList names; QString metadataError;
    VERIFY(WlrRecordingWriter::visitFiles({path}, [](qint64, const QVector<double> &, qint64) { return true; }, metadataError, nullptr, nullptr, &names));
    VERIFY(names == profile.outputChannelNames());
    auto nanDefinition = profile.definition; auto fields = nanDefinition["channels"].toArray(); auto field = fields[0].toObject();
    field.insert("max", 10); fields[0] = field; nanDefinition.insert("channels", fields); nanDefinition.insert("outOfRangePolicy", "nan-channel");
    ProtocolProfile nanProfile; VERIFY(ProtocolProfileValidator::profile(nanDefinition, nanProfile, metadataError)); worker.applyProtocol(4, selection, nanProfile); VERIFY(accepted);
    take(worker); peer->write(payload); peer->flush(); QVector<QVector<double>> nanFrames;
    VERIFY(until([&] { if (nanFrames.isEmpty()) nanFrames = take(worker); return !nanFrames.isEmpty() && std::isnan(nanFrames[0][0]) && std::abs(nanFrames[0][1] - 3.3) < 1e-5; }));
    worker.stopConnection(); return 0;
}
static int testBle(const QString &directory, const ProtocolProfile &text, const ProtocolProfile &binary) {
    BleStreamWorker worker; ProtocolSelection selection; selection.kind = "custom"; selection.id = text.id;
    int ack = 0, starts = 0; bool accepted = false;
    QObject::connect(&worker, &BleStreamWorker::protocolApplyFinished, [&](quint64 request, bool ok, const QString &) { ack = int(request); accepted = ok; });
    QObject::connect(&worker, &BleStreamWorker::recordingStartFinished, [&](quint64, quint64, bool ok, const QString &) { if (ok) ++starts; });
    const qint64 stamp = 1791370000123456700LL;
    worker.beginSession(1, selection, text); VERIFY(worker.enqueueData(1, "DATA:25.", stamp)); worker.stopRecordingNow(); VERIFY(take(worker).isEmpty());
    auto wrong = selection; wrong.id = binary.id; worker.applyProtocol(1, wrong, text); VERIFY(until([&] { return ack == 1; })); VERIFY(!accepted);
    VERIFY(worker.enqueueData(1, "34,3.30\r\n", stamp)); worker.stopRecordingNow(); auto frames = take(worker); VERIFY(frames.size() == 1 && frames[0] == QVector<double>({25.34, 3.3}));
    worker.startRecording(directory + "/ble.csv", 250, 1); VERIFY(until([&] { return starts == 1; }));
    worker.applyProtocol(2, ProtocolSelection::fromLegacy(0), {}); VERIFY(until([&] { return ack == 2; })); VERIFY(!accepted);
    VERIFY(worker.enqueueData(1, "DATA:1,2\r\nDATA:3,4\r\n", stamp)); worker.stopRecordingNow();
    QFile csv(directory + "/ble.csv"); VERIFY(csv.open(QIODevice::ReadOnly)); const auto bytes = csv.readAll();
    VERIFY(bytes.contains("1791370000.123456700,0,1.000000,2.000000") && bytes.contains("1791370000.123456700,1,3.000000,4.000000"));
    VERIFY(bytes.contains(QStringLiteral(",温度,电压\n").toUtf8()));
    take(worker); worker.startRecording(directory + "/ble-four.wlr", 250, 4); VERIFY(until([&] { return starts == 2; }));
    VERIFY(worker.enqueueData(1, "DATA:25.34,3.30,2.5,2.8\r\n", stamp)); worker.stopRecordingNow();
    frames = take(worker); VERIFY(frames.size() == 1 && frames[0] == QVector<double>({25.34, 3.3, 2.5, 2.8}));
    int fourCount = 0; QString fourError;
    VERIFY(WlrRecordingWriter::visitFiles({directory + "/ble-four.wlr"}, [&](qint64 t, const QVector<double> &v, qint64) { ++fourCount; return t == stamp && v == QVector<double>({25.34, 3.3, 2.5, 2.8}); }, fourError)); VERIFY(fourCount == 1);
    QStringList fourNames;
    VERIFY(WlrRecordingWriter::visitFiles({directory + "/ble-four.wlr"}, [](qint64, const QVector<double> &, qint64) { return true; }, fourError, nullptr, nullptr, &fourNames));
    VERIFY(fourNames.mid(0, 4) == QStringList({"温度", "电压", "Ch3", "Ch4"}));
    selection.id = binary.id; worker.applyProtocol(3, selection, binary); VERIFY(until([&] { return ack == 3; })); VERIFY(accepted);
    worker.startRecording(directory + "/ble.wlr", 250, 2); VERIFY(until([&] { return starts == 3; }));
    const auto packet = binaryPacket(binary);
    for (int i = 0; i < packet.size(); ++i) VERIFY(worker.enqueueData(1, packet.mid(i, 1), stamp + i)); worker.stopRecordingNow();
    int count = 0; QString error;
    VERIFY(WlrRecordingWriter::visitFiles({directory + "/ble.wlr"}, [&](qint64 timestamp, const QVector<double> &values, qint64) { ++count; return timestamp == stamp + packet.size() - 1 && values[0] == 2534; }, error)); VERIFY(count == 1);
    selection.id = text.id; worker.applyProtocol(4, selection, text); VERIFY(until([&] { return ack == 4; }));
    worker.startRecording(directory + "/burst.wlr", 250, 3); VERIFY(until([&] { return starts == 4; }));
    VERIFY(worker.enqueueData(1, QByteArray("DATA:1,2\r\n").repeated(10000), stamp)); worker.stopRecordingNow();
    quint64 dropped = 0; frames = worker.takePlotFrames(20000, dropped); VERIFY(frames.size() == 8192 && dropped == 1808);
    count = 0; VERIFY(WlrRecordingWriter::visitFiles({directory + "/burst.wlr"}, [&](qint64 timestamp, const QVector<double> &values, qint64) { ++count; return timestamp == stamp && values == QVector<double>({1, 2}); }, error)); VERIFY(count == 10000);
    auto nanDefinition = text.definition; auto fields = nanDefinition["channels"].toArray(); auto field = fields[0].toObject();
    field.insert("max", 0); fields[0] = field; nanDefinition.insert("channels", fields); nanDefinition.insert("outOfRangePolicy", "nan-channel");
    ProtocolProfile nanProfile; VERIFY(ProtocolProfileValidator::profile(nanDefinition, nanProfile, error)); worker.applyProtocol(5, selection, nanProfile); VERIFY(until([&] { return ack == 5; }) && accepted);
    worker.startRecording(directory + "/ble-nan.wlr", 250, 5); VERIFY(until([&] { return starts == 5; }));
    VERIFY(worker.enqueueData(1, "DATA:1,2\r\nDATA:0,3\r\n", stamp)); worker.stopRecordingNow();
    int nanRows = 0; VERIFY(WlrRecordingWriter::visitFiles({directory + "/ble-nan.wlr"}, [&](qint64, const QVector<double> &v, qint64) {
        ++nanRows; return nanRows == 1 ? v == QVector<double>({1, 2}) : v == QVector<double>({0, 3});
    }, error)); VERIFY(nanRows == 2);
    worker.endSession(); worker.stopRecordingNow(); return 0;
}
static int testConfig(const QString &directory, const ProtocolProfile &text, const ProtocolProfile &binary) {
    QString error; ProtocolProfileStore store(directory + "/protocols.json"); VERIFY(store.load(error));
    auto annotated = text.definition; auto fields = annotated["channels"].toArray(); auto field = fields[0].toObject();
    const QString note = "通道 A1 备注 / 中文 + English 123，±0.5%"; field.insert("note", note); fields[0] = field; annotated.insert("channels", fields);
    ProtocolProfile annotatedText; VERIFY(ProtocolProfileValidator::profile(annotated, annotatedText, error));
    auto original = store.snapshot(); original.profiles = {annotatedText, binary}; original.shared.kind = "custom"; original.shared.id = text.id; original.ble.kind = "custom"; original.ble.id = binary.id; VERIFY(store.commit(original, error));
    QSettings main("WaveLink", "WaveLink"), adv("WaveLink", "AdvancedApp"); main.setValue("sentinel", "keep"); main.setValue("comboProtocol", 4); main.setValue("Ble/protocol", 3); main.sync();
    const auto commands = directory + "/commands.json", ini = directory + "/config.ini";
    QSettings legacyCache("MyCompany", "WaveLink");
    const QStringList cacheNames{"editCacheDir", "CacheDir"};
    const QStringList cachePrefixes{"WaveLink/", "AdvancedApp/", "MyCompany/"};
    for (auto settings : {&main, &adv, &legacyCache}) for (const auto &key : cacheNames) settings->setValue(key, directory + "/local-cache");
    main.setValue("spinCacheLimit", 321); main.sync(); adv.sync(); legacyCache.sync();
    VERIFY(ProtocolConfigTransfer::exportFile(ini, original, commands, error)); QVariantMap values; VERIFY(ProtocolConfigTransfer::read(ini, values, error));
    for (const auto &prefix : cachePrefixes) for (const auto &key : cacheNames) VERIFY(!values.contains(prefix + key));
    VERIFY(values.value("WaveLink/spinCacheLimit").toInt() == 321);
    VERIFY(!values.contains("WaveLink/Ble/protocol") && !values.contains("WaveLink/comboProtocol") && !values.contains("AdvancedApp/Protocol") && !values.contains("AdvancedApp/Endianness"));
    VERIFY(values.contains("DataProtocols/json")); ProtocolSnapshot roundTrip; VERIFY(ProtocolProfileValidator::decode(values["DataProtocols/json"].toString().toUtf8(), true, roundTrip, error)); VERIFY(roundTrip.shared.id == text.id && roundTrip.ble.id == text.id);
    VERIFY(roundTrip.profiles[0].definition["channels"].toArray()[0].toObject()["note"].toString() == note);
    values.insert("WaveLink/comboProtocol", 0); values.insert("AdvancedApp/Protocol", 0); values.insert("AdvancedApp/Endianness", 1); values.insert("WaveLink/Ble/protocol", 0); values.insert("WaveLink/sentinel", "new");
    // Simulate another PC's old exports, with both grouped and flat path keys.
    for (const auto &prefix : cachePrefixes) for (const auto &key : cacheNames) values.insert(prefix + key, "Z:/missing-on-this-PC/foreign-cache");
    for (const auto &key : cacheNames) values.insert(key, "Z:/missing-on-this-PC/flat-cache");
    VERIFY(ProtocolConfigTransfer::apply(values, store, -1, true, 0, commands, error)); VERIFY(store.snapshot().shared.id == text.id && store.snapshot().ble.id == text.id && main.value("comboProtocol").toInt() == 4 && main.value("sentinel").toString() == "new");
    for (auto settings : {&main, &adv, &legacyCache}) for (const auto &key : cacheNames) VERIFY(settings->value(key).toString() == directory + "/local-cache");
    // A fresh installation must not acquire any cache path from a foreign file.
    for (auto settings : {&main, &adv, &legacyCache}) for (const auto &key : cacheNames) settings->remove(key);
    VERIFY(ProtocolConfigTransfer::apply(values, store, -1, false, 0, commands, error));
    for (auto settings : {&main, &adv, &legacyCache}) for (const auto &key : cacheNames) VERIFY(!settings->contains(key));
    VERIFY(!main.contains("Ble/protocol"));
    VERIFY(ProtocolConfigTransfer::apply(values, store, 0, true, 0, commands, error)); VERIFY(store.snapshot().profiles.size() == 4 && store.snapshot().shared.id != text.id && store.snapshot().ble.id != binary.id);
    const auto appendedId = store.snapshot().shared.id; VERIFY(ProtocolConfigTransfer::apply(values, store, 1, false, 0, commands, error)); VERIFY(store.snapshot().shared.id == appendedId);
    const auto beforeReplace = ProtocolProfileValidator::encode(store.snapshot(), true, error);
    VERIFY(!ProtocolConfigTransfer::apply(values, store, 2, false, 0, commands, error)); VERIFY(ProtocolProfileValidator::encode(store.snapshot(), true, error) == beforeReplace);
    VERIFY(ProtocolConfigTransfer::apply(values, store, 2, true, 0, commands, error)); VERIFY(store.snapshot().profiles.size() == 2 && store.snapshot().shared.id == text.id);
    VERIFY(store.snapshot().profiles[0].definition["channels"].toArray()[0].toObject()["note"].toString() == note);
    auto invalid = values; invalid.insert("DataProtocols/json", "{broken}"); invalid.insert("WaveLink/sentinel", "corrupt");
    VERIFY(!ProtocolConfigTransfer::apply(invalid, store, 1, true, 0, commands, error)); VERIFY(main.value("sentinel").toString() == "new");
    const auto saved = ProtocolProfileValidator::encode(store.snapshot(), true, error);
    auto withCommands = values; withCommands.insert("CommandPanel/json", "[{\"id\":\"p\",\"name\":\"panel\",\"commands\":[]}]");
    // Failure after committing the protocol library must restore it and all
    // settings; no command-file parent directory exists at the target.
    VERIFY(!ProtocolConfigTransfer::apply(withCommands, store, 0, true, 1, directory + "/missing/commands.json", error));
    VERIFY(ProtocolProfileValidator::encode(store.snapshot(), true, error) == saved && main.value("sentinel").toString() == "new");
    VERIFY(!QFile::exists(directory + "/global-config-transaction.json"));
    VERIFY(ProtocolConfigTransfer::apply(withCommands, store, 1, true, 1, commands, error));
    VERIFY(ProtocolConfigTransfer::apply(withCommands, store, 1, true, 2, commands, error));
    QFile commandFile(commands); VERIFY(commandFile.open(QIODevice::ReadOnly)); const auto array = QJsonDocument::fromJson(commandFile.readAll()).array(); VERIFY(array.size() == 2 && array[1].toObject()["name"].toString() == "panel_1"); commandFile.close();
    // A real full export carries the complete library, selected protocol and
    // command panels together. Names, channel rules and binary rules survive.
    const auto exportSnapshot = ProtocolProfileValidator::encode(store.snapshot(), true, error);
    VERIFY(ProtocolConfigTransfer::exportFile(ini, store.snapshot(), commands, error));
    QVariantMap complete; VERIFY(ProtocolConfigTransfer::read(ini, complete, error));
    VERIFY(complete.contains("DataProtocols/json") && complete.contains("CommandPanel/json"));
    VERIFY(ProtocolProfileValidator::decode(complete["DataProtocols/json"].toString().toUtf8(), true, roundTrip, error));
    VERIFY(ProtocolProfileValidator::encode(roundTrip, true, error) == exportSnapshot);
    VERIFY(QJsonDocument::fromJson(complete["CommandPanel/json"].toString().toUtf8()).array() == array);
    // Skipping corrupted protocol data must still allow normal settings and
    // command panels to import, without touching the library or its selection.
    auto skipBroken = complete; skipBroken.insert("DataProtocols/json", "{broken}");
    skipBroken.insert("WaveLink/configSkipTest", "imported");
    VERIFY(ProtocolConfigTransfer::apply(skipBroken, store, -1, true, 1, commands, error));
    VERIFY(ProtocolProfileValidator::encode(store.snapshot(), true, error) == exportSnapshot && main.value("configSkipTest").toString() == "imported");
    VERIFY(commandFile.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(commandFile.readAll()).array() == array); commandFile.close();
    QVariantMap legacy{{"comboProtocol", 2}, {"Endianness", 1}}; VERIFY(ProtocolConfigTransfer::apply(legacy, store, 1, true, 0, commands, error)); VERIFY(store.snapshot().shared.id == "builtin.justfloat" && store.snapshot().shared.bigEndian && store.snapshot().profiles.size() == 2);
    // A stale multi-instance store must neither undo the newer library nor
    // leave a journal that falsely claims it modified that library.
    ProtocolProfileStore stale(store.path()); VERIFY(stale.load(error)); QString id; auto d = text.definition; d.insert("name", "updated externally"); VERIFY(store.saveProfile(d, id, error));
    VERIFY(!ProtocolConfigTransfer::apply(values, stale, 1, true, 0, commands, error)); VERIFY(!QFile::exists(directory + "/global-config-transaction.json"));
    VERIFY(store.load(error) && store.snapshot().profiles[0].name == "updated externally");
    // Simulate termination after protocol/settings writes but before removal
    // of the durable journal, then exercise the exact startup recovery path.
    auto pack = [](QSettings &s) { QVariantMap map; for (const auto &key : s.allKeys()) map.insert(key, s.value(key)); QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_5_12); stream << map; return QString::fromLatin1(bytes.toBase64()); };
    QSettings my("MyCompany", "WaveLink");
    QJsonObject journal{{"schema", 1}, {"main", pack(main)}, {"advanced", pack(adv)}, {"my", pack(my)}};
    auto altered = store.snapshot(); altered.shared = ProtocolSelection::fromLegacy(0);
    const auto alteredBytes = ProtocolProfileValidator::encode(altered, true, error);
    const QStringList paths{store.path(), store.path() + ".bak", commands}; QJsonArray entries; QVector<QByteArray> before;
    for (int i = 0; i < paths.size(); ++i) {
        QFile f(paths[i]); VERIFY(f.open(QIODevice::ReadOnly)); before.append(f.readAll());
        entries.append(QJsonObject{{"path", QFileInfo(paths[i]).absoluteFilePath()}, {"existed", true}, {"before", QString::fromLatin1(before.last().toBase64())}, {"after", QString::fromLatin1((i == 0 ? alteredBytes : before.last()).toBase64())}});
    }
    journal.insert("files", entries);
    QFile journalFile(directory + "/global-config-transaction.json"); VERIFY(journalFile.open(QIODevice::WriteOnly)); journalFile.write(QJsonDocument(journal).toJson()); journalFile.close();
    VERIFY(ProtocolProfileStore::writeFile(store.path(), alteredBytes, error)); main.setValue("sentinel", "interrupted"); main.sync();
    VERIFY(ProtocolConfigTransfer::recover(store.path(), commands, error)); VERIFY(main.value("sentinel").toString() == "new" && !QFile::exists(journalFile.fileName()));
    VERIFY(store.load(error) && ProtocolProfileValidator::encode(store.snapshot(), true, error) == before[0]);
    return 0;
}
static int testDynamicTransport(const QString &directory, int delimiterMode = 0) {
    QString error; ProtocolSnapshot doc;
    QFile file(QStringLiteral(WAVELINK_PROTOCOL_EXAMPLE_DIR) + (delimiterMode ? "/delimited_array_justfloat.json" : "/dynamic_array_float32.json"));
    VERIFY(file.open(QIODevice::ReadOnly) && ProtocolProfileValidator::decode(file.readAll(), false, doc, error));
    auto profile = doc.profiles.first();
    if (delimiterMode >= 2) {
        auto d = profile.definition, frame = d.value("frame").toObject();
        frame.insert("headerHex", "AA55"); frame.insert("tailHex", delimiterMode == 2 ? "" : "0D0A");
        frame.insert("payloadOffset", 2); frame.insert("payloadEndOffset", delimiterMode == 2 ? 0 : 2); frame.insert("minFrameBytes", delimiterMode == 2 ? 6 : 8);
        d.insert("frame", frame); VERIFY(ProtocolProfileValidator::profile(d, profile, error));
    }
    ProtocolSelection selection; selection.kind = "custom"; selection.id = profile.id;
    QByteArray all; for (int count : {2, 5, 1}) { QByteArray frame; VERIFY(ProtocolSampleGenerator::generate(profile, frame, error, count)); if (delimiterMode == 2) frame.chop(2); all += frame; }
    if (delimiterMode == 2) all += QByteArray::fromHex("AA55");
    auto goodFrames = [](const QVector<QVector<double>> &frames) { return frames.size() == 3 && frames[0] == QVector<double>({1, 2}) && frames[1] == QVector<double>({1, 2, 3, 4, 5}) && frames[2] == QVector<double>({1}); };
    auto *port = new ReceivePort; TestSerialHandler serial(port); serial.applyProtocol(1, selection, profile); serial.openPort("fake", 115200, 8, "None", "1", 1, false, false);
    serial.startRecording(directory + "/dynamic-serial.csv", 250);
    for (char b : all) port->feed(QByteArray(1, b)); VERIFY(serial.skippedRecordingFrames() == 2); serial.stopRecording(); VERIFY(goodFrames(take(serial)));
    VERIFY(!QFile::exists(directory + "/dynamic-serial_part002.csv")); serial.closePort();
    BleStreamWorker ble; ble.beginSession(1, selection, profile); int starts = 0; bool failed = false;
    QObject::connect(&ble, &BleStreamWorker::recordingStartFinished, [&](quint64, quint64, bool ok, const QString &) { starts += ok ? 1 : 0; failed = failed || !ok; });
    ble.startRecording(directory + "/dynamic-ble.wlr", 250, 1); VERIFY(until([&] { return starts == 1 || failed; }) && !failed);
    for (int i = 0; i < all.size(); ++i) VERIFY(ble.enqueueData(1, all.mid(i, 1), 1700000000000000000LL + i));
    VERIFY(until([&] { return ble.parsedFrames() == 3; }) && ble.skippedRecordingFrames() == 2 && ble.recordedFrames() == 1);
    ble.stopRecordingNow(); VERIFY(goodFrames(take(ble)));
    int recorded = 0; VERIFY(WlrRecordingWriter::visitFiles({directory + "/dynamic-ble.wlr"}, [&](qint64, const QVector<double> &v, qint64) { ++recorded; return v == QVector<double>({1, 2}); }, error) && recorded == 1); ble.endSession();
    for (bool client : {false, true}) {
        NetworkHandler worker; worker.applyProtocol(1, selection, profile); QTcpServer server; QTcpSocket socket; QTcpSocket *peer; bool peerConnected = false;
        QObject::connect(&worker, &NetworkHandler::clientConnected, [&](const QString &) { peerConnected = true; });
        VERIFY(server.listen(QHostAddress::LocalHost, 0)); const int port = server.serverPort();
        if (client) { VERIFY(worker.startConnection(1, "127.0.0.1", port)); VERIFY(until([&] { return server.hasPendingConnections(); })); peer = server.nextPendingConnection(); }
        else { server.close(); VERIFY(worker.startConnection(2, "127.0.0.1", port)); socket.connectToHost(QHostAddress::LocalHost, port); peer = &socket; VERIFY(until([&] { return peerConnected && socket.state() == QAbstractSocket::ConnectedState; })); }
        QVector<QVector<double>> received; const auto path = directory + (client ? "/dynamic-client.wlr" : "/dynamic-server.wlr"); worker.startRecording(path, 250);
        VERIFY(peer->write(all) == all.size()); peer->flush(); VERIFY(until([&] { received += take(worker); return received.size() >= 3; })); VERIFY(goodFrames(received) && worker.skippedRecordingFrames() == 2); worker.stopRecording();
        recorded = 0; VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64, const QVector<double> &v, qint64) { ++recorded; return v == QVector<double>({1, 2}); }, error) && recorded == 1); worker.stopConnection();
    }
    NetworkHandler udp; udp.applyProtocol(1, selection, profile); quint16 localPort = 0;
    QObject::connect(&udp, &NetworkHandler::localEndpointChanged, [&](const QString &, int port) { localPort = quint16(port); });
    VERIFY(udp.startUdp("127.0.0.1", 0, "", 0, false) && localPort);
    QUdpSocket peer; VERIFY(peer.writeDatagram(all, QHostAddress::LocalHost, localPort) == all.size());
    VERIFY(until([&] { return udp.getRxCount() == all.size(); }) && goodFrames(take(udp)));
    auto rx = udp.getRxCount();
    VERIFY(peer.writeDatagram(all.left(1), QHostAddress::LocalHost, localPort) == 1);
    VERIFY(until([&] { return udp.getRxCount() == rx + 1; }) && take(udp).isEmpty());
    // Datagram boundary clears the first byte: this complete next datagram
    // still decodes all frames instead of being joined to a previous prefix.
    VERIFY(peer.writeDatagram(all, QHostAddress::LocalHost, localPort) == all.size());
    VERIFY(until([&] { return udp.getRxCount() == rx + 1 + all.size(); }) && goodFrames(take(udp))); udp.stopConnection();
    return 0;
}
int main(int argc, char **argv) {
    TestRegistrySandbox registry; VERIFY(registry.ready()); QCoreApplication app(argc, argv); QTemporaryDir directory; VERIFY(directory.isValid());
    qRegisterMetaType<ProtocolSelection>("DataProtocols::ProtocolSelection"); qRegisterMetaType<ProtocolProfile>("DataProtocols::ProtocolProfile");
    const auto text = example(false), binary = example(true); VERIFY(!text.id.isEmpty() && !binary.id.isEmpty());
    VERIFY(testSerial(directory.path(), text, binary) == 0);
    VERIFY(testNetwork(directory.path(), text, true) == 0 && testNetwork(directory.path(), text, false) == 0);
    VERIFY(testBle(directory.path(), text, binary) == 0); VERIFY(testConfig(directory.path(), text, binary) == 0);
    const auto variable = example(true, true); VERIFY(!variable.id.isEmpty());
    const auto advancedDir = directory.path() + "/advanced"; VERIFY(QDir().mkpath(advancedDir));
    VERIFY(testSerial(advancedDir, text, variable) == 0);
    VERIFY(testNetwork(advancedDir, variable, true) == 0 && testNetwork(advancedDir, variable, false) == 0);
    VERIFY(testBle(advancedDir, text, variable) == 0 && testConfig(advancedDir, text, variable) == 0);
    VERIFY(testDynamicTransport(directory.path()) == 0);
    const auto delimitedDir = directory.path() + "/delimited"; VERIFY(QDir().mkpath(delimitedDir));
    for (int mode = 1; mode <= 3; ++mode) {
        const auto modeDir = delimitedDir + QString("/mode%1").arg(mode); VERIFY(QDir().mkpath(modeDir));
        VERIFY(testDynamicTransport(modeDir, mode) == 0);
    }
    ProtocolSnapshot delimitedDoc; QString error; QFile file(QStringLiteral(WAVELINK_PROTOCOL_EXAMPLE_DIR) + "/delimited_array_justfloat.json");
    VERIFY(file.open(QIODevice::ReadOnly) && ProtocolProfileValidator::decode(file.readAll(), false, delimitedDoc, error));
    VERIFY(testConfig(delimitedDir, text, delimitedDoc.profiles.first()) == 0);
    std::cout << "Custom serial/TCP client/TCP server/BLE parsing, frozen recording, receive timestamps, full-rate recording and transactional config tests passed.\n"; return 0;
}
