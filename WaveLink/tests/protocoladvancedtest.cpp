#include "ProtocolParserFactory.h"
#include "CustomProtocolParser.h"
#include "ProtocolProfileStore.h"
#include "ProtocolChecksum.h"
#include "ProtocolSampleGenerator.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <memory>
#include <cmath>
#include <iostream>
using namespace DataProtocols;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Advanced protocol failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)

static QJsonObject example() {
    return {{"id", "03bcc49e-d8e9-4886-a50f-9f47b9304d6a"}, {"revision", 1}, {"name", "Length frame"}, {"type", "binary"}, {"byteOrder", "little"},
        {"frame", QJsonObject{{"mode", "length"}, {"headerHex", "AA55"}, {"tailHex", "0D"}, {"minFrameBytes", 11}, {"maxFrameBytes", 4096}, {"payloadOffset", 3}, {"payloadBytes", 6},
            {"lengthField", QJsonObject{{"offset", 2}, {"type", "uint8"}, {"unitBytes", 1}, {"adjustment", 5}}}}},
        {"checksum", QJsonObject{{"algorithm", "crc8-smbus"}, {"range", QJsonObject{{"offset", 2}, {"endOffset", 2}}}, {"field", QJsonObject{{"from", "end"}, {"offset", 2}, {"type", "uint8"}}}}},
        {"channels", QJsonArray{QJsonObject{{"id", "temperature"}, {"name", "Temperature"}, {"offset", 0}, {"type", "int16"}, {"scale", .01}},
            QJsonObject{{"id", "voltage"}, {"name", "Voltage"}, {"offset", 2}, {"type", "float32"}}}}};
}
static QJsonObject frameChange(QJsonObject d, const QJsonObject &changes) {
    auto f = d.value("frame").toObject(); for (auto it = changes.begin(); it != changes.end(); ++it) f.insert(it.key(), it.value()); d.insert("frame", f); return d;
}
static bool valid(const QJsonObject &d) { ProtocolProfile p; QString e; return ProtocolProfileValidator::profile(d, p, e); }
struct Result { QVector<QVector<double>> values; QByteArray remaining; ProtocolParseStats stats; };
static bool run(const QJsonObject &d, const QByteArray &input, int chunk, Result &result) {
    ProtocolProfile p; QString error;
    if (!ProtocolProfileValidator::profile(d, p, error)) { std::cerr << error.toStdString() << "\n"; return false; }
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(p, nullptr, error)));
    if (!parser) return false;
    Result r; QObject::connect(parser.get(), &CustomProtocolParser::channelsUpdated, [&](const QVector<double> &v) { r.values.append(v); });
    quint32 seed = 71;
    for (int pos = 0; pos < input.size();) {
        seed = seed * 1664525u + 1013904223u; int n = qMin(chunk ? chunk : int(seed % 31) + 1, input.size() - pos);
        r.remaining += input.mid(pos, n); pos += n;
        int consumed = parser->processData(r.remaining.constData(), r.remaining.size());
        if (consumed < 0 || consumed > r.remaining.size()) return false;
        r.remaining.remove(0, consumed);
    }
    r.stats = parser->stats(); result = r; return true;
}
static QByteArray stored(quint64 value, int width, bool big = false) {
    QByteArray result(width, 0); for (int i = 0; i < width; ++i) result[big ? width - 1 - i : i] = char(value >> (8 * i)); return result;
}
// Independent bitwise oracle (runtime implementation uses compiled tables).
static quint32 reference(const QString &algorithm, const QByteArray &bytes) {
    quint32 crc = 0, poly = 0; int bits = 8; bool reflected = false; quint32 xorOut = 0;
    if (algorithm == "crc8-smbus") poly = 7;
    if (algorithm == "crc16-modbus") { bits = 16; crc = 0xffff; poly = 0xa001; reflected = true; }
    if (algorithm == "crc16-ccitt-false") { bits = 16; crc = 0xffff; poly = 0x1021; }
    if (algorithm == "crc16-xmodem") { bits = 16; poly = 0x1021; }
    if (algorithm == "crc32-iso-hdlc") { bits = 32; crc = 0xffffffffu; poly = 0xedb88320u; reflected = true; xorOut = 0xffffffffu; }
    const quint32 mask = bits == 32 ? 0xffffffffu : (quint32(1) << bits) - 1;
    for (unsigned char b : bytes) {
        if (algorithm == "sum8") { crc = (crc + b) & 255; continue; }
        if (algorithm == "xor8") { crc ^= b; continue; }
        crc ^= reflected ? b : quint32(b) << (bits - 8);
        for (int i = 0; i < 8; ++i) crc = reflected ? (crc & 1 ? (crc >> 1) ^ poly : crc >> 1)
            : ((crc & (quint32(1) << (bits - 1)) ? (crc << 1) ^ poly : crc << 1) & mask);
    }
    return (crc ^ xorOut) & mask;
}
static QByteArray packet(const QJsonObject &d, const QByteArray &extra = {}) {
    const auto f = d.value("frame").toObject(), lf = f.value("lengthField").toObject(), c = d.value("checksum").toObject();
    const int lw = ProtocolProfileValidator::typeBytes(lf.value("type").toString()), cw = ProtocolChecksum::width(c.value("algorithm").toString());
    const int total = 2 + lw + 6 + extra.size() + cw + 1;
    const int length = (total - lf.value("adjustment").toInt()) / lf.value("unitBytes").toInt();
    auto body = stored(quint64(length), lw, lf.value("byteOrder") == QJsonValue("big")) + QByteArray::fromHex("E60933335340") + extra;
    return QByteArray::fromHex("AA55") + body + stored(reference(c.value("algorithm").toString(), body), cw, c.value("field").toObject().value("byteOrder") == QJsonValue("big")) + QByteArray::fromHex("0D");
}
// Exercise the exact copyable help code as C99, not as C++ or another oracle.
static bool compileChecksumExamples() {
    QTemporaryDir dir; if (!dir.isValid()) return false;
    QByteArray code;
    for (const auto &preset : ProtocolChecksum::presets()) code += ProtocolChecksum::cExample(preset.id).toUtf8();
    code += "\nint main(void) {\n    const uint8_t check[] = \"123456789\";\n    uint8_t random[1024];\n    uint32_t seed = 71;\n    for (size_t i = 0; i < sizeof random; ++i) { seed = seed * 1664525u + 1013904223u; random[i] = (uint8_t)(seed >> 24); }\n";
    QByteArray bytes(1024, 0); quint32 seed = 71;
    for (int i = 0; i < bytes.size(); ++i) { seed = seed * 1664525u + 1013904223u; bytes[i] = char(seed >> 24); }
    const QVector<quint32> known{0, 0xdd, 0x31, 0x4b37, 0xf4, 0x29b1, 0x31c3, 0xcbf43926u};
    int index = 0;
    for (const auto &preset : ProtocolChecksum::presets()) {
        const QString function = "wavelink_" + QString(preset.id).replace('-', '_');
        ProtocolChecksum crc(preset.id);
        code += QString("    if (%1(check, 9) != UINT32_C(0x%2)) return %3;\n")
            .arg(function).arg(known[index], 0, 16).arg(index + 1).toUtf8();
        for (int count : {0, 1, 7, 255, 1024})
            code += QString("    if (%1(random, %2) != UINT32_C(0x%3)) return %4;\n")
                .arg(function).arg(count).arg(crc.calculate(bytes.constData(), count), 0, 16).arg(index + 20).toUtf8();
        ++index;
    }
    code += "    return 0;\n}\n";
    const auto source = dir.filePath("checksum-examples.c"), executable = dir.filePath("checksum-examples.exe");
    QFile file(source); if (!file.open(QIODevice::WriteOnly) || file.write(code) != code.size()) return false; file.close();
    QProcess compiler;
    compiler.start(QString::fromUtf8(WAVELINK_C_TEST_COMPILER), {"-x", "c", "-std=c99", "-Wall", "-Wextra", "-Werror", source, "-o", executable});
    if (!compiler.waitForFinished(15000) || compiler.exitStatus() != QProcess::NormalExit || compiler.exitCode()) {
        std::cerr << compiler.readAllStandardError().constData() << "\n"; return false;
    }
    QProcess program; program.start(executable, QStringList());
    return program.waitForFinished(5000) && program.exitStatus() == QProcess::NormalExit && program.exitCode() == 0;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); Result r; auto d = example();
    VERIFY(compileChecksumExamples());
    // Generated samples must survive the same streaming decoder, arbitrary
    // chunk boundaries, every checksum, and length byte order/unit/adjustment.
    for (const auto &preset : ProtocolChecksum::presets()) for (bool big : {false, true}) {
        auto candidate = d; auto frame = candidate.value("frame").toObject(); auto length = frame.value("lengthField").toObject();
        length.insert("type", "uint16"); length.insert("byteOrder", big ? "big" : "little"); length.insert("unitBytes", 2); length.insert("adjustment", 5 + preset.bytes);
        frame.insert("payloadOffset", 4); frame.insert("minFrameBytes", 11 + preset.bytes); frame.insert("lengthField", length); candidate.insert("frame", frame);
        if (!preset.bytes) candidate.insert("checksum", QJsonObject{{"algorithm", "none"}});
        else {
            QJsonObject field{{"from", "end"}, {"offset", preset.bytes + 1}, {"type", preset.bytes == 1 ? "uint8" : preset.bytes == 2 ? "uint16" : "uint32"}};
            if (preset.bytes > 1) field.insert("byteOrder", big ? "big" : "little");
            candidate.insert("checksum", QJsonObject{{"algorithm", preset.id}, {"range", QJsonObject{{"offset", 0}, {"endOffset", preset.bytes + 1}}}, {"field", field}});
        }
        ProtocolProfile profile; QString error; QByteArray generated; VERIFY(ProtocolProfileValidator::profile(candidate, profile, error));
        VERIFY(ProtocolSampleGenerator::generate(profile, generated, error));
        for (int chunk : {0, 1, 4096}) VERIFY(run(candidate, generated, chunk, r) && r.stats.validFrames == 1 && !r.stats.rejectedFrames && r.values[0].size() == 2);
        VERIFY(generated.size() >= frame.value("minFrameBytes").toInt() && (generated.size() - length.value("adjustment").toInt()) % 2 == 0);
    }
    {
        auto constrained = d; constrained.insert("conditions", QJsonArray{QJsonObject{{"offset", 2}, {"equalsHex", "08"}}});
        ProtocolProfile profile; QString error; QByteArray generated; VERIFY(ProtocolProfileValidator::profile(constrained, profile, error));
        VERIFY(ProtocolSampleGenerator::generate(profile, generated, error) && generated.size() == 13 && static_cast<unsigned char>(generated[2]) == 8);
        // Impossible fixed conditions cannot replace an existing sample.
        constrained.insert("conditions", QJsonArray{QJsonObject{{"offset", 0}, {"equalsHex", "0000"}}});
        VERIFY(ProtocolProfileValidator::profile(constrained, profile, error)); generated = "KEEP";
        VERIFY(!ProtocolSampleGenerator::generate(profile, generated, error) && generated == "KEEP" && !error.isEmpty());
        auto text = QJsonObject{{"id", d.value("id")}, {"revision", 1}, {"name", "Generated UTF8"}, {"type", "text"},
            {"frame", QJsonObject{{"encoding", "utf8"}, {"prefix", "数据:"}, {"suffix", "END"}, {"delimiterMode", "literal"}, {"delimiter", ","}, {"terminatorHex", "0D0A"}, {"trimFields", true}, {"maxFrameBytes", 4096}}},
            {"channels", QJsonArray{QJsonObject{{"id", "a"}, {"column", 2}, {"scale", -2}, {"bias", 3}, {"min", 10}, {"max", 12}}}}};
        VERIFY(ProtocolProfileValidator::profile(text, profile, error) && ProtocolSampleGenerator::generate(profile, generated, error));
        VERIFY(generated.startsWith(QString("数据:").toUtf8()) && generated.endsWith("END\r\n") && run(text, generated, 1, r) && r.values[0].size() == 3 && r.values[0][0] >= 10 && r.values[0][0] <= 12);
    }
    const auto good = QByteArray::fromHex("AA5506E60933335340DB0D");
    VERIFY(packet(d) == good);
    const auto extra = packet(d, QByteArray::fromHex("AA550000AA55FF"));
    for (int chunk = 0; chunk <= 33; ++chunk) {
        VERIFY(run(d, QByteArray::fromHex("00FFAA") + good + extra + good.left(7), chunk, r));
        VERIFY(r.stats.validFrames == 2 && !r.stats.rejectedFrames && r.remaining == good.left(7));
        VERIFY(r.values[0] == r.values[1] && std::abs(r.values[0][0] - 25.34) < 1e-10 && std::abs(r.values[0][1] - 3.3) < 1e-5);
        auto badLength = good; badLength[2] = 0; auto badCheck = good; badCheck[9] = 0;
        auto badTail = good; badTail[10] = 0;
        VERIFY(run(d, badLength + badCheck + badTail + good, chunk, r));
        VERIFY(r.stats.validFrames == 1 && r.stats.rejectedFrames == 3 && r.stats.checksumFailures == 1 && r.remaining.isEmpty());
        // A truncated packet is rejected once its stated size is available;
        // the following valid header survives resynchronization.
        VERIFY(run(d, good.left(7) + good, chunk, r));
        VERIFY(r.stats.validFrames == 1 && r.stats.rejectedFrames == 1 && r.remaining.isEmpty());
    }
    // Legal incomplete lengths remain buffered unless a later COMPLETE frame
    // passes all rules. A plausible damaged length must not block that frame.
    auto pending = good.left(4); pending[2] = char(200);
    VERIFY(run(d, pending, 1, r) && r.stats.validFrames == 0 && r.remaining == pending);
    VERIFY(run(d, pending + good, 1, r) && r.stats.validFrames == 1 && r.stats.rejectedFrames == 1 && r.remaining.isEmpty());
    // Upper limit is checked before buffering a body; recovery is immediate.
    auto capped = frameChange(d, {{"maxFrameBytes", 32}});
    VERIFY(run(capped, pending + good, 1, r) && r.stats.validFrames == 1 && r.stats.rejectedFrames == 1);
    for (int lw : {1, 2, 4}) for (bool big : {false, true}) {
        auto f = d.value("frame").toObject();
        QJsonObject length{{"offset", 2}, {"type", lw == 1 ? "uint8" : lw == 2 ? "uint16" : "uint32"}, {"unitBytes", 1}, {"adjustment", 4 + lw}};
        if (lw > 1) length.insert("byteOrder", big ? "big" : "little");
        f.insert("lengthField", length); f.insert("payloadOffset", 2 + lw); f.insert("minFrameBytes", 10 + lw);
        auto candidate = d; candidate.insert("frame", f);
        for (int chunk = 0; chunk < 17; ++chunk) VERIFY(run(candidate, packet(candidate) + packet(candidate, "ABCD"), chunk, r) && r.stats.validFrames == 2);
        if (lw > 1) {
            auto inherited = candidate; inherited.insert("byteOrder", big ? "big" : "little");
            auto inheritedFrame = f, inheritedLength = length; inheritedLength.remove("byteOrder"); inheritedFrame.insert("lengthField", inheritedLength); inherited.insert("frame", inheritedFrame);
            ProtocolProfile profile; QString error;
            VERIFY(ProtocolProfileValidator::profile(inherited, profile, error));
            VERIFY(profile.definition.value("frame").toObject().value("lengthField").toObject().value("byteOrder") == QJsonValue(big ? "big" : "little"));
            VERIFY(run(inherited, packet(candidate), 1, r) && r.stats.validFrames == 1);
        }
        length.insert("adjustment", -3); f.insert("lengthField", length); candidate.insert("frame", f);
        VERIFY(run(candidate, packet(candidate), 1, r) && r.stats.validFrames == 1);
        if (lw == 4) {
            length.insert("unitBytes", 65536); length.insert("adjustment", 0); f.insert("lengthField", length); f.insert("maxFrameBytes", 65536); candidate.insert("frame", f);
            VERIFY(run(candidate, QByteArray::fromHex("AA55FFFFFFFF"), 1, r) && r.stats.rejectedFrames == 1 && r.remaining.isEmpty());
        }
    }
    auto f = d.value("frame").toObject(), length = f.value("lengthField").toObject();
    length.insert("unitBytes", 2); length.insert("adjustment", 4); f.insert("lengthField", length); auto words = d; words.insert("frame", f);
    VERIFY(run(words, packet(words, "X"), 1, r) && r.stats.validFrames == 1);
    // Known answers are hard-coded, not generated by the implementation.
    const QVector<QPair<QString, quint32>> answers{{"sum8", 0xdd}, {"xor8", 0x31}, {"crc16-modbus", 0x4b37}, {"crc8-smbus", 0xf4},
        {"crc16-ccitt-false", 0x29b1}, {"crc16-xmodem", 0x31c3}, {"crc32-iso-hdlc", 0xcbf43926u}};
    for (const auto &answer : answers) {
        ProtocolChecksum crc(answer.first); const int cw = crc.width();
        VERIFY(crc.calculate("123456789", 9) == answer.second);
        VERIFY(reference(answer.first, "123456789") == answer.second);
        for (bool big : {false, true}) {
            auto fixed = d; fixed.insert("frame", QJsonObject{{"mode", "fixed"}, {"headerHex", "AA55"}, {"tailHex", "0D"}, {"sizeBytes", 12 + cw}, {"payloadOffset", 2}, {"payloadBytes", 9}, {"maxFrameBytes", 4096}});
            QJsonObject field{{"offset", 11}, {"type", cw == 1 ? "uint8" : cw == 2 ? "uint16" : "uint32"}};
            if (cw > 1) field.insert("byteOrder", big ? "big" : "little");
            fixed.insert("checksum", QJsonObject{{"algorithm", answer.first}, {"range", QJsonObject{{"offset", 2}, {"length", 9}}}, {"field", field}});
            fixed.insert("channels", QJsonArray{QJsonObject{{"id", "a"}, {"name", "A"}, {"offset", 0}, {"type", "uint8"}}});
            const auto bytes = QByteArray::fromHex("AA55") + "123456789" + stored(answer.second, cw, big) + QByteArray::fromHex("0D");
            VERIFY(run(fixed, bytes, 1, r) && r.stats.validFrames == 1 && r.values[0][0] == 49);
            auto changed = bytes; changed[11] = char(static_cast<unsigned char>(changed[11]) ^ 1);
            VERIFY(run(fixed, changed + bytes, 0, r) && r.stats.validFrames == 1 && r.stats.checksumFailures == 1);
            // End-relative placement and range also work in fixed mode.
            field.insert("from", "end"); field.insert("offset", cw + 1);
            fixed.insert("checksum", QJsonObject{{"algorithm", answer.first}, {"range", QJsonObject{{"offset", 2}, {"endOffset", cw + 1}}}, {"field", field}});
            VERIFY(run(fixed, bytes, 1, r) && r.stats.validFrames == 1);
            auto variable = d; f = variable.value("frame").toObject(); length = f.value("lengthField").toObject(); length.insert("adjustment", cw + 4);
            f.insert("lengthField", length); f.insert("minFrameBytes", 10 + cw); variable.insert("frame", f);
            variable.insert("checksum", fixed.value("checksum"));
            VERIFY(run(variable, packet(variable) + packet(variable, "extra"), 1, r) && r.stats.validFrames == 2);
        }
    }
    auto noCheck = d; noCheck.remove("checksum"); VERIFY(run(noCheck, good, 1, r) && r.stats.validFrames == 1);
    auto conditional = d; conditional.insert("conditions", QJsonArray{QJsonObject{{"offset", 3}, {"equalsHex", "E609"}}}); VERIFY(run(conditional, good, 1, r) && r.stats.validFrames == 1);
    conditional.insert("conditions", QJsonArray{QJsonObject{{"offset", 3}, {"equalsHex", "0000"}}}); VERIFY(run(conditional, good, 1, r) && r.stats.rejectedFrames == 1);
    // Configuration errors must fail before a parser is constructed.
    VERIFY(!valid(frameChange(d, {{"headerHex", ""}})));
    VERIFY(!valid(frameChange(d, {{"minFrameBytes", 5}})));
    VERIFY(!valid(frameChange(d, {{"maxFrameBytes", 10}})));
    VERIFY(!valid(frameChange(d, {{"sizeBytes", 11}})));
    for (const auto &change : QVector<QJsonObject>{{{"type", "int16"}}, {{"offset", 3}}, {{"unitBytes", 0}}, {{"unitBytes", 1.5}}, {{"adjustment", 65537}}, {{"adjustment", 5000}}, {{"type", "uint16"}}, {{"unexpected", 1}}}) {
        auto bad = d; f = bad.value("frame").toObject(); length = f.value("lengthField").toObject(); for (auto it = change.begin(); it != change.end(); ++it) length.insert(it.key(), it.value()); f.insert("lengthField", length); bad.insert("frame", f); VERIFY(!valid(bad));
    }
    auto bad = d; auto check = bad.value("checksum").toObject(); auto range = check.value("range").toObject(); range.insert("length", 6); check.insert("range", range); bad.insert("checksum", check); VERIFY(!valid(bad));
    bad = d; check = bad.value("checksum").toObject(); range = check.value("range").toObject(); range.insert("endOffset", 1); check.insert("range", range); bad.insert("checksum", check); VERIFY(!valid(bad));
    bad = d; check = bad.value("checksum").toObject(); auto field = check.value("field").toObject(); field.insert("offset", 1); check.insert("field", field); bad.insert("checksum", check); VERIFY(!valid(bad));
    bad = d; check.insert("algorithm", "crc16"); bad.insert("checksum", check); VERIFY(!valid(bad));
    // Maximum frame / full uint32 length and random chunking remain bounded.
    f = d.value("frame").toObject(); length = f.value("lengthField").toObject(); length.insert("type", "uint32"); length.insert("byteOrder", "little"); length.insert("adjustment", 8);
    f.insert("lengthField", length); f.insert("payloadOffset", 6); f.insert("minFrameBytes", 14); f.insert("maxFrameBytes", 65536); auto maximum = d; maximum.insert("frame", f);
    const auto maxPacket = packet(maximum, QByteArray(65536 - 14, 'z'));
    VERIFY(maxPacket.size() == 65536 && run(maximum, maxPacket + good.left(2), 0, r) && r.stats.validFrames == 1 && r.remaining == good.left(2));
    VERIFY(run(d, good.repeated(20000), 8192, r) && r.stats.validFrames == 20000);
    // Store / share / merge normalize obsolete checksum and length rules.
    QTemporaryDir dir; VERIFY(dir.isValid()); QString error; ProtocolProfile p; VERIFY(ProtocolProfileValidator::profile(d, p, error));
    ProtocolProfileStore store(dir.filePath("protocols.json")); VERIFY(store.load(error)); auto s = store.snapshot(); s.profiles = {p}; s.shared.kind = "custom"; s.shared.id = p.id;
    const auto normalized = ProtocolProfileValidator::standardizeBinary(p.definition, true);
    VERIFY(store.commit(s, error)); ProtocolProfileStore reopened(store.path()); VERIFY(reopened.load(error) && reopened.snapshot().profiles[0].definition == normalized);
    auto exported = ProtocolProfileValidator::encode(s, false, error); ProtocolSnapshot decoded; VERIFY(ProtocolProfileValidator::decode(exported, false, decoded, error) && decoded.profiles[0].definition == normalized);
    ProtocolImportPreview preview; VERIFY(ProtocolProfileStore::prepareImport(s, exported, false, ProtocolImportMode::Append, false, preview, error) && preview.snapshot.profiles.size() == 2);
    std::cout << "Length-field boundaries, checksum known answers, resynchronization, chunks and persistence passed.\n";
    return 0;
}
