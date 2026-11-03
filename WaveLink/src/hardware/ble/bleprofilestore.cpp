#include "bleprofilestore.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <QStringList>
#include "blesendpolicy.h"
#include "blechannelpolicy.h"
#include <cmath>

QString BleProfileStore::normalizeUuid(const QString &input)
{
    QString text = input.trimmed().toLower();
    static const QRegularExpression shortUuid("^[0-9a-f]{4}([0-9a-f]{4})?$");
    static const QRegularExpression fullUuid("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    if (text.startsWith('{') && text.endsWith('}')) text = text.mid(1, text.size() - 2);
    if (shortUuid.match(text).hasMatch()) text = text.rightJustified(8, '0') + "-0000-1000-8000-00805f9b34fb";
    if (!fullUuid.match(text).hasMatch() || QUuid(text).isNull()) return {};
    return text;
}
bool BleProfileStore::normalize(const QJsonObject &input, QJsonObject &output, QString &error)
{
    error.clear(); output = {};
    const auto version = input.value("schemaVersion");
    if (!version.isUndefined() && (!version.isDouble() || (version.toDouble() != 1 && version.toDouble() != 2 && version.toDouble() != 3))) {
        error = QStringLiteral("不支持此 BLE 配置版本。"); return false;
    }
    QJsonObject result;
    bool any = false;
    for (const auto direction : {QStringLiteral("rx"), QStringLiteral("tx")}) {
        const QString serviceKey = direction + "Service", uuidKey = direction + "Uuid";
        for (const auto &key : {serviceKey, uuidKey}) {
            if (input.contains(key) && !input.value(key).isString()) { error = QStringLiteral("UUID 必须为文本。"); return false; }
        }
        const QString service = input.value(serviceKey).toString().trimmed(), characteristic = input.value(uuidKey).toString().trimmed();
        if (service.isEmpty() != characteristic.isEmpty()) { error = QStringLiteral("每个方向都需要同时填写服务和特征 UUID，或同时留空。"); return false; }
        if (!service.isEmpty()) {
            const QString s = normalizeUuid(service), c = normalizeUuid(characteristic);
            if (s.isEmpty() || c.isEmpty()) { error = QStringLiteral("BLE 服务或特征 UUID 无效。"); return false; }
            result.insert(serviceKey, s); result.insert(uuidKey, c); any = true;
        } else { result.insert(serviceKey, ""); result.insert(uuidKey, ""); }
    }
    if (!any) { error = QStringLiteral("至少配置一个接收或发送方向。"); return false; }
    for (const auto key : {"withoutResponse", "stream"}) {
        if (input.contains(key) && !input.value(key).isBool()) { error = QStringLiteral("写入模式和透传确认值无效。"); return false; }
        result.insert(key, input.value(key).toBool());
    }
    for (const auto key : {"packetSize", "intervalMs"}) {
        const bool packet = QString(key) == "packetSize";
        const int fallback = packet ? 20 : 10;
        const auto v = input.value(key);
        const double n = v.isUndefined() ? fallback : v.toDouble(-1);
        if ((!v.isUndefined() && !v.isDouble()) || !std::isfinite(n) || n < (packet ? 1 : 0) || n > (packet ? 512 : 1000) || n != int(n) ||
            (version.toInt() < 3 && n != fallback)) {
            error = QStringLiteral("发送参数无效：分包须为 1–512 字节，间隔须为 0–1000 ms；旧配置仅支持 20 字节 / 10 ms。"); return false;
        }
        result.insert(key, int(n));
    }
    result.insert("schemaVersion", 3); output = result; return true;
}
QJsonObject BleProfileStore::nus()
{
    return {{"schemaVersion", 2}, {"rxService", "6e400001-b5a3-f393-e0a9-e50e24dcca9e"},
            {"rxUuid", "6e400003-b5a3-f393-e0a9-e50e24dcca9e"}, {"txService", "6e400001-b5a3-f393-e0a9-e50e24dcca9e"},
            {"txUuid", "6e400002-b5a3-f393-e0a9-e50e24dcca9e"}, {"withoutResponse", false}, {"stream", false}, {"packetSize", 20}, {"intervalMs", 10}};
}
QString BleProfileStore::deviceKey(const QJsonObject &device)
{
    bool ok = false; const auto address = device.value("address").toString().toULongLong(&ok);
    const double type = device.value("addressType").toDouble(-1);
    if (!ok || !address || address > 0xffffffffffffULL || (type != 0 && type != 1)) return {};
    return "Ble/Profiles/" + QString::number(address) + "_" + QString::number(int(type));
}
QJsonObject BleProfileStore::deviceProfile(const QJsonObject &device, QString &error) const
{
    error.clear(); const auto key = deviceKey(device); if (key.isEmpty()) return {};
    const auto bytes = m_settings.value(key).toByteArray(); if (bytes.isEmpty()) return {};
    if (bytes.size() > 65536) { error = QStringLiteral("保存的 BLE 配置过大，已保留原设置，请手动核对。"); return {}; }
    QJsonParseError parseError; const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    QJsonObject profile;
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) error = QStringLiteral("保存的 BLE 配置格式无效，原设置未被覆盖。");
    else if (normalize(doc.object(), profile, error)) {
        // Local successful-session evidence is not part of portable templates.
        const auto signature = m_settings.value(key + "/ValidatedGatt").toString();
        if (!signature.isEmpty()) profile.insert("_validatedGatt", signature);
    }
    return profile;
}
bool BleProfileStore::saveDeviceProfile(const QJsonObject &device, const QJsonObject &profile, QString &error,
                                      const QJsonArray &validatedEntries)
{
    const auto key = deviceKey(device); QJsonObject normalized;
    if (key.isEmpty()) { error = QStringLiteral("设备身份无效，不能保存配置。"); return false; }
    if (!normalize(profile, normalized, error)) return false;
    // Invalidate old evidence before replacing the profile (also crash-safe
    // when an unverified next-connection edit is being saved).
    m_settings.remove(key + "/ValidatedGatt");
    m_settings.setValue(key, QJsonDocument(normalized).toJson(QJsonDocument::Compact));
    const auto signature = BleChannelPolicy::fingerprint(validatedEntries);
    if (!signature.isEmpty()) m_settings.setValue(key + "/ValidatedGatt", signature);
    m_settings.setValue("Ble/schemaVersion", 3); m_settings.sync();
    if (m_settings.status() != QSettings::NoError) { error = QStringLiteral("BLE 配置保存失败，请检查用户设置的访问权限。"); return false; }
    return true;
}
bool BleProfileStore::validName(const QString &name)
{
    if (name.isEmpty() || name.size() > 64 || name != name.trimmed()) return false;
    for (auto c : name) if (!c.isPrint()) return false;
    return true;
}
QJsonArray BleProfileStore::templates(QString &error) const
{
    error.clear(); const auto bytes = m_settings.value("Ble/Templates").toByteArray(); if (bytes.isEmpty()) return {};
    if (bytes.size() > 65536) { error = QStringLiteral("模板库超过 64 KiB 上限，原模板库未被覆盖。"); return {}; }
    QJsonParseError pe; const auto doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > 64) {
        error = QStringLiteral("模板库格式无效或超过上限，原模板库未被覆盖。"); return {};
    }
    QJsonArray result; QStringList names;
    for (const auto &v : doc.array()) {
        const auto obj = v.toObject(); const QString name = obj.value("name").toString(); QJsonObject profile;
        if (!validName(name) || names.contains(name.toCaseFolded()) || !normalize(obj.value("profile").toObject(), profile, error)) {
            if (error.isEmpty()) error = QStringLiteral("模板名称无效或重复，原模板库未被覆盖。"); return {};
        }
        names.append(name.toCaseFolded()); result.append(QJsonObject{{"name", name}, {"profile", profile}});
    }
    return result;
}
bool BleProfileStore::saveTemplate(const QString &name, const QJsonObject &profile, bool replace, QString &error)
{
    if (!validName(name)) { error = QStringLiteral("模板名称须为 1–64 个可显示字符。"); return false; }
    QJsonObject normalized; if (!normalize(profile, normalized, error)) return false;
    auto list = templates(error); if (!error.isEmpty()) return false;
    int match = -1;
    for (int i = 0; i < list.size(); ++i) if (list[i].toObject().value("name").toString().compare(name, Qt::CaseInsensitive) == 0) match = i;
    if (match >= 0 && !replace) { error = QStringLiteral("已存在同名模板，请确认后再覆盖。"); return false; }
    const QJsonObject item{{"name", name}, {"profile", normalized}};
    if (match >= 0) list[match] = item;
    else { if (list.size() >= 64) { error = QStringLiteral("最多保存 64 个自定义模板。"); return false; } list.append(item); }
    const auto bytes = QJsonDocument(list).toJson(QJsonDocument::Compact);
    if (bytes.size() > 65536) { error = QStringLiteral("模板库超过 64 KiB 上限，未保存新模板。"); return false; }
    m_settings.setValue("Ble/Templates", bytes); m_settings.sync();
    if (m_settings.status() != QSettings::NoError) { error = QStringLiteral("模板保存失败。"); return false; }
    return true;
}
bool BleProfileStore::parseTemplate(const QByteArray &bytes, QString &name, QJsonObject &profile, QString &error)
{
    error.clear(); name.clear(); profile = {};
    if (bytes.size() > 65536) { error = QStringLiteral("模板文件超过 64 KiB 上限。"); return false; }
    QJsonParseError pe; const auto doc = QJsonDocument::fromJson(bytes, &pe); const auto obj = doc.object();
    if (pe.error != QJsonParseError::NoError || !doc.isObject() || obj.value("kind") != "WaveLink.BleTemplate" || obj.value("schemaVersion").toDouble() != 1) {
        error = QStringLiteral("不是受支持的 WaveLink BLE 模板文件。"); return false;
    }
    name = obj.value("name").toString();
    if (!validName(name)) { error = QStringLiteral("模板名称无效。"); return false; }
    return normalize(obj.value("profile").toObject(), profile, error);
}
QByteArray BleProfileStore::exportTemplate(const QString &name, const QJsonObject &profile, QString &error)
{
    QJsonObject normalized;
    if (!validName(name)) { error = QStringLiteral("模板名称无效。"); return {}; }
    if (!normalize(profile, normalized, error)) return {};
    return QJsonDocument(QJsonObject{{"kind", "WaveLink.BleTemplate"}, {"schemaVersion", 1}, {"name", name}, {"profile", normalized}}).toJson(QJsonDocument::Indented);
}
