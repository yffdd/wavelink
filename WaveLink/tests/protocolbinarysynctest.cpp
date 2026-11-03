#include "CustomProtocolParser.h"
#include "ProtocolParserFactory.h"
#include "ProtocolSampleGenerator.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <memory>
#include <iostream>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Binary sync failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)

static QJsonObject definition(bool variable, bool array, bool head, bool tail, bool crc) {
    const int h = head ? 2 : 0, t = tail ? 4 : 0, c = crc ? 2 : 0;
    const int offset = h + (variable ? 2 : 0), payload = array ? 4 : 40;
    QJsonObject frame{{"mode", variable ? "length" : "fixed"}, {"headerHex", head ? "AA55" : ""},
        {"tailHex", tail ? "0000807F" : ""}, {"payloadOffset", offset}, {"maxFrameBytes", 4096}};
    frame.insert(variable ? "minFrameBytes" : "sizeBytes", offset + payload + c + t);
    frame.insert(array ? "payloadEndOffset" : "payloadBytes", array ? c + t : payload);
    if (variable) frame.insert("lengthField", QJsonObject{{"offset", h}, {"type", "uint16"}, {"byteOrder", "little"},
        {"meaning", "payload-bytes"}, {"unitBytes", 1}, {"adjustment", offset + c + t}});
    QJsonArray channels;
    if (!array) for (int i = 0; i < 10; ++i) channels.append(QJsonObject{{"id", QString("c%1").arg(i)},
        {"name", QString("Ch%1").arg(i + 1)}, {"offset", i * 4}, {"type", "float32"}, {"scale", 1}, {"bias", 0}});
    QJsonObject d{{"id", "5166e64d-c60e-4428-aa9a-7af15a3c2cfa"}, {"revision", 1}, {"name", "EEG sync regression"},
        {"type", "binary"}, {"byteOrder", "little"}, {"frame", frame}, {"channels", channels}};
    if (array) d.insert("array", QJsonObject{{"type", "float32"}, {"minChannels", 1}, {"maxChannels", 256}});
    if (crc) d.insert("checksum", QJsonObject{{"algorithm", "crc16-modbus"},
        {"range", QJsonObject{{"offset", 0}, {"endOffset", c + t}}},
        {"field", QJsonObject{{"from", "end"}, {"offset", c + t}, {"type", "uint16"}, {"byteOrder", "little"}}}});
    return d;
}
struct Result { QVector<QVector<double>> frames; ProtocolParseStats stats; QByteArray remaining; bool bounded = true; };
static Result parse(const ProtocolProfile &profile, const QByteArray &input, int chunk) {
    Result r; QString error;
    std::unique_ptr<CustomProtocolParser> p(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error)));
    if (!p) { r.bounded = false; r.stats.lastError = error; return r; }
    QObject::connect(p.get(), &ProtocolParser::channelsUpdated, [&r](const QVector<double> &v) { r.frames.append(v); });
    quint32 random = 7381;
    for (int offset = 0; offset < input.size();) {
        random = random * 1664525u + 1013904223u;
        const int n = qMin(chunk ? chunk : int(random % 37) + 1, input.size() - offset);
        r.remaining += input.mid(offset, n); offset += n;
        const int consumed = p->processData(r.remaining.constData(), r.remaining.size());
        if (consumed < 0 || consumed > r.remaining.size()) { r.bounded = false; break; }
        r.remaining.remove(0, consumed);
        if (r.remaining.size() >= profile.definition["frame"].toObject()["maxFrameBytes"].toInt()) r.bounded = false;
    }
    r.stats = p->stats(); return r;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QString error; ProtocolProfile p;
    // Every delimiter combination in fixed mode, plus dynamic-array and
    // fixed-field length frames, with/without CRC and with optional tails.
    for (bool variable : {false, true}) for (bool array : {false, true}) {
        if (array && !variable) continue;
        for (bool head : {false, true}) for (bool tail : {false, true}) for (bool crc : {false, true}) {
            const auto d = definition(variable, array, head, tail, crc);
            if ((!head && !tail) || (variable && !head)) {
                VERIFY(!ProtocolProfileValidator::profile(d, p, error));
                if (!variable) VERIFY(error.contains("不能同时为空"));
                continue;
            }
            const bool valid = ProtocolProfileValidator::profile(d, p, error);
            if (!valid) std::cerr << "profile variable=" << variable << " array=" << array << " head=" << head << " tail=" << tail << " crc=" << crc << " error=" << error.toStdString() << '\n';
            VERIFY(valid);
            QByteArray first, second, third, fourth;
            VERIFY(ProtocolSampleGenerator::generate(p, first, error, array ? 10 : 0));
            VERIFY(ProtocolSampleGenerator::generate(p, second, error, array ? 3 : 0));
            VERIFY(ProtocolSampleGenerator::generate(p, third, error, array ? 17 : 0));
            VERIFY(ProtocolSampleGenerator::generate(p, fourth, error, array ? 1 : 0));
            const auto suffix = second + third + fourth;
            const auto expected = parse(p, suffix, 4096).frames;
            VERIFY(expected.size() == 3);
            const bool tailOnlyWithoutChecksum = !variable && !head && tail && !crc;
            const auto afterUntrustedPrefix = tailOnlyWithoutChecksum ? expected.mid(1) : expected;
            for (int chunk : {1, 2, 3, 7, 17, 0, 4096}) {
                // Reconnect at every byte. An unchecked tail-only stream
                // starting inside the tail must first find a complete tail.
                for (int offset = 1; offset < first.size(); ++offset) {
                    const auto r = parse(p, first.mid(offset) + suffix, chunk);
                    const auto &recovered = tailOnlyWithoutChecksum && offset > first.size() - 4
                        ? afterUntrustedPrefix : expected;
                    if (r.frames != recovered) std::cerr << "case variable=" << variable << " array=" << array
                        << " head=" << head << " tail=" << tail << " crc=" << crc << " chunk=" << chunk << " offset=" << offset << '\n';
                    VERIFY(r.bounded && r.frames == recovered && r.remaining.isEmpty());
                }
                auto r = parse(p, QByteArray::fromHex("0012AA") + suffix, chunk);
                if (r.frames != afterUntrustedPrefix || !r.remaining.isEmpty()) std::cerr << "garbage case variable=" << variable << " array=" << array << " head=" << head << " tail=" << tail << " crc=" << crc << " chunk=" << chunk << " frames=" << r.frames.size() << " remaining=" << r.remaining.toHex().toStdString() << '\n';
                VERIFY(r.bounded && r.frames == afterUntrustedPrefix && r.remaining.isEmpty());
                // A header alone cannot authenticate a truncated body that
                // happens to contain valid numeric bytes. Tail or CRC makes
                // these damaged candidates rejectable without guessing.
                if (tail || crc) {
                    r = parse(p, first.left(first.size() / 2) + suffix, chunk);
                    if (r.frames != afterUntrustedPrefix) std::cerr << "truncated case variable=" << variable << " array=" << array << " head=" << head << " tail=" << tail << " crc=" << crc << " chunk=" << chunk << '\n';
                    VERIFY(r.bounded && r.frames == afterUntrustedPrefix && r.remaining.isEmpty());
                    auto dropped = first; dropped.remove(first.size() / 2, 1);
                    r = parse(p, dropped + suffix, chunk);
                    VERIFY(r.bounded && r.frames == expected && r.remaining.isEmpty());
                }
                r = parse(p, suffix + first.left(first.size() - 1), chunk);
                VERIFY(r.bounded && r.frames == expected && r.remaining == first.left(first.size() - 1));
                if (variable) {
                    // A plausible 200-byte body must not mask the following
                    // shorter COMPLETE packet.
                    const auto pending = QByteArray::fromHex("AA55C800");
                    r = parse(p, pending, chunk); VERIFY(r.frames.isEmpty() && r.remaining == pending);
                    r = parse(p, pending + suffix, chunk);
                    VERIFY(r.bounded && r.frames == expected && r.remaining.isEmpty());
                }
                if (crc || tail) {
                    auto damaged = first; damaged[damaged.size() - 1] = char(damaged.at(damaged.size() - 1) ^ 1);
                    r = parse(p, damaged + suffix, chunk);
                    VERIFY(r.bounded && r.frames == afterUntrustedPrefix && r.remaining.isEmpty());
                }
            }
            // No marker: retain only a bounded partial candidate, never an
            // ever-growing buffer. Any trailing split header survives.
            const auto r = parse(p, QByteArray(100000, char(0x12)), 4096);
            VERIFY(r.bounded && r.frames.isEmpty() && r.remaining.size() < first.size());
        }
    }
    // Protect the length with a header CRC, then deliberately supply a valid
    // header for a truncated body. A later dual-CRC frame still restores sync.
    auto dual = definition(true, true, true, true, true);
    auto frame = dual["frame"].toObject(), length = frame["lengthField"].toObject();
    frame.insert("payloadOffset", 6); frame.insert("minFrameBytes", 16);
    length.insert("adjustment", 12); frame.insert("lengthField", length); dual.insert("frame", frame);
    dual.insert("headerChecksum", QJsonObject{{"algorithm", "crc16-modbus"}, {"range", QJsonObject{{"offset", 0}, {"length", 4}}},
        {"field", QJsonObject{{"offset", 4}, {"type", "uint16"}, {"byteOrder", "little"}}}});
    for (const QString meaning : {"payload-bytes", "frame-bytes", "elements"}) {
        length.insert("meaning", meaning); length.insert("unitBytes", meaning == "elements" ? 4 : 1);
        length.insert("adjustment", meaning == "frame-bytes" ? 0 : 12); frame.insert("lengthField", length); dual.insert("frame", frame);
        VERIFY(ProtocolProfileValidator::profile(dual, p, error)); QByteArray packet;
        VERIFY(ProtocolSampleGenerator::generate(p, packet, error, 10));
        auto pending = packet.left(6); pending[2] = char(200); pending[3] = 0;
        const auto crc = ProtocolChecksum("crc16-modbus").calculate(pending.constData(), 4);
        pending[4] = char(crc); pending[5] = char(crc >> 8);
        const auto expected = parse(p, packet.repeated(3), 4096).frames;
        for (int chunk : {1, 3, 7, 0, 4096}) {
            auto r = parse(p, pending + packet.repeated(3), chunk);
            VERIFY(r.bounded && r.frames == expected && r.frames.size() == 3 && r.remaining.isEmpty());
            for (int offset = 1; offset < packet.size(); ++offset) {
                r = parse(p, packet.mid(offset) + packet.repeated(3), chunk);
                VERIFY(r.bounded && r.frames == expected && r.remaining.isEmpty());
            }
        }
    }
    // JustFloat-compatible fixed frame: markers inside an incomplete payload
    // must not cause early cutting. These unaligned bytes decode as finite
    // floats and the embedded tail occurs well before the real frame end.
    VERIFY(ProtocolProfileValidator::profile(definition(false, false, false, true, false), p, error));
    auto embedded = QByteArray(40, char(0x11)); embedded.replace(1, 4, QByteArray::fromHex("0000807F")); embedded += QByteArray::fromHex("0000807F");
    for (int chunk : {1, 3, 7, 44}) { auto r = parse(p, embedded.repeated(3), chunk); VERIFY(r.bounded && r.frames.size() == 3 && r.remaining.isEmpty()); }
    // Invalid numeric frames are NOT rescued as good values. Only their intact
    // successors are accepted, even in tail-only mode.
    QByteArray good; VERIFY(ProtocolSampleGenerator::generate(p, good, error));
    auto bad = good; bad.replace(0, 4, QByteArray::fromHex("000080FF"));
    auto r = parse(p, bad + good.repeated(3), 1); VERIFY(r.frames.size() == 3 && r.stats.rejectedFrames > 0);
    // After acquiring sync, a later tail cannot authenticate the suffix of a
    // damaged span. This reproduces false zeros from padding in an extra-long span.
    const auto tail = good.right(4);
    const auto tooLong = good.chopped(4) + QByteArray(22, '\0') + tail;
    const auto tooShort = good.left(18) + tail;
    const auto intact = parse(p, good.repeated(3), 4096).frames;
    for (int chunk : {1, 2, 7, 44, 0, 4096}) {
        const auto result = parse(p, good + tooLong + good + tooShort + good, chunk);
        VERIFY(result.frames == intact && result.remaining.isEmpty() && result.bounded);
        VERIFY(result.stats.validFrames == 3 && result.stats.rejectedFrames == 2);
        VERIFY(result.stats.discardedBytes == quint64(tooLong.size() + tooShort.size()));
        VERIFY(result.stats.lastError.contains("22 字节"));
        const auto startup = parse(p, tooLong + good.repeated(3), chunk);
        VERIFY(startup.frames == intact && startup.stats.rejectedFrames == 1);
        VERIFY(startup.stats.discardedBytes == quint64(tooLong.size()) && startup.remaining.isEmpty());
    }
    ProtocolSelection selection;
    selection.kind = "custom";
    selection.id = p.id;
    for (int chunk : {1, 7, 4096}) {
        std::unique_ptr<ProtocolParser> live(ProtocolParserFactory::create(selection, p, nullptr, error));
        VERIFY(live);
        QVector<QVector<double>> displayed, recorded;
        QObject::connect(live.get(), &ProtocolParser::channelsUpdated, live.get(),
            [&](const QVector<double> &values) { displayed.append(values); });
        QObject::connect(live.get(), &ProtocolParser::recordingFrameReady, live.get(),
            [&](const QVector<double> &values) { recorded.append(values); });
        QByteArray pending;
        const auto feed = [&](const QByteArray &bytes) {
            for (int offset = 0; offset < bytes.size(); offset += chunk) {
                pending += bytes.mid(offset, chunk);
                pending.remove(0, live->processData(pending.constData(), pending.size()));
            }
        };
        feed(good.repeated(2));
        VERIFY(displayed.isEmpty() && recorded.isEmpty());
        feed(tooLong + good.repeated(2) + bad + good.repeated(2));
        VERIFY(displayed.isEmpty() && recorded.isEmpty());
        feed(good);
        VERIFY(displayed == intact.mid(2) && recorded == displayed);
        feed(good);
        VERIFY(displayed == intact.mid(1) && recorded == displayed);
        feed(tooShort + good.repeated(2));
        VERIFY(displayed.size() == 2 && recorded == displayed);
        feed(good);
        VERIFY(displayed == intact && recorded == displayed);
        live->reset();
        feed(good.repeated(2));
        VERIFY(displayed == intact && recorded == displayed);
        feed(good);
        VERIFY(displayed.size() == 4 && recorded == displayed);
    }
    // Performance / stable fast path for long acquisition.
    QElapsedTimer elapsed; elapsed.start(); r = parse(p, good.repeated(50000), 8192);
    VERIFY(r.bounded && r.frames.size() == 50000 && r.remaining.isEmpty());
    std::cout << "Binary synchronization passed: every initial byte offset, all fixed delimiter combinations, variable fixed fields/dynamic arrays, CRC, chunks, corruption, pending lengths, bounded buffers; 50000 EEG frames in " << elapsed.elapsed() << " ms.\n";
    return 0;
}
