// Deterministic process-boundary tests; no Bluetooth hardware is accessed.
#include "ble/bleipc.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QDateTime>
#include <iostream>
#include <thread>
#include <chrono>
#include <io.h>
#include <fcntl.h>
#include <QFile>
#include "ble/blesendpolicy.h"

static void send(QJsonObject object)
{
    if (object.value("event").toString() == "data" && !object.contains("timestamp100ns"))
        object.insert("timestamp100ns", QString::number(116444736000000000LL + QDateTime::currentMSecsSinceEpoch() * 10000LL));
    const QByteArray frame = BleIpc::encode(object);
    // Deliberately fragment every IPC frame, including the handshake.
    std::cout.write(frame.constData(), 2); std::cout.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::cout.write(frame.constData() + 2, frame.size() - 2); std::cout.flush();
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
    const bool reader = app.arguments().contains("--file-reader");
    send({{"event", "hello"}, {"version", 1}, {"role", reader ? "file-reader" : "ble"}, {"fileReaderVersion", 1}, {"sendControlVersion", qEnvironmentVariableIsSet("WAVELINK_FAKE_LEGACY") ? 0 : 1}});
    QFile file; QString source; qint64 total = 0;
    QString address; int mtu = 0, packet = 20, effective = 20, interval = 10;
    quint64 revision = 1; bool changed = false, blocked = false;
    auto snapshot = [&] { return QJsonObject{{"revision", QString::number(revision)}, {"mtu", mtu}, {"packetSize", packet}, {"effectivePacket", effective}, {"intervalMs", interval}, {"blocked", blocked}}; };
    for (;;) {
        char prefix[4]; if (!std::cin.read(prefix, 4)) return 0;
        const quint32 length = qFromLittleEndian<quint32>(reinterpret_cast<uchar *>(prefix));
        if (!length || length > BleIpc::MaxPayload) return 1;
        QByteArray bytes(int(length), '\0'); if (!std::cin.read(bytes.data(), length)) return 1;
        auto request = QJsonDocument::fromJson(bytes).object();
        const auto op = request.value("op").toString();
        QJsonObject event{{"session", request.value("session")}, {"request", request.value("request")}};
        if (reader) {
            event.insert("transfer", request.value("transfer"));
            if (op == "open") {
                source = request.value("path").toString();
                if (source == "fake:hang-open") continue;
                if (source == "fake:slow-open") { std::this_thread::sleep_for(std::chrono::seconds(30)); continue; }
                if (source == "fake:exit") return 77;
                if (source == "fake:role") { send({{"event", "hello"}, {"version", 1}, {"role", "ble"}}); continue; }
                if (source.startsWith("fake:")) total = source == "fake:large" ? 4294967313LL : 21;
                else {
                    file.setFileName(source);
                    if (!file.open(QIODevice::ReadOnly)) {
                        event.insert("event", "error"); event.insert("message", file.errorString()); send(event); continue;
                    }
                    total = file.size();
                }
                event.insert("event", "opened"); event.insert("size", QString::number(total)); send(event);
            } else if (op == "read") {
                if (source == "fake:hang-read") continue;
                if (source == "fake:read-error") { event.insert("event", "error"); event.insert("message", "Injected disk error"); send(event); continue; }
                const qint64 offset = request.value("offset").toString().toLongLong();
                const int count = request.value("length").toInt();
                const auto data = source.startsWith("fake:") ? QByteArray(count, 'y') : file.read(count);
                event.insert("event", "chunk"); event.insert("offset", QString::number(offset));
                event.insert("data", QString::fromLatin1(data.toBase64())); event.insert("eof", offset + data.size() == total);
                if (source == "fake:stale") { auto old = event; old.insert("transfer", "obsolete"); send(old); old = event; old.insert("request", "obsolete"); send(old); }
                if (source == "fake:offset") event.insert("offset", QString::number(offset + 1));
                if (source == "fake:eof") event.insert("eof", !event.value("eof").toBool());
                if (source == "fake:encoding") event.insert("data", "!");
                if (source == "fake:early") event.insert("data", "");
                send(event);
            }
            continue;
        }
        if (op == "scan") {
            event.insert("event", "device"); event.insert("name", "Fake BLE"); event.insert("address", "1");
            event.insert("addressType", 0); event.insert("rssi", -40); event.insert("connectable", true); send(event);
        } else if (op == "connect") {
            address = request.value("address").toString();
            mtu = (address == "8" || address == "11" || address == "15") ? 247 : address == "10" ? 517 : address == "12" ? 23 : 0;
            if (address == "2") { event.insert("event", "error"); event.insert("message", "Test access denied"); event.insert("fatal", true); send(event); continue; }
            if (address == "3") continue; // In-flight OS operation until cancellation.
            if (address == "4") return 99;
            if (address == "5") { quint32 bad = BleIpc::MaxPayload + 1; std::cout.write(reinterpret_cast<char *>(&bad), 4); std::cout.flush(); continue; }
            if (address == "6") { send({{"event", "hello"}, {"version", 2}}); continue; }
            event.insert("event", "discovering"); send(event);
            event.insert("event", "services"); event.insert("characteristics", QJsonArray{
                QJsonObject{{"service", "0000ffe0-0000-1000-8000-00805f9b34fb"}, {"uuid", "0000ffe1-0000-1000-8000-00805f9b34fb"}, {"handle", 1}, {"serviceHandle", 2}, {"properties", 26}}});
            if (address == "7") { auto entries = event.value("characteristics").toArray(); auto duplicate = entries[0].toObject(); duplicate["handle"] = 3; entries.append(duplicate); event["characteristics"] = entries; }
            send(event);
        } else if (op == "configure" || op == "sendSettings") {
            if (address == "14" && op == "sendSettings") {
                event["event"] = "error"; event["fatal"] = false; event["message"] = "Injected settings rejection"; send(event); continue;
            }
            const int p = request.value("packetSize").toInt(20), i = request.value("intervalMs").toInt(10);
            if (!BleSendPolicy::valid(p, i) || (BleSendPolicy::needsConfirmation(p, i) && !request.value("advancedConfirmed").toBool())) return 78;
            packet = p; interval = i; effective = BleSendPolicy::effective(p, mtu); blocked = false; ++revision;
            if (op == "sendSettings") {
                event["event"] = "sendSettingsApplied"; event["sendSettings"] = snapshot();
                if (address == "16" && !changed) {
                    changed = true; mtu = 247; ++revision;
                    auto newer = event; newer["event"] = "capabilities"; newer["sendSettings"] = snapshot(); send(newer);
                }
                send(event); continue;
            }
            if (request.value("rx").toInt()) send({{"event", "data"}, {"session", request.value("session")}, {"data", "cHJl"}});
            event.insert("event", "ready"); event.insert("canSend", request.value("tx").toInt() != 0); event.insert("canReceive", request.value("rx").toInt() != 0); event["sendSettings"] = snapshot();
            if (address == "13") { auto bad = snapshot(); bad["effectivePacket"] = 21; bad["packetSize"] = 21; event["sendSettings"] = bad; }
            send(event);
            send({{"event", "data"}, {"session", "obsolete"}, {"data", "eA=="}});
            if (request.value("rx").toInt()) send({{"event", "data"}, {"session", request.value("session")}, {"data", "YWJj"}});
        } else if (op == "read") {
            if (request.value("handle").toInt() == 9) {
                event.insert("event", "error"); event.insert("fatal", false); event.insert("message", "Read denied");
            } else { event.insert("event", "read"); event.insert("data", "aGk="); event.insert("handle", request.value("handle")); }
            send(event);
        } else if (op == "write") {
            const int bytes = QByteArray::fromBase64(request.value("data").toString().toLatin1()).size();
            if (bytes < 1 || bytes > effective || bytes > BleSendPolicy::limit(mtu) || blocked) return 79;
            if (!changed && (address == "11" || address == "12" || address == "15")) {
                changed = true; mtu = address == "12" ? 247 : 23; ++revision;
                blocked = effective > BleSendPolicy::limit(mtu);
                auto caps = event; caps["event"] = address == "15" ? "writeRejected" : "capabilities"; caps["sendSettings"] = snapshot(); send(caps);
                if (address == "15") continue; // Rejected before submission; must not count as Tx.
                auto stale = caps; auto old = snapshot(); old["revision"] = "1"; old["mtu"] = 0; stale["sendSettings"] = old; send(stale);
            }
            send({{"event", "data"}, {"session", request.value("session")}, {"data", request.value("data")}});
            event.insert("event", "written"); event.insert("bytes", bytes);
            auto stale = event; stale.insert("request", "old"); send(stale); send(event);
        }
    }
}
