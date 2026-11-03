#include "ProtocolProfileStore.h"
#include "ProtocolParserFactory.h"
#include "ProtocolParser.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QLockFile>
#include <QUuid>
#include <cmath>
#include <iostream>
#include <memory>

using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Protocol failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)

static QJsonObject binaryExample() {
    return QJsonDocument::fromJson(R"JSON({
      "id":"b90c4e0d-f4c2-4d34-937a-54a29edaa728","revision":1,
      "name":"温度与电压","description":"定长小端采集帧","type":"binary","byteOrder":"little",
      "frame":{"mode":"fixed","headerHex":"AA55","sizeBytes":11,"tailHex":"0D0A",
               "payloadOffset":2,"payloadBytes":6,"maxFrameBytes":4096},
      "checksum":{"algorithm":"sum8","range":{"offset":2,"length":6},"field":{"offset":8,"type":"uint8"}},
      "channels":[{"id":"temperature","name":"温度","unit":"°C","offset":0,"type":"int16","scale":0.01,"bias":0},
                  {"id":"voltage","name":"电压","unit":"V","offset":2,"type":"float32","scale":1,"bias":0}]
    })JSON").object();
}
static QJsonObject textExample() {
    return QJsonDocument::fromJson(R"JSON({
      "id":"f75b780c-b3a8-42a8-8f2a-8adeb18a3991","revision":1,"name":"文本温度与电压","type":"text",
      "frame":{"encoding":"utf8","terminatorHex":"0D0A","prefix":"DATA:","suffix":"",
               "delimiterMode":"literal","delimiter":",","trimFields":true,"maxFrameBytes":4096},
      "channels":[{"id":"temperature","name":"温度","unit":"°C","column":0},
                  {"id":"voltage","name":"电压","unit":"V","column":1}]
    })JSON").object();
}
static QByteArray share(const QVector<ProtocolProfile> &profiles, QString &error) {
    ProtocolSnapshot snapshot; snapshot.profiles = profiles;
    return ProtocolProfileValidator::encode(snapshot, false, error);
}
static QByteArray serialized(const ProtocolSnapshot &snapshot) {
    QString error; return ProtocolProfileValidator::encode(snapshot, true, error);
}
static ProtocolSelection custom(const QString &id) {
    ProtocolSelection s; s.kind = "custom"; s.id = id; return s;
}

struct ParseResult { QVector<QVector<double>> frames; QByteArray remaining; int textEmissions = 0; };
static ParseResult parse(ProtocolParser *parser, const QByteArray &bytes, int chunk) {
    ParseResult result;
    QObject::connect(parser, &ProtocolParser::channelsUpdated, parser, [&result](const QVector<double> &v) { result.frames.append(v); });
    QObject::connect(parser, &ProtocolParser::textReady, parser, [&result](const QByteArray &) { ++result.textEmissions; });
    for (int i = 0; i < bytes.size(); i += chunk) {
        result.remaining += bytes.mid(i, chunk);
        int consumed = parser->processData(result.remaining.constData(), result.remaining.size());
        if (consumed < 0 || consumed > result.remaining.size()) { result.textEmissions = -1; break; }
        result.remaining.remove(0, consumed);
    }
    QObject::disconnect(parser, nullptr, parser, nullptr); return result;
}
static bool same(const ParseResult &a, const ParseResult &b) {
    if (a.remaining != b.remaining || a.textEmissions != b.textEmissions || a.frames.size() != b.frames.size()) return false;
    for (int i = 0; i < a.frames.size(); ++i) {
        if (a.frames[i].size() != b.frames[i].size()) return false;
        for (int j = 0; j < a.frames[i].size(); ++j)
            if (!(std::isnan(a.frames[i][j]) && std::isnan(b.frames[i][j])) && a.frames[i][j] != b.frames[i][j]) return false;
    }
    return true;
}
static ProtocolParser *directParser(int index, bool big) {
    if (index == 1) return new FireWaterParser;
    if (index == 2) { auto p = new JustFloatParser; p->setBigEndian(big); return p; }
    if (index == 3) return new AMU4Parser;
    return new RawParser;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("WaveLinkProtocolTests");
    QCoreApplication::setApplicationName("IsolatedProtocolStore");
    QTemporaryDir temp; VERIFY(temp.isValid()); QString error;
    ProtocolProfile binary, text;
    VERIFY(ProtocolProfileValidator::profile(binaryExample(), binary, error));
    VERIFY(ProtocolProfileValidator::profile(textExample(), text, error));
    VERIFY(binary.id == binaryExample().value("id").toString() && binary.revision == 1);
    auto tab = textExample(); auto frame = tab.value("frame").toObject(); frame.insert("delimiter", "\t"); tab.insert("frame", frame);
    ProtocolProfile checked; VERIFY(ProtocolProfileValidator::profile(tab, checked, error));
    // Editor linkage and range policy travel in both standalone exports and
    // full configuration snapshots without losing their explicit settings.
    for (bool automatic : {false, true}) for (const QString recording : {QString("raw"), QString("converted")}) {
        auto linked = binaryExample(); linked.insert("autoChannelLayout", automatic); linked.insert("outOfRangePolicy", "nan-channel");
        linked.insert("recordingValueMode", recording);
        auto notedChannels = linked["channels"].toArray(); auto noted = notedChannels[0].toObject();
        noted.insert("note", "温度传感器 A1 / °C，偏置校准 ±0.5%"); notedChannels[0] = noted; linked.insert("channels", notedChannels);
        ProtocolProfile profile; VERIFY(ProtocolProfileValidator::profile(linked, profile, error));
        ProtocolSnapshot snapshot; snapshot.profiles = {profile}; snapshot.shared = custom(profile.id);
        for (bool includeSelection : {false, true}) {
            const auto encoded = ProtocolProfileValidator::encode(snapshot, includeSelection, error); VERIFY(!encoded.isEmpty());
            ProtocolSnapshot decoded; VERIFY(ProtocolProfileValidator::decode(encoded, includeSelection, decoded, error));
            VERIFY(decoded.profiles[0].definition == ProtocolProfileValidator::standardizeBinary(profile.definition, true) && decoded.profiles[0].definition.value("autoChannelLayout").toBool() == automatic);
            VERIFY(decoded.profiles[0].recordsConvertedValues() == (recording == "converted"));
        }
        const auto modePath = temp.filePath(QString("values-%1-%2.json").arg(automatic).arg(recording));
        ProtocolProfileStore modeStore(modePath); VERIFY(modeStore.load(error) && modeStore.commit(snapshot, error));
        ProtocolProfileStore reopened(modePath); VERIFY(reopened.load(error));
        VERIFY(reopened.snapshot().profiles[0].recordsConvertedValues() == (recording == "converted"));
        const auto exportPath = modePath + ".export.json";
        VERIFY(reopened.exportProfiles(exportPath, {profile.id}, error));
        QByteArray bytes; VERIFY(ProtocolProfileStore::readFile(exportPath, bytes, error));
        ProtocolImportPreview imported;
        VERIFY(ProtocolProfileStore::prepareImport({}, bytes, false, ProtocolImportMode::Append, false, imported, error));
        VERIFY(imported.snapshot.profiles[0].recordsConvertedValues() == (recording == "converted"));
        VERIFY(imported.snapshot.profiles[0].definition.value("channels") == profile.definition.value("channels"));
        VERIFY(imported.snapshot.profiles[0].definition["channels"].toArray()[0].toObject()["note"] == noted["note"]);
    }

    // Unsupported types, non-integral offsets, bounds, overlaps, malformed
    // HEX and unknown rules must all reject before an import changes storage.
    QVector<QJsonObject> badProfiles;
    for (auto key : {QString("id"), QString("name"), QString("type")}) { auto bad = binaryExample(); bad.insert(key, ""); badProfiles.append(bad); }
    auto bad = binaryExample(); bad.insert("revision", 0); badProfiles.append(bad);
    bad = binaryExample(); bad.insert("revision", 1.5); badProfiles.append(bad);
    bad = binaryExample(); bad.insert("byteOrder", "native"); badProfiles.append(bad);
    bad = binaryExample(); bad.insert("arbitraryRule", true); badProfiles.append(bad);
    bad = binaryExample(); bad.insert("autoChannelLayout", "true"); badProfiles.append(bad);
    for (const auto &value : {QJsonValue(-1), QJsonValue(1.5), QJsonValue(1e100), QJsonValue("2"), QJsonValue(2147483647)}) {
        bad = binaryExample(); auto f = bad.value("frame").toObject(); f.insert("payloadOffset", value); bad.insert("frame", f); badProfiles.append(bad);
    }
    for (const auto &value : {QJsonValue("AA5"), QJsonValue("AAZZ"), QJsonValue(true)}) {
        bad = binaryExample(); auto f = bad.value("frame").toObject(); f.insert("headerHex", value); bad.insert("frame", f); badProfiles.append(bad);
    }
    for (const auto &change : {QJsonObject{{"offset", 5}}, QJsonObject{{"offset", 1}}, QJsonObject{{"type", "uint64"}},
         QJsonObject{{"id", "temperature"}}, QJsonObject{{"scale", "1"}}, QJsonObject{{"min", 5}, {"max", 4}},
         QJsonObject{{"note", 123}}, QJsonObject{{"note", QString(1025, 'x')}}, QJsonObject{{"note", QString("invalid\ncontrol")}}}) {
        bad = binaryExample(); auto channels = bad.value("channels").toArray(); auto c = channels[1].toObject();
        for (auto it = change.begin(); it != change.end(); ++it) c.insert(it.key(), it.value());
        channels[1] = c; bad.insert("channels", channels); badProfiles.append(bad);
    }
    bad = binaryExample(); auto checksum = bad.value("checksum").toObject(); checksum.insert("range", QJsonObject{{"offset", 8}, {"length", 1}});
    bad.insert("checksum", checksum); badProfiles.append(bad);
    bad = binaryExample(); bad.insert("channels", QJsonArray{}); badProfiles.append(bad);
    bad = textExample(); auto f = bad.value("frame").toObject(); f.insert("terminatorHex", ""); bad.insert("frame", f); badProfiles.append(bad);
    for (const auto &invalid : badProfiles) {
        checked = binary;
        VERIFY(!ProtocolProfileValidator::profile(invalid, checked, error) && !error.isEmpty());
        VERIFY(checked.definition == binary.definition);
    }

    const auto path = temp.filePath("store/custom_protocols.json");
    ProtocolProfileStore store(path); VERIFY(store.load(error) && store.ready());
    QString binaryId, textId;
    VERIFY(store.saveProfile(binaryExample(), binaryId, error));
    VERIFY(store.saveProfile(textExample(), textId, error));
    VERIFY(store.setSelection(custom(binaryId), error));
    VERIFY(store.defaultExportFileName() == "WaveLink_Protocols.json");
    VERIFY(store.defaultExportFileName({binaryId}) == "WaveLink_Protocols_温度与电压.json");
    VERIFY(store.defaultExportFileName({binaryId, textId}) == "WaveLink_Protocols_温度与电压_文本温度与电压.json");
    VERIFY(store.defaultExportFileName({textId, binaryId}) == "WaveLink_Protocols_文本温度与电压_温度与电压.json");
    VERIFY(store.defaultExportFileName({binaryId, binaryId}) == store.defaultExportFileName({binaryId}));
    {
        ProtocolProfileStore filenames(temp.filePath("filenames.json")); VERIFY(filenames.load(error));
        auto named = textExample(); named.insert("name", "温度<>:\"/\\|?* ."); QString id;
        VERIFY(filenames.saveProfile(named, id, error));
        VERIFY(filenames.defaultExportFileName({id}) == "WaveLink_Protocols_温度_________.json");
        QStringList longIds;
        for (int i = 0; i < 5; ++i) {
            named.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
            named.insert("name", QString(60, QChar('A')) + QString::number(i));
            VERIFY(filenames.saveProfile(named, id, error)); longIds << id;
        }
        const auto bounded = filenames.defaultExportFileName(longIds);
        VERIFY(bounded.size() <= 184 && bounded.endsWith("_等5个协议.json"));
        VERIFY(filenames.defaultExportFileName() == "WaveLink_Protocols.json");
    }
    ProtocolProfileStore reloaded(path); VERIFY(reloaded.load(error));
    VERIFY(serialized(reloaded.snapshot()) == serialized(store.snapshot()));
    VERIFY(!store.exportProfiles(path, {}, error));
    VERIFY(!store.exportProfiles(path + ".bak", {}, error));
    {
        QLockFile lock(path + ".lock"); VERIFY(lock.tryLock(0));
        VERIFY(!store.commit(store.snapshot(), error));
    }
    VERIFY(!store.removeProfile(binaryId, error));
    QString copy1, copy2;
    VERIFY(store.copyProfile(binaryId, copy1, error) && store.copyProfile(binaryId, copy2, error));
    VERIFY(copy1 != copy2 && copy1 != binaryId);
    VERIFY(store.snapshot().profiles[0].id == binaryId && store.snapshot().profiles[3].id == textId);
    VERIFY(store.snapshot().profiles[1].id == copy2 && store.snapshot().profiles[1].name == binary.name + "_2");
    VERIFY(store.snapshot().profiles[2].id == copy1 && store.snapshot().profiles[2].name == binary.name + "_1");
    VERIFY(store.snapshot().profiles[2].definition.value("channels") == binary.definition.value("channels"));
    VERIFY(reloaded.load(error) && serialized(reloaded.snapshot()) == serialized(store.snapshot()));
    VERIFY(store.moveProfile(copy2, 1, error) && store.snapshot().profiles[2].id == copy2);
    auto changed = binaryExample(); changed.insert("description", "更新备注");
    VERIFY(store.saveProfile(changed, binaryId, error));
    VERIFY(store.snapshot().profiles[0].revision == 2);
    VERIFY(store.saveProfile(changed, binaryId, error) && store.snapshot().profiles[0].revision == 2);
    VERIFY(store.removeProfile(copy1, error));
    VERIFY(store.exportProfiles(temp.filePath("selected.json"), {binaryId, textId}, error));
    QByteArray exported; VERIFY(ProtocolProfileStore::readFile(temp.filePath("selected.json"), exported, error));
    ProtocolSnapshot standalone;
    VERIFY(ProtocolProfileValidator::decode(exported, false, standalone, error) && standalone.profiles.size() == 2);
    VERIFY(!exported.contains("selections") && !exported.contains(path.toUtf8()));
    const auto before = serialized(store.snapshot());
    ProtocolImportPreview preview;
    VERIFY(ProtocolProfileStore::prepareImport(store.snapshot(), exported, false, ProtocolImportMode::Append, false, preview, error));
    VERIFY(preview.added == 2 && preview.updated == 0 && preview.idMap.value(binaryId) != binaryId);
    VERIFY(preview.snapshot.shared.id == binaryId && serialized(store.snapshot()) == before);
    VERIFY(preview.snapshot.profiles[3].name == binary.name + "_1");
    VERIFY(store.commit(preview.snapshot, error));

    // Global backup restore remaps references when appended as new profiles.
    ProtocolSnapshot global; global.profiles = {binary, text}; global.shared = custom(binary.id); global.ble = custom(text.id);
    const auto globalBytes = serialized(global);
    ProtocolSnapshot empty;
    VERIFY(ProtocolProfileStore::prepareImport(empty, globalBytes, true, ProtocolImportMode::Append, true, preview, error));
    VERIFY(preview.snapshot.shared.id == preview.idMap.value(binary.id));
    VERIFY(preview.snapshot.ble.id == preview.snapshot.shared.id);
    VERIFY(ProtocolProfileStore::prepareImport(global, globalBytes, true, ProtocolImportMode::Replace, true, preview, error));
    VERIFY(serialized(preview.snapshot) == globalBytes);
    const auto textOnly = share({text}, error);
    VERIFY(!ProtocolProfileStore::prepareImport(global, textOnly, false, ProtocolImportMode::Replace, false, preview, error));
    VERIFY(!ProtocolProfileStore::prepareImport(empty, exported, false, ProtocolImportMode::Append, true, preview, error));
    changed = binaryExample(); changed.insert("description", "来自分享文件的新内容");
    VERIFY(ProtocolProfileValidator::profile(changed, checked, error));
    VERIFY(ProtocolProfileStore::prepareImport(global, share({checked}, error), false, ProtocolImportMode::MergeById, false, preview, error));
    VERIFY(preview.updated == 1 && preview.added == 0 && preview.snapshot.profiles[0].revision == 2);

    // Invalid input and references leave the caller's snapshot untouched.
    auto root = QJsonDocument::fromJson(exported).object(); root.insert("schemaVersion", 99);
    QVector<QByteArray> invalidDocuments{QJsonDocument(root).toJson(), "{bad", QByteArray(MaxDocumentBytes + 1, ' '),
        QByteArray(20, '[') + QByteArray(20, ']'), globalBytes}; // Last has wrong document kind.
    root = QJsonDocument::fromJson(exported).object(); auto profiles = root.value("profiles").toArray(); profiles.append(profiles[0]);
    root.insert("profiles", profiles); invalidDocuments.append(QJsonDocument(root).toJson());
    for (const auto &bytes : invalidDocuments) {
        const auto unchanged = serialized(store.snapshot());
        VERIFY(!ProtocolProfileStore::prepareImport(store.snapshot(), bytes, false, ProtocolImportMode::Append, false, preview, error));
        VERIFY(serialized(store.snapshot()) == unchanged);
    }
    auto invalidSnapshot = store.snapshot(); invalidSnapshot.shared = custom(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QByteArray onDisk; VERIFY(ProtocolProfileStore::readFile(path, onDisk, error));
    VERIFY(!store.commit(invalidSnapshot, error));
    QByteArray after; VERIFY(ProtocolProfileStore::readFile(path, after, error) && after == onDisk);

    // A blocked backup path simulates storage failure portably, even when
    // tests run under an account that bypasses filesystem permissions.
    const auto failPath = temp.filePath("failure.json"); ProtocolProfileStore failing(failPath); VERIFY(failing.load(error));
    VERIFY(failing.saveProfile(binaryExample(), binaryId, error));
    const auto failBefore = serialized(failing.snapshot());
    VERIFY(QDir().mkdir(failPath + ".bak"));
    VERIFY(!failing.saveProfile(textExample(), textId, error) && serialized(failing.snapshot()) == failBefore);
    VERIFY(ProtocolProfileStore::readFile(failPath, after, error) && after == failBefore);

    const auto concurrentPath = temp.filePath("concurrent.json");
    ProtocolProfileStore writer(concurrentPath), stale(concurrentPath);
    VERIFY(writer.load(error) && writer.commit(global, error) && stale.load(error));
    changed = binaryExample(); changed.insert("description", "其他实例保存的修改");
    VERIFY(writer.saveProfile(changed, binaryId, error));
    VERIFY(!stale.commit(global, error) && !stale.ready());
    VERIFY(ProtocolProfileStore::readFile(concurrentPath, after, error) && after == serialized(writer.snapshot()));

    QByteArray backup; VERIFY(ProtocolProfileStore::readFile(path + ".bak", backup, error));
    VERIFY(ProtocolProfileStore::writeFile(path, "{damaged", error));
    VERIFY(!store.load(error) && !store.ready());
    VERIFY(!store.commit(global, error));
    VERIFY(ProtocolProfileStore::readFile(path, after, error) && after == "{damaged");
    VERIFY(store.restoreBackup(error) && store.ready());
    VERIFY(serialized(store.snapshot()) == backup);
    VERIFY(QDir(temp.filePath("store")).entryList({"*.before-restore-*"}, QDir::Files).size() == 1);

    // Migration never writes settings, and main settings have priority.
    QSettings main(temp.filePath("main.ini"), QSettings::IniFormat);
    QSettings advanced(temp.filePath("advanced.ini"), QSettings::IniFormat);
    main.setValue("comboProtocol", 2); main.setValue("Ble/protocol", 3);
    advanced.setValue("Protocol", 1); advanced.setValue("Endianness", 1);
    const auto oldKeys = main.allKeys(); auto migrated = ProtocolProfileStore::legacySelections(main, advanced);
    VERIFY(migrated.shared.id == "builtin.justfloat" && migrated.shared.bigEndian && migrated.ble.id == migrated.shared.id);
    VERIFY(main.allKeys() == oldKeys && main.value("comboProtocol").toInt() == 2);
    main.remove("comboProtocol"); migrated = ProtocolProfileStore::legacySelections(main, advanced);
    VERIFY(migrated.shared.id == "builtin.firewater");
    {
        // Existing valid non-uniform rules are rewritten once, with a safe
        // backup. Only the global protocol selection survives persistence.
        const auto legacyPath = temp.filePath("migration.json");
        QJsonObject oldDocument{{"kind", "WaveLink.ProtocolStore"}, {"schemaVersion", 1}, {"profiles", QJsonArray{binaryExample(), textExample()}},
            {"selections", QJsonObject{{"shared", custom(binary.id).toJson()}, {"ble", custom(text.id).toJson()}}}};
        const auto oldBytes = QJsonDocument(oldDocument).toJson(QJsonDocument::Compact);
        VERIFY(ProtocolProfileStore::writeFile(legacyPath, oldBytes, error)); ProtocolProfileStore upgraded(legacyPath);
        VERIFY(upgraded.load(error)); QByteArray currentBytes, preserved;
        VERIFY(ProtocolProfileStore::readFile(legacyPath, currentBytes, error) && ProtocolProfileStore::readFile(legacyPath + ".bak", preserved, error));
        const auto selections = QJsonDocument::fromJson(currentBytes).object()["selections"].toObject();
        VERIFY(selections.size() == 1 && selections["shared"].toObject()["id"] == QJsonValue(binary.id) && preserved == oldBytes);
        const auto rule = upgraded.snapshot().profiles[0].definition["checksum"].toObject();
        VERIFY(rule["range"].toObject()["offset"] == QJsonValue(0) && rule["field"].toObject()["from"] == QJsonValue("end"));
        VERIFY(upgraded.load(error)); QByteArray afterSecondLoad; VERIFY(ProtocolProfileStore::readFile(legacyPath, afterSecondLoad, error) && afterSecondLoad == currentBytes);
        VERIFY(ProtocolProfileStore::readFile(legacyPath + ".bak", preserved, error) && preserved == oldBytes);
    }

    for (int chunk = 1; chunk <= 32; ++chunk) {
        JustFloatParser strict;
        const auto result = parse(&strict, QByteArray::fromHex("0000803F0000FF4E000000400000807F").repeated(3), chunk);
        VERIFY(result.frames.size() == 2);
        for (const auto &frame : result.frames) VERIFY(frame == QVector<double>({1, 2139095040.0, 2}));
        FireWaterParser text;
        const auto clean = parse(&text, "1,2,3\n\nDEBUG: device ready\n4,5,6\n", chunk);
        VERIFY(clean.frames.size() == 2 && clean.frames[0] == QVector<double>({1,2,3}) && clean.frames[1] == QVector<double>({4,5,6}));
    }
    // Golden answers plus equivalence under every chunk size. This catches
    // accidental changes to the established parsers when sharing a factory.
    const QVector<QByteArray> inputs{"hello\r\n", "image:skip\nDATA:1.25,-2,invalid\npartial", QByteArray::fromHex("0000c03f000010c00000807f").repeated(3), "0:1\r\n1:2\r\n2:3\r\n"};
    for (int index = 0; index <= 3; ++index) for (int chunk = 1; chunk <= inputs[index].size(); ++chunk) {
        std::unique_ptr<ProtocolParser> factory(ProtocolParserFactory::createLegacy(index, false, nullptr));
        std::unique_ptr<ProtocolParser> direct(directParser(index, false));
        auto a = parse(factory.get(), inputs[index], chunk); auto b = parse(direct.get(), inputs[index], chunk);
        VERIFY(same(a, b) && a.textEmissions == 0);
        if (index == 0) VERIFY(a.frames.isEmpty() && a.remaining.isEmpty());
        if (index == 1) VERIFY(a.frames.size() == 1 && a.frames[0][0] == 1.25 && a.frames[0][1] == -2 && std::isnan(a.frames[0][2]));
        if (index == 2) VERIFY(a.frames.size() == 2 && a.frames[0][0] == 1.5 && a.frames[0][1] == -2.25);
        if (index == 3) VERIFY(a.frames.size() == 1 && a.frames[0] == QVector<double>({1, 2, 3}));
    }
    std::unique_ptr<ProtocolParser> big(ProtocolParserFactory::create(ProtocolSelection::fromLegacy(2, true), nullptr, error));
    VERIFY(big && error.isEmpty());
    auto bigFrames = parse(big.get(), QByteArray::fromHex("3fc00000c01000007f800000").repeated(3), 1);
    VERIFY(bigFrames.frames.size() == 2 && bigFrames.frames[0] == QVector<double>({1.5, -2.25}));
    VERIFY(!ProtocolParserFactory::create(custom(binary.id), nullptr, error) && !error.isEmpty());
    ProtocolSelection unknown; unknown.id = "builtin.not-supported";
    VERIFY(!ProtocolParserFactory::create(unknown, nullptr, error));
    std::unique_ptr<ProtocolParser> legacyUnknown(ProtocolParserFactory::createLegacy(9, false, nullptr));
    VERIFY(qobject_cast<RawParser *>(legacyUnknown.get()));
    std::cout << "Protocol model/store/import and builtin parser regressions passed.\n"; return 0;
}
