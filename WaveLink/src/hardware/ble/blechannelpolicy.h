#pragma once
#include "bleprofilestore.h"
#include "blesendpolicy.h"
#include <QCryptographicHash>
#include <QStringList>

// Pure discovery policy. Recommendations never probe/write to the peripheral.
namespace BleChannelPolicy {
inline bool firmwareService(const QString &service)
{
    const auto s = BleProfileStore::normalizeUuid(service);
    return s == "0000fe59-0000-1000-8000-00805f9b34fb" ||
           s == "00001530-1212-efde-1523-785feabcd123" ||
           s == "8ec90000-f315-4f60-9fb8-838830daea50";
}
inline QString fingerprint(const QJsonArray &entries)
{
    QStringList rows;
    for (const auto &v : entries) {
        const auto c = v.toObject();
        const auto s = BleProfileStore::normalizeUuid(c.value("service").toString());
        const auto u = BleProfileStore::normalizeUuid(c.value("uuid").toString());
        if (s.isEmpty() || u.isEmpty() || c.value("handle").toInt() <= 0) return {};
        rows << s + "/" + u + "/" + QString::number(c.value("properties").toInt());
    }
    if (rows.isEmpty()) return {};
    rows.sort(); // Discovery order and OS-assigned handles may change on reconnect.
    return QString::fromLatin1(QCryptographicHash::hash(rows.join('\n').toUtf8(), QCryptographicHash::Sha256).toHex());
}
inline int resolve(const QJsonArray &entries, const QString &service, const QString &characteristic, int required)
{
    if (service.isEmpty() || characteristic.isEmpty()) return 0;
    const auto s = BleProfileStore::normalizeUuid(service), u = BleProfileStore::normalizeUuid(characteristic);
    int result = 0, matches = 0;
    if (s.isEmpty() || u.isEmpty()) return 0;
    for (const auto &v : entries) {
        const auto c = v.toObject();
        if (BleProfileStore::normalizeUuid(c.value("service").toString()) == s &&
            BleProfileStore::normalizeUuid(c.value("uuid").toString()) == u) {
            ++matches;
            if ((c.value("properties").toInt() & required) && c.value("handle").toInt() > 0)
                result = c.value("handle").toInt();
        }
    }
    return matches == 1 ? result : 0;
}
inline bool quickConnect(const QJsonArray &entries, const QJsonObject &saved, int &rx, int &tx)
{
    rx = tx = 0;
    QJsonObject p; QString error;
    if (!BleProfileStore::normalize(saved, p, error)) return false;
    const auto signature = fingerprint(entries);
    if (signature.isEmpty() || signature != saved.value("_validatedGatt").toString() ||
        firmwareService(p.value("rxService").toString()) || firmwareService(p.value("txService").toString()) ||
        BleSendPolicy::needsConfirmation(p.value("packetSize").toInt(), p.value("intervalMs").toInt())) return false;
    const bool hasRx = !p.value("rxUuid").toString().isEmpty(), hasTx = !p.value("txUuid").toString().isEmpty();
    const int r = resolve(entries, p.value("rxService").toString(), p.value("rxUuid").toString(), 16 | 32);
    const int t = resolve(entries, p.value("txService").toString(), p.value("txUuid").toString(), p.value("withoutResponse").toBool() ? 4 : 8);
    if ((hasRx && !r) || (hasTx && (!t || !p.value("stream").toBool()))) return false;
    rx = r; tx = t; return rx || tx;
}
struct Candidate { QString name; int rx = 0, tx = 0; };
inline QList<Candidate> candidates(const QJsonArray &entries)
{
    QList<Candidate> result; QStringList services;
    for (const auto &v : entries) {
        const auto s = BleProfileStore::normalizeUuid(v.toObject().value("service").toString());
        if (!s.isEmpty() && !services.contains(s)) services << s;
    }
    for (const auto &s : services) {
        // Standard SIG services (battery, device info, HID, etc.) are not UART.
        if (firmwareService(s) || (s.startsWith("000018") && s.endsWith("-0000-1000-8000-00805f9b34fb"))) continue;
        QList<QJsonObject> receive, writeOnly, write;
        for (const auto &v : entries) {
            const auto c = v.toObject(); const int p = c.value("properties").toInt();
            if (BleProfileStore::normalizeUuid(c.value("service").toString()) != s || c.value("handle").toInt() <= 0) continue;
            if (p & (16 | 32)) receive << c;
            if (p & (4 | 8)) { write << c; if (!(p & (16 | 32))) writeOnly << c; }
        }
        if (receive.size() != 1) continue;
        const auto &writers = writeOnly.isEmpty() ? write : writeOnly;
        const int rx = resolve(entries, s, receive.first().value("uuid").toString(), 16 | 32);
        int tx = 0;
        if (writers.size() == 1) tx = resolve(entries, s, writers.first().value("uuid").toString(), 4 | 8);
        if (!rx) continue;
        const bool nus = s == "6e400001-b5a3-f393-e0a9-e50e24dcca9e" &&
            BleProfileStore::normalizeUuid(receive.first().value("uuid").toString()) == "6e400003-b5a3-f393-e0a9-e50e24dcca9e" &&
            tx && BleProfileStore::normalizeUuid(writers.first().value("uuid").toString()) == "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
        const QString shortService = s.endsWith("-0000-1000-8000-00805f9b34fb") ?
            s.mid(s.startsWith("0000") ? 4 : 0, s.startsWith("0000") ? 4 : 8).toUpper() : s;
        result << Candidate{nus ? QStringLiteral("Nordic UART 结构匹配（仍需确认用途）") :
            QStringLiteral("服务 %1（属性匹配候选，待确认用途）").arg(shortService), rx, tx};
    }
    return result;
}
}
