#include "ProtocolSampleGenerator.h"
#include "CustomProtocolParser.h"
#include "ProtocolChecksum.h"
#include "ProtocolParserFactory.h"
#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace DataProtocols {
namespace {
QByteArray stored(quint64 value, int width, bool big) {
    QByteArray result(width, 0);
    for (int i = 0; i < width; ++i) result[big ? width - 1 - i : i] = char(value >> (8 * i));
    return result;
}
bool sampleValue(const QJsonObject &field, bool binary, int index, double &value) {
    const auto type = field.value("type").toString();
    const bool integer = binary && type != "float32" && type != "float64";
    long double lower = -std::numeric_limits<double>::max(), upper = std::numeric_limits<double>::max();
    if (integer) {
        const int bits = ProtocolProfileValidator::typeBytes(type) * 8; const bool sign = type.startsWith("int");
        lower = sign ? -std::ldexp(1.0L, bits - 1) : 0;
        upper = std::ldexp(1.0L, sign ? bits - 1 : bits) - 1;
    } else if (type == "float32") { lower = -std::numeric_limits<float>::max(); upper = std::numeric_limits<float>::max(); }
    const long double scale = field.value("scale").toDouble(1), bias = field.value("bias").toDouble();
    if (scale != 0) {
        if (field.contains("min")) { const long double bound = (static_cast<long double>(field.value("min").toDouble()) - bias) / scale; if (scale > 0) lower = qMax(lower, bound); else upper = qMin(upper, bound); }
        if (field.contains("max")) { const long double bound = (static_cast<long double>(field.value("max").toDouble()) - bias) / scale; if (scale > 0) upper = qMin(upper, bound); else lower = qMax(lower, bound); }
    }
    if (integer) { lower = std::ceil(lower); upper = std::floor(upper); }
    if (lower > upper) return false;
    const QVector<long double> candidates{qBound(lower, static_cast<long double>(index + 1), upper), lower / 2 + upper / 2, lower, upper, 0};
    for (auto candidate : candidates) {
        if (candidate < lower || candidate > upper) continue;
        double raw = static_cast<double>(integer ? std::round(candidate) : candidate);
        if (type == "float32") raw = static_cast<float>(raw);
        const double converted = raw * static_cast<double>(scale) + static_cast<double>(bias);
        if (std::isfinite(raw) && std::isfinite(converted) && (!field.contains("min") || converted >= field.value("min").toDouble())
            && (!field.contains("max") || converted <= field.value("max").toDouble())) { value = raw; return true; }
    }
    return false;
}
}
bool ProtocolSampleGenerator::generate(const ProtocolProfile &input, QByteArray &output, QString &error, int arrayChannels) {
    ProtocolProfile profile; if (!ProtocolProfileValidator::profile(input.definition, profile, error)) return false;
    const auto definition = profile.definition, frame = definition.value("frame").toObject();
    auto fields = definition.value("channels").toArray(); const bool binary = definition.value("type") == QJsonValue("binary");
    const auto array = definition.value("array").toObject();
    const int arrayWidth = ProtocolProfileValidator::typeBytes(array.value("type").toString());
    if (profile.isDynamicArray()) {
        int count = arrayChannels;
        if (!count) {
            count = array.value("minChannels").toInt();
            for (const auto &v : fields) count = qMax(count, v.toObject().value("index").toInt() + 1);
        }
        if (count < array.value("minChannels").toInt() || count > array.value("maxChannels").toInt()) {
            error = "样例通道数量超出此协议允许的范围。"; return false;
        }
        fields = profile.outputChannels(count);
        for (int i = 0; i < fields.size(); ++i) {
            auto field = fields[i].toObject(); field.insert("type", array.value("type")); field.insert("offset", i * arrayWidth); fields[i] = field;
        }
    }
    QByteArray bytes;
    if (profile.isKeyValue()) {
        QByteArray ending; ProtocolProfileValidator::parseHex(frame.value("terminatorHex").toString(), ending);
        for (int i = 0; i < fields.size(); ++i) {
            const auto field = fields[i].toObject(); double raw;
            if (!sampleValue(field, false, i, raw)) { error = "无法为通道“" + field.value("name").toString() + "”生成满足范围的数值。"; return false; }
            const auto line = (frame.value("prefix").toString() + field.value("key").toString() + frame.value("delimiter").toString()
                + QString::number(raw, 'g', 17) + frame.value("suffix").toString()).toUtf8() + ending;
            if (line.size() > frame.value("maxFrameBytes").toInt()) { error = "生成的键值行超过最大行长，请调整规则。"; return false; }
            bytes += line;
        }
        if (bytes.size() > 256 * 1024) { error = "生成的键值样例超过测试区 256 KiB 上限，请缩短前后缀或通道键。"; return false; }
    } else if (!binary) {
        int count = 1; QSet<int> columns;
        for (auto value : fields) { const int column = value.toObject().value("column").toInt(); count = qMax(count, column + 1); columns.insert(column); }
        if (count - columns.size() + fields.size() > MaxChannels) { error = "生成完整文本帧将超过 256 通道，请调整列号或手动提供样例。"; return false; }
        QStringList tokens; for (int i = 0; i < count; ++i) tokens.append(QString::number(i + 1));
        for (int i = 0; i < fields.size(); ++i) {
            const auto field = fields[i].toObject(); double raw;
            if (!sampleValue(field, false, i, raw)) { error = "无法为通道“" + field.value("name").toString() + "”生成满足范围的数值，请手动输入样例。"; return false; }
            tokens[field.value("column").toInt()] = QString::number(raw, 'g', 17);
        }
        const auto delimiter = frame.value("delimiterMode") == QJsonValue("whitespace") ? QString(" ") : frame.value("delimiter").toString();
        bytes = (frame.value("prefix").toString() + tokens.join(delimiter) + frame.value("suffix").toString()).toUtf8();
        QByteArray ending; ProtocolProfileValidator::parseHex(frame.value("terminatorHex").toString(), ending); bytes += ending;
    } else {
        const bool variable = frame.value("mode") == QJsonValue("length");
        const bool delimiterFramed = profile.isDelimiterFramed();
        const auto length = frame.value("lengthField").toObject(); const int lengthWidth = ProtocolProfileValidator::typeBytes(length.value("type").toString());
        const int lengthOffset = length.value("offset").toInt(); const bool lengthBig = length.value("byteOrder") == QJsonValue("big");
        int total = frame.value(variable ? "minFrameBytes" : "sizeBytes").toInt(); quint64 n = 0;
        if (delimiterFramed) {
            total = frame.value("payloadOffset").toInt() + fields.size() * arrayWidth + frame.value("payloadEndOffset").toInt();
            if (total < frame.value("minFrameBytes").toInt() || total > frame.value("maxFrameBytes").toInt()) {
                error = "样例通道数对应的帧长不在允许范围，请核对通道上下限、帧长和保留字节。"; return false;
            }
        }
        if (variable) {
            const qint64 unit = length.value("unitBytes").toInt(), adjustment = length.value("adjustment").toInt();
            const qint64 first = qMax<qint64>(0, (qint64(total) - adjustment + unit - 1) / unit);
            const qint64 last = qMin<qint64>((quint64(1) << (lengthWidth * 8)) - 1, (qint64(frame.value("maxFrameBytes").toInt()) - adjustment) / unit);
            bool found = false;
            // At most 65536 candidate lengths. Conditions in the length field
            // are checked before allocating a frame, never brute-force bodies.
            for (qint64 candidate = first; candidate <= last; ++candidate) {
                if (profile.isDynamicArray() && candidate * unit + adjustment - frame.value("payloadOffset").toInt()
                    - frame.value("payloadEndOffset").toInt() != fields.size() * arrayWidth) continue;
                const auto encoded = stored(quint64(candidate), lengthWidth, lengthBig); bool matches = true;
                for (auto v : definition.value("conditions").toArray()) {
                    const auto condition = v.toObject(); QByteArray expected; ProtocolProfileValidator::parseHex(condition.value("equalsHex").toString(), expected);
                    const int start = condition.value("offset").toInt();
                    for (int i = 0; i < expected.size(); ++i) if (start + i >= lengthOffset && start + i < lengthOffset + lengthWidth && expected[i] != encoded[start + i - lengthOffset]) matches = false;
                }
                if (matches) { n = quint64(candidate); total = int(candidate * unit + adjustment); found = true; break; }
            }
            if (!found) { error = "长度公式、帧长上下限、样例通道数和固定字节条件无法生成样例，请核对设置。"; return false; }
        }
        bytes = QByteArray(total, 0); QByteArray header, tail;
        ProtocolProfileValidator::parseHex(frame.value("headerHex").toString(), header); ProtocolProfileValidator::parseHex(frame.value("tailHex").toString(), tail);
        bytes.replace(0, header.size(), header); bytes.replace(total - tail.size(), tail.size(), tail);
        if (variable) bytes.replace(lengthOffset, lengthWidth, stored(n, lengthWidth, lengthBig));
        for (int i = 0; i < fields.size(); ++i) {
            const auto field = fields[i].toObject(); double raw;
            if (!sampleValue(field, true, i, raw)) { error = "无法为通道“" + field.value("name").toString() + "”生成类型及范围均有效的数值，请手动输入样例。"; return false; }
            const auto type = field.value("type").toString(); quint64 bits = 0;
            if (type == "float32") { const float value = static_cast<float>(raw); quint32 word; std::memcpy(&word, &value, sizeof word); bits = word; }
            else if (type == "float64") std::memcpy(&bits, &raw, sizeof bits);
            else bits = type.startsWith("int") ? quint64(qint64(raw)) : quint64(raw);
            const bool big = field.value("byteOrder").toString(definition.value("byteOrder").toString()) == "big";
            const int width = ProtocolProfileValidator::typeBytes(type);
            bytes.replace(frame.value("payloadOffset").toInt() + field.value("offset").toInt(), width, stored(bits, width, big));
        }
        for (auto v : definition.value("conditions").toArray()) {
            const auto condition = v.toObject(); QByteArray expected; ProtocolProfileValidator::parseHex(condition.value("equalsHex").toString(), expected);
            bytes.replace(condition.value("offset").toInt(), expected.size(), expected);
        }
        const auto headerCheck = definition.value("headerChecksum").toObject();
        ProtocolChecksum headerCalculator(headerCheck.value("algorithm").toString());
        if (headerCalculator.width()) {
            const auto range = headerCheck.value("range").toObject(), field = headerCheck.value("field").toObject();
            bytes.replace(field.value("offset").toInt(), headerCalculator.width(), stored(headerCalculator.calculate(bytes.constData()
                + range.value("offset").toInt(), range.value("length").toInt()), headerCalculator.width(), field.value("byteOrder") == QJsonValue("big")));
        }
        const auto checksum = definition.value("checksum").toObject(); ProtocolChecksum calculator(checksum.value("algorithm").toString());
        if (calculator.width() > 0) {
            const auto range = checksum.value("range").toObject(), field = checksum.value("field").toObject();
            const int start = range.value("offset").toInt(), count = range.contains("endOffset") ? total - range.value("endOffset").toInt() - start : range.value("length").toInt();
            const int offset = field.value("from") == QJsonValue("end") ? total - field.value("offset").toInt() : field.value("offset").toInt();
            bytes.replace(offset, calculator.width(), stored(calculator.calculate(bytes.constData() + start, count), calculator.width(), field.value("byteOrder") == QJsonValue("big")));
        }
    }
    if (!profile.isKeyValue() && bytes.size() > frame.value("maxFrameBytes").toInt()) { error = "生成样例超过最大帧长，请调整规则。"; return false; }
    // Conditions can constrain payload bytes or conflict with CRC/length.
    // Validate through the real parser and do not claim arbitrary rules are
    // solvable. No generated invalid frame silently replaces the user's input.
    std::unique_ptr<CustomProtocolParser> parser(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(profile, nullptr, error))); if (!parser) return false;
    bool finite = true;
    QObject::connect(parser.get(), &CustomProtocolParser::channelsUpdated, [&finite](const QVector<double> &values) { for (double value : values) finite = finite && std::isfinite(value); });
    // In header-only mode the following header is necessary lookahead, not
    // part of the generated frame. Leave it pending for the next frame.
    QByteArray lookahead;
    if (profile.isDelimiterFramed() && frame.value("tailHex").toString().isEmpty())
        ProtocolProfileValidator::parseHex(frame.value("headerHex").toString(), lookahead);
    const int frameSize = bytes.size(); bytes += lookahead;
    const int consumed = parser->processData(bytes.constData(), bytes.size()); const auto stats = parser->stats();
    if (consumed != frameSize || stats.validFrames != 1 || stats.rejectedFrames || !finite) {
        error = "暂不能生成满足全部格式、固定条件和数值范围的有效样例，请手动输入；协议和原样例未改变。";
        if (!stats.lastError.isEmpty()) error += "\n" + stats.lastError;
        if (profile.isDelimiterFramed()) error += "\n请检查数据/保留字节/校验值是否包含配置的分隔标记；首尾分隔无法转义或区分数据内的相同标记。";
        return false;
    }
    output = bytes; error.clear(); return true;
}
}
