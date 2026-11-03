#include "CustomProtocolParser.h"
#include <QJsonArray>
#include <QTextCodec>
#include <cmath>
#include <cstring>
#include <limits>

namespace DataProtocols {
namespace {
quint64 unsignedValue(const char *data, int width, bool big) {
    quint64 result = 0;
    for (int i = 0; i < width; ++i) {
        const int index = big ? i : width - 1 - i;
        result = (result << 8) | static_cast<unsigned char>(data[index]);
    }
    return result;
}
int matchingSuffix(const QByteArray &bytes, int start, const QByteArray &header) {
    // Keep only a prefix that could complete the header on the next read.
    for (int n = qMin(header.size() - 1, bytes.size() - start); n > 0; --n)
        if (std::memcmp(bytes.constData() + bytes.size() - n, header.constData(), size_t(n)) == 0) return n;
    return 0;
}
QString hexBytes(const QByteArray &bytes) {
    QString result = bytes.left(16).toHex(' ').toUpper();
    if (bytes.isEmpty()) return QStringLiteral("（无字节）");
    if (bytes.size() > 16) result += QStringLiteral(" …（共 %1 字节）").arg(bytes.size());
    return result;
}
QString visibleText(QString text) {
    const bool shortened = text.size() > 64;
    text = text.left(64); text.replace('\\', "\\\\"); text.replace('\r', "\\r"); text.replace('\n', "\\n"); text.replace('\t', "\\t"); text.replace(QChar(0), "\\0");
    return QStringLiteral("“%1%2”").arg(text, shortened ? QStringLiteral("…") : QString());
}
int firstMismatch(const QByteArray &actual, const QByteArray &expected) {
    int offset = 0;
    while (offset < actual.size() && offset < expected.size() && actual[offset] == expected[offset]) ++offset;
    return offset;
}
QByteArray checksumBytes(quint32 value, int width, bool big) {
    QByteArray bytes(width, '\0');
    for (int i = 0; i < width; ++i) bytes[i] = char(value >> (8 * (big ? width - i - 1 : i)));
    return bytes;
}
int invalidUtf8Offset(const QByteArray &bytes) {
    for (int i = 0; i < bytes.size();) {
        const quint8 lead = quint8(bytes[i]);
        if (lead < 0x80) { ++i; continue; }
        const int width = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (!width || i + width > bytes.size()) return i;
        for (int j = 1; j < width; ++j) if ((quint8(bytes[i + j]) & 0xc0) != 0x80) return i + j;
        const quint8 second = quint8(bytes[i + 1]);
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
            (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) return i + 1;
        i += width;
    }
    return 0;
}
int utf8BomBytes(const QByteArray &line, const QString &decoded) {
    return line.startsWith("\xEF\xBB\xBF") && !decoded.startsWith(QChar(0xfeff)) ? 3 : 0;
}
}
CustomProtocolParser::CustomProtocolParser(const ProtocolProfile &profile, QObject *parent, bool confirmLiveSync)
    : ProtocolParser(parent), m_confirmLiveSync(confirmLiveSync),
      m_number(QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$")),
      m_spaces(QStringLiteral("\\s+")) {
    m_channelNames = profile.outputChannelNames();
    m_recordConverted = profile.recordsConvertedValues();
    m_markRangeNaN = profile.definition.value("outOfRangePolicy") == QJsonValue("nan-channel");
    const auto d = profile.definition, frame = d.value("frame").toObject();
    m_keyValue = profile.isKeyValue();
    m_text = d.value("type") == QJsonValue("text") || m_keyValue;
    m_limit = frame.value("maxFrameBytes").toInt();
    if (m_text) {
        m_ascii = frame.value("encoding") == QJsonValue("ascii");
        ProtocolProfileValidator::parseHex(frame.value("terminatorHex").toString(), m_ending);
        m_prefix = frame.value("prefix").toString(); m_suffix = frame.value("suffix").toString();
        m_delimiter = frame.value("delimiter").toString(); m_trim = frame.value("trimFields").toBool();
        m_whitespace = frame.value("delimiterMode") == QJsonValue("whitespace");
        if (m_keyValue) {
            m_groupTimeoutMs = frame.value("groupTimeoutMs").toInt(1000);
            m_unknownKeysIgnored = frame.value("unknownKeyPolicy") == QJsonValue("ignore");
            m_replaceDuplicateKeys = frame.value("duplicateKeyPolicy") == QJsonValue("replace");
        }
    } else {
        m_variable = frame.value("mode") == QJsonValue("length");
        m_delimiterFramed = profile.isDelimiterFramed();
        m_array = profile.isDynamicArray();
        m_payloadOffset = frame.value("payloadOffset").toInt();
        m_payloadEndOffset = frame.value("payloadEndOffset").toInt();
        m_size = frame.value("sizeBytes").toInt();
        m_minSize = (m_variable || m_delimiterFramed) ? frame.value("minFrameBytes").toInt() : m_size;
        const auto length = frame.value("lengthField").toObject();
        m_lengthOffset = length.value("offset").toInt();
        m_lengthWidth = ProtocolProfileValidator::typeBytes(length.value("type").toString());
        m_lengthBig = length.value("byteOrder") == QJsonValue("big");
        m_lengthUnit = length.value("unitBytes").toInt(1); m_lengthAdjustment = length.value("adjustment").toInt();
        ProtocolProfileValidator::parseHex(frame.value("headerHex").toString(), m_header);
        ProtocolProfileValidator::parseHex(frame.value("tailHex").toString(), m_tail);
        const auto check = d.value("checksum").toObject();
        const auto algorithm = check.value("algorithm").toString();
        m_checksumAlgorithm = algorithm;
        m_checksum = ProtocolChecksum(algorithm);
        m_checkStart = check.value("range").toObject().value("offset").toInt();
        m_checkLength = check.value("range").toObject().value("length").toInt();
        m_checkOffset = check.value("field").toObject().value("offset").toInt();
        m_checkToEnd = check.value("range").toObject().contains("endOffset");
        m_checkEndOffset = check.value("range").toObject().value("endOffset").toInt();
        m_checkFromEnd = check.value("field").toObject().value("from") == QJsonValue("end");
        m_checkBig = check.value("field").toObject().value("byteOrder") == QJsonValue("big");
        const auto headerCheck = d.value("headerChecksum").toObject();
        m_headerChecksumAlgorithm = headerCheck.value("algorithm").toString();
        m_headerChecksum = ProtocolChecksum(headerCheck.value("algorithm").toString());
        m_headerCheckStart = headerCheck.value("range").toObject().value("offset").toInt();
        m_headerCheckLength = headerCheck.value("range").toObject().value("length").toInt();
        m_headerCheckOffset = headerCheck.value("field").toObject().value("offset").toInt();
        m_headerCheckBig = headerCheck.value("field").toObject().value("byteOrder") == QJsonValue("big");
        for (auto v : d.value("conditions").toArray()) {
            const auto c = v.toObject(); QByteArray expected;
            ProtocolProfileValidator::parseHex(c.value("equalsHex").toString(), expected);
            m_conditions.append(qMakePair(c.value("offset").toInt(), expected));
        }
    }
    const QStringList types{"int8", "uint8", "int16", "uint16", "int32", "uint32", "float32", "float64"};
    if (m_array) {
        const auto array = d.value("array").toObject();
        m_arrayField.type = static_cast<ValueType>(types.indexOf(array.value("type").toString()));
        m_arrayField.width = ProtocolProfileValidator::typeBytes(array.value("type").toString());
        m_arrayField.big = d.value("byteOrder") == QJsonValue("big");
        m_arrayMin = array.value("minChannels").toInt(1); m_arrayMax = array.value("maxChannels").toInt(MaxChannels);
    }
    for (auto v : d.value("channels").toArray()) {
        const auto c = v.toObject(); Field f;
        if (m_array) f = m_arrayField;
        f.offset = m_text ? c.value("column").toInt() : frame.value("payloadOffset").toInt() + c.value("offset").toInt();
        if (m_keyValue) m_keyIndices.insert(c.value("key").toString(), m_fields.size());
        else if (m_text) m_textConfiguredColumns.insert(f.offset);
        if (!m_array) {
            f.type = static_cast<ValueType>(types.indexOf(c.value("type").toString()));
            f.width = ProtocolProfileValidator::typeBytes(c.value("type").toString());
        }
        f.big = c.value("byteOrder").toString(d.value("byteOrder").toString()) == "big";
        f.scale = c.value("scale").toDouble(); f.bias = c.value("bias").toDouble();
        f.hasMin = c.contains("min"); f.hasMax = c.contains("max");
        f.min = c.value("min").toDouble(); f.max = c.value("max").toDouble();
        if (m_array) m_arrayOverrides.insert(c.value("index").toInt(), f); else m_fields.append(f);
    }
    if (m_keyValue) {
        m_keyReceived.resize(m_fields.size()); m_groupRaw.resize(m_fields.size()); m_groupValues.resize(m_fields.size());
        resetKeyValueGroup();
    }
}
void CustomProtocolParser::resetKeyValueGroup() {
    m_receivedKeys = 0; m_keyReceived.fill(false); m_groupClock.invalidate();
}
bool CustomProtocolParser::decodeLineEnvelope(const QByteArray &line, QString &text, Failure &error) const {
    if (m_ascii) for (int i = 0; i < line.size(); ++i) if (quint8(line[i]) > 127) {
        error = {QStringLiteral("ASCII 编码错误：此字节为 %1；ASCII 仅允许 00–7F，请检查文本编码。").arg(hexBytes(line.mid(i, 1))), i}; return false;
    }
    QTextCodec::ConverterState state;
    text = QTextCodec::codecForName("UTF-8")->toUnicode(line.constData(), line.size(), &state);
    if (state.invalidChars || state.remainingChars) {
        error = {QStringLiteral("编码错误：文本不是完整有效的 UTF-8（含无效或截断的多字节字符）；请检查发送编码和帧边界。"), invalidUtf8Offset(line)}; return false;
    }
    if (!text.startsWith(m_prefix)) {
        const int bom = utf8BomBytes(line, text);
        const auto expected = m_prefix.toUtf8(); const auto actual = line.mid(bom, expected.size());
        error = {QStringLiteral("固定前缀不匹配：预期 %1（HEX %2），实际开头 %3（HEX %4）。")
            .arg(visibleText(m_prefix), hexBytes(expected), visibleText(text.left(m_prefix.size())), hexBytes(actual)), bom + firstMismatch(actual, expected), true};
        return false;
    }
    if (!text.endsWith(m_suffix) || text.size() < m_prefix.size() + m_suffix.size()) {
        const auto expected = m_suffix.toUtf8(); const int start = qMax(0, line.size() - expected.size()); const auto actual = line.mid(start);
        error = {QStringLiteral("固定后缀不匹配：预期 %1（HEX %2），实际结尾 HEX %3；后缀不包含结束符。")
            .arg(visibleText(m_suffix), hexBytes(expected), hexBytes(actual)), start + firstMismatch(actual, expected), true};
        return false;
    }
    return true;
}
QString CustomProtocolParser::valueFailure(double raw, const Field &f) const {
    if (std::isinf(raw)) return QStringLiteral("原始数值为无穷大（Inf），请检查数据类型、大小端或帧位置。");
    const double converted = raw * f.scale + f.bias;
    if (!std::isfinite(converted)) return QStringLiteral("换算溢出：原始值 %1 × 系数 %2 + 偏移 %3 不是有限数值。")
        .arg(QString::number(raw, 'g', 12), QString::number(f.scale, 'g', 12), QString::number(f.bias, 'g', 12));
    return QStringLiteral("换算后数值 %1 超出配置范围 [%2, %3]（原始值 %4）；当前策略为拒绝整帧。")
        .arg(QString::number(converted, 'g', 12), f.hasMin ? QString::number(f.min, 'g', 12) : QStringLiteral("−∞"),
             f.hasMax ? QString::number(f.max, 'g', 12) : QStringLiteral("+∞"), QString::number(raw, 'g', 12));
}
QString CustomProtocolParser::delimiterHint(const QString &token) const {
    if (m_whitespace) return QStringLiteral(" 请检查是否使用了空白分隔，以及每列是否都是数值。");
    for (const QString &candidate : {QString(","), QString(";"), QString("|"), QString("\t")})
        if (candidate != m_delimiter && token.contains(candidate)) return QStringLiteral(" 可能是分隔符不一致：当前为 %1，此列含 %2；请核对分隔符配置。")
            .arg(visibleText(m_delimiter), visibleText(candidate));
    return QStringLiteral(" 当前分隔符为 %1，请检查分隔符和数值格式。").arg(visibleText(m_delimiter));
}
int CustomProtocolParser::columnByteOffset(const QString &text, const QString &body, int column) const {
    int start = 0;
    if (m_whitespace) {
        auto matches = QRegularExpression(QStringLiteral("\\S+")).globalMatch(body);
        for (int i = 0; i <= column; ++i) { if (!matches.hasNext()) { start = body.size(); break; } start = matches.next().capturedStart(); }
    } else for (int i = 0; i < column; ++i) {
        const int separator = body.indexOf(m_delimiter, start);
        if (separator < 0) { start = body.size(); break; }
        start = separator + m_delimiter.size();
    }
    return text.left(m_prefix.size() + start).toUtf8().size();
}
bool CustomProtocolParser::decodeKeyValue(const QByteArray &line, int offset, QVector<double> &raw, QVector<double> &values, Failure &error) {
    QString text;
    if (!decodeLineEnvelope(line, text, error)) {
        // Keep the existing ignore policy only for non-target envelopes, not
        // invalid encodings that could corrupt an in-progress group.
        if (m_unknownKeysIgnored && error.nonTarget) return true;
        return false;
    }
    const auto body = text.mid(m_prefix.size(), text.size() - m_prefix.size() - m_suffix.size());
    const int bom = utf8BomBytes(line, text);
    const int separator = body.indexOf(m_delimiter);
    QString key = separator < 0 ? body : body.left(separator);
    if (m_trim) key = key.trimmed();
    if (!m_keyIndices.contains(key)) {
        if (m_unknownKeysIgnored) return true;
        error = {"未知通道键：" + visibleText(key) + "；请核对通道表的键（区分大小写），已清空未完成帧。", bom + m_prefix.toUtf8().size()}; return false;
    }
    if (separator < 0) { error = {"通道键“" + key + "”缺少键值分隔符 " + visibleText(m_delimiter), line.size() - m_suffix.toUtf8().size()}; return false; }
    const int index = m_keyIndices.value(key);
    auto token = body.mid(separator + m_delimiter.size()); if (m_trim) token = token.trimmed();
    double number, converted;
    const int valueOffset = bom + text.left(m_prefix.size() + separator + m_delimiter.size()).toUtf8().size();
    if (!parseTextNumber(token, number)) {
        error = {"通道键“" + key + "”的值 " + visibleText(token) + " 无效，预期有限数值或 NaN。已清空未完成帧。", valueOffset}; return false;
    }
    if (!convert(number, m_fields[index], converted)) {
        error = {"通道键“" + key + "”：" + valueFailure(number, m_fields[index]) + " 已清空未完成帧。", valueOffset}; return false;
    }
    if (m_receivedKeys && m_groupTimeoutMs > 0 && m_groupClock.elapsed() >= m_groupTimeoutMs) {
        reject(offset, "键值组帧超时，已丢弃未完成帧，从当前行重新收集"); resetKeyValueGroup();
    }
    if (m_keyReceived[index] && !m_replaceDuplicateKeys) {
        reject(offset, "收到重复通道键“" + key + "”，已丢弃未完成帧，从当前行重新收集"); resetKeyValueGroup();
    }
    if (!m_receivedKeys) m_groupClock.start();
    if (!m_keyReceived[index]) { m_keyReceived[index] = true; ++m_receivedKeys; }
    m_groupRaw[index] = number; m_groupValues[index] = converted;
    if (m_receivedKeys == m_fields.size()) { raw = m_groupRaw; values = m_groupValues; resetKeyValueGroup(); }
    return true;
}
bool CustomProtocolParser::parseTextNumber(const QString &token, double &number) const {
    if (isTextNaN(token)) {
        number = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (!m_number.match(token).hasMatch()) return false;
    bool ok = false;
    number = token.toDouble(&ok);
    return ok && std::isfinite(number);
}
bool CustomProtocolParser::convert(double raw, const Field &f, double &output) const {
    if (std::isnan(raw)) {
        output = raw;
        return true;
    }
    if (!std::isfinite(raw)) return false;
    output = raw * f.scale + f.bias;
    if (!std::isfinite(output)) return false;
    if ((!f.hasMin || output >= f.min) && (!f.hasMax || output <= f.max)) return true;
    if (!m_markRangeNaN) return false;
    output = std::numeric_limits<double>::quiet_NaN(); return true;
}
void CustomProtocolParser::reject(int offset, const QString &reason, bool checksum) {
    ++m_stats.rejectedFrames; if (checksum) ++m_stats.checksumFailures;
    m_stats.lastError = reason; emit frameRejected(m_stats.consumedBytes + quint64(offset), reason);
}
void CustomProtocolParser::reject(int frameOffset, const Failure &error, bool checksum) {
    reject(frameOffset + error.offset, error.message + QStringLiteral("（帧内偏移 %1；本帧起始字节 %2）")
        .arg(error.offset).arg(m_stats.consumedBytes + quint64(frameOffset)), checksum);
}
void CustomProtocolParser::publish(const QVector<double> &raw, const QVector<double> &values) {
    for (int i = 0; i < values.size(); ++i)
        if (std::isfinite(raw[i]) && std::isnan(values[i])) ++m_stats.outOfRangeValues;
    ++m_stats.validFrames; publishChannels(raw, values); emit frameDecoded(raw, values);
}
int CustomProtocolParser::processData(const char *data, int size) {
    if (!data || size <= 0) return 0;
    const auto bytes = QByteArray::fromRawData(data, size);
    const int consumed = m_text ? processText(bytes) : processBinary(bytes);
    m_stats.consumedBytes += quint64(consumed); return consumed;
}
bool CustomProtocolParser::decodeText(const QByteArray &line, QVector<double> &raw, QVector<double> &values, Failure &error) const {
    QString text; if (!decodeLineEnvelope(line, text, error)) return false;
    const int bom = utf8BomBytes(line, text);
    const auto body = text.mid(m_prefix.size(), text.size() - m_prefix.size() - m_suffix.size());
    const auto columns = m_whitespace ? body.trimmed().split(m_spaces, Qt::SkipEmptyParts) : body.split(m_delimiter, Qt::KeepEmptyParts);
    int outputCount = m_fields.size();
    for (int column = 0; column < columns.size(); ++column)
        if (!m_textConfiguredColumns.contains(column)) ++outputCount;
    if (outputCount == 0 || outputCount > MaxChannels) {
        error = QStringLiteral("文本帧输出通道数 %1 不在允许范围 1–%2 内；整帧拒绝，不截断数据").arg(outputCount).arg(MaxChannels);
        return false;
    }
    raw.reserve(outputCount); values.reserve(outputCount);
    for (int i = 0; i < m_fields.size(); ++i) {
        const auto &f = m_fields[i];
        if (f.offset >= columns.size()) { error = {QStringLiteral("通道 %1 缺少第 %2 列：实际只有 %3 列（配置列号 %4，从 0 开始）。%5")
            .arg(i + 1).arg(f.offset + 1).arg(columns.size()).arg(f.offset).arg(delimiterHint(body)), bom + text.left(text.size() - m_suffix.size()).toUtf8().size()}; return false; }
        auto token = columns[f.offset]; if (m_trim) token = token.trimmed();
        double number, converted;
        if (!parseTextNumber(token, number)) {
            error = {QStringLiteral("通道 %1（第 %2 列，列号 %3）的值 %4 无效，预期有限数值或 NaN。%5")
                .arg(i + 1).arg(f.offset + 1).arg(f.offset).arg(visibleText(token), delimiterHint(token)), bom + columnByteOffset(text, body, f.offset)}; return false;
        }
        if (!convert(number, f, converted)) {
            error = {QStringLiteral("通道 %1（第 %2 列）：%3").arg(i + 1).arg(f.offset + 1).arg(valueFailure(number, f)), bom + columnByteOffset(text, body, f.offset)}; return false;
        }
        raw.append(number); values.append(converted);
    }
    // Do not silently discard numeric columns merely because no custom rule
    // was entered for them. Requiring a valid value in EVERY column keeps the
    // implicit channel layout stable (empty/invalid columns never shift it).
    for (int column = 0; column < columns.size(); ++column) {
        if (m_textConfiguredColumns.contains(column)) continue;
        auto token = columns[column]; if (m_trim) token = token.trimmed();
        double number;
        if (!parseTextNumber(token, number)) {
            error = {QStringLiteral("未配置的第 %1 列（列号 %2）的值 %3 无效，预期有限数值或 NaN。%4")
                .arg(column + 1).arg(column).arg(visibleText(token), delimiterHint(token)), bom + columnByteOffset(text, body, column)}; return false;
        }
        raw.append(number); values.append(number);
    }
    return true;
}
int CustomProtocolParser::processText(const QByteArray &bytes) {
    int pos = 0;
    while (pos < bytes.size()) {
        const int end = bytes.indexOf(m_ending, pos);
        if (end < 0) {
            if (m_discardingLine || bytes.size() - pos > m_limit) {
                if (!m_discardingLine) { reject(pos, QStringLiteral("文本帧超过最大帧长，跳过至下一个结束符")); m_discardingLine = true; resetKeyValueGroup(); }
                const int kept = matchingSuffix(bytes, pos, m_ending);
                m_stats.discardedBytes += quint64(bytes.size() - pos - kept); return bytes.size() - kept;
            }
            break;
        }
        const int next = end + m_ending.size(), length = next - pos;
        QVector<double> raw, values; Failure error;
        if (m_discardingLine) { m_stats.discardedBytes += quint64(length); m_discardingLine = false; }
        else if (length > m_limit) { reject(pos, QStringLiteral("文本帧超过最大帧长")); m_stats.discardedBytes += quint64(length); resetKeyValueGroup(); }
        else if (!(m_keyValue ? decodeKeyValue(bytes.mid(pos, end - pos), pos, raw, values, error) : decodeText(bytes.mid(pos, end - pos), raw, values, error))) {
            reject(pos, error); m_stats.discardedBytes += quint64(length); resetKeyValueGroup();
        }
        else if (!raw.isEmpty()) publish(raw, values);
        pos = next;
    }
    return pos;
}
double CustomProtocolParser::readNumber(const char *data, const Field &f) {
    const auto bits = unsignedValue(data, f.width, f.big);
    if (f.type == Float32) { quint32 word = quint32(bits); float v; std::memcpy(&v, &word, 4); return v; }
    if (f.type == Float64) { double v; std::memcpy(&v, &bits, 8); return v; }
    if (f.type == Signed8 || f.type == Signed16 || f.type == Signed32)
        return (bits & (quint64(1) << (f.width * 8 - 1))) ? double(bits) - std::ldexp(1.0, f.width * 8) : double(bits);
    return double(bits);
}
bool CustomProtocolParser::decodeBinary(const char *frame, int size, QVector<double> &raw, QVector<double> &values, Failure &error, bool &checksumFailure) const {
    if (!m_tail.isEmpty() && std::memcmp(frame + size - m_tail.size(), m_tail.constData(), size_t(m_tail.size()))) {
        const int start = size - m_tail.size(); const auto actual = QByteArray::fromRawData(frame + start, m_tail.size());
        error = {QStringLiteral("帧尾不匹配：帧内 [%1, %2) 应为 %3，实际为 %4。请核对帧尾、总帧长及数据字节数（当前总帧长 %5）。")
            .arg(start).arg(size).arg(hexBytes(m_tail), hexBytes(actual)).arg(size), start + firstMismatch(actual, m_tail)}; return false;
    }
    for (const auto &condition : m_conditions)
        if (std::memcmp(frame + condition.first, condition.second.constData(), size_t(condition.second.size()))) {
            const auto actual = QByteArray::fromRawData(frame + condition.first, condition.second.size());
            error = {QStringLiteral("固定字节条件不匹配：帧内 [%1, %2) 预期 %3，实际 %4；请核对命令码/保留字节及条件偏移。")
                .arg(condition.first).arg(condition.first + condition.second.size()).arg(hexBytes(condition.second), hexBytes(actual)), condition.first + firstMismatch(actual, condition.second)}; return false;
        }
    if (m_checksum.width()) {
        const int position = m_checkFromEnd ? size - m_checkOffset : m_checkOffset;
        const int length = m_checkToEnd ? size - m_checkEndOffset - m_checkStart : m_checkLength;
        // Defensive checks even though both size endpoints were validated.
        if (position < 0 || position > size - m_checksum.width() || length <= 0 || m_checkStart > size - length) {
            error = {QStringLiteral("校验位置或范围超出当前 %1 字节帧：校验位置 %2，覆盖 [%3, %4)。请检查长度字段和帧结构。")
                .arg(size).arg(position).arg(m_checkStart).arg(m_checkStart + length), qBound(0, position, size)}; return false;
        }
        const auto calculated = m_checksum.calculate(frame + m_checkStart, length);
        if (calculated != unsignedValue(frame + position, m_checksum.width(), m_checkBig)) {
            checksumFailure = true;
            const auto expected = checksumBytes(calculated, m_checksum.width(), m_checkBig);
            const auto actual = QByteArray::fromRawData(frame + position, m_checksum.width());
            error = {QStringLiteral("数据校验失败（%1）：帧内 [%2, %3) 预期 %4，实际 %5；校验覆盖 [%6, %7)，%8存储。请核对算法、大小端及发送内容。")
                .arg(m_checksumAlgorithm.toUpper()).arg(position).arg(position + m_checksum.width()).arg(hexBytes(expected), hexBytes(actual))
                .arg(m_checkStart).arg(m_checkStart + length).arg(m_checkBig ? "大端" : "小端"), position + firstMismatch(actual, expected)}; return false;
        }
    }
    int count = m_fields.size();
    if (m_array) {
        const int bytes = size - m_payloadOffset - m_payloadEndOffset;
        if (bytes <= 0 || bytes % m_arrayField.width) { error = {QStringLiteral("动态数据区 [%1, %2) 共 %3 字节，不能完整组成 %4 字节数值（余 %5 字节）；请检查类型、长度和保留字节，不截断尾部。")
            .arg(m_payloadOffset).arg(size - m_payloadEndOffset).arg(bytes).arg(m_arrayField.width).arg(qMax(0, bytes) % m_arrayField.width), m_payloadOffset}; return false; }
        count = bytes / m_arrayField.width;
        if (count < m_arrayMin || count > m_arrayMax) { error = QStringLiteral("动态数组 %1 路超出允许的 %2–%3 路，拒绝整帧，不截断通道").arg(count).arg(m_arrayMin).arg(m_arrayMax); return false; }
    }
    raw.reserve(count); values.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto f = m_array ? m_arrayOverrides.value(i, m_arrayField) : m_fields[i];
        const double number = readNumber(frame + (m_array ? m_payloadOffset + i * f.width : f.offset), f);
        double converted;
        if (!convert(number, f, converted)) {
            const int position = m_array ? m_payloadOffset + i * f.width : f.offset;
            error = {QStringLiteral("通道 %1，帧内 [%2, %3)，原始 HEX %4：%5")
                .arg(i + 1).arg(position).arg(position + f.width).arg(hexBytes(QByteArray::fromRawData(frame + position, f.width)), valueFailure(number, f)), position}; return false;
        }
        raw.append(number); values.append(converted);
    }
    return true;
}
int CustomProtocolParser::binaryFrameSize(const char *frame, int available, Failure &error, bool &checksumFailure) const {
    if (m_headerChecksum.width()) {
        const int needed = qMax(m_headerCheckStart + m_headerCheckLength, m_headerCheckOffset + m_headerChecksum.width());
        if (available < needed) return 0;
        const auto calculated = m_headerChecksum.calculate(frame + m_headerCheckStart, m_headerCheckLength);
        if (calculated != unsignedValue(frame + m_headerCheckOffset, m_headerChecksum.width(), m_headerCheckBig)) {
            checksumFailure = true;
            const auto expected = checksumBytes(calculated, m_headerChecksum.width(), m_headerCheckBig);
            const auto actual = QByteArray::fromRawData(frame + m_headerCheckOffset, m_headerChecksum.width());
            error = {QStringLiteral("帧头校验失败（%1，长度字段不可信）：帧内 [%2, %3) 预期 %4，实际 %5；覆盖 [%6, %7)。")
                .arg(m_headerChecksumAlgorithm.toUpper()).arg(m_headerCheckOffset).arg(m_headerCheckOffset + m_headerChecksum.width())
                .arg(hexBytes(expected), hexBytes(actual)).arg(m_headerCheckStart).arg(m_headerCheckStart + m_headerCheckLength), m_headerCheckOffset + firstMismatch(actual, expected)}; return -1;
        }
    }
    if (!m_variable) return m_size;
    if (available < m_lengthOffset + m_lengthWidth) return 0;
    // Never allocate according to an untrusted length or narrow it to int
    // before checking the configured limits.
    const auto length = unsignedValue(frame + m_lengthOffset, m_lengthWidth, m_lengthBig);
    const qint64 total = qint64(length) * m_lengthUnit + m_lengthAdjustment;
    if (total < m_minSize || total > m_limit) {
        error = {QStringLiteral("长度字段错误：帧内 [%1, %2)，HEX %3，%4读得 n=%5；总帧长 T=%5×%6+(%7)=%8，不在允许范围 %9–%10 字节内。请核对长度含义、类型和大小端。")
            .arg(m_lengthOffset).arg(m_lengthOffset + m_lengthWidth).arg(hexBytes(QByteArray::fromRawData(frame + m_lengthOffset, m_lengthWidth)), m_lengthBig ? "大端" : "小端")
            .arg(length).arg(m_lengthUnit).arg(m_lengthAdjustment).arg(total).arg(m_minSize).arg(m_limit), m_lengthOffset}; return -1;
    }
    if (m_array) {
        const int payload = int(total) - m_payloadOffset - m_payloadEndOffset;
        if (payload <= 0 || payload % m_arrayField.width) {
            error = {QStringLiteral("长度字段与动态数据区不一致：总帧长 %1，数据区 [%2, %3) 共 %4 字节，不能完整组成 %5 字节数值（余 %6 字节）。请核对长度含义、数据类型和保留字节。")
                .arg(total).arg(m_payloadOffset).arg(total - m_payloadEndOffset).arg(payload).arg(m_arrayField.width).arg(qMax(0, payload) % m_arrayField.width), m_lengthOffset}; return -1;
        }
        const int count = payload / m_arrayField.width;
        if (count < m_arrayMin || count > m_arrayMax) {
            error = {QStringLiteral("长度字段对应 %1 路动态数组，超出允许的 %2–%3 路（数据区 %4 字节，每路 %5 字节）；拒绝整帧，不截断通道。")
                .arg(count).arg(m_arrayMin).arg(m_arrayMax).arg(payload).arg(m_arrayField.width), m_lengthOffset}; return -1;
        }
    }
    return int(total);
}
int CustomProtocolParser::findCompleteBinaryFrame(const QByteArray &bytes, int start) const {
    // An embedded marker alone is NOT sufficient to abandon a half-packet.
    // A replacement must pass its length, tail, conditions, checksums and
    // numeric rules. Probe without publishing or counting payload markers as
    // rejected frames. The original candidate stays buffered if none passes.
    for (int head = bytes.indexOf(m_header, start); head >= 0; head = bytes.indexOf(m_header, head + 1)) {
        Failure error; bool checksum = false;
        const int size = binaryFrameSize(bytes.constData() + head, bytes.size() - head, error, checksum);
        if (size <= 0 || size > bytes.size() - head) continue;
        QVector<double> raw, values;
        if (decodeBinary(bytes.constData() + head, size, raw, values, error, checksum)) return head;
    }
    return -1;
}
int CustomProtocolParser::processTailDelimitedFixed(const QByteArray &bytes) {
    int pos = 0, search = 0;
    const bool checkSpan = !m_checksum.width();
    constexpr int confirmationFrames = 3;
    while (pos < bytes.size()) {
        int tail;
        if (checkSpan) {
            if (!m_fixedTailDiscardedBytes && bytes.size() - pos < m_size) return pos;
            // Check the expected position first. A matching marker inside a
            // correctly sized payload is still ordinary data.
            if (!m_fixedTailDiscardedBytes &&
                std::memcmp(bytes.constData() + pos + m_size - m_tail.size(), m_tail.constData(), size_t(m_tail.size())) == 0) {
                tail = pos + m_size - m_tail.size();
            } else {
                m_fixedTailValidStreak = 0;
                // The last m_size bytes before a later tail might splice two
                // damaged frames. Discard the whole span and resync at its tail.
                tail = bytes.indexOf(m_tail, pos);
                if (tail < 0) {
                    const int consumed = bytes.size() - matchingSuffix(bytes, pos, m_tail);
                    const auto discarded = quint64(consumed - pos);
                    m_fixedTailDiscardedBytes += discarded;
                    m_stats.discardedBytes += discarded;
                    return consumed;
                }
                const int end = tail + m_tail.size();
                const auto discarded = quint64(end - pos);
                reject(tail, QStringLiteral("仅帧尾固定帧长度错误。到帧尾为止的片段共 %1 字节，配置要求 %2 字节。已丢弃该片段并从帧尾重新同步。")
                    .arg(m_fixedTailDiscardedBytes + discarded).arg(m_size));
                m_fixedTailDiscardedBytes = 0;
                m_stats.discardedBytes += discarded;
                pos = end;
                continue;
            }
        } else {
            // A checksum can validate a complete fixed-length candidate
            // even when a connection starts mid-frame or after a corrupt prefix.
            tail = bytes.indexOf(m_tail, search);
            if (tail < 0) break;
        }
        const int end = tail + m_tail.size(), start = end - m_size;
        search = tail + 1;
        // This may be the end of the initial partial frame OR a marker inside
        // an incomplete payload. Keep preceding bytes for later candidates.
        if (start < pos) continue;
        QVector<double> raw, values; Failure error; bool checksum = false;
        if (!decodeBinary(bytes.constData() + start, m_size, raw, values, error, checksum)) {
            reject(start, error, checksum);
            if (checkSpan) {
                m_fixedTailValidStreak = 0;
                m_stats.discardedBytes += quint64(m_size);
                pos = end;
            }
            continue;
        }
        m_stats.discardedBytes += quint64(start - pos);
        if (checkSpan && m_confirmLiveSync && m_fixedTailValidStreak < confirmationFrames) {
            ++m_fixedTailValidStreak;
            if (m_fixedTailValidStreak < confirmationFrames) {
                // Do not replay tentative frames with a later read's timestamp.
                m_stats.discardedBytes += quint64(m_size);
                pos = end;
                continue;
            }
        }
        publish(raw, values); pos = end; search = end;
    }
    // Retain enough bytes to form a full fixed frame ending at a future tail,
    // including split tails. Garbage cannot grow the caller's buffer forever.
    const int consumed = qMax(pos, bytes.size() - (m_size - 1));
    m_stats.discardedBytes += quint64(consumed - pos);
    return consumed;
}
int CustomProtocolParser::processDelimitedArray(const QByteArray &bytes) {
    // A header-only stream closes a frame at the NEXT header, which is kept
    // as the start of the following frame. Never guess boundaries by timing.
    const bool headerOnly = m_tail.isEmpty();
    const auto &delimiter = headerOnly ? m_header : m_tail;
    int pos = 0, search = m_delimiterScanOffset;
    while (pos < bytes.size()) {
        if (m_delimiterDiscarding) {
            const int marker = bytes.indexOf(m_tail, pos);
            if (marker < 0) {
                const int consumed = bytes.size() - matchingSuffix(bytes, pos, m_tail);
                m_stats.discardedBytes += quint64(consumed - pos); m_delimiterScanOffset = 0; return consumed;
            }
            const int end = marker + m_tail.size(); m_stats.discardedBytes += quint64(end - pos);
            pos = end; search = pos; m_delimiterDiscarding = false; continue;
        }
        if (!m_header.isEmpty()) {
            const int head = bytes.indexOf(m_header, pos);
            if (head < 0) {
                const int consumed = bytes.size() - matchingSuffix(bytes, pos, m_header);
                m_stats.discardedBytes += quint64(consumed - pos); m_delimiterScanOffset = 0; return consumed;
            }
            m_stats.discardedBytes += quint64(head - pos); pos = head;
        }
        const int scan = qMax(search, pos + m_header.size());
        const int marker = bytes.indexOf(delimiter, scan);
        // With both markers, a new header before the tail abandons an
        // unfinished frame. Delimiters must not occur unescaped in payload.
        const int nextHeader = !headerOnly && !m_header.isEmpty() ? bytes.indexOf(m_header, scan) : -1;
        if (nextHeader >= 0 && (marker < 0 || nextHeader < marker)) {
            // A tail may itself start with the header. Do not mistake a split
            // tail prefix for a new header just because this read ended early.
            const int partialTail = matchingSuffix(bytes, scan, m_tail);
            if (partialTail && bytes.size() - partialTail <= nextHeader && bytes.size() - pos <= m_limit) {
                m_delimiterScanOffset = bytes.size() - partialTail - pos; return pos;
            }
            reject(pos, Failure(QStringLiteral("帧尾 %1 缺失：在帧内偏移 %2 先遇到新的帧头 %3，丢弃不完整帧并重新同步。数据内不能包含分隔标记。")
                .arg(hexBytes(m_tail)).arg(nextHeader - pos).arg(hexBytes(m_header)), nextHeader - pos));
            m_stats.discardedBytes += quint64(nextHeader - pos); pos = nextHeader; search = pos; continue;
        }
        if (marker < 0) {
            // Header lookahead does not belong to the preceding frame.
            const int lookahead = headerOnly ? m_header.size() - 1 : 0;
            if (bytes.size() - pos > m_limit + lookahead) {
                reject(pos, QStringLiteral("首尾分隔帧超过最大帧长 %1 字节，仍未匹配%2 %3；丢弃本帧并重新同步，不截取尾部数值。")
                    .arg(m_limit).arg(headerOnly ? "下一个帧头" : "帧尾", hexBytes(delimiter)));
                if (!m_header.isEmpty()) {
                    const int next = bytes.indexOf(m_header, pos + 1);
                    if (next >= 0) { m_stats.discardedBytes += quint64(next - pos); pos = next; search = pos; continue; }
                    const int consumed = bytes.size() - matchingSuffix(bytes, pos, m_header);
                    m_stats.discardedBytes += quint64(consumed - pos); m_delimiterScanOffset = 0; return consumed;
                }
                m_delimiterDiscarding = true;
                const int consumed = bytes.size() - matchingSuffix(bytes, pos, m_tail);
                m_stats.discardedBytes += quint64(consumed - pos); m_delimiterScanOffset = 0; return consumed;
            }
            // Only revisit the possible split delimiter, not the entire frame
            // after each incoming byte (bounded, linear delimiter scanning).
            m_delimiterScanOffset = qMax(pos + m_header.size(), bytes.size() - qMax(m_header.size(), m_tail.size()) + 1) - pos;
            return pos;
        }
        const int end = marker + (headerOnly ? 0 : m_tail.size()), size = end - pos;
        QVector<double> raw, values; Failure error; bool checksum = false;
        bool valid = false;
        if (size < m_minSize || size > m_limit) error = {QStringLiteral("首尾分隔帧字节数量错误：实际 %1，允许 %2–%3 字节；%4 %5 从帧内偏移 %6 开始。请检查分隔字节、数据类型及通道数量。")
            .arg(size).arg(m_minSize).arg(m_limit).arg(headerOnly ? "下一帧头" : "帧尾", hexBytes(delimiter)).arg(marker - pos), marker - pos};
        else valid = decodeBinary(bytes.constData() + pos, size, raw, values, error, checksum);
        if (valid) publish(raw, values);
        else {
            reject(pos, error, checksum);
            // Retry an intact later header inside a rejected fragment. Never
            // strip a few payload bytes to force array alignment.
            const int next = m_header.isEmpty() ? -1 : bytes.indexOf(m_header, pos + 1);
            if (next >= 0 && next < end) { m_stats.discardedBytes += quint64(next - pos); pos = next; search = pos; continue; }
            m_stats.discardedBytes += quint64(size);
        }
        pos = end; search = end;
    }
    m_delimiterScanOffset = 0; return pos;
}
int CustomProtocolParser::processBinary(const QByteArray &bytes) {
    if (m_delimiterFramed) return processDelimitedArray(bytes);
    // Validation guarantees at least one delimiter for fixed frames, and a
    // header plus length field for variable frames.
    if (m_header.isEmpty()) return processTailDelimitedFixed(bytes);
    int pos = 0;
    while (pos < bytes.size()) {
        const int head = bytes.indexOf(m_header, pos);
        if (head < 0) {
            const int end = bytes.size() - matchingSuffix(bytes, pos, m_header);
            m_stats.discardedBytes += quint64(end - pos); return end;
        }
        m_stats.discardedBytes += quint64(head - pos); pos = head;
        QVector<double> raw, values; Failure error; bool checksum = false;
        const int size = binaryFrameSize(bytes.constData() + pos, bytes.size() - pos, error, checksum);
        if (size < 0) {
            reject(pos, error, checksum); ++m_stats.discardedBytes; ++pos; continue;
        }
        if (!size) break;
        if (bytes.size() - pos < size) {
            // A plausible false length must not block subsequent intact
            // packets indefinitely. Do not guess from a header pattern alone.
            const int next = m_variable ? findCompleteBinaryFrame(bytes, pos + 1) : -1;
            if (next < 0) break;
            reject(pos, QStringLiteral("前方帧不完整或错位，已找到后续完整有效帧并恢复同步"));
            m_stats.discardedBytes += quint64(next - pos); pos = next; continue;
        }
        if (decodeBinary(bytes.constData() + pos, size, raw, values, error, checksum)) { publish(raw, values); pos += size; }
        else {
            reject(pos, error, checksum);
            // Retry overlapping headers instead of dropping the candidate's
            // entire claimed size, which could include valid following frames.
            ++m_stats.discardedBytes; ++pos;
        }
    }
    return pos;
}
QStringList CustomProtocolParser::testInputDiagnostics(const QByteArray &input, const QByteArray &remaining) const {
    QStringList messages;
    auto at = [&](quint64 offset, const QString &reason) { messages.append(QStringLiteral("字节偏移 %1：%2").arg(offset).arg(reason)); };
    const quint64 base = m_stats.consumedBytes;
    if (m_text) {
        if (!remaining.isEmpty()) {
            QString hint;
            if (m_ending == QByteArray::fromHex("0D0A") && remaining.endsWith('\n') && !remaining.endsWith("\r\n"))
                hint = QStringLiteral(" 末尾只有 LF（0A），当前配置要求 CRLF（0D 0A）。");
            else if (remaining.endsWith("\\r\\n") || remaining.endsWith("\\n") || remaining.endsWith("\\r"))
                hint = QStringLiteral(" 末尾看起来是字面反斜杠转义字符，不是真正的结束符字节；请检查样例格式/发送内容。");
            at(input.size(), QStringLiteral("结束符缺失或半帧：从字节 %1 起剩余 %2 字节，测试输入已结束，尚未匹配结束符 %3；实际末尾 %4。%5 实际接收时会等待后续字节，本测试不会自动补齐。")
                .arg(base).arg(remaining.size()).arg(hexBytes(m_ending), hexBytes(remaining.right(12)), hint));
        }
        if (m_keyValue && m_receivedKeys) {
            QStringList missing;
            for (int i = 0; i < m_keyReceived.size() && missing.size() < 12; ++i) if (!m_keyReceived[i])
                for (auto it = m_keyIndices.cbegin(); it != m_keyIndices.cend(); ++it) if (it.value() == i) missing.append(visibleText(it.key()));
            at(input.size(), QStringLiteral("键值组帧未完成：已收到 %1/%2 个通道，仍缺键 %3%4；必须收齐配置键才输出一帧。请核对键、大小写及键值分隔符 %5。")
                .arg(m_receivedKeys).arg(m_fields.size()).arg(missing.join("、"), m_fields.size() - m_receivedKeys > 12 ? "…" : "", visibleText(m_delimiter)));
        } else if (m_keyValue && !m_stats.validFrames && !m_stats.rejectedFrames && remaining.isEmpty()) {
            at(0, QStringLiteral("未得到完整键值帧：输入行未匹配配置的键或前后缀，被“忽略未知键/非目标行”规则忽略。请核对键值分隔符 %1、通道键和前后缀。")
                .arg(visibleText(m_delimiter)));
        }
    } else if (m_delimiterFramed) {
        if (!m_header.isEmpty() && input.indexOf(m_header) < 0)
            at(firstMismatch(input.left(m_header.size()), m_header), QStringLiteral("未找到帧头 %1；输入开头实际为 %2，请核对所选帧头或样例边界。")
                .arg(hexBytes(m_header), hexBytes(input.left(m_header.size()))));
        if (m_delimiterDiscarding) at(input.size(), QStringLiteral("超长帧已丢弃，仍在等待帧尾 %1 重新同步；不会把超长帧的剩余后半段当作新帧。").arg(hexBytes(m_tail)));
        else if (m_tail.isEmpty()) {
            if (!remaining.isEmpty() && (remaining != m_header || !m_stats.validFrames)) at(input.size(), QStringLiteral("仅帧头分隔：从字节 %1 起剩余 %2 字节，需要完整数据及下一个帧头 %3 才能确定本帧结束。实际接收继续等待；不会按超时或当前字节数猜测帧尾。生成样例会自动附加一个下一帧头。")
                .arg(base).arg(remaining.size()).arg(hexBytes(m_header)));
        } else if (!remaining.isEmpty()) at(input.size(), QStringLiteral("帧尾缺失或半帧：从字节 %1 起剩余 %2 字节，尚未匹配完整帧尾 %3，实际末尾 %4。测试已结束，实际接收会等待后续字节；不自动补齐或截取数值。")
            .arg(base).arg(remaining.size()).arg(hexBytes(m_tail), hexBytes(remaining.right(12))));
    } else {
        bool explained = false;
        if (m_fixedTailDiscardedBytes) {
            at(input.size(), QStringLiteral("固定长度帧已超过预期长度，已丢弃 %1 字节。尚未找到帧尾 %2，收到完整帧尾后将恢复同步。")
                .arg(m_fixedTailDiscardedBytes).arg(hexBytes(m_tail)));
            explained = true;
        }
        if (!m_header.isEmpty() && input.indexOf(m_header) < 0) {
            const int partial = matchingSuffix(input, 0, m_header);
            if (partial) at(input.size(), QStringLiteral("帧头不完整：输入末尾从字节 %1 起只有 %2，完整帧头应为 %3，尚缺 %4 字节。")
                .arg(input.size() - partial).arg(hexBytes(input.right(partial)), hexBytes(m_header)).arg(m_header.size() - partial));
            else at(firstMismatch(input.left(m_header.size()), m_header), QStringLiteral("未找到帧头 %1；输入开头实际为 %2。可能是帧头配置错误、样例从半帧开始，或选择了错误的协议/样例格式。")
                .arg(hexBytes(m_header), hexBytes(input.left(m_header.size()))));
            explained = true;
        }
        if (!explained && m_header.isEmpty() && !m_tail.isEmpty() && !m_stats.validFrames && !m_stats.rejectedFrames) {
            const int tail = input.indexOf(m_tail);
            if (tail < 0) at(input.size(), QStringLiteral("未找到帧尾 %1；当前为仅帧尾分帧，固定总帧长 %2 字节，输入共 %3 字节，末尾实际为 %4。请检查帧尾及字节数量。")
                .arg(hexBytes(m_tail)).arg(m_size).arg(input.size()).arg(hexBytes(input.right(m_tail.size()))));
            else if (tail + m_tail.size() < m_size) at(tail, QStringLiteral("帧尾标记前字节不足：首个标记 %1 在 [%2, %3)，截至此处只有 %3 字节，配置总帧长为 %4。可能从半帧开始，或帧长/标记配置不符。")
                .arg(hexBytes(m_tail)).arg(tail).arg(tail + m_tail.size()).arg(m_size));
            explained = !messages.isEmpty();
        }
        if (!remaining.isEmpty() && !explained) {
            const int head = m_header.isEmpty() ? 0 : remaining.indexOf(m_header);
            if (head < 0) {
                at(base, QStringLiteral("末尾留下不完整帧头：实际 %1，预期完整帧头 %2；测试输入已结束。")
                    .arg(hexBytes(remaining), hexBytes(m_header)));
            } else {
                const int available = remaining.size() - head;
                Failure error; bool checksum = false;
                const int expected = binaryFrameSize(remaining.constData() + head, available, error, checksum);
                if (expected == 0) {
                    const int needed = qMax(m_header.size(), qMax(m_lengthOffset + m_lengthWidth,
                        m_headerChecksum.width() ? qMax(m_headerCheckOffset + m_headerChecksum.width(), m_headerCheckStart + m_headerCheckLength) : 0));
                    at(base + head + available, QStringLiteral("帧头后字段不完整：从字节 %1 起仅有 %2 字节，读取长度字段/帧头校验至少需要 %3 字节，尚缺 %4；请检查样例是否被截断。")
                        .arg(base + head).arg(available).arg(needed).arg(qMax(0, needed - available)));
                } else if (expected > available) {
                    QString length;
                    if (m_variable) length = QStringLiteral(" 长度字段在帧内 [%1, %2)，HEX %3，%4读得 n=%5，T=n×%6+(%7)。")
                        .arg(m_lengthOffset).arg(m_lengthOffset + m_lengthWidth)
                        .arg(hexBytes(remaining.mid(head + m_lengthOffset, m_lengthWidth)), m_lengthBig ? "大端" : "小端")
                        .arg(unsignedValue(remaining.constData() + head + m_lengthOffset, m_lengthWidth, m_lengthBig)).arg(m_lengthUnit).arg(m_lengthAdjustment);
                    at(base + head + available, QStringLiteral("字节数量不足：从字节 %1 起的帧应有 %2 字节，实际仅 %3，尚缺 %4。%5 请检查数据数量、总帧长及长度字段含义；不自动补零。")
                        .arg(base + head).arg(expected).arg(available).arg(expected - available).arg(length));
                } else if (expected < 0) at(base + head + error.offset, error.message);
            }
        }
        if (m_stats.discardedBytes && !m_stats.rejectedFrames && messages.isEmpty())
            messages.append(QStringLiteral("同步提示：共跳过 %1 个无法组成有效帧的字节，可能是前后杂字节或接收从半帧开始；已解析出的有效帧仍保留。请核对样例边界及帧头/帧尾。")
                .arg(m_stats.discardedBytes));
    }
    if (m_stats.outOfRangeValues) messages.append(QStringLiteral("通道超限：%1 个换算结果超出配置范围，已按策略标记为 NaN，其他通道保留；这不是帧结构/校验错误。具体通道见上方结果表。")
        .arg(m_stats.outOfRangeValues));
    return messages;
}
} // namespace DataProtocols
