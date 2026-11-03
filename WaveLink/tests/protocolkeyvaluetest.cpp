#include "CustomProtocolParser.h"
#include "ProtocolParserFactory.h"
#include "ProtocolSampleGenerator.h"
#include "ProtocolProfileStore.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QThread>
#include <QFile>
#include <memory>
#include <cmath>
#include <iostream>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Key-value failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
struct Result { QVector<QVector<double>> raw, values; ProtocolParseStats stats; QByteArray remaining; };
static Result parse(QJsonObject definition, const QByteArray &bytes, int chunk) {
    Result result; ProtocolProfile profile; QString error;
    if (!ProtocolProfileValidator::profile(definition, profile, error)) { std::cerr << error.toStdString(); return result; }
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error)));
    QObject::connect(parser.get(), &CustomProtocolParser::frameDecoded, [&](const QVector<double> &raw, const QVector<double> &values) { result.raw << raw; result.values << values; });
    for (int pos = 0; pos < bytes.size(); pos += chunk) {
        result.remaining += bytes.mid(pos, chunk);
        const int consumed = parser->processData(result.remaining.constData(), result.remaining.size());
        if (consumed < 0 || consumed > result.remaining.size()) return {};
        result.remaining.remove(0, consumed);
    }
    result.stats = parser->stats(); return result;
}
static QJsonObject frameChange(QJsonObject d, const QString &key, const QJsonValue &v) {
    auto frame = d.value("frame").toObject(); frame.insert(key, v); d.insert("frame", frame); return d;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QTemporaryDir directory; VERIFY(directory.isValid()); QString error; ProtocolProfile profile;
    auto generic = ProtocolProfileValidator::keyValueTemplate(), cbf = ProtocolProfileValidator::keyValueTemplate(true);
    VERIFY(ProtocolProfileValidator::profile(generic, profile, error) && profile.isKeyValue() && profile.outputChannelNames() == QStringList({"Ch1", "Ch2", "Ch3"}));
    const QString example = "样例：0:0\\r\\n1:1\\r\\n2:2\\r\\n";
    VERIFY(generic.value("description").toString().startsWith(example) && cbf.value("description").toString().startsWith(example));
    VERIFY(generic.value("outOfRangePolicy") == QJsonValue("nan-channel") && cbf.value("outOfRangePolicy") == QJsonValue("nan-channel"));
    QByteArray sample; VERIFY(ProtocolSampleGenerator::generate(profile, sample, error));
    VERIFY(sample == "0:1\r\n1:2\r\n2:3\r\n");
    VERIFY(parse(generic, "0:0\r\n1:1\r\n2:2\r\n", 1).values == QVector<QVector<double>>({{0, 1, 2}}));
    // Grouping tests below deliberately use a two-channel configuration.
    auto twoChannels = generic.value("channels").toArray(); twoChannels.removeLast(); generic.insert("channels", twoChannels);
    for (int chunk : {1, 2, 3, 7, 4096}) {
        const auto r = parse(cbf, "2:3.30\r\n0:25.34\r\n1:2.5\r\n1:20\r\n0:10\r\n2:30\r\n", chunk);
        VERIFY(r.values == QVector<QVector<double>>({{25.34, 2.5, 3.3}, {10, 20, 30}}) && r.stats.validFrames == 2 && r.stats.rejectedFrames == 0 && r.remaining.isEmpty());
    }
    auto unknown = parse(generic, "0:1\r\nlog:debug\r\n1:2\r\n", 1);
    VERIFY(unknown.values == QVector<QVector<double>>({{1, 2}}) && unknown.stats.rejectedFrames == 0);
    auto strict = frameChange(generic, "unknownKeyPolicy", "reject-group");
    auto rejected = parse(strict, "0:1\r\nother:99\r\n1:2\r\n0:3\r\n", 3);
    VERIFY(rejected.values == QVector<QVector<double>>({{3, 2}}) && rejected.stats.rejectedFrames == 1);
    auto invalid = parse(generic, "0:1\r\n1:Inf\r\n1:2\r\n0:3\r\n", 7);
    VERIFY(invalid.values == QVector<QVector<double>>({{3, 2}}) && invalid.stats.rejectedFrames == 1);
    const auto missingSample = parse(generic, "0:1\r\n1:NaN\r\n0:3\r\n1:2\r\n", 1);
    VERIFY(missingSample.stats.validFrames == 2 && missingSample.stats.rejectedFrames == 0 && missingSample.stats.outOfRangeValues == 0);
    VERIFY(missingSample.raw[0][0] == 1 && std::isnan(missingSample.raw[0][1]) && std::isnan(missingSample.values[0][1]));
    VERIFY(missingSample.values[1] == QVector<double>({3, 2}));
    auto restart = frameChange(cbf, "duplicateKeyPolicy", "restart");
    auto repeated = parse(restart, "0:1\r\n1:2\r\n0:10\r\n2:30\r\n1:20\r\n", 1);
    VERIFY(repeated.values == QVector<QVector<double>>({{10, 20, 30}}) && repeated.stats.rejectedFrames == 1);
    auto replaced = parse(cbf, "0:1\r\n1:2\r\n0:10\r\n2:30\r\n", 1);
    VERIFY(replaced.values == QVector<QVector<double>>({{10, 2, 30}}) && replaced.stats.rejectedFrames == 0);
    auto mapped = generic; auto channels = mapped.value("channels").toArray();
    auto first = channels[0].toObject(); first.insert("key", "温度"); first.insert("name", "温度"); first.insert("scale", .01); channels[0] = first;
    auto second = channels[1].toObject(); second.insert("key", "V"); second.insert("bias", 1); channels[1] = second; mapped.insert("channels", channels);
    mapped = frameChange(mapped, "prefix", "DATA:"); mapped = frameChange(mapped, "delimiter", "=");
    VERIFY(parse(mapped, "DATA:V=2.3\r\nDATA:温度=2534\r\n", 1).values == QVector<QVector<double>>({{25.34, 3.3}}));
    auto swapped = channels; swapped[0] = second; swapped[1] = first; mapped.insert("channels", swapped);
    VERIFY(parse(mapped, "DATA:温度=2534\r\nDATA:V=2.3\r\n", 2).values == QVector<QVector<double>>({{3.3, 25.34}}));
    auto limited = generic; channels = limited.value("channels").toArray(); first = channels[0].toObject(); first.insert("max", 5); channels[0] = first;
    limited.insert("channels", channels);
    const auto ranged = parse(limited, "0:10\r\n1:2\r\n", 1);
    VERIFY(ranged.values.size() == 1 && std::isnan(ranged.values[0][0]) && ranged.values[0][1] == 2 && ranged.raw[0][0] == 10 && ranged.stats.outOfRangeValues == 1);
    // Explicit rejection must still discard the group after the UI options move.
    auto rejectRange = limited; rejectRange.insert("outOfRangePolicy", "reject-frame");
    const auto rejectedRange = parse(rejectRange, "0:10\r\n1:2\r\n0:3\r\n", 1);
    VERIFY(rejectedRange.values == QVector<QVector<double>>({{3, 2}}) && rejectedRange.stats.rejectedFrames == 1);
    auto tooLong = frameChange(generic, "maxFrameBytes", 16);
    const auto recovered = parse(tooLong, "0:1\r\n" + QByteArray(64, 'X') + "\r\n1:2\r\n0:3\r\n", 1);
    VERIFY(recovered.values == QVector<QVector<double>>({{3, 2}}) && recovered.stats.rejectedFrames == 1);
    // Reconnect/reset and timeout must never combine values from old sessions.
    auto timed = frameChange(generic, "groupTimeoutMs", 5); VERIFY(ProtocolProfileValidator::profile(timed, profile, error));
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error)));
    QVector<QVector<double>> values; QObject::connect(parser.get(), &ProtocolParser::channelsUpdated, [&](const QVector<double> &v) { values << v; });
    QByteArray line("0:1\r\n"); parser->processData(line.constData(), line.size()); QThread::msleep(15);
    line = "1:2\r\n"; parser->processData(line.constData(), line.size()); VERIFY(values.isEmpty() && parser->stats().rejectedFrames == 1);
    line = "0:3\r\n"; parser->processData(line.constData(), line.size()); VERIFY(values == QVector<QVector<double>>({{3, 2}}));
    line = "0:99\r\n"; parser->processData(line.constData(), line.size()); parser->reset();
    line = "1:4\r\n0:5\r\n"; parser->processData(line.constData(), line.size()); VERIFY(values.last() == QVector<double>({5, 4}));
    auto duplicate = generic; channels = duplicate.value("channels").toArray(); second = channels[1].toObject(); second.insert("key", "0"); channels[1] = second; duplicate.insert("channels", channels);
    VERIFY(!ProtocolProfileValidator::profile(duplicate, profile, error));
    VERIFY(!ProtocolProfileValidator::profile(frameChange(generic, "terminatorHex", ""), profile, error));
    auto missing = generic; missing.insert("channels", QJsonArray{}); VERIFY(!ProtocolProfileValidator::profile(missing, profile, error));
    // Templates are opt-in; only an old active builtin.amu4 choice migrates.
    ProtocolProfileStore store(directory.filePath("protocols.json")); VERIFY(store.load(error) && store.snapshot().profiles.isEmpty());
    ProtocolSnapshot old; old.shared = ProtocolSelection::fromLegacy(3); old.ble = old.shared;
    VERIFY(store.commit(old, error) && store.snapshot().profiles.size() == 1 && store.snapshot().shared.kind == "custom");
    const auto migrated = store.snapshot(); VERIFY(migrated.profiles[0].name == "CBF/IR/RED" && migrated.profiles[0].isKeyValue());
    VERIFY(migrated.profiles[0].definition.value("devicePreset").toObject().value("enabled").toBool());
    VERIFY(migrated.profiles[0].definition.value("frame").toObject().value("groupTimeoutMs").toInt() == 0);
    VERIFY(store.load(error) && store.snapshot().profiles.size() == 1 && store.snapshot().shared.id == migrated.shared.id);
    const auto sharedBytes = ProtocolProfileValidator::encode(migrated, false, error); ProtocolSnapshot decoded;
    VERIFY(!sharedBytes.isEmpty() && ProtocolProfileValidator::decode(sharedBytes, false, decoded, error) && decoded.profiles[0].definition == migrated.profiles[0].definition);
    ProtocolProfileStore fresh(directory.filePath("old-first-launch.json")); VERIFY(fresh.load(error, old) && QFile::exists(fresh.path()) && fresh.snapshot().shared.kind == "custom");
    // Device templates opt in to their preset; generic parsing has no preset.
    VERIFY(cbf.value("devicePreset").toObject().value("enabled").toBool() && !generic.contains("devicePreset"));
    const auto cbfFields = cbf.value("channels").toArray();
    VERIFY(cbfFields[0].toObject().value("unit") == QJsonValue("MHz") && cbfFields[1].toObject().value("unit").toString().isEmpty()
        && cbfFields[2].toObject().value("unit").toString().isEmpty());
    VERIFY(cbf.value("description").toString().contains("\n设备采样率默认31Hz，采用TCP服务端数据接口，默认端口4970。\n"));
    return 0;
}
