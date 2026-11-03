#include "CustomProtocolParser.h"
#include "ProtocolParserFactory.h"
#include "ProtocolSampleGenerator.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <memory>
#include <iostream>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Protocol diagnostics failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static QJsonObject fixed() {
    return {{"id", "5e08b00f-1d0d-43c5-8df9-b7f496f2b655"}, {"revision", 1}, {"name", "Diagnostic fixture"}, {"type", "binary"}, {"byteOrder", "little"},
        {"frame", QJsonObject{{"mode", "fixed"}, {"headerHex", "AA55"}, {"tailHex", "0D0A"}, {"sizeBytes", 11}, {"payloadOffset", 2}, {"payloadBytes", 6}, {"maxFrameBytes", 4096}}},
        {"checksum", QJsonObject{{"algorithm", "sum8"}, {"range", QJsonObject{{"offset", 0}, {"length", 8}}}, {"field", QJsonObject{{"offset", 8}, {"type", "uint8"}}}}},
        {"channels", QJsonArray{QJsonObject{{"id", "a"}, {"name", "Ch1"}, {"offset", 0}, {"type", "int16"}, {"scale", 1}, {"bias", 0}},
            QJsonObject{{"id", "b"}, {"name", "Ch2"}, {"offset", 2}, {"type", "float32"}, {"scale", 1}, {"bias", 0}}}}};
}
static QJsonObject text() {
    return {{"id", "5e08b00f-1d0d-43c5-8df9-b7f496f2b655"}, {"revision", 1}, {"name", "Text fixture"}, {"type", "text"},
        {"frame", QJsonObject{{"encoding", "utf8"}, {"terminatorHex", "0D0A"}, {"prefix", "DATA:"}, {"suffix", ""}, {"delimiterMode", "literal"}, {"delimiter", ","}, {"trimFields", true}, {"maxFrameBytes", 4096}}},
        {"channels", QJsonArray{QJsonObject{{"id", "a"}, {"column", 0}}, QJsonObject{{"id", "b"}, {"column", 1}}}}};
}
static QJsonObject dynamic() {
    return {{"id", "5e08b00f-1d0d-43c5-8df9-b7f496f2b655"}, {"revision", 1}, {"name", "Dynamic fixture"}, {"type", "binary"}, {"byteOrder", "little"},
        {"array", QJsonObject{{"type", "float32"}, {"minChannels", 1}, {"maxChannels", 256}}},
        {"frame", QJsonObject{{"mode", "length"}, {"headerHex", "AA55"}, {"tailHex", "0D0A"}, {"payloadOffset", 4}, {"payloadEndOffset", 2}, {"minFrameBytes", 10}, {"maxFrameBytes", 4096},
            {"lengthField", QJsonObject{{"offset", 2}, {"type", "uint16"}, {"byteOrder", "little"}, {"meaning", "payload-bytes"}, {"unitBytes", 1}, {"adjustment", 6}}}}}, {"channels", QJsonArray{}}};
}
static QJsonObject frameChange(QJsonObject d, const QString &key, const QJsonValue &value) {
    auto frame = d["frame"].toObject(); frame.insert(key, value); d.insert("frame", frame); return d;
}
struct Result {
    bool ready = false;
    QVector<quint64> offsets;
    QStringList errors, ending;
    ProtocolParseStats stats;
    QByteArray remaining;
};
static Result parse(const QJsonObject &definition, const QByteArray &bytes, int chunk) {
    Result result; ProtocolProfile profile; QString error;
    if (!ProtocolProfileValidator::profile(definition, profile, error)) { std::cerr << error.toStdString() << '\n'; return result; }
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error)));
    if (!parser) return result;
    QObject::connect(parser.get(), &CustomProtocolParser::frameRejected, [&](quint64 offset, const QString &message) { result.offsets.append(offset); result.errors.append(message); });
    quint32 random = 23;
    for (int pos = 0; pos < bytes.size();) {
        random = random * 1664525u + 1013904223u;
        const int count = qMin(chunk ? chunk : int(random % 17) + 1, bytes.size() - pos);
        result.remaining += bytes.mid(pos, count); pos += count;
        const int consumed = parser->processData(result.remaining.constData(), result.remaining.size());
        if (consumed < 0 || consumed > result.remaining.size()) return result;
        result.remaining.remove(0, consumed);
    }
    result.stats = parser->stats(); result.ending = parser->testInputDiagnostics(bytes, result.remaining);
    // Offline inspection must not modify counters, pending bytes or grouping.
    result.ready = parser->stats().consumedBytes == result.stats.consumedBytes
        && parser->stats().rejectedFrames == result.stats.rejectedFrames && parser->stats().validFrames == result.stats.validFrames;
    return result;
}
static QByteArray sample(const QJsonObject &definition, int channels = 0) {
    ProtocolProfile profile; QString error; QByteArray bytes;
    if (!ProtocolProfileValidator::profile(definition, profile, error) || !ProtocolSampleGenerator::generate(profile, bytes, error, channels)) std::cerr << error.toStdString() << '\n';
    return bytes;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); const auto good = sample(fixed()); VERIFY(good.size() == 11);
    for (int chunk : {1, 2, 7, 0, 4096}) {
        auto r = parse(fixed(), good.repeated(2), chunk); VERIFY(r.ready && r.stats.validFrames == 2 && r.errors.isEmpty() && r.ending.isEmpty());
        auto bad = good; bad[8] = char(quint8(bad[8]) ^ 1);
        r = parse(fixed(), QByteArray::fromHex("00FF") + bad + good, chunk);
        VERIFY(r.ready && r.stats.validFrames == 1 && r.stats.checksumFailures == 1 && r.offsets == QVector<quint64>({10}));
        VERIFY(r.errors[0].contains("数据校验失败") && r.errors[0].contains("SUM8") && r.errors[0].contains("预期") && r.errors[0].contains("实际") && r.errors[0].contains("[0, 8)"));
        bad = good; bad[10] = 0;
        r = parse(fixed(), good + bad, chunk); VERIFY(r.ready && r.stats.validFrames == 1 && r.offsets[0] == 21 && r.errors[0].contains("帧尾不匹配") && r.errors[0].contains("0D 0A"));
        auto conditional = fixed(); conditional.insert("conditions", QJsonArray{QJsonObject{{"offset", 2}, {"equalsHex", "02"}}});
        r = parse(conditional, good, chunk); VERIFY(r.ready && r.offsets[0] == 2 && r.errors[0].contains("固定字节条件") && r.errors[0].contains("02"));
        auto unchecked = fixed(); unchecked.remove("checksum"); bad = good; bad.replace(4, 4, QByteArray::fromHex("0000807F"));
        r = parse(unchecked, bad, chunk); VERIFY(r.ready && r.offsets[0] == 4 && r.errors[0].contains("通道 2") && r.errors[0].contains("无穷大"));
        auto limited = fixed(); auto fields = limited["channels"].toArray(); auto field = fields[0].toObject(); field.insert("max", 0); fields[0] = field; limited.insert("channels", fields);
        limited.insert("outOfRangePolicy", "reject-frame");
        r = parse(limited, good, chunk); VERIFY(r.ready && r.offsets[0] == 2 && r.errors[0].contains("超出配置范围"));
        limited.insert("outOfRangePolicy", "nan-channel"); r = parse(limited, good, chunk);
        VERIFY(r.ready && r.stats.validFrames == 1 && r.stats.outOfRangeValues == 1 && r.errors.isEmpty() && r.ending.join('\n').contains("标记为 NaN"));
        r = parse(fixed(), good.left(8), chunk); VERIFY(r.ready && r.errors.isEmpty() && r.ending.join('\n').contains("应有 11 字节，实际仅 8，尚缺 3"));
        r = parse(fixed(), good + good.left(8), chunk); VERIFY(r.ready && r.stats.validFrames == 1 && r.ending.join('\n').contains("字节偏移 19") && r.ending.join('\n').contains("从字节 11 起"));
        r = parse(fixed(), QByteArray::fromHex("AA"), chunk); VERIFY(r.ready && r.ending.join('\n').contains("帧头不完整"));
        bad = good; bad[1] = char(0x56); r = parse(fixed(), bad, chunk);
        VERIFY(r.ready && r.stats.validFrames == 0 && r.ending.join('\n').contains("字节偏移 1") && r.ending.join('\n').contains("未找到帧头 AA 55"));
        auto onlyTail = frameChange(fixed(), "headerHex", ""); bad = sample(onlyTail); VERIFY(bad.size() == 11); bad[10] = 0;
        r = parse(onlyTail, bad, chunk); VERIFY(r.ready && r.ending.join('\n').contains("未找到帧尾"));
        r = parse(onlyTail, QByteArray::fromHex("0D0A"), chunk); VERIFY(r.ready && r.ending.join('\n').contains("帧尾标记前字节不足"));
        auto onlyHeader = frameChange(unchecked, "tailHex", ""); const auto headSample = sample(onlyHeader);
        r = parse(onlyHeader, headSample, chunk); VERIFY(r.ready && r.stats.validFrames == 1 && r.ending.isEmpty());

        r = parse(text(), "DATA:1;2\r\n", chunk); VERIFY(r.ready && r.offsets[0] == 5 && r.errors[0].contains("可能是分隔符不一致"));
        r = parse(text(), "DATA:1,\r\n", chunk); VERIFY(r.ready && r.offsets[0] == 7 && r.errors[0].contains("第 2 列"));
        r = parse(text(), "DATA:1\r\n", chunk); VERIFY(r.ready && r.offsets[0] == 6 && r.errors[0].contains("缺少第 2 列"));
        r = parse(text(), "DOTA:1,2\r\n", chunk); VERIFY(r.ready && r.offsets[0] == 1 && r.errors[0].contains("固定前缀不匹配"));
        r = parse(frameChange(text(), "suffix", "END"), "DATA:1,2BAD\r\n", chunk); VERIFY(r.ready && r.offsets[0] == 8 && r.errors[0].contains("固定后缀不匹配"));
        r = parse(text(), "DATA:1,2\n", chunk); VERIFY(r.ready && r.ending.join('\n').contains("CRLF") && r.ending.join('\n').contains("结束符缺失"));
        r = parse(text(), QByteArray("DATA:1,") + char(0xff) + "\r\n", chunk); VERIFY(r.ready && r.offsets[0] == 7 && r.errors[0].contains("UTF-8"));
        const auto unicode = frameChange(text(), "prefix", "温度:");
        r = parse(unicode, QString("温度:1,bad\r\n").toUtf8(), chunk); VERIFY(r.ready && r.offsets[0] == 9);
        r = parse(unicode, QByteArray::fromHex("EFBBBF") + QString("温度:1,bad\r\n").toUtf8(), chunk); VERIFY(r.ready && r.offsets[0] == 12);
        r = parse(frameChange(text(), "encoding", "ascii"), QString("DATA:1,温\r\n").toUtf8(), chunk); VERIFY(r.ready && r.offsets[0] == 7 && r.errors[0].contains("ASCII"));
        r = parse(text(), "DATA:1,2\r\nDATA:3,4\r\n", chunk); VERIFY(r.ready && r.stats.validFrames == 2 && r.errors.isEmpty() && r.ending.isEmpty());

        const auto array = dynamic(); const auto arraySample = sample(array, 3); VERIFY(arraySample.size() == 18);
        r = parse(array, arraySample, chunk); VERIFY(r.ready && r.stats.validFrames == 1 && r.ending.isEmpty());
        bad = arraySample; bad.replace(2, 2, QByteArray::fromHex("FFFF")); r = parse(array, bad, chunk);
        VERIFY(r.ready && r.offsets[0] == 2 && r.errors[0].contains("长度字段错误") && r.errors[0].contains("n=65535") && r.errors[0].contains("65541"));
        bad = arraySample; bad.replace(2, 2, QByteArray::fromHex("0500")); r = parse(array, bad, chunk);
        VERIFY(r.ready && r.offsets[0] == 2 && r.errors[0].contains("不能完整组成 4 字节数值") && r.errors[0].contains("余 1 字节"));
        r = parse(array, arraySample.left(9), chunk); VERIFY(r.ready && r.ending.join('\n').contains("应有 18 字节，实际仅 9，尚缺 9") && r.ending.join('\n').contains("n=12"));
        r = parse(array, arraySample.left(3), chunk); VERIFY(r.ready && r.ending.join('\n').contains("帧头后字段不完整"));
        auto headerCheck = frameChange(array, "payloadOffset", 5); headerCheck = frameChange(headerCheck, "minFrameBytes", 11);
        auto f = headerCheck["frame"].toObject(), len = f["lengthField"].toObject(); len.insert("adjustment", 7); f.insert("lengthField", len); headerCheck.insert("frame", f);
        headerCheck.insert("headerChecksum", QJsonObject{{"algorithm", "xor8"}, {"range", QJsonObject{{"offset", 0}, {"length", 4}}}, {"field", QJsonObject{{"offset", 4}, {"type", "uint8"}}}});
        bad = sample(headerCheck, 3); VERIFY(bad.size() == 19); bad[4] = char(quint8(bad[4]) ^ 1);
        r = parse(headerCheck, bad, chunk); VERIFY(r.ready && r.offsets[0] == 4 && r.errors[0].contains("帧头校验失败") && r.errors[0].contains("XOR8"));
        r = parse(ProtocolProfileValidator::keyValueTemplate(), "0:1\r\n1:2\r\n", chunk);
        VERIFY(r.ready && r.stats.validFrames == 0 && r.errors.isEmpty() && r.ending.join('\n').contains("仍缺键 “2”"));
        r = parse(ProtocolProfileValidator::keyValueTemplate(), "unknown:1\r\n", chunk);
        VERIFY(r.ready && r.ending.join('\n').contains("忽略未知键"));
    }
    // Byte-level checksum offsets also work for multi-byte big-endian fields.
    auto crc = frameChange(fixed(), "sizeBytes", 12); crc.insert("checksum", QJsonObject{{"algorithm", "crc16-modbus"},
        {"range", QJsonObject{{"offset", 0}, {"length", 8}}}, {"field", QJsonObject{{"offset", 8}, {"type", "uint16"}, {"byteOrder", "big"}}}});
    auto bytes = sample(crc); VERIFY(bytes.size() == 12); bytes[9] = char(quint8(bytes[9]) ^ 1);
    const auto r = parse(crc, bytes, 1); VERIFY(r.ready && r.offsets[0] == 9 && r.errors[0].contains("大端"));
    ProtocolProfile profile; QString error; VERIFY(ProtocolProfileValidator::profile(fixed(), profile, error));
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error)));
    bytes = good.left(7); VERIFY(parser->processData(bytes.constData(), bytes.size()) == 0);
    VERIFY(!parser->testInputDiagnostics(bytes, bytes).isEmpty()); bytes += good.mid(7);
    VERIFY(parser->processData(bytes.constData(), bytes.size()) == good.size() && parser->stats().validFrames == 1 && parser->stats().rejectedFrames == 0);
    std::cout << "Diagnostic offsets, expected/actual bytes, framing, checksums, text, dynamic arrays and offline-only completion checks passed.\n";
    return 0;
}
