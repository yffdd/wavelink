#include "ProtocolParserFactory.h"
#include "ProtocolSampleGenerator.h"
#include "CustomProtocolParser.h"
#include "fakeserialbackend.h"
#include "networkhandler.h"
#include "ble/blestreamworker.h"
#include "recordingchannelnames.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QBuffer>
#include <functional>
#include <memory>
#include <cmath>
#include <iostream>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Raw recording failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(const std::function<bool()> &condition) {
    QElapsedTimer clock; clock.start(); while (!condition() && clock.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); } return condition();
}
class ReceivePort : public FakePort {
public:
    void feed(const QByteArray &bytes) { incoming += bytes; emit readyRead(); }
    qint64 bytesAvailable() const override { return incoming.size() + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 size) override { const auto n = qMin<qint64>(size, incoming.size()); memcpy(data, incoming.constData(), size_t(n)); incoming.remove(0, int(n)); return n; }
private: QByteArray incoming;
};
static bool sameValues(const QVector<double> &a, const QVector<double> &b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) if (a[i] != b[i] && !(std::isnan(a[i]) && std::isnan(b[i]))) return false;
    return true;
}
static bool savedValues(const QString &path, const QVector<double> &values, const QStringList &names) {
    QString error; int rows = 0; QByteArray csv;
    if (path.endsWith(".wlr")) {
        QStringList storedNames;
        if (!WlrRecordingWriter::visitFiles({path}, [&](qint64 time, const QVector<double> &stored, qint64) {
            ++rows; return time > 1000000000000000000LL && sameValues(stored, values);
        }, error, nullptr, nullptr, &storedNames) || rows != 2 || storedNames != names) return false;
        QBuffer output(&csv); output.open(QIODevice::WriteOnly);
        if (!WlrRecordingWriter::exportToCsv({path}, &output, error)) return false;
    } else {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return false; csv = file.readAll();
    }
    const auto lines = csv.trimmed().split('\n');
    if (lines.size() != 3 || !lines[0].startsWith(RecordingChannels::csvUtf8Bom() + "UnixTime(s;FS=250),SampleIndex")) return false;
    for (int row = 1; row <= 2; ++row) {
        const auto cells = lines[row].split(','); if (cells.size() != values.size() + 2) return false;
        if (cells[0].toDouble() < 1e9 || cells[1].toInt() != row - 1) return false;
        for (int ch = 0; ch < values.size(); ++ch) {
            if (std::isnan(values[ch])) { if (cells[ch + 2].toLower() != "nan") return false; }
            else if (cells[ch + 2].toDouble() != values[ch]) return false;
        }
    }
    return true;
}
static bool configureMonitor(StatusMonitoring::Session *session) {
    using namespace StatusMonitoring;
    Rule raw; raw.id="raw"; raw.name="raw"; raw.kind=RuleKind::Threshold;
    Range any; any.state="valid"; raw.ranges={any};
    Rule converted=raw; converted.id="converted"; converted.converted=true;
    QString error; return session->configure({raw,converted},error);
}
template<class Worker> static bool displayConverted(Worker &worker, const QVector<double> &expected) {
    QVector<QVector<double>> frames;
    if (!until([&] { quint64 dropped = 0; frames += worker.takePlotFrames(8, dropped); return frames.size() >= 2; }) || frames.size() != 2) return false;
    for (const auto &frame : frames) {
        if (!sameValues(frame, expected)) return false;
    }
    const auto states=worker.statusMonitor()->snapshot();
    if(states.size()!=2 || states[0].quality!=StatusMonitoring::Quality::Valid || states[0].sequence!=2
        || states[1].quality!=(std::isnan(expected[0]) ? StatusMonitoring::Quality::Invalid : StatusMonitoring::Quality::Valid)) return false;
    return true;
}
static int transport(int mode, const ProtocolProfile &profile, const QByteArray &sample, const QVector<double> &raw, const QVector<double> &display, const QString &path) {
    ProtocolSelection selection; selection.kind = "custom"; selection.id = profile.id;
    if (mode == 0) {
        auto port = new ReceivePort; TestSerialHandler worker(port); worker.applyProtocol(1, selection, profile);
        VERIFY(configureMonitor(worker.statusMonitor()));
        worker.openPort("SOFTWARE_ONLY", 115200, 8, "None", "1", 1, false, false);
        worker.startRecording(path, 250); port->feed(sample.repeated(2));
        VERIFY(displayConverted(worker, display) && worker.finishRecording().isEmpty()); worker.closePort();
        VERIFY(worker.statusMonitor()->snapshot()[0].quality==StatusMonitoring::Quality::Disconnected);
    } else if (mode == 4) {
        BleStreamWorker worker; VERIFY(configureMonitor(worker.statusMonitor())); worker.beginSession(1, selection, profile); worker.startRecording(path, 250, 1);
        VERIFY(worker.enqueueData(1, sample.repeated(2), 1791370000123456700LL));
        VERIFY(worker.stopRecordingNow().isEmpty() && displayConverted(worker, display));
        VERIFY(worker.parsedFrames() == 2 && worker.recordedFrames() == 2); worker.endSession(); worker.stopRecordingNow();
        VERIFY(worker.statusMonitor()->snapshot()[0].quality==StatusMonitoring::Quality::Disconnected);
    } else {
        NetworkHandler worker; worker.applyProtocol(1, selection, profile);
        VERIFY(configureMonitor(worker.statusMonitor()));
        QTcpServer server; QTcpSocket client; QUdpSocket udp; QTcpSocket *peer = nullptr;
        server.setProxy(QNetworkProxy::NoProxy); client.setProxy(QNetworkProxy::NoProxy); udp.setProxy(QNetworkProxy::NoProxy);
        if (mode == 1) {
            VERIFY(server.listen(QHostAddress::LocalHost, 0));
            VERIFY(worker.startConnection(1, "127.0.0.1", server.serverPort()) && until([&] { return server.hasPendingConnections(); }));
            peer = server.nextPendingConnection();
        } else if (mode == 2) {
            VERIFY(worker.startConnection(2, "127.0.0.1", 0));
            auto listener = worker.findChild<QTcpServer *>(); VERIFY(listener);
            client.connectToHost(QHostAddress::LocalHost, listener->serverPort()); peer = &client;
            VERIFY(until([&] { return worker.sendReady() && client.state() == QAbstractSocket::ConnectedState; }));
        } else VERIFY(worker.startUdp("127.0.0.1", 0, "", 0, true));
        worker.startRecording(path, 250);
        if (mode == 5) {
            auto listener = worker.findChild<QUdpSocket *>(); VERIFY(listener);
            VERIFY(udp.writeDatagram(sample.repeated(2), QHostAddress::LocalHost, listener->localPort()) == sample.size() * 2);
        } else { VERIFY(peer->write(sample.repeated(2)) == sample.size() * 2); peer->flush(); }
        VERIFY(displayConverted(worker, display) && worker.finishRecording().isEmpty()); worker.stopConnection();
        VERIFY(worker.statusMonitor()->snapshot()[0].quality==StatusMonitoring::Quality::Disconnected);
    }
    VERIFY(savedValues(path, profile.recordsConvertedValues() ? display : raw, profile.outputChannelNames())); return 0;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QTemporaryDir dir; VERIFY(dir.isValid());
    qRegisterMetaType<QVector<double>>("QVector<double>"); QString error;
    QStringList fixtures{"text_temperature_voltage.json", "binary_temperature_voltage.json", "length_temperature_voltage.json", "dynamic_array_float32.json", "keyvalue"};
    for (int index = 0; index < fixtures.size(); ++index) {
        ProtocolProfile original;
        if (fixtures[index] == "keyvalue") VERIFY(ProtocolProfileValidator::profile(ProtocolProfileValidator::keyValueTemplate(), original, error));
        else {
            QFile file(QStringLiteral(WAVELINK_PROTOCOL_EXAMPLE_DIR) + '/' + fixtures[index]); ProtocolSnapshot document;
            VERIFY(file.open(QIODevice::ReadOnly) && ProtocolProfileValidator::decode(file.readAll(), false, document, error)); original = document.profiles.first();
        }
        QByteArray sample; VERIFY(ProtocolSampleGenerator::generate(original, sample, error, original.isDynamicArray() ? 4 : 0));
        if (fixtures[index] == "text_temperature_voltage.json") {
            const int lastColumn = sample.lastIndexOf(',') + 1;
            VERIFY(lastColumn > 0 && sample.endsWith("\r\n"));
            sample.replace(lastColumn, sample.size() - lastColumn - 2, "NaN");
        }
        QVector<double> raw;
        std::unique_ptr<ProtocolParser> decoder(ProtocolParserFactory::createCustom(original, nullptr, error)); VERIFY(decoder);
        QObject::connect(qobject_cast<CustomProtocolParser *>(decoder.get()), &CustomProtocolParser::frameDecoded, [&](const QVector<double> &values, const QVector<double> &) { raw = values; });
        VERIFY(decoder->processData(sample.constData(), sample.size()) == sample.size() && raw.size() >= 2);
        if (fixtures[index] == "text_temperature_voltage.json") VERIFY(std::isnan(raw.last()));
        auto definition = original.definition; auto fields = original.outputChannels(raw.size());
        for (int ch = 0; ch < fields.size(); ++ch) {
            auto field = fields[ch].toObject(); field.insert("scale", ch == 0 ? 0 : 2); field.insert("bias", ch == 0 ? 10 : -1);
            field.remove("min"); field.remove("max"); if (ch == 0) field.insert("max", 9); fields[ch] = field;
        }
        definition.insert("channels", fields); definition.insert("outOfRangePolicy", "nan-channel");
        QVector<double> display = raw; display[0] = qQNaN();
        for (int ch = 1; ch < display.size(); ++ch) display[ch] = raw[ch] * 2 - 1;
        // Text and arrays must retain every unconfigured value unchanged,
        // including in converted recording mode (implicit scale 1, bias 0).
        if (original.isDynamicArray() || original.definition.value("type") == QJsonValue("text")) {
            fields.removeLast(); definition.insert("channels", fields); display.last() = raw.last();
        }
        ProtocolProfile profile; VERIFY(ProtocolProfileValidator::profile(definition, profile, error) && !profile.recordsConvertedValues());
        for (const QString kind : {QString("raw"), QString("converted")}) {
            definition.insert("recordingValueMode", kind); VERIFY(ProtocolProfileValidator::profile(definition, profile, error));
            QVector<double> observedRaw, observedRecording;
            std::unique_ptr<ProtocolParser> parser(ProtocolParserFactory::createCustom(profile, nullptr, error)); VERIFY(parser);
            QObject::connect(parser.get(), &ProtocolParser::rawFrameReady, [&](const QVector<double> &v) { observedRaw = v; });
            QObject::connect(parser.get(), &ProtocolParser::recordingFrameReady, [&](const QVector<double> &v) { observedRecording = v; });
            VERIFY(parser->processData(sample.constData(), sample.size()) == sample.size());
            VERIFY(sameValues(observedRaw, raw) && sameValues(observedRecording, kind == "raw" ? raw : display));
            for (int mode : {0, 1, 2, 4, 5}) for (const QString suffix : {QString("csv"), QString("wlr")})
                VERIFY(transport(mode, profile, sample, raw, display, dir.filePath(QString("%1-%2-%3.%4").arg(kind).arg(index).arg(mode).arg(suffix))) == 0);
        }
        definition.insert("recordingValueMode", "unknown"); VERIFY(!ProtocolProfileValidator::profile(definition, profile, error));
        definition.insert("recordingValueMode", 1); VERIFY(!ProtocolProfileValidator::profile(definition, profile, error));
    }
    std::cout << "Raw/converted CSV/WLR recordings, unchanged implicit channels, NaN handling and CSV conversion passed: five custom protocol layouts over serial/TCP client/server/BLE/UDP (100 combinations).\n";
    return 0;
}
