#pragma once
#include "bleprofilestore.h"
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
inline bool validatedChannels(const QJsonArray &entries, const QJsonObject &saved, int &rx, int &tx)
{
    rx = tx = 0;
    QJsonObject p; QString error;
    if (!BleProfileStore::normalize(saved, p, error)) return false;
    const auto signature = fingerprint(entries);
    if (signature.isEmpty() || signature != saved.value("_validatedGatt").toString() ||
        firmwareService(p.value("rxService").toString()) || firmwareService(p.value("txService").toString())) return false;
    const bool hasRx = !p.value("rxUuid").toString().isEmpty(), hasTx = !p.value("txUuid").toString().isEmpty();
    const int r = resolve(entries, p.value("rxService").toString(), p.value("rxUuid").toString(), 16 | 32);
    const int t = resolve(entries, p.value("txService").toString(), p.value("txUuid").toString(), p.value("withoutResponse").toBool() ? 4 : 8);
    if ((hasRx && !r) || (hasTx && (!t || !p.value("stream").toBool()))) return false;
    rx = r; tx = t; return rx || tx;
}
struct Candidate { QString name; int rx = 0, tx = 0; };
inline Candidate nordicUart(const QJsonArray &entries)
{
    const auto profile = BleProfileStore::nus();
    const auto service = profile.value("rxService").toString();
    const int rx = resolve(entries, service, profile.value("rxUuid").toString(), 16);
    const int tx = resolve(entries, service, profile.value("txUuid").toString(), 4 | 8);
    if (!rx || !tx || rx == tx) return {};
    int serviceHandle = 0;
    for (const auto &v : entries) {
        const auto c = v.toObject();
        if (BleProfileStore::normalizeUuid(c.value("service").toString()) != service) continue;
        const int handle = c.value("serviceHandle").toInt();
        if (handle <= 0 || (serviceHandle && handle != serviceHandle)) return {};
        serviceHandle = handle;
    }
    return {QStringLiteral("Nordic UART · 已识别收发通道"), rx, tx};
}
inline QJsonObject connectionProfile(const QJsonArray &entries, const QJsonObject &saved, int &rx, int &tx)
{
    if (validatedChannels(entries, saved, rx, tx)) {
        QJsonObject profile;
        QString error;
        BleProfileStore::normalize(saved, profile, error);
        return profile;
    }
    const auto known = nordicUart(entries);
    if (!known.tx) return {};
    const auto defaults = BleProfileStore::nus();
    if (!saved.isEmpty()) {
        for (const auto key : {"rxService", "rxUuid", "txService", "txUuid"})
            if (saved.value(key) != defaults.value(key)) return {};
    }
    auto profile = saved.isEmpty() ? defaults : saved;
    profile.remove("_validatedGatt");
    profile["schemaVersion"] = 3;
    profile["stream"] = true;
    rx = known.rx;
    tx = known.tx;
    const bool preferWithoutResponse = profile.value("withoutResponse").toBool();
    if (resolve(entries, profile.value("txService").toString(), profile.value("txUuid").toString(), preferWithoutResponse ? 4 : 8) != tx)
        profile["withoutResponse"] = !preferWithoutResponse;
    return profile;
}
inline QList<Candidate> candidates(const QJsonArray &entries)
{
    QList<Candidate> result; QStringList services;
    const auto nus = nordicUart(entries);
    if (nus.tx) result.append(nus);
    const auto nusService = BleProfileStore::nus().value("rxService").toString();
    for (const auto &v : entries) {
        const auto s = BleProfileStore::normalizeUuid(v.toObject().value("service").toString());
        if (!s.isEmpty() && !services.contains(s)) services << s;
    }
    for (const auto &s : services) {
        // Standard SIG services (battery, device info, HID, etc.) are not UART.
        if ((nus.tx && s == nusService) || firmwareService(s) ||
            (s.startsWith("000018") && s.endsWith("-0000-1000-8000-00805f9b34fb"))) continue;
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
        const QString shortService = s.endsWith("-0000-1000-8000-00805f9b34fb") ?
            s.mid(s.startsWith("0000") ? 4 : 0, s.startsWith("0000") ? 4 : 8).toUpper() : s;
        result << Candidate{QStringLiteral("服务 %1（属性匹配候选，待确认用途）").arg(shortService), rx, tx};
    }
    return result;
}
}
