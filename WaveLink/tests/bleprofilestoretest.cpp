#include "ble/bleprofilestore.h"
#include "ble/blechannelpolicy.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <iostream>

#define VERIFY(c) do { if (!(c)) { std::cerr << "Profile failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); QTemporaryDir dir; VERIFY(dir.isValid());
    QSettings settings(dir.filePath("isolated.ini"), QSettings::IniFormat); BleProfileStore store(settings);
    const QJsonObject device{{"address", "1"}, {"addressType", 0}};
    QJsonObject legacy{{"schemaVersion", 1}, {"rxService", "FFE0"}, {"rxUuid", "{0000ffe1-0000-1000-8000-00805f9b34fb}"},
        {"txService", ""}, {"txUuid", ""}, {"withoutResponse", false}, {"stream", false}};
    const auto original = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
    settings.setValue("Ble/schemaVersion", 1); settings.setValue("Ble/Profiles/1_0", original);
    settings.setValue("Ble/protocol", 2); settings.setValue("Other/Keep", "unchanged");
    QString error; auto profile = store.deviceProfile(device, error);
    VERIFY(error.isEmpty() && profile.value("schemaVersion") == 3 && profile.value("packetSize") == 20);
    VERIFY(profile.value("rxService").toString() == "0000ffe0-0000-1000-8000-00805f9b34fb");
    VERIFY(settings.value("Ble/Profiles/1_0").toByteArray() == original && settings.value("Ble/schemaVersion").toInt() == 1);
    VERIFY(store.saveDeviceProfile(device, profile, error));
    VERIFY(settings.value("Ble/schemaVersion").toInt() == 3 && settings.value("Ble/protocol").toInt() == 2 && settings.value("Other/Keep").toString() == "unchanged");
    VERIFY(store.deviceProfile({{"address", "1"}, {"addressType", 1}}, error).isEmpty());
    VERIFY(!store.saveDeviceProfile({{"address", "0"}, {"addressType", 0}}, profile, error));
    VERIFY(!store.saveDeviceProfile({{"address", "1"}, {"addressType", 0.5}}, profile, error));
    VERIFY(BleProfileStore::normalizeUuid("12345678") == "12345678-0000-1000-8000-00805f9b34fb");
    VERIFY(BleProfileStore::normalizeUuid("not-a-uuid").isEmpty());
    VERIFY(BleProfileStore::normalizeUuid("00000000-0000-0000-0000-000000000000").isEmpty());
    QJsonObject normalized;
    for (const auto &bad : {QJsonObject{{"schemaVersion", 4}}, QJsonObject{{"rxUuid", "bad"}}, QJsonObject{{"rxService", ""}},
         QJsonObject{{"withoutResponse", "false"}}, QJsonObject{{"stream", 1}}, QJsonObject{{"packetSize", 0}}, QJsonObject{{"packetSize", 513}},
         QJsonObject{{"packetSize", 20.5}}, QJsonObject{{"packetSize", 1e100}}, QJsonObject{{"packetSize", "244"}}, QJsonObject{{"intervalMs", -1}}, QJsonObject{{"intervalMs", 1001}}}) {
        auto input = profile; for (auto i = bad.begin(); i != bad.end(); ++i) input.insert(i.key(), i.value());
        VERIFY(!BleProfileStore::normalize(input, normalized, error) && normalized.isEmpty() && !error.isEmpty());
    }
    auto advanced = profile; advanced["packetSize"] = 512; advanced["intervalMs"] = 0; advanced["advancedConfirmed"] = true; advanced["mtu"] = 517;
    VERIFY(BleProfileStore::normalize(advanced, normalized, error) && normalized["packetSize"] == 512 && normalized["intervalMs"] == 0);
    VERIFY(!normalized.contains("advancedConfirmed") && !normalized.contains("mtu"));
    auto oldAdvanced = advanced; oldAdvanced["schemaVersion"] = 2;
    VERIFY(!BleProfileStore::normalize(oldAdvanced, normalized, error));
    VERIFY(store.saveDeviceProfile(device, advanced, error) && store.deviceProfile(device, error)["packetSize"] == 512);
    QString advancedName; QJsonObject advancedImported;
    const auto advancedBytes = BleProfileStore::exportTemplate("Advanced", advanced, error);
    VERIFY(BleProfileStore::parseTemplate(advancedBytes, advancedName, advancedImported, error));
    VERIFY(advancedName == "Advanced" && advancedImported["packetSize"] == 512 && advancedImported["intervalMs"] == 0 && !advancedImported.contains("advancedConfirmed") && !advancedImported.contains("mtu"));
    auto slow = advanced; slow["packetSize"] = 1; slow["intervalMs"] = 1000;
    VERIFY(BleProfileStore::normalize(slow, normalized, error));
    auto transmitOnly = BleProfileStore::nus(); transmitOnly["rxService"] = ""; transmitOnly["rxUuid"] = "";
    VERIFY(BleProfileStore::normalize(transmitOnly, normalized, error));
    auto privateProfile = profile; privateProfile["address"] = "private address"; privateProfile["path"] = "C:/secret"; privateProfile["handle"] = 98;
    const auto bytes = BleProfileStore::exportTemplate("My UART", privateProfile, error);
    VERIFY(!bytes.isEmpty() && !bytes.contains("secret") && !bytes.contains("private address") && !bytes.contains("handle"));
    QString name; QJsonObject imported; VERIFY(BleProfileStore::parseTemplate(bytes, name, imported, error));
    VERIFY(name == "My UART" && imported == profile);
    VERIFY(!BleProfileStore::parseTemplate(QByteArray(65537, 'x'), name, imported, error));
    VERIFY(!BleProfileStore::parseTemplate("{}", name, imported, error));
    auto doc = QJsonDocument::fromJson(bytes).object(); doc["schemaVersion"] = 2;
    VERIFY(!BleProfileStore::parseTemplate(QJsonDocument(doc).toJson(), name, imported, error));
    VERIFY(store.saveTemplate("UART", profile, false, error));
    VERIFY(!store.saveTemplate("uart", transmitOnly, false, error));
    VERIFY(store.saveTemplate("uart", transmitOnly, true, error));
    VERIFY(store.templates(error).size() == 1 && store.templates(error)[0].toObject()["profile"].toObject()["rxUuid"].toString().isEmpty());
    VERIFY(!store.saveTemplate(" name ", profile, false, error));
    VERIFY(!store.saveTemplate("bad\nname", profile, false, error));
    for (int i = 1; i < 64; ++i) VERIFY(store.saveTemplate(QString("Template %1").arg(i), profile, false, error));
    VERIFY(!store.saveTemplate("Overflow", profile, false, error) && store.templates(error).size() == 64);
    // Discovery policy: DFU is never recommended; vendor UART is explicitly
    // a candidate, and successful local evidence is required for shortcuts.
    auto characteristic = [](const QString &service, const QString &id, int handle, int p) {
        return QJsonObject{{"service", BleProfileStore::normalizeUuid(service)}, {"uuid", BleProfileStore::normalizeUuid(id)},
                           {"handle", handle}, {"serviceHandle", 1}, {"properties", p}};
    };
    const QJsonArray gatt{characteristic("FE59", "8ec90001-f315-4f60-9fb8-838830daea50", 2, 24),
        characteristic("FE59", "8ec90002-f315-4f60-9fb8-838830daea50", 5, 4),
        characteristic("FFF0", "FFF1", 11, 18), characteristic("FFF0", "FFF2", 14, 12),
        characteristic("F000", "F001", 17, 14)};
    const auto choices = BleChannelPolicy::candidates(gatt);
    VERIFY(choices.size() == 1 && choices[0].rx == 11 && choices[0].tx == 14 && choices[0].name.contains(QStringLiteral("候选")));
    QJsonObject uart{{"schemaVersion", 3}, {"rxService", "FFF0"}, {"rxUuid", "FFF1"}, {"txService", "FFF0"},
                     {"txUuid", "FFF2"}, {"stream", true}, {"withoutResponse", false}};
    int rx = -1, tx = -1;
    VERIFY(!BleChannelPolicy::quickConnect(gatt, uart, rx, tx) && rx == 0 && tx == 0);
    VERIFY(store.saveDeviceProfile(device, uart, error, gatt));
    auto validated = store.deviceProfile(device, error);
    VERIFY(BleChannelPolicy::quickConnect(gatt, validated, rx, tx) && rx == 11 && tx == 14);
    QJsonArray reordered;
    for (int i = gatt.size() - 1; i >= 0; --i) { auto c = gatt[i].toObject(); c["handle"] = c["handle"].toInt() + 100; reordered.append(c); }
    VERIFY(BleChannelPolicy::quickConnect(reordered, validated, rx, tx) && rx == 111 && tx == 114);
    auto changed = gatt; auto c = changed[3].toObject(); c["properties"] = 4; changed[3] = c;
    VERIFY(!BleChannelPolicy::quickConnect(changed, validated, rx, tx));
    auto duplicate = gatt; duplicate.append(gatt[2]);
    VERIFY(BleChannelPolicy::candidates(duplicate).isEmpty());
    auto ambiguous = validated; ambiguous["_validatedGatt"] = BleChannelPolicy::fingerprint(duplicate);
    VERIFY(!BleChannelPolicy::quickConnect(duplicate, ambiguous, rx, tx));
    auto accelerated = validated; accelerated["packetSize"] = 244; accelerated["intervalMs"] = 0;
    VERIFY(!BleChannelPolicy::quickConnect(gatt, accelerated, rx, tx));
    auto unconfirmed = validated; unconfirmed["stream"] = false;
    VERIFY(!BleChannelPolicy::quickConnect(gatt, unconfirmed, rx, tx));
    auto rxOnly = validated; rxOnly["txService"] = ""; rxOnly["txUuid"] = ""; rxOnly["stream"] = false;
    VERIFY(BleChannelPolicy::quickConnect(gatt, rxOnly, rx, tx) && rx == 11 && tx == 0);
    auto dfu = validated; dfu["rxService"] = "FE59"; dfu["rxUuid"] = "8ec90001-f315-4f60-9fb8-838830daea50";
    VERIFY(!BleChannelPolicy::quickConnect(gatt, dfu, rx, tx));
    const auto portable = BleProfileStore::exportTemplate("UART", validated, error);
    VERIFY(!portable.contains("validatedGatt") && !portable.contains("ValidatedGatt"));
    VERIFY(store.saveDeviceProfile(device, uart, error)); // Editing next settings invalidates local evidence.
    VERIFY(!store.deviceProfile(device, error).contains("_validatedGatt"));
    const QByteArray corrupt("not json"); settings.setValue("Ble/Templates", corrupt);
    VERIFY(store.templates(error).isEmpty() && !error.isEmpty());
    VERIFY(!store.saveTemplate("Dont overwrite", profile, false, error) && settings.value("Ble/Templates").toByteArray() == corrupt);
    settings.setValue("Ble/Profiles/1_0", corrupt);
    VERIFY(store.deviceProfile(device, error).isEmpty() && !error.isEmpty() && settings.value("Ble/Profiles/1_0").toByteArray() == corrupt);
    std::cout << "BLE profiles: non-mutating legacy migration, validated UUID/directions, identity-free templates, duplicates, limits and corrupt-store preservation passed.\n";
    return 0;
}
