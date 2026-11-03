#include "ProtocolProfileStore.h"
#include "ProtocolParserFactory.h"
#include "CustomProtocolParser.h"
#include "ProtocolSampleGenerator.h"
#include "ProtocolChecksum.h"
#include "csvrecordingwriter.h"
#include "wlrrecordingwriter.h"
#include "recordingchannelnames.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QBuffer>
#include <QJsonDocument>
#include <QFile>
#include <memory>
#include <iostream>
#include <cmath>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Dynamic array failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static QJsonObject definition(const QString &type = "float32", const QString &meaning = "payload-bytes", bool big = false, int headerCrc = 0, int bodyCrc = 0) {
    const int width = ProtocolProfileValidator::typeBytes(type), offset = 4 + headerCrc, end = 2 + bodyCrc;
    QJsonObject d{{"id", "fb6a0697-4d65-4857-8882-bf9be66c5fbb"}, {"revision", 1}, {"name", "Dynamic array"}, {"type", "binary"}, {"byteOrder", big ? "big" : "little"},
        {"array", QJsonObject{{"type", type}, {"minChannels", 1}, {"maxChannels", 256}}},
        {"frame", QJsonObject{{"mode", "length"}, {"headerHex", "AA55"}, {"tailHex", "0D0A"}, {"payloadOffset", offset}, {"payloadEndOffset", end}, {"minFrameBytes", offset + width + end}, {"maxFrameBytes", 4096},
            {"lengthField", QJsonObject{{"offset", 2}, {"type", "uint16"}, {"byteOrder", big ? "big" : "little"}, {"meaning", meaning}, {"unitBytes", meaning == "elements" ? width : 1}, {"adjustment", meaning == "frame-bytes" ? 0 : offset + end}}}}},
        {"channels", QJsonArray{}}};
    if (headerCrc) d.insert("headerChecksum", QJsonObject{{"algorithm", "crc16-modbus"}, {"range", QJsonObject{{"offset", 0}, {"length", 4}}}, {"field", QJsonObject{{"offset", 4}, {"type", "uint16"}, {"byteOrder", big ? "big" : "little"}}}});
    if (bodyCrc) d.insert("checksum", QJsonObject{{"algorithm", "crc16-modbus"}, {"range", QJsonObject{{"offset", 0}, {"endOffset", end}}}, {"field", QJsonObject{{"from", "end"}, {"offset", end}, {"type", "uint16"}, {"byteOrder", big ? "big" : "little"}}}});
    return d;
}
struct Result { QVector<QVector<double>> frames; ProtocolParseStats stats; QByteArray remaining; };
static Result parse(const ProtocolProfile &profile, const QByteArray &input, int chunk) {
    Result r; QString error;
    std::unique_ptr<CustomProtocolParser> p(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error)));
    if (!p) { r.stats.lastError = error; return r; }
    QObject::connect(p.get(), &CustomProtocolParser::channelsUpdated, [&r](const QVector<double> &v) { r.frames.append(v); });
    quint32 random = 93;
    for (int offset = 0; offset < input.size();) {
        random = random * 1664525u + 1013904223u;
        const int n = qMin(chunk ? chunk : int(random % 31) + 1, input.size() - offset);
        r.remaining += input.mid(offset, n); offset += n;
        const int consumed = p->processData(r.remaining.constData(), r.remaining.size());
        if (consumed < 0 || consumed > r.remaining.size()) { r.stats.lastError = "invalid consumption"; return r; }
        r.remaining.remove(0, consumed);
    }
    r.stats = p->stats(); return r;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QString error; QTemporaryDir dir; VERIFY(dir.isValid());
    for (const QString type : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "float32", "float64"})
        for (const QString meaning : {"payload-bytes", "frame-bytes", "elements"}) for (bool big : {false, true}) {
            ProtocolProfile p; VERIFY(ProtocolProfileValidator::profile(definition(type, meaning, big, 2, 2), p, error));
            QByteArray all;
            for (int count : {1, 2, 17, 256, 3}) { QByteArray frame; VERIFY(ProtocolSampleGenerator::generate(p, frame, error, count)); all += frame; }
            for (int chunk : {1, 0, 4096}) {
                const auto r = parse(p, all, chunk); VERIFY(r.remaining.isEmpty() && r.frames.size() == 5 && r.stats.rejectedFrames == 0);
                const QVector<int> counts{1, 2, 17, 256, 3};
                for (int i = 0; i < counts.size(); ++i) { VERIFY(r.frames[i].size() == counts[i]); for (int ch = 0; ch < counts[i]; ++ch) {
                    const double expected = type == "int8" ? qMin(ch + 1, 127) : type == "uint8" ? qMin(ch + 1, 255) : ch + 1;
                    VERIFY(r.frames[i][ch] == expected);
                } }
            }
        }
    ProtocolProfile p; VERIFY(ProtocolProfileValidator::profile(definition(), p, error));
    auto d = p.definition;
    d.insert("channels", QJsonArray{QJsonObject{{"id", "temperature"}, {"index", 0}, {"name", "温度"}, {"scale", .5}, {"unit", "°C"}},
        QJsonObject{{"id", "fourth"}, {"index", 3}, {"name", "电压"}, {"max", 3}}});
    d.insert("outOfRangePolicy", "nan-channel"); VERIFY(ProtocolProfileValidator::profile(d, p, error));
    // Build without range limits so channel 4 deliberately violates its rule.
    ProtocolProfile raw; VERIFY(ProtocolProfileValidator::profile(definition(), raw, error)); QByteArray packet;
    VERIFY(ProtocolSampleGenerator::generate(raw, packet, error, 4));
    auto r = parse(p, packet, 1); VERIFY(r.frames.size() == 1 && r.frames[0][0] == .5 && std::isnan(r.frames[0][3]) && r.stats.outOfRangeValues == 1);
    VERIFY(p.outputChannelNames(4) == QStringList({"温度", "Ch2", "Ch3", "电压"}));
    const auto metadata = p.outputChannels(4); VERIFY(p.outputChannels(2)[1] == metadata[1]);
    VERIFY(ProtocolSampleGenerator::generate(raw, packet, error, 1)); r = parse(p, packet, 0); VERIFY(r.frames.size() == 1 && r.frames[0].size() == 1); // An absent override is not an absent required channel.
    ProtocolSnapshot snapshot; snapshot.profiles.append(p); snapshot.shared.kind = "custom"; snapshot.shared.id = p.id;
    auto saved = ProtocolProfileValidator::encode(snapshot, true, error); ProtocolSnapshot restored;
    VERIFY(!saved.isEmpty() && ProtocolProfileValidator::decode(saved, true, restored, error) && restored.profiles[0].definition == ProtocolProfileValidator::standardizeBinary(p.definition, true));
    ProtocolProfileStore store(dir.filePath("protocols.json")); VERIFY(store.load(error) && store.commit(snapshot, error));
    ProtocolProfileStore reload(store.path()); VERIFY(reload.load(error) && reload.snapshot().profiles[0].definition == ProtocolProfileValidator::standardizeBinary(p.definition, true));
    // Reject invalid layouts before any parser is created.
    auto invalid = definition(); auto f = invalid["frame"].toObject(); f.insert("payloadEndOffset", 1); invalid.insert("frame", f); VERIFY(!ProtocolProfileValidator::profile(invalid, p, error));
    invalid = definition(); f = invalid["frame"].toObject(); auto len = f["lengthField"].toObject(); len.insert("adjustment", 99); f.insert("lengthField", len); invalid.insert("frame", f); VERIFY(!ProtocolProfileValidator::profile(invalid, p, error));
    invalid = definition(); invalid.insert("channels", QJsonArray{QJsonObject{{"id", "a"}, {"name", "A"}, {"index", 0}}, QJsonObject{{"id", "b"}, {"name", "B"}, {"index", 0}}}); VERIFY(!ProtocolProfileValidator::profile(invalid, p, error));
    VERIFY(ProtocolProfileValidator::profile(definition(), p, error)); QByteArray good; VERIFY(ProtocolSampleGenerator::generate(p, good, error, 2));
    auto partial = parse(p, good.left(good.size() - 1), 1); VERIFY(partial.frames.isEmpty() && partial.remaining == good.left(good.size() - 1));
    // Header and CRLF patterns inside a valid payload are ordinary bytes.
    const auto embedded = QByteArray::fromHex("AA5508000D0AAA3F55AA00400D0A"); r = parse(p, embedded, 1); VERIFY(r.frames.size() == 1 && r.frames[0].size() == 2);
    const auto unaligned = QByteArray::fromHex("AA5505000000803F010D0A");
    r = parse(p, unaligned + good, 1); VERIFY(r.frames.size() == 1 && r.stats.rejectedFrames > 0);
    // Oversized counts/lengths are errors, not successful truncated frames.
    QByteArray tooMany = QByteArray::fromHex("AA550404") + QByteArray(257 * 4, 0) + QByteArray::fromHex("0D0A");
    r = parse(p, tooMany + good, 0); VERIFY(r.frames.size() == 1 && r.stats.rejectedFrames > 0);
    r = parse(p, QByteArray::fromHex("AA55FFFF") + good, 1); VERIFY(r.frames.size() == 1 && r.stats.rejectedFrames > 0);
    VERIFY(ProtocolProfileValidator::profile(definition("float32", "payload-bytes", false, 2, 2), p, error)); VERIFY(ProtocolSampleGenerator::generate(p, good, error, 3));
    auto corrupt = good; corrupt[2] = char(0x00); corrupt[3] = char(0x0C); // plausible but incorrect length, bad header CRC.
    r = parse(p, corrupt.left(6) + good, 1); VERIFY(r.frames.size() == 1 && r.stats.checksumFailures > 0 && r.remaining.isEmpty());
    corrupt = good; corrupt[6] = char(corrupt[6] ^ 1); r = parse(p, corrupt + good, 0); VERIFY(r.frames.size() == 1 && r.stats.checksumFailures > 0);
    // Both recording modes preserve all transition frames. CSV makes safe
    // parts, then exports a union-width file with NaN rather than invented data.
    CsvRecordingWriter csv; WlrRecordingWriter wlr; const QStringList names{"温度", "电压", "Ch3"};
    VERIFY(csv.start(dir.filePath("dynamic.csv"), error, 250, names, true) && wlr.start(dir.filePath("dynamic.wlr"), error, 250, names, true));
    for (const QVector<double> values : {QVector<double>{1, 2}, QVector<double>{3, 4, 5}, QVector<double>{6}}) { VERIFY(csv.append(values, error, 1700000000000000000LL) && wlr.append(values, error, 1700000000000000000LL)); }
    VERIFY(csv.finish(error) && wlr.finish(error) && csv.completedFiles().size() == 3);
    QByteArray csvBytes, wlrBytes; QBuffer csvOut(&csvBytes), wlrOut(&wlrBytes); csvOut.open(QIODevice::WriteOnly); wlrOut.open(QIODevice::WriteOnly);
    VERIFY(CsvRecordingWriter::exportToCsv(csv.completedFiles(), &csvOut, error) && WlrRecordingWriter::exportToCsv(wlr.completedFiles(), &wlrOut, error));
    VERIFY(csvBytes == wlrBytes && csvBytes.count('\n') == 4 && csvBytes.count(RecordingChannels::csvUtf8Bom()) == 1);
    VERIFY(csvBytes.contains(",0,1.000000,2.000000,nan\n") && csvBytes.contains(",2,6.000000,nan,nan\n"));
    QVector<int> counts; VERIFY(WlrRecordingWriter::visitFiles(wlr.completedFiles(), [&](qint64, const QVector<double> &v, qint64) { counts.append(v.size()); return true; }, error)); VERIFY(counts == QVector<int>({2, 3, 1}));
    std::cout << "Dynamic arrays: all numeric types/endian/length meanings, chunking, dual CRC, metadata, persistence and CSV/WLR width transitions passed.\n";
    return 0;
}
