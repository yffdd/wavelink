#include "ble/blehostclient.h"
#include "ble/bleipc.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <functional>
#include <QTemporaryDir>
#include <QFile>
#include <QDateTime>
#include <QEvent>
#include "ble/blesendpolicy.h"

static bool until(const std::function<bool()> &condition, int timeout = 5000)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QThread::msleep(2); }
    return condition();
}
#define VERIFY(condition) do { if (!(condition)) { std::cerr << "Failed at line " << __LINE__ << ": " #condition << "\n"; return 1; } } while (0)
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    VERIFY(argc == 2);
    QString error; QJsonObject decoded; QByteArray buffer;
    const auto frame = BleIpc::encode({{"value", "中文"}});
    for (int i = 0; i < frame.size(); ++i) {
        buffer += frame[i]; const bool ready = BleIpc::take(buffer, decoded, error);
        VERIFY(error.isEmpty()); VERIFY(ready == (i == frame.size() - 1));
    }
    VERIFY(decoded.value("value").toString() == QStringLiteral("中文"));
    buffer = frame + frame; VERIFY(BleIpc::take(buffer, decoded, error)); VERIFY(BleIpc::take(buffer, decoded, error)); VERIFY(buffer.isEmpty());
    buffer = QByteArray(4, '\0'); VERIFY(!BleIpc::take(buffer, decoded, error)); VERIFY(!error.isEmpty());
    qToLittleEndian<quint32>(BleIpc::MaxPayload + 1, reinterpret_cast<uchar *>(buffer.data()));
    VERIFY(!BleIpc::take(buffer, decoded, error)); VERIFY(!error.isEmpty());

    BleHostClient client(nullptr, QString::fromLocal8Bit(argv[1]));
    int found = 0, failures = 0, readFinished = 0; QByteArray received;
    QVector<qint64> timestamps;
    QObject::connect(&client, &BleHostClient::notificationReceived, [&](const QByteArray &, qint64 time) { timestamps.append(time); });
    QObject::connect(&client, &BleHostClient::deviceFound, [&] { ++found; });
    QObject::connect(&client, &BleHostClient::errorOccurred, [&](const QString &, bool) { ++failures; });
    QObject::connect(&client, &BleHostClient::dataReceived, [&](const QByteArray &data) { received += data; });
    QObject::connect(&client, &BleHostClient::characteristicRead, [&](int, const QByteArray &data) { if (data == "hi") ++readFinished; });
    client.scan(); VERIFY(until([&] { return found == 1; })); client.stopScan(); VERIFY(client.state() == BleHostClient::State::Idle);
    client.connectDevice("1", 0); VERIFY(until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; }));
    VERIFY(client.readCharacteristic(1)); VERIFY(!client.configure(1, 1, false));
    VERIFY(until([&] { return readFinished == 1 && !client.readPending(); }));
    VERIFY(client.readCharacteristic(9)); VERIFY(until([&] { return failures == 1 && !client.readPending(); }));
    VERIFY(client.state() == BleHostClient::State::AwaitingConfiguration);
    VERIFY(client.configure(1, 1, false)); VERIFY(until([&] { return client.state() == BleHostClient::State::Ready && received == "preabc"; }));
    VERIFY(client.rxCount() == 6); VERIFY(client.canSend() && client.canReceive());
    VERIFY(client.diagnostics().value("notifications").toString() == "2");
    VERIFY(client.diagnostics().value("connectMs").toDouble(-1) >= 0 && client.diagnostics().value("discoverMs").toDouble(-1) >= 0);
    VERIFY(timestamps.size() == 2 && timestamps[0] > 1700000000000000000LL && timestamps[1] >= timestamps[0]);
    VERIFY(!client.sendData(QByteArray(256 * 1024 + 1, 'x')));
    VERIFY(client.sendData(QByteArray(70, 'x'))); VERIFY(until([&] { return client.txCount() == 70; }));
    QTemporaryDir temp; QFile source(temp.filePath("send.bin")); VERIFY(source.open(QIODevice::WriteOnly));
    QByteArray fileData; for (int i = 0; i < 4207; ++i) fileData += char(i % 256);
    VERIFY(source.write(fileData) == fileData.size()); source.close();
    QString fileError; int fileSuccess = 0, fileStopped = 0; qint64 progress = -1;
    QObject::connect(&client, &BleHostClient::fileSendFinished, [&](bool ok, const QString &) { ok ? ++fileSuccess : ++fileStopped; });
    QObject::connect(&client, &BleHostClient::fileSendProgress, [&](qint64 done, qint64 total) { if (total == fileData.size()) progress = done; });
    received.clear(); VERIFY(client.startFileSend(source.fileName(), fileError));
    VERIFY(!client.sendData("command must not interleave"));
    // Fragmented fake pipe messages deliberately add per-packet latency.
    const bool completedFile = until([&] { return fileSuccess == 1; }, 45000);
    if (!completedFile) std::cerr << "file progress " << progress << " active " << client.fileSendActive() << " rx " << received.size() << " stopped " << fileStopped << " tx " << client.txCount() << "\n";
    VERIFY(completedFile);
    VERIFY(progress == fileData.size() && received == fileData && !client.fileSendActive());
    VERIFY(client.txCount() == 70 + quint64(fileData.size()));
    VERIFY(until([&] { return !client.fileSendBusy(); }));
    bool cancelOnData = true, cancelledPacketBlocked = false;
    QObject::connect(&client, &BleHostClient::dataReceived, [&](const QByteArray &) {
        if (cancelOnData && client.fileSendActive()) {
            cancelOnData = false; client.cancelFileSend();
            cancelledPacketBlocked = !client.sendData("must wait for cancelled in-flight packet");
        }
    });
    VERIFY(client.startFileSend(source.fileName(), fileError));
    VERIFY(until([&] { return fileStopped == 1; }));
    VERIFY(!client.fileSendActive() && cancelledPacketBlocked);
    VERIFY(until([&] { return !client.fileSendBusy(); }));
    const auto beforeAfterCancel = client.txCount();
    VERIFY(client.sendData("after cancel"));
    VERIFY(until([&] { return received.endsWith("after cancel"); }));
    VERIFY(until([&] { return client.txCount() == beforeAfterCancel + QByteArray("after cancel").size(); }));
    VERIFY(fileSuccess == 1);
    const int beforeFileFailure = fileStopped;
    VERIFY(client.startFileSend(temp.filePath("missing-file.bin"), fileError));
    VERIFY(until([&] { return fileStopped == beforeFileFailure + 1 && !client.fileSendBusy(); }));
    VERIFY(client.state() == BleHostClient::State::Ready && client.canReceive());
    const auto beforeHealthy = client.txCount();
    VERIFY(client.sendData("healthy after disk error"));
    VERIFY(until([&] { return received.endsWith("healthy after disk error"); }));
    VERIFY(until([&] { return client.txCount() == beforeHealthy + QByteArray("healthy after disk error").size(); }));
    client.resetCounters(); VERIFY(client.txCount() == 0 && client.rxCount() == 0);
    QElapsedTimer cancel; cancel.start(); client.disconnectDevice(); VERIFY(cancel.elapsed() < 250);
    VERIFY(!client.sendData("x")); VERIFY(client.state() == BleHostClient::State::Idle);
    for (const auto address : {"2", "4", "5", "6"}) {
        const int before = failures; client.connectDevice(address, 0);
        VERIFY(until([&] { return failures == before + 1; })); VERIFY(client.state() == BleHostClient::State::Idle);
    }
    // Cancellation + immediate reconnect; old child output cannot change the new session.
    for (int i = 0; i < 10; ++i) { client.connectDevice("3", 0); QCoreApplication::processEvents(); client.disconnectDevice(); }
    const int before = failures; received.clear(); client.connectDevice("1", 0);
    VERIFY(until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; }));
    VERIFY(client.configure(1, 0, false)); VERIFY(until([&] { return client.state() == BleHostClient::State::Ready; }));
    VERIFY(!client.canSend() && client.canReceive()); VERIFY(!client.sendData("x")); VERIFY(failures == before);
    client.disconnectDevice(); QCoreApplication::processEvents();
    // These are software process cycles, NOT 100 wireless reconnections.
    for (int cycle = 0; cycle < 100; ++cycle) {
        received.clear(); client.connectDevice("1", cycle % 2);
        VERIFY(until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; }));
        VERIFY(client.configure(1, 1, cycle % 2));
        VERIFY(until([&] { return client.state() == BleHostClient::State::Ready && received == "preabc"; }));
        VERIFY(client.sendData("ack")); VERIFY(until([&] { return client.txCount() == 3 && received.endsWith("ack"); }));
        VERIFY(client.diagnostics().value("notifications").toString() == "3");
        client.disconnectDevice();
        VERIFY(until([&] { return client.findChildren<QProcess *>().isEmpty(); }));
        VERIFY(client.state() == BleHostClient::State::Idle && failures == before);
    }
    std::cout << "100 software connect/configure/duplex/disconnect cycles passed (both address types and write modes; no radio access).\n";
    VERIFY(BleSendPolicy::limit(0) == 20 && BleSendPolicy::limit(23) == 20 && BleSendPolicy::limit(247) == 244 && BleSendPolicy::limit(517) == 512);
    bool collect = false; QVector<int> packets; QVector<qint64> packetTimes; QElapsedTimer sendClock;
    QObject::connect(&client, &BleHostClient::dataReceived, [&](const QByteArray &data) { if (collect) { packets.append(data.size()); packetTimes.append(sendClock.elapsed()); } });
    int sendStopped = 0; QObject::connect(&client, &BleHostClient::sendingStopped, [&](const QString &) { ++sendStopped; });
    auto setup = [&](const char *address, int packetSize, int interval) {
        collect = false; client.disconnectDevice(); received.clear(); packets.clear(); packetTimes.clear();
        client.connectDevice(address, 0);
        if (!until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; })) return false;
        if (!client.configure(1, 1, false, packetSize, interval, true)) return false;
        if (!until([&] { return client.state() == BleHostClient::State::Ready && received == "preabc"; })) return false;
        received.clear(); sendClock.start(); collect = true; return true;
    };
    VERIFY(setup("8", 512, 0)); VERIFY(client.supportsSendControl());
    VERIFY(client.sendSettings()["mtu"] == 247 && client.sendSettings()["effectivePacket"] == 244);
    VERIFY(!client.applySendSettings(0, 10, true) && !client.applySendSettings(513, 10, true) && !client.applySendSettings(20, -1, true) && !client.applySendSettings(20, 1001, true));
    VERIFY(!client.applySendSettings(244, 0)); // Persisted/imported values do not authorize faster sending.
    VERIFY(client.sendData(QByteArray(512, 'a')) && !client.applySendSettings(20, 10));
    VERIFY(until([&] { return client.txCount() == 512 && !client.sendBusy(); }));
    VERIFY(received == QByteArray(512, 'a') && packets == QVector<int>({244, 244, 24}));
    VERIFY(client.applySendSettings(1, 100)); VERIFY(!client.sendData("during settings acknowledgement"));
    VERIFY(until([&] { return !client.sendSettings()["pending"].toBool(); }));
    packets.clear(); packetTimes.clear(); VERIFY(client.sendData("abc"));
    VERIFY(until([&] { return client.txCount() == 515 && !client.sendBusy(); }));
    VERIFY(packets == QVector<int>({1, 1, 1}) && packetTimes.size() == 3 && packetTimes[1] - packetTimes[0] >= 99 && packetTimes[2] - packetTimes[1] >= 99);
    VERIFY(setup("9", 244, 0)); VERIFY(client.sendSettings()["effectivePacket"] == 20 && client.sendSettings()["packetSize"] == 244 && client.sendSettings()["mtu"] == 0);
    VERIFY(client.sendData(QByteArray(45, 'u'))); VERIFY(until([&] { return client.txCount() == 45 && !client.sendBusy(); }));
    VERIFY(packets == QVector<int>({20, 20, 5}));
    VERIFY(setup("10", 512, 0)); VERIFY(client.sendSettings()["effectivePacket"] == 512);
    const auto fastFileBefore = fileSuccess; VERIFY(client.startFileSend(source.fileName(), fileError));
    VERIFY(until([&] { return fileSuccess == fastFileBefore + 1 && !client.sendBusy(); }));
    VERIFY(client.txCount() == quint64(fileData.size()) && received == fileData); // Chunk boundaries never drop/duplicate bytes.
    VERIFY(setup("11", 244, 0)); const auto beforeShrinkFile = fileStopped;
    VERIFY(client.startFileSend(source.fileName(), fileError));
    VERIFY(until([&] { return sendStopped == 1 && fileStopped == beforeShrinkFile + 1 && client.txCount() == 244 && !client.sendBusy(); }));
    VERIFY(client.state() == BleHostClient::State::Ready && client.canReceive() && !client.canSend());
    VERIFY(client.sendSettings()["mtu"] == 23 && client.sendSettings()["blocked"].toBool());
    VERIFY(received == fileData.left(244) && !client.sendData("no automatic continuation"));
    VERIFY(client.applySendSettings(244, 50, true)); VERIFY(until([&] { return client.canSend(); }));
    VERIFY(client.sendSettings()["effectivePacket"] == 20 && client.sendData("safe"));
    VERIFY(until([&] { return client.txCount() == 248 && !client.sendBusy(); }));
    VERIFY(setup("12", 244, 0)); VERIFY(client.sendSettings()["effectivePacket"] == 20);
    VERIFY(client.sendData(QByteArray(120, 'g'))); VERIFY(until([&] { return client.txCount() == 120 && !client.sendBusy(); }));
    VERIFY(client.sendSettings()["mtu"] == 247 && client.sendSettings()["effectivePacket"] == 20 && packets == QVector<int>({20, 20, 20, 20, 20, 20}));
    VERIFY(client.applySendSettings(244, 0, true)); VERIFY(until([&] { return !client.sendSettings()["pending"].toBool(); }));
    VERIFY(client.sendSettings()["effectivePacket"] == 244); // Enlargement only after explicit application.
    VERIFY(setup("14", 20, 10)); const auto beforeRejectedSettings = failures;
    VERIFY(client.applySendSettings(10, 20)); VERIFY(until([&] { return failures == beforeRejectedSettings + 1 && !client.sendBusy(); }));
    VERIFY(client.state() == BleHostClient::State::Ready && client.canSend() && client.canReceive() && client.sendSettings()["effectivePacket"] == 20);
    VERIFY(setup("15", 244, 0)); VERIFY(client.sendData(QByteArray(512, 'r')));
    VERIFY(until([&] { return sendStopped == 2 && !client.sendBusy(); }));
    VERIFY(client.txCount() == 0 && !client.canSend() && client.canReceive() && received.isEmpty());
    client.disconnectDevice(); const int beforeInvalid = failures; client.connectDevice("13", 0);
    VERIFY(until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; }));
    VERIFY(client.configure(1, 1, false)); VERIFY(until([&] { return failures == beforeInvalid + 1 && client.state() == BleHostClient::State::Idle; }));
    VERIFY(setup("16", 20, 10)); const auto beforeStaleAck = failures;
    VERIFY(client.applySendSettings(10, 20));
    VERIFY(until([&] { return failures == beforeStaleAck + 1 && sendStopped == 3 && !client.sendBusy(); }));
    VERIFY(!client.canSend() && client.canReceive() && client.sendSettings()["blocked"].toBool() && client.sendSettings()["mtu"] == 247);
    VERIFY(client.applySendSettings(10, 20)); VERIFY(until([&] { return client.canSend(); }));
    VERIFY(client.sendData("recover")); VERIFY(until([&] { return client.txCount() == 7 && !client.sendBusy(); }));
    qputenv("WAVELINK_FAKE_LEGACY", "1"); VERIFY(setup("1", 20, 10)); qunsetenv("WAVELINK_FAKE_LEGACY");
    VERIFY(!client.supportsSendControl() && !client.applySendSettings(10, 20));
    VERIFY(client.sendData("legacy")); VERIFY(until([&] { return client.txCount() == 6 && !client.sendBusy(); }));
    client.disconnectDevice(); VERIFY(until([&] { return client.findChildren<QProcess *>().isEmpty(); }));
    std::cout << "Advanced send control: validated ranges, explicit consent, MTU/unknown limits, exact packets, minimum interval, live idle-only apply, capability shrink/growth, file safety, rejection and legacy fallback passed.\n";
    std::cout << "BLE framing, stale events, backpressure, read errors, cancellation and helper failures passed.\n";
    return 0;
}
