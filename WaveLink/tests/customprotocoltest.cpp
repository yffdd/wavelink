#include "CustomProtocolParser.h"
#include "ProtocolParserFactory.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <memory>
#include <cmath>
#include <iostream>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Custom protocol failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static QJsonObject binary() {
    return QJsonDocument::fromJson(R"JSON({"id":"b90c4e0d-f4c2-4d34-937a-54a29edaa728","revision":1,"name":"binary","type":"binary","byteOrder":"little",
    "frame":{"mode":"fixed","headerHex":"AA55","sizeBytes":11,"tailHex":"0D0A","payloadOffset":2,"payloadBytes":6,"maxFrameBytes":4096},
    "checksum":{"algorithm":"sum8","range":{"offset":2,"length":6},"field":{"offset":8,"type":"uint8"}},
    "channels":[{"id":"temperature","name":"temperature","offset":0,"type":"int16","scale":0.01,"bias":0},{"id":"voltage","name":"voltage","offset":2,"type":"float32"}]})JSON").object();
}
static QJsonObject text() {
    return QJsonDocument::fromJson(R"JSON({"id":"f75b780c-b3a8-42a8-8f2a-8adeb18a3991","revision":1,"name":"text","type":"text",
    "frame":{"encoding":"utf8","terminatorHex":"0D0A","prefix":"DATA:","suffix":"","delimiterMode":"literal","delimiter":",","trimFields":true,"maxFrameBytes":4096},
    "channels":[{"id":"a","name":"A","column":0,"scale":2,"bias":-1},{"id":"b","name":"B","column":1}]})JSON").object();
}
struct Result { QVector<QVector<double>> raw, converted; ProtocolParseStats stats; QByteArray remaining; };
static bool run(const QJsonObject &definition, const QByteArray &input, int chunk, Result &result) {
    ProtocolProfile p; QString error;
    if (!ProtocolProfileValidator::profile(definition, p, error)) { std::cerr << error.toStdString(); return false; }
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(p, nullptr, error)));
    if (!parser) return false;
    Result r;
    QObject::connect(parser.get(), &CustomProtocolParser::frameDecoded, parser.get(), [&](const QVector<double> &raw, const QVector<double> &values) { r.raw.append(raw); r.converted.append(values); });
    quint32 random = 7;
    for (int pos = 0; pos < input.size();) {
        random = random * 1664525u + 1013904223u;
        const int count = chunk ? chunk : int(random % 23) + 1;
        r.remaining += input.mid(pos, count); pos += qMin(count, input.size() - pos);
        const int consumed = parser->processData(r.remaining.constData(), r.remaining.size());
        if (consumed < 0 || consumed > r.remaining.size()) return false;
        r.remaining.remove(0, consumed);
    }
    r.stats = parser->stats(); result = r; return true;
}
static QJsonObject frameChange(QJsonObject profile, const QJsonObject &changes) {
    auto f = profile.value("frame").toObject(); for (auto it = changes.begin(); it != changes.end(); ++it) f.insert(it.key(), it.value()); profile.insert("frame", f); return profile;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); Result r;
    const auto good = QByteArray::fromHex("AA55E60933335340E80D0A");
    VERIFY(run(binary(), good.repeated(3), 4096, r) && r.stats.validFrames == 3);
    VERIFY(run(text(), "DATA:1,2\r\nDATA:3,4\r\n", 4096, r) && r.stats.validFrames == 2);
    VERIFY(run(text(), "DATA:NaN,+nAn,-NAN\r\n", 1, r) && r.stats.validFrames == 1 && r.stats.outOfRangeValues == 0);
    VERIFY(r.raw[0].size() == 3 && r.converted[0].size() == 3);
    for (int i = 0; i < 3; ++i) VERIFY(std::isnan(r.raw[0][i]) && std::isnan(r.converted[0][i]));
    // Configured fields are overrides, NOT a limit on the text frame's width.
    for (int chunk = 0; chunk < 24; ++chunk) {
        VERIFY(run(text(), "DATA:25.34,3.30,2.5,2.8\r\nDATA:1,2\r\nDATA:3,4,5,6\r\n", chunk, r));
        VERIFY(r.stats.validFrames == 3 && r.stats.rejectedFrames == 0 && r.remaining.isEmpty());
        VERIFY(r.raw[0] == QVector<double>({25.34, 3.3, 2.5, 2.8}));
        VERIFY(r.converted[0] == QVector<double>({49.68, 3.3, 2.5, 2.8}));
        VERIFY(r.converted[1] == QVector<double>({1, 2}) && r.converted[2] == QVector<double>({5, 4, 5, 6}));
        VERIFY(run(text(), "DATA:1,2,\r\nDATA:1,2,nan\r\nDATA:1,2,1e999\r\nDATA:1,2,LOG\r\nDATA:1,2,3\r\n", chunk, r));
        VERIFY(r.stats.validFrames == 2 && r.stats.rejectedFrames == 3 && r.stats.outOfRangeValues == 0);
        VERIFY(r.raw[0].size() == 3 && std::isnan(r.raw[0][2]) && std::isnan(r.converted[0][2]) && r.raw[1] == QVector<double>({1, 2, 3}));
    }
    auto mapped = text(); auto mappedFields = mapped.value("channels").toArray();
    auto mappedFirst = mappedFields[0].toObject(); mappedFirst.insert("column", 2); mappedFirst.insert("id", "auto-column-0"); mappedFields[0] = mappedFirst;
    auto mappedSecond = mappedFields[1].toObject(); mappedSecond.insert("column", 0); mappedFields[1] = mappedSecond; mapped.insert("channels", mappedFields);
    VERIFY(run(mapped, "DATA:10,20,30,40\r\n", 1, r) && r.converted[0] == QVector<double>({59, 10, 20, 40}));
    ProtocolProfile mappedProfile; QString metadataError;
    VERIFY(ProtocolProfileValidator::profile(mapped, mappedProfile, metadataError));
    auto metadata = mappedProfile.outputChannels(4);
    VERIFY(metadata.size() == 4 && metadata[0].toObject()["name"] == QJsonValue("A") && metadata[1].toObject()["name"] == QJsonValue("B"));
    VERIFY(metadata[2].toObject()["name"] == QJsonValue("Ch2") && metadata[2].toObject()["column"].toInt() == 1);
    VERIFY(metadata[3].toObject()["name"] == QJsonValue("Ch4") && metadata[3].toObject()["unit"].toString().isEmpty());
    VERIFY(mappedProfile.outputChannels(3)[2] == metadata[2]); // Width changes keep identities stable.
    auto automatic = text(); automatic.insert("channels", QJsonArray{});
    VERIFY(ProtocolProfileValidator::profile(automatic, mappedProfile, metadataError));
    VERIFY(run(automatic, "DATA:25.34,3.30,2.5,2.8\r\n", 1, r) && r.raw[0] == r.converted[0] && r.raw[0].size() == 4);
    VERIFY(mappedProfile.outputChannels(4)[3].toObject()["name"] == QJsonValue("Ch4"));
    auto unnamed = text(); auto unnamedFields = unnamed["channels"].toArray();
    auto unnamedFirst = unnamedFields[0].toObject(), unnamedSecond = unnamedFields[1].toObject();
    unnamedFirst.remove("name"); unnamedSecond.insert("name", "   ");
    unnamedFields[0] = unnamedFirst; unnamedFields[1] = unnamedSecond; unnamed.insert("channels", unnamedFields);
    VERIFY(ProtocolProfileValidator::profile(unnamed, mappedProfile, metadataError));
    VERIFY(mappedProfile.outputChannelNames(2) == QStringList({"Ch1", "Ch2"}));
    auto collision = text(); auto collisionFields = collision["channels"].toArray();
    auto collisionField = collisionFields[0].toObject(); collisionField.insert("id", "auto-column-2"); collisionFields[0] = collisionField; collision.insert("channels", collisionFields);
    VERIFY(ProtocolProfileValidator::profile(collision, mappedProfile, metadataError));
    VERIFY(mappedProfile.outputChannels(3)[2].toObject()["id"] == QJsonValue("auto-column-2_1"));
    // Repeated mappings stay intentional separate converted channels, while
    // automatically discovered columns are not duplicated.
    collisionField.insert("column", 1); collisionFields[0] = collisionField; collision.insert("channels", collisionFields);
    VERIFY(run(collision, "DATA:10,20,30\r\n", 1, r) && r.converted[0] == QVector<double>({39, 20, 10, 30}));
    for (int chunk = 0; chunk < 24; ++chunk) {
        VERIFY(run(binary(), good.repeated(3) + good.left(5), chunk, r));
        VERIFY(r.stats.validFrames == 3 && r.stats.rejectedFrames == 0 && r.remaining == good.left(5));
        VERIFY(std::abs(r.converted[0][0] - 25.34) < 1e-12 && std::abs(r.converted[0][1] - 3.3) < 1e-6);
        VERIFY(r.raw[0][0] == 2534);
        VERIFY(run(text(), "log: hello\r\nDATA:25.34,3.30\r\nDATA:1e2,-.25\r\nDATA:25.34,,3.30\r\nDATA:nan,1\r\nDATA:", chunk, r));
        VERIFY(r.stats.validFrames == 3 && r.stats.rejectedFrames == 2 && r.remaining == "DATA:");
        VERIFY(r.converted[0] == QVector<double>({49.68, 3.3}) && r.converted[1] == QVector<double>({199, -.25}));
    }
    auto damaged = good; damaged[4] = char(0x32);
    for (int chunk = 0; chunk < 15; ++chunk) {
        VERIFY(run(binary(), QByteArray::fromHex("00FFAA") + damaged + good, chunk, r));
        VERIFY(r.stats.validFrames == 1 && r.stats.checksumFailures == 1 && r.stats.rejectedFrames == 1 && r.remaining.isEmpty());
        VERIFY(r.stats.discardedBytes == quint64(3 + good.size()));
        auto badTail = good; badTail[10] = 0; VERIFY(run(binary(), badTail + good, chunk, r));
        VERIFY(r.stats.validFrames == 1 && r.stats.rejectedFrames == 1 && r.stats.checksumFailures == 0);
    }
    auto xorProfile = binary(); auto check = xorProfile.value("checksum").toObject(); check.insert("algorithm", "xor8"); xorProfile.insert("checksum", check);
    auto xorFrame = good; xorFrame[8] = char(0xfc); VERIFY(run(xorProfile, xorFrame, 1, r) && r.stats.validFrames == 1);
    auto mixedEndian = binary(); mixedEndian.remove("checksum");
    auto mixedFields = mixedEndian.value("channels").toArray(); auto bigField = mixedFields[1].toObject(); bigField.insert("byteOrder", "big"); mixedFields[1] = bigField; mixedEndian.insert("channels", mixedFields);
    VERIFY(run(mixedEndian, QByteArray::fromHex("AA55E60940533333000D0A"), 1, r) && r.stats.validFrames == 1);
    VERIFY(std::abs(r.converted[0][1] - 3.3) < 1e-6);
    auto crcProfile = binary(); crcProfile = frameChange(crcProfile, {{"sizeBytes", 15}, {"payloadBytes", 9}});
    crcProfile.insert("checksum", QJsonObject{{"algorithm", "crc16-modbus"}, {"range", QJsonObject{{"offset", 2}, {"length", 9}}}, {"field", QJsonObject{{"offset", 11}, {"type", "uint16"}, {"byteOrder", "little"}}}});
    crcProfile.insert("channels", QJsonArray{QJsonObject{{"id", "a"}, {"name", "A"}, {"offset", 0}, {"type", "uint8"}}});
    // Independent known answer: CRC16/MODBUS("123456789") == 0x4B37.
    VERIFY(run(crcProfile, QByteArray::fromHex("AA55") + "123456789" + QByteArray::fromHex("374B0D0A"), 1, r));
    VERIFY(r.stats.validFrames == 1 && r.raw[0][0] == 49);
    crcProfile.insert("checksum", QJsonObject{{"algorithm", "crc16-modbus"}, {"range", QJsonObject{{"offset", 2}, {"length", 9}}}, {"field", QJsonObject{{"offset", 11}, {"type", "uint16"}, {"byteOrder", "big"}}}});
    VERIFY(run(crcProfile, QByteArray::fromHex("AA55") + "123456789" + QByteArray::fromHex("4B370D0A"), 0, r) && r.stats.validFrames == 1);

    auto types = frameChange(binary(), {{"sizeBytes", 32}, {"payloadBytes", 28}}); types.remove("checksum"); types.insert("byteOrder", "big");
    const QStringList names{"int8", "uint8", "int16", "uint16", "int32", "uint32", "float32", "float64"};
    const QVector<int> offsets{0, 1, 2, 4, 6, 10, 14, 18}; QJsonArray fields;
    for (int i = 0; i < names.size(); ++i) fields.append(QJsonObject{{"id", QString::number(i)}, {"name", names[i]}, {"offset", offsets[i]}, {"type", names[i]}});
    types.insert("channels", fields);
    VERIFY(run(types, QByteArray::fromHex("AA5580FF8000FFFF80000000FFFFFFFF3FC00000400400000000000000000D0A"), 1, r));
    VERIFY(r.stats.validFrames == 1 && r.raw[0] == QVector<double>({-128, 255, -32768, 65535, -2147483648., 4294967295., 1.5, 2.5}));
    const auto bigEndianNaN = QByteArray::fromHex("AA5580FF8000FFFF80000000FFFFFFFF3FC000007FF800000000004200000D0A");
    VERIFY(run(types, bigEndianNaN, 1, r) && r.stats.validFrames == 1 && r.stats.outOfRangeValues == 0);
    VERIFY(r.converted[0][6] == 1.5 && std::isnan(r.raw[0][7]) && std::isnan(r.converted[0][7]));
    auto invalidNumeric = frameChange(binary(), {{"sizeBytes", 8}, {"payloadBytes", 4}}); invalidNumeric.remove("checksum");
    invalidNumeric.insert("channels", QJsonArray{QJsonObject{{"id", "a"}, {"name", "A"}, {"offset", 0}, {"type", "float32"}}});
    VERIFY(run(invalidNumeric, QByteArray::fromHex("AA550000807F0D0A") + QByteArray::fromHex("AA550000C03F0D0A"), 1, r));
    VERIFY(r.stats.validFrames == 1 && r.stats.rejectedFrames == 1 && r.raw[0][0] == 1.5);
    auto channels = invalidNumeric.value("channels").toArray(); auto field = channels[0].toObject(); field.insert("scale", 1e308); channels[0] = field; invalidNumeric.insert("channels", channels);
    VERIFY(run(invalidNumeric, QByteArray::fromHex("AA55000020400D0A"), 0, r) && r.stats.rejectedFrames == 1);
    auto restricted = binary(); channels = restricted.value("channels").toArray(); field = channels[0].toObject(); field.insert("max", 20); channels[0] = field; restricted.insert("channels", channels);
    VERIFY(run(restricted, good, 1, r) && r.stats.rejectedFrames == 1);
    auto conditional = binary(); conditional.insert("conditions", QJsonArray{QJsonObject{{"offset", 2}, {"equalsHex", "00"}}});
    VERIFY(run(conditional, good, 0, r) && r.stats.rejectedFrames == 1);
    auto noHead = frameChange(binary(), {{"headerHex", ""}}); VERIFY(run(noHead, damaged + good, 1, r) && r.stats.validFrames == 1);

    auto longText = frameChange(text(), {{"maxFrameBytes", 32}});
    VERIFY(run(longText, QByteArray(1000, 'x') + "\r\nDATA:1,2\r\n", 4096, r));
    VERIFY(r.stats.validFrames == 1 && r.stats.rejectedFrames == 1 && r.stats.discardedBytes == 1002);
    for (int chunk = 0; chunk < 12; ++chunk) {
        VERIFY(run(longText, QByteArray(1000, 'x') + "\r\nDATA:1,2\r\n", chunk, r));
        VERIFY(r.stats.validFrames == 1 && r.stats.rejectedFrames == 1 && r.stats.discardedBytes == 1002 && r.remaining.isEmpty());
    }
    auto utf = frameChange(text(), {{"prefix", "温度:"}, {"delimiter", "\t"}});
    VERIFY(run(utf, QStringLiteral("温度:1\t2\r\n").toUtf8(), 1, r) && r.stats.validFrames == 1);
    VERIFY(run(text(), QByteArray("DATA:1,") + char(0xc3) + "\r\n", 1, r) && r.stats.rejectedFrames == 1);
    auto whitespace = frameChange(text(), {{"delimiterMode", "whitespace"}}); auto f = whitespace.value("frame").toObject(); f.remove("delimiter"); whitespace.insert("frame", f);
    VERIFY(run(whitespace, "DATA:1  \t  2\r\n", 1, r) && r.stats.validFrames == 1);
    auto ascii = frameChange(utf, {{"encoding", "ascii"}}); VERIFY(run(ascii, QStringLiteral("温度:1\t2\r\n").toUtf8(), 1, r) && r.stats.rejectedFrames == 1);
    auto noTrim = frameChange(text(), {{"trimFields", false}}); VERIFY(run(noTrim, "DATA: 1,2\r\n", 0, r) && r.stats.rejectedFrames == 1);
    auto suffix = frameChange(text(), {{"suffix", ":END"}, {"terminatorHex", "0A"}});
    VERIFY(run(suffix, "DATA:1,2:END\n", 1, r) && r.stats.validFrames == 1);
    // High channel count uses the same fixed field ordering.
    auto many = text(); fields = {}; QByteArray values;
    for (int i = 0; i < 256; ++i) { fields.append(QJsonObject{{"id", QString::number(i)}, {"name", QString::number(i)}, {"column", i}}); if (i) values += ','; values += QByteArray::number(i); }
    many.insert("channels", fields); VERIFY(run(many, "DATA:" + values + "\r\n", 0, r) && r.raw[0].size() == 256 && r.raw[0][255] == 255);
    VERIFY(run(automatic, "DATA:" + values + "\r\n", 1, r) && r.raw[0].size() == 256 && r.raw[0][255] == 255);
    VERIFY(run(text(), "DATA:" + values + "\r\n", 0, r) && r.raw[0].size() == 256 && r.raw[0][255] == 255);
    VERIFY(run(text(), "DATA:" + values + ",256\r\nDATA:1,2,3,4\r\n", 1, r));
    VERIFY(r.stats.rejectedFrames == 1 && r.stats.validFrames == 1 && r.raw[0].size() == 4 && r.stats.lastError.contains("256"));
    auto autoWhitespace = whitespace; autoWhitespace.insert("channels", QJsonArray{});
    VERIFY(run(autoWhitespace, "DATA: 1  \t 2 3 4\r\n", 1, r) && r.raw[0] == QVector<double>({1, 2, 3, 4}));
    VERIFY(run(autoWhitespace, "DATA: \r\n", 1, r) && r.stats.rejectedFrames == 1);
    auto bounded = text(); auto boundedFields = bounded["channels"].toArray(); auto boundedField = boundedFields[0].toObject();
    boundedField.insert("min", 0); boundedField.insert("max", 10); boundedFields[0] = boundedField; bounded.insert("channels", boundedFields);
    bounded.insert("outOfRangePolicy", "nan-channel");
    for (int chunk : {0, 1, 7}) {
        VERIFY(run(bounded, "DATA:10,2,3,4\r\nDATA:2,5,6,7\r\nDATA:0.5,8\r\nDATA:5.5,9\r\n", chunk, r));
        VERIFY(r.stats.validFrames == 4 && r.stats.rejectedFrames == 0 && r.stats.outOfRangeValues == 1);
        VERIFY(r.raw[0][0] == 10 && std::isnan(r.converted[0][0]) && r.converted[0].mid(1) == QVector<double>({2, 3, 4}));
        VERIFY(r.converted[1][0] == 3 && r.converted[2][0] == 0 && r.converted[3][0] == 10); // Inclusive, post-conversion boundaries.
        VERIFY(run(bounded, "DATA:nan,2\r\nDATA:Inf,3\r\nDATA:1e999,4\r\nDATA:bad,5\r\nDATA:2\r\nDATA:2,5\r\n", chunk, r));
        VERIFY(r.stats.validFrames == 2 && r.stats.rejectedFrames == 4 && r.stats.outOfRangeValues == 0);
        VERIFY(std::isnan(r.raw[0][0]) && std::isnan(r.converted[0][0]) && r.converted[0][1] == 2);
    }
    auto overflow = bounded; boundedField.insert("scale", 1e308); boundedFields[0] = boundedField; overflow.insert("channels", boundedFields);
    VERIFY(run(overflow, "DATA:2,3\r\n", 1, r) && r.stats.validFrames == 0);
    auto binaryNaN = restricted; binaryNaN.insert("outOfRangePolicy", "nan-channel");
    const auto validBinary = QByteArray::fromHex("AA55E60933335340E80D0A");
    for (int chunk : {0, 1, 5}) {
        VERIFY(run(binaryNaN, validBinary, chunk, r) && r.stats.validFrames == 1 && r.stats.outOfRangeValues == 1);
        VERIFY(std::isnan(r.converted[0][0]) && std::abs(r.converted[0][1] - 3.3) < 1e-5 && r.raw[0][0] == 2534);
        auto corrupt = validBinary; corrupt[8] = 0;
        VERIFY(run(binaryNaN, corrupt, chunk, r) && r.stats.validFrames == 0 && r.stats.checksumFailures == 1);
    }
    const auto wireNaN = QByteArray::fromHex("AA55E6090000C07F2E0D0A");
    VERIFY(run(binaryNaN, wireNaN, 1, r) && r.stats.validFrames == 1 && r.stats.outOfRangeValues == 1);
    VERIFY(r.raw[0][0] == 2534 && std::isnan(r.raw[0][1]) && std::isnan(r.converted[0][0]) && std::isnan(r.converted[0][1]));
    auto badNaNChecksum = wireNaN; badNaNChecksum[8] = 0;
    VERIFY(run(binaryNaN, badNaNChecksum, 1, r) && r.stats.validFrames == 0 && r.stats.checksumFailures == 1);

    FireWaterParser fireWater;
    AMU4Parser amu;
    QVector<QVector<double>> builtinFrames;
    for (auto *parser : {static_cast<ProtocolParser *>(&fireWater), static_cast<ProtocolParser *>(&amu)})
        QObject::connect(parser, &ProtocolParser::channelsUpdated, [&](const QVector<double> &frame) { builtinFrames.append(frame); });
    const QByteArray fireWaterSample("DATA:NaN,-NAN,+nan\n");
    const QByteArray amuSample("0:-NaN\r\n1:2\r\n2:3\r\n");
    fireWater.processData(fireWaterSample.constData(), fireWaterSample.size());
    amu.processData(amuSample.constData(), amuSample.size());
    VERIFY(builtinFrames.size() == 2 && builtinFrames[0].size() == 3 && builtinFrames[1].size() == 3);
    for (double value : builtinFrames[0]) VERIFY(std::isnan(value));
    VERIFY(std::isnan(builtinFrames[1][0]) && builtinFrames[1][1] == 2 && builtinFrames[1][2] == 3);
    ProtocolProfile persisted; QString policyError;
    VERIFY(ProtocolProfileValidator::profile(bounded, persisted, policyError)); ProtocolSnapshot policySnapshot; policySnapshot.profiles = {persisted};
    const auto policyJson = ProtocolProfileValidator::encode(policySnapshot, false, policyError); ProtocolSnapshot policyRestored;
    VERIFY(ProtocolProfileValidator::decode(policyJson, false, policyRestored, policyError) && policyRestored.profiles[0].definition["outOfRangePolicy"] == QJsonValue("nan-channel"));
    bounded.insert("outOfRangePolicy", "silent-clamp"); VERIFY(!ProtocolProfileValidator::profile(bounded, persisted, policyError));
    std::cout << "Custom text/binary, checksum, resynchronization and chunk regressions passed.\n"; return 0;
}
