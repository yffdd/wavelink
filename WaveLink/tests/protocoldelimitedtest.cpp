#include "ProtocolProfileStore.h"
#include "ProtocolParserFactory.h"
#include "CustomProtocolParser.h"
#include "ProtocolSampleGenerator.h"
#include "ProtocolChecksum.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <memory>
#include <iostream>
#include <cmath>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Delimited array failure at " << __LINE__ << ": " #c << "\n" << error.toStdString() << "\n"; return 1; } } while (0)
static QJsonObject definition(const QString &head = {}, const QString &tail = "0000807F", const QString &type = "float32", bool big = false, const QString &algorithm = "none") {
    const int h = QByteArray::fromHex(head.toLatin1()).size(), t = QByteArray::fromHex(tail.toLatin1()).size();
    const int width = ProtocolProfileValidator::typeBytes(type), end = t + ProtocolChecksum::width(algorithm);
    QJsonObject d{{"id", "1ad1e993-0eae-42db-8f4d-a8fde196e5f8"}, {"revision", 1}, {"name", "首尾分隔"}, {"type", "binary"}, {"byteOrder", big ? "big" : "little"},
        {"outOfRangePolicy", "nan-channel"}, {"autoFrameLayout", true},
        {"array", QJsonObject{{"type", type}, {"minChannels", 1}, {"maxChannels", 256}}},
        {"frame", QJsonObject{{"mode", "delimiter"}, {"headerHex", head}, {"tailHex", tail}, {"payloadOffset", h}, {"payloadEndOffset", end}, {"minFrameBytes", h + width + end}, {"maxFrameBytes", 4096}}},
        {"checksum", QJsonObject{{"algorithm", algorithm}}}, {"channels", QJsonArray{}}};
    return ProtocolProfileValidator::standardizeBinary(d, false);
}
struct Result { QVector<QVector<double>> frames, raw; ProtocolParseStats stats; QByteArray remaining; QStringList diagnostics; int largestBuffer = 0; };
static Result parse(const ProtocolProfile &profile, const QByteArray &input, int chunk) {
    Result r; QString error;
    std::unique_ptr<CustomProtocolParser> p(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error)));
    if (!p) { r.stats.lastError = error; return r; }
    QObject::connect(p.get(), &CustomProtocolParser::frameDecoded, [&](const QVector<double> &raw, const QVector<double> &v) { r.frames.append(v); r.raw.append(raw); });
    quint32 random = 93;
    for (int offset = 0; offset < input.size();) {
        random = random * 1664525u + 1013904223u;
        const int n = qMin(chunk ? chunk : int(random % 31) + 1, input.size() - offset);
        r.remaining += input.mid(offset, n); offset += n;
        const int consumed = p->processData(r.remaining.constData(), r.remaining.size());
        if (consumed < 0 || consumed > r.remaining.size()) { r.stats.lastError = "invalid consumption"; return r; }
        r.remaining.remove(0, consumed); r.largestBuffer = qMax(r.largestBuffer, r.remaining.size());
    }
    r.stats = p->stats(); r.diagnostics = p->testInputDiagnostics(input, r.remaining); return r;
}
// Generator appends the next header for an immediately testable sample.
static QByteArray wireFrame(const ProtocolProfile &p, int count, QString &error) {
    QByteArray b; if (!ProtocolSampleGenerator::generate(p, b, error, count)) return {};
    const auto f = p.definition.value("frame").toObject();
    if (f.value("tailHex").toString().isEmpty()) b.chop(QByteArray::fromHex(f.value("headerHex").toString().toLatin1()).size());
    return b;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QString error; QTemporaryDir dir; VERIFY(dir.isValid());
    ProtocolProfile p;
    for (const auto &boundaries : {qMakePair(QString(), QString("DEADBEEFCAFEBABE")), qMakePair(QString("AA55"), QString()), qMakePair(QString("AA55"), QString("0D0A"))})
        for (const QString type : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "float32", "float64"})
            for (bool big : {false, true}) for (const QString crc : {"none", "sum8", "xor8", "crc8-smbus", "crc16-modbus", "crc16-ccitt-false", "crc16-xmodem", "crc32-iso-hdlc"}) {
                // A JustFloat tail is endian-specific; the bytes themselves
                // remain user configured, independent of numeric endian.
                VERIFY(ProtocolProfileValidator::profile(definition(boundaries.first, boundaries.second, type, big, crc), p, error));
                QByteArray all; const QVector<int> counts{1, 2, 10, 17, 256, 3};
                for (int count : counts) { auto b = wireFrame(p, count, error); if (b.isEmpty()) error += QString("\ntype=%1 big=%2 crc=%3 count=%4 head=%5 tail=%6").arg(type).arg(big).arg(crc).arg(count).arg(boundaries.first, boundaries.second); VERIFY(!b.isEmpty()); all += b; }
                const auto lookahead = boundaries.second.isEmpty() ? QByteArray::fromHex(boundaries.first.toLatin1()) : QByteArray(); all += lookahead;
                for (int chunk : {1, 2, 3, 0, 4096}) {
                    const auto r = parse(p, all, chunk);
                    if (r.frames.size() != counts.size()) error = r.stats.lastError + " type=" + type + " crc=" + crc;
                    VERIFY(r.frames.size() == counts.size() && r.remaining == lookahead && r.stats.rejectedFrames == 0 && r.diagnostics.isEmpty());
                    for (int i = 0; i < counts.size(); ++i) for (int ch = 0; ch < counts[i]; ++ch) {
                        const double expected = type == "int8" ? qMin(ch + 1, 127) : type == "uint8" ? qMin(ch + 1, 255) : ch + 1;
                        VERIFY(r.frames[i].size() == counts[i] && r.frames[i][ch] == expected);
                    }
                }
            }
    VERIFY(ProtocolProfileValidator::profile(definition(), p, error));
    const auto ten = wireFrame(p, 10, error); VERIFY(ten == QByteArray::fromHex("0000803F0000004000004040000080400000A0400000C0400000E0400000004100001041000020410000807F"));
    const auto good = wireFrame(p, 3, error); VERIFY(!good.isEmpty());
    // Split tails that contain the head, identical head/tail, and markers
    // with self-overlapping prefixes must not depend on receive chunk size.
    for (const auto &markers : {qMakePair(QString("AA55"), QString("AA550D0A")), qMakePair(QString("AA55"), QString("AA55")), qMakePair(QString("ABAB"), QString("ABABAB0A"))}) {
        ProtocolProfile overlap; VERIFY(ProtocolProfileValidator::profile(definition(markers.first, markers.second), overlap, error));
        const auto packet = wireFrame(overlap, 3, error); VERIFY(!packet.isEmpty());
        for (int chunk : {1, 2, 3, 0, 4096}) { const auto r = parse(overlap, packet + packet, chunk); VERIFY(r.frames.size() == 2 && r.stats.rejectedFrames == 0 && r.remaining.isEmpty()); }
    }
    // A real checksum/payload combination can also collide with a marker;
    // generation must reject it rather than pretending it is a safe sample.
    ProtocolProfile collision; VERIFY(ProtocolProfileValidator::profile(definition({}, "0000807F", "int32", false, "crc16-modbus"), collision, error));
    QByteArray collisionSample; VERIFY(!ProtocolSampleGenerator::generate(collision, collisionSample, error, 2) && error.contains("分隔标记"));
    for (int offset = 0; offset < ten.size(); ++offset) for (int chunk : {1, 0, 4096}) {
        const auto r = parse(p, ten.mid(offset) + good + good + good, chunk);
        VERIFY(r.frames.size() >= 2 && r.frames[r.frames.size() - 2] == QVector<double>({1, 2, 3}) && r.frames.last() == QVector<double>({1, 2, 3}));
    }
    // Never shave bytes off a misaligned fragment to invent valid channels.
    for (const auto &bad : {QByteArray(1, '\x01') + ten, QByteArray::fromHex("0000807F"), QByteArray(257 * 4, 0) + QByteArray::fromHex("0000807F"), QByteArray::fromHex("000080FF0000807F")}) {
        const auto r = parse(p, bad + good, 1); VERIFY(r.frames.size() == 1 && r.stats.rejectedFrames > 0);
    }
    const auto wireNaN = parse(p, QByteArray::fromHex("0000C07F0000807F") + good, 1);
    VERIFY(wireNaN.frames.size() == 2 && wireNaN.stats.rejectedFrames == 0 && wireNaN.stats.outOfRangeValues == 0);
    VERIFY(wireNaN.frames[0].size() == 1 && std::isnan(wireNaN.raw[0][0]) && std::isnan(wireNaN.frames[0][0]));
    VERIFY(wireNaN.frames[1] == QVector<double>({1, 2, 3}));
    // Value scaling, NaN policy, notes, raw values and metadata are shared.
    auto d = definition(); d.insert("channels", QJsonArray{QJsonObject{{"id", "temperature"}, {"index", 0}, {"name", "温度"}, {"unit", "°C"}, {"note", "保留备注"}, {"scale", 2}, {"bias", 1}},
        QJsonObject{{"id", "limited"}, {"index", 1}, {"name", "Ch2"}, {"max", 1}}});
    VERIFY(ProtocolProfileValidator::profile(d, p, error)); auto r = parse(p, good, 1);
    VERIFY(r.frames.size() == 1 && r.frames[0][0] == 3 && std::isnan(r.frames[0][1]) && r.frames[0][2] == 3 && r.raw[0] == QVector<double>({1, 2, 3}));
    VERIFY(p.outputChannelNames(3) == QStringList({"温度", "Ch2", "Ch3"}));
    ProtocolSnapshot snapshot; snapshot.profiles.append(p); snapshot.shared.kind = "custom"; snapshot.shared.id = p.id;
    for (bool selection : {false, true}) { ProtocolSnapshot loaded; auto b = ProtocolProfileValidator::encode(snapshot, selection, error);
        VERIFY(!b.isEmpty() && ProtocolProfileValidator::decode(b, selection, loaded, error) && loaded.profiles[0].definition == p.definition); }
    ProtocolProfileStore store(dir.filePath("protocols.json")); VERIFY(store.load(error) && store.commit(snapshot, error));
    ProtocolProfileStore reload(store.path()); VERIFY(reload.load(error) && reload.snapshot().profiles[0].definition == p.definition);
    // Checksum is immediately before the tail, or at frame end without tail.
    VERIFY(ProtocolProfileValidator::profile(definition("AA55", "0D0A", "float32", false, "crc16-modbus"), p, error));
    const auto checked = wireFrame(p, 3, error); auto corrupt = checked; corrupt[3] = char(corrupt[3] ^ 1);
    r = parse(p, corrupt + checked, 1); VERIFY(r.frames.size() == 1 && r.stats.checksumFailures == 1);
    // Missing tail, a fresh header, then complete frame: do not merge frames.
    r = parse(p, checked.left(checked.size() - 2) + checked, 1); VERIFY(r.frames.size() == 1 && r.stats.rejectedFrames == 1 && r.stats.lastError.contains("帧尾"));
    r = parse(p, checked.left(checked.size() - 1), 1); VERIFY(r.frames.isEmpty() && r.diagnostics.join(" ").contains("帧尾缺失"));
    for (const QString tail : {QString(), QString("0D0A")}) {
        VERIFY(ProtocolProfileValidator::profile(definition("AA55", tail, "float32", false, "crc16-modbus"), p, error));
        const auto first = wireFrame(p, 10, error), next = wireFrame(p, 3, error);
        const auto lookahead = tail.isEmpty() ? QByteArray::fromHex("AA55") : QByteArray();
        for (int offset = 0; offset < first.size(); ++offset) for (int chunk : {1, 0, 4096}) {
            const auto r = parse(p, first.mid(offset) + next + next + next + lookahead, chunk);
            VERIFY(r.frames.size() >= 3 && r.frames.last() == QVector<double>({1, 2, 3}) && r.remaining == lookahead);
        }
        const auto r = parse(p, QByteArray::fromHex("AA55") + QByteArray(8000, '\x01') + next + lookahead, 1);
        VERIFY(r.frames.size() == 1 && r.frames[0] == QVector<double>({1, 2, 3}) && r.stats.rejectedFrames > 0);
    }
    // Optional reserved bytes and fixed conditions stay outside the array,
    // but remain inside the unified checksum range.
    d = definition("AA55", "0D0A", "float32", false, "sum8"); auto reserved = d["frame"].toObject();
    reserved.insert("payloadOffset", 5); reserved.insert("payloadEndOffset", 5); reserved.insert("minFrameBytes", 14); d.insert("frame", reserved);
    d.insert("conditions", QJsonArray{QJsonObject{{"offset", 2}, {"equalsHex", "010203"}}});
    VERIFY(ProtocolProfileValidator::profile(d, p, error)); const auto reservedPacket = wireFrame(p, 3, error); VERIFY(!reservedPacket.isEmpty());
    r = parse(p, reservedPacket, 1); VERIFY(r.frames == QVector<QVector<double>>({{1, 2, 3}}));
    VERIFY(ProtocolProfileValidator::profile(definition("AA55", ""), p, error)); const auto headerFrame = wireFrame(p, 3, error);
    r = parse(p, headerFrame, 1); VERIFY(r.frames.isEmpty() && r.diagnostics.join(" ").contains("下一个帧头"));
    QByteArray sample; VERIFY(ProtocolSampleGenerator::generate(p, sample, error, 3)); r = parse(p, sample, 1);
    VERIFY(r.frames.size() == 1 && r.remaining == QByteArray::fromHex("AA55") && r.diagnostics.isEmpty());
    // Partial headers at exactly the maximum frame length are lookahead,
    // not an oversized previous frame, regardless of receive chunk size.
    d = definition("AA55", ""); auto f = d["frame"].toObject(); f.insert("maxFrameBytes", headerFrame.size()); d.insert("frame", f);
    VERIFY(ProtocolProfileValidator::profile(d, p, error)); r = parse(p, sample, 1); VERIFY(r.frames.size() == 1 && r.stats.rejectedFrames == 0);
    // Oversized tail-only frames discard THROUGH the next delimiter. A valid
    // looking suffix of an overlong frame must never be published.
    d = definition(); f = d["frame"].toObject(); f.insert("maxFrameBytes", 44); d.insert("frame", f); VERIFY(ProtocolProfileValidator::profile(d, p, error));
    for (int chunk : {1, 0, 4096}) { r = parse(p, QByteArray(10000, '\x01') + ten + good, chunk);
        VERIFY(r.frames.size() == 1 && r.frames[0] == QVector<double>({1, 2, 3}) && r.stats.rejectedFrames == 1);
        if (chunk == 1) VERIFY(r.largestBuffer <= 44); }
    r = parse(p, QByteArray(10000, '\x01'), 1); VERIFY(r.frames.isEmpty() && r.diagnostics.join(" ").contains("重新同步"));
    // Reconnect/datagram reset clears discard/scan state; caller drops bytes.
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(p, nullptr, error))); VERIFY(parser);
    QByteArray garbage(100, 1); parser->processData(garbage.constData(), garbage.size()); parser->endDatagram();
    parser->processData(good.constData(), good.size()); VERIFY(parser->stats().validFrames == 1); parser->reset();
    parser->processData(good.constData(), good.size()); VERIFY(parser->stats().validFrames == 1 && parser->stats().rejectedFrames == 0);
    // Invalid layouts cannot be saved, imported or used to create a parser.
    for (int invalid = 0; invalid < 7; ++invalid) {
        d = definition(); f = d["frame"].toObject();
        if (invalid == 0) { f.insert("headerHex", ""); f.insert("tailHex", ""); }
        if (invalid == 1) d.remove("array");
        if (invalid == 2) f.insert("lengthField", QJsonObject{});
        if (invalid == 3) d.insert("headerChecksum", QJsonObject{{"algorithm", "none"}});
        if (invalid == 4) f.insert("payloadEndOffset", 3);
        if (invalid == 5) f.insert("sizeBytes", 12);
        if (invalid == 6) f.insert("maxFrameBytes", MaxFrameBytes + 1);
        d.insert("frame", f); VERIFY(!ProtocolProfileValidator::profile(d, p, error));
    }
    // Byte-at-a-time scanning of a large frame must remain bounded/linear.
    d = definition({}, "0000807F", "float64"); f = d["frame"].toObject(); f.insert("payloadOffset", 60000); f.insert("minFrameBytes", 60012); f.insert("maxFrameBytes", 65536); d.insert("frame", f);
    VERIFY(ProtocolProfileValidator::profile(d, p, error)); const auto large = wireFrame(p, 1, error); VERIFY(large.size() == 60012);
    QElapsedTimer timer; timer.start(); r = parse(p, large, 1); VERIFY(r.frames.size() == 1 && r.remaining.isEmpty() && timer.elapsed() < 5000);
    std::cout << "Delimited arrays: header/tail/both, all types/endian/checksums, 1-256 channels, chunking, resync, bounded buffering, diagnostics, metadata and persistence passed.\n";
    return 0;
}
