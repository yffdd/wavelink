#include "ProtocolProfile.h"
#include "ProtocolChecksum.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QMap>
#include <QUuid>
#include <cmath>

namespace DataProtocols {
QJsonArray ProtocolProfile::outputChannels(int count) const {
    count = qBound(0, count, MaxChannels);
    const auto configured = definition.value("channels").toArray();
    QJsonArray result; QSet<int> columns; QSet<QString> ids;
    if (isDynamicArray()) {
        count = qMin(count, definition.value("array").toObject().value("maxChannels").toInt(MaxChannels));
        QMap<int, QJsonObject> overrides;
        for (const auto &value : configured) {
            const auto field = value.toObject(); overrides.insert(field.value("index").toInt(), field);
            ids.insert(field.value("id").toString());
        }
        for (int index = 0; index < count; ++index) {
            if (overrides.contains(index)) { result.append(overrides[index]); continue; }
            const QString base = QStringLiteral("auto-array-%1").arg(index);
            QString id = base; int suffix = 0;
            while (ids.contains(id)) id = base + QStringLiteral("_%1").arg(++suffix);
            ids.insert(id);
            result.append(QJsonObject{{"id", id}, {"name", QStringLiteral("Ch%1").arg(index + 1)},
                {"unit", QString()}, {"index", index}, {"scale", 1.0}, {"bias", 0.0}});
        }
        return result;
    }
    for (const auto &value : configured) {
        const auto field = value.toObject();
        columns.insert(field.value("column").toInt()); ids.insert(field.value("id").toString());
        if (result.size() < count) result.append(field);
    }
    if (definition.value("type") != QJsonValue("text")) return result;
    for (int column = 0; result.size() < count; ++column) {
        if (columns.contains(column)) continue;
        const QString base = QStringLiteral("auto-column-%1").arg(column);
        QString id = base; int suffix = 0;
        while (ids.contains(id)) id = base + QStringLiteral("_%1").arg(++suffix);
        ids.insert(id);
        result.append(QJsonObject{{"id", id}, {"name", QStringLiteral("Ch%1").arg(column + 1)},
            {"unit", QString()}, {"column", column}, {"scale", 1.0}, {"bias", 0.0}});
    }
    return result;
}
QStringList ProtocolProfile::outputChannelNames(int count) const {
    QStringList result;
    for (const auto &value : outputChannels(count)) result.append(value.toObject().value("name").toString());
    return result;
}
namespace {
bool fail(QString &error, const QString &message) { error = message; return false; }
bool keys(const QJsonObject &object, const QStringList &allowed, QString &error) {
    for (auto i = object.begin(); i != object.end(); ++i)
        if (!allowed.contains(i.key())) return fail(error, QStringLiteral("不支持的配置项：%1").arg(i.key()));
    return true;
}
bool integer(const QJsonObject &object, const QString &key, int min, int max, int &value, QString &error) {
    const auto v = object.value(key);
    const double d = v.toDouble();
    if (!v.isDouble() || !std::isfinite(d) || d < min || d > max || std::floor(d) != d)
        return fail(error, QStringLiteral("%1 必须是 %2–%3 范围内的整数").arg(key).arg(min).arg(max));
    value = static_cast<int>(d); return true;
}
bool finite(const QJsonObject &object, const QString &key, QString &error) {
    if (!object.value(key).isDouble() || !std::isfinite(object.value(key).toDouble()))
        return fail(error, QStringLiteral("%1 必须是有限数值").arg(key));
    return true;
}
bool text(const QJsonObject &object, const QString &key, int max, bool required, QString &error) {
    const auto v = object.value(key);
    if (!required && v.isUndefined()) return true;
    if (!v.isString() || v.toString().size() > max || (required && v.toString().trimmed().isEmpty()))
        return fail(error, QStringLiteral("%1 文本为空、类型错误或超过 %2 个字符").arg(key).arg(max));
    for (auto ch : v.toString())
        if (ch.isNull() || (ch.category() == QChar::Other_Control &&
            ((key != "description" && key != "prefix" && key != "suffix") || (ch != '\n' && ch != '\t' && ch != '\r'))))
            return fail(error, QStringLiteral("%1 含无效控制字符").arg(key));
    return true;
}
bool uuid(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return pattern.match(id).hasMatch() && !QUuid(id).isNull();
}
QString normalizedId(const QString &id) { return QUuid(id).toString(QUuid::WithoutBraces); }
bool byteOrder(const QString &value) { return value == "little" || value == "big"; }
bool objectAt(const QJsonObject &object, const QString &key, QJsonObject &value, QString &error) {
    if (!object.value(key).isObject()) return fail(error, QStringLiteral("%1 必须是对象").arg(key));
    value = object.value(key).toObject(); return true;
}
bool hexAt(QJsonObject &object, const QString &key, QByteArray &bytes, int limit, bool required, QString &error) {
    if (!required && !object.contains(key)) object.insert(key, QString());
    if (!object.value(key).isString() || !ProtocolProfileValidator::parseHex(object.value(key).toString(), bytes)
        || bytes.size() > limit || (required && bytes.isEmpty()))
        return fail(error, QStringLiteral("%1 必须是有效 HEX（最多 %2 字节）").arg(key).arg(limit));
    object.insert(key, QString::fromLatin1(bytes.toHex().toUpper())); return true;
}
bool within(int offset, int length, int size) { return offset >= 0 && length > 0 && offset <= size && length <= size - offset; }
bool overlap(int a, int an, int b, int bn) { return a < b + bn && b < a + an; }
// Bound JSON nesting before Qt allocates the tree; strings/escapes do not count.
bool boundedJson(const QByteArray &bytes) {
    int depth = 0; bool quoted = false, escaped = false;
    for (char c : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return false; }
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return depth == 0 && !quoted;
}
const QStringList builtinIds{QStringLiteral("builtin.raw"), QStringLiteral("builtin.firewater"),
    QStringLiteral("builtin.justfloat"), QStringLiteral("builtin.amu4")};
} // namespace

ProtocolSelection ProtocolSelection::fromLegacy(int index, bool endian) {
    ProtocolSelection result;
    result.id = builtinIds.value(index, builtinIds[0]);
    result.bigEndian = index == 2 && endian;
    return result;
}
int ProtocolSelection::legacyIndex() const { return kind == "builtin" ? builtinIds.indexOf(id) : -1; }
QJsonObject ProtocolSelection::toJson() const {
    QJsonObject result{{"kind", kind}, {"id", id}};
    if (id == "builtin.justfloat" && kind == "builtin") result.insert("bigEndian", bigEndian);
    return result;
}
bool ProtocolProfileValidator::parseHex(const QString &text, QByteArray &output) {
    output.clear(); QString compact;
    for (auto ch : text) {
        if (ch.isSpace()) continue;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
        compact += ch;
    }
    if (compact.size() % 2) return false;
    output = QByteArray::fromHex(compact.toLatin1()); return true;
}
int ProtocolProfileValidator::typeBytes(const QString &type) {
    if (type == "int8" || type == "uint8") return 1;
    if (type == "int16" || type == "uint16") return 2;
    if (type == "int32" || type == "uint32" || type == "float32") return 4;
    if (type == "float64") return 8;
    return 0;
}
QJsonObject ProtocolProfileValidator::keyValueTemplate(bool cbf) {
    QJsonArray channels;
    const QStringList names = cbf ? QStringList{"CBF", "IR", "RED"} : QStringList{"Ch1", "Ch2", "Ch3"};
    for (int i = 0; i < names.size(); ++i) channels.append(QJsonObject{{"id", QString("key-%1").arg(i)},
        {"name", names[i]}, {"key", QString::number(i)}, {"unit", cbf && i == 0 ? "MHz" : ""}, {"scale", 1}, {"bias", 0}});
    QJsonObject result{{"id", cbf ? "723eb1d5-0b2a-43eb-a06a-cbf112000001" : QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"revision", 1}, {"name", cbf ? "CBF/IR/RED" : "多通道键值协议"},
        {"description", cbf ? "样例：0:0\\r\\n1:1\\r\\n2:2\\r\\n\n设备采样率默认31Hz，采用TCP服务端数据接口，默认端口4970。\n设备模板：0=CBF（MHz），1=IR（AFE4400 LED1），2=RED（AFE4400 LED2）。三个键全部收到后输出一帧；不交换数值。默认启用TCP服务器预设，可按需关闭。"
                            : "样例：0:0\\r\\n1:1\\r\\n2:2\\r\\n\n按键映射通道，所有配置的键都收到后输出一帧。键可为数字或文本，输出顺序由通道表决定。"},
        {"type", "keyvalue"}, {"outOfRangePolicy", "nan-channel"},
        {"frame", QJsonObject{{"encoding", "utf8"}, {"terminatorHex", "0D0A"}, {"prefix", ""}, {"suffix", ""},
            {"delimiter", ":"}, {"trimFields", true}, {"maxFrameBytes", 4096}, {"groupMode", "all-channels"},
            {"groupTimeoutMs", 1000}, {"duplicateKeyPolicy", cbf ? "replace" : "restart"}, {"unknownKeyPolicy", "ignore"}}},
        {"channels", channels}};
    if (cbf) result.insert("devicePreset", QJsonObject{{"enabled", true}, {"interface", "tcp-server"}, {"port", 4970}, {"separateViews", true}});
    return result;
}
bool ProtocolProfileValidator::selection(const QJsonObject &input, ProtocolSelection &output, QString &error) {
    error.clear();
    if (!keys(input, {"kind", "id", "bigEndian"}, error) || !input.value("kind").isString() || !input.value("id").isString())
        return error.isEmpty() ? fail(error, QStringLiteral("协议选择格式无效")) : false;
    ProtocolSelection result;
    result.kind = input.value("kind").toString(); result.id = input.value("id").toString();
    if (result.kind == "builtin") {
        if (!builtinIds.contains(result.id)) return fail(error, QStringLiteral("未知内置协议：%1").arg(result.id));
    } else if (result.kind == "custom" && uuid(result.id)) result.id = normalizedId(result.id);
    else return fail(error, QStringLiteral("协议类型或 ID 无效"));
    if (input.contains("bigEndian")) {
        if (result.id != "builtin.justfloat" || result.kind != "builtin" || !input.value("bigEndian").isBool())
            return fail(error, QStringLiteral("bigEndian 仅适用于内置 JustFloat"));
        result.bigEndian = input.value("bigEndian").toBool();
    }
    output = result; return true;
}

QJsonObject ProtocolProfileValidator::standardizeBinary(QJsonObject d, bool repairLimits) {
    if (d.value("type") != QJsonValue("binary")) return d;
    auto frame = d.value("frame").toObject(); QByteArray head, tail;
    if (!parseHex(frame.value("headerHex").toString(), head) || !parseHex(frame.value("tailHex").toString(), tail)) return d;
    const bool lengthFramed = frame.value("mode") == QJsonValue("length"), delimiterFramed = frame.value("mode") == QJsonValue("delimiter");
    const bool variable = lengthFramed || delimiterFramed, array = d.contains("array");
    const auto algorithm = d.value("checksum").toObject().value("algorithm").toString("none");
    const int width = ProtocolChecksum::width(algorithm), distance = tail.size() + width;
    if (width > 0) {
        QJsonObject field{{"from", "end"}, {"offset", distance}, {"type", width == 4 ? "uint32" : width == 2 ? "uint16" : "uint8"}};
        if (width > 1) field.insert("byteOrder", d.value("byteOrder"));
        d.insert("checksum", QJsonObject{{"algorithm", algorithm}, {"range", QJsonObject{{"offset", 0}, {"endOffset", distance}}}, {"field", field}});
    } else d.insert("checksum", QJsonObject{{"algorithm", algorithm}});
    const auto headerAlgorithm = delimiterFramed ? QString("none") : d.value("headerChecksum").toObject().value("algorithm").toString("none");
    if (delimiterFramed) d.remove("headerChecksum");
    const int headerWidth = ProtocolChecksum::width(headerAlgorithm);
    const int lengthEnd = lengthFramed ? frame.value("lengthField").toObject().value("offset").toInt() + typeBytes(frame.value("lengthField").toObject().value("type").toString()) : head.size();
    if (repairLimits && headerWidth) frame.insert("payloadOffset", qMax(frame.value("payloadOffset").toInt(), qMax(head.size(), lengthEnd) + headerWidth));
    if (headerWidth) {
        const int position = frame.value("payloadOffset").toInt() - headerWidth;
        QJsonObject field{{"offset", position}, {"type", headerWidth == 4 ? "uint32" : headerWidth == 2 ? "uint16" : "uint8"}};
        if (headerWidth > 1) field.insert("byteOrder", d.value("byteOrder"));
        d.insert("headerChecksum", QJsonObject{{"algorithm", headerAlgorithm}, {"range", QJsonObject{{"offset", 0}, {"length", position}}}, {"field", field}});
    }
    if (repairLimits) {
        if (array) frame.insert("payloadEndOffset", qMax(frame.value("payloadEndOffset").toInt(), distance));
        const int payload = array ? typeBytes(d.value("array").toObject().value("type").toString()) * d.value("array").toObject().value("minChannels").toInt(1) : frame.value("payloadBytes").toInt();
        const int minimum = qMax(frame.value("payloadOffset").toInt() + payload + (array ? frame.value("payloadEndOffset").toInt() : distance), lengthEnd + distance);
        const QString sizeKey = variable ? "minFrameBytes" : "sizeBytes";
        frame.insert(sizeKey, qMax(frame.value(sizeKey).toInt(), minimum));
        frame.insert("maxFrameBytes", qMax(frame.value("maxFrameBytes").toInt(4096), frame.value(sizeKey).toInt()));
    }
    if (lengthFramed) {
        auto length = frame.value("lengthField").toObject();
        QString meaning = length.value("meaning").toString();
        if (meaning != "frame-bytes" && !(array && meaning == "elements")) meaning = "payload-bytes";
        const int overhead = array ? frame.value("payloadOffset").toInt() + frame.value("payloadEndOffset").toInt()
            : frame.value("minFrameBytes").toInt() - frame.value("payloadBytes").toInt();
        length.insert("meaning", meaning);
        length.insert("unitBytes", meaning == "elements" ? typeBytes(d.value("array").toObject().value("type").toString()) : 1);
        length.insert("adjustment", meaning == "frame-bytes" ? 0 : overhead);
        if (typeBytes(length.value("type").toString()) > 1) length.insert("byteOrder", d.value("byteOrder"));
        frame.insert("lengthField", length);
    }
    d.insert("frame", frame); return d;
}

bool ProtocolProfileValidator::profile(const QJsonObject &input, ProtocolProfile &output, QString &error) {
    error.clear(); QJsonObject result = input;
    if (!keys(input, {"id", "revision", "name", "description", "type", "byteOrder", "frame", "checksum", "headerChecksum", "array", "conditions", "channels", "outOfRangePolicy", "autoChannelLayout", "autoFrameLayout", "devicePreset", "recordingValueMode"}, error)) return false;
    if (input.contains("recordingValueMode") && input.value("recordingValueMode") != QJsonValue("raw") && input.value("recordingValueMode") != QJsonValue("converted"))
        return fail(error, QStringLiteral("未知的录制保存数据类型，只支持 raw（原始数值）或 converted（换算后数值）"));
    if (input.contains("autoChannelLayout") && !input.value("autoChannelLayout").isBool()) return fail(error, QStringLiteral("autoChannelLayout 必须为布尔值"));
    if (input.contains("autoFrameLayout") && !input.value("autoFrameLayout").isBool()) return fail(error, QStringLiteral("autoFrameLayout 必须为布尔值"));
    if (input.contains("outOfRangePolicy") && input.value("outOfRangePolicy") != QJsonValue("reject-frame") && input.value("outOfRangePolicy") != QJsonValue("nan-channel"))
        return fail(error, QStringLiteral("未知的通道超限处理方式"));
    if (!input.value("id").isString() || !uuid(input.value("id").toString())) return fail(error, QStringLiteral("方案 id 必须是非空 UUID"));
    int revision;
    if (!integer(input, "revision", 1, 2147483647, revision, error)
        || !text(input, "name", 64, true, error) || !text(input, "description", 4096, false, error)) return false;
    result.insert("id", normalizedId(input.value("id").toString()));
    result.insert("name", input.value("name").toString().trimmed());
    if (!result.contains("description")) result.insert("description", QString());
    const QString type = input.value("type").toString();
    if (type != "text" && type != "binary" && type != "keyvalue") return fail(error, QStringLiteral("协议 type 必须是 text、binary 或 keyvalue"));
    if (input.contains("devicePreset")) {
        QJsonObject preset; int port;
        if (type != "keyvalue" || !objectAt(input, "devicePreset", preset, error)
            || !keys(preset, {"enabled", "interface", "port", "separateViews"}, error)
            || !preset.value("enabled").isBool() || !preset.value("separateViews").isBool()
            || preset.value("interface") != QJsonValue("tcp-server") || !integer(preset, "port", 1, 65535, port, error))
            return fail(error, error.isEmpty() ? QStringLiteral("设备预设仅支持键值协议的可选 TCP 服务器与分离视图配置") : error);
    }
    const bool array = input.contains("array");
    int arrayWidth = 0, minChannels = 1, maxChannels = MaxChannels;
    if (array) {
        QJsonObject layout;
        if (type != "binary" || !objectAt(input, "array", layout, error)
            || !keys(layout, {"type", "minChannels", "maxChannels"}, error)) return fail(error, error.isEmpty() ? QStringLiteral("动态数组仅适用于变长二进制协议") : error);
        arrayWidth = typeBytes(layout.value("type").toString());
        if (!arrayWidth) return fail(error, QStringLiteral("动态数组的数据类型无效"));
        if (!layout.contains("minChannels")) layout.insert("minChannels", 1);
        if (!layout.contains("maxChannels")) layout.insert("maxChannels", MaxChannels);
        if (!integer(layout, "minChannels", 1, MaxChannels, minChannels, error)
            || !integer(layout, "maxChannels", minChannels, MaxChannels, maxChannels, error)) return false;
        result.insert("array", layout);
    }
    QJsonObject frame;
    if (!objectAt(input, "frame", frame, error)) return false;
    int maxFrame;
    if (!frame.contains("maxFrameBytes")) frame.insert("maxFrameBytes", 4096);
    if (!integer(frame, "maxFrameBytes", 1, MaxFrameBytes, maxFrame, error)) return false;
    int size = 0, payloadOffset = 0, payloadSize = 0, payloadEndOffset = 0;
    if (type == "text" || type == "keyvalue") {
        if (input.contains("byteOrder") || input.contains("checksum") || input.contains("headerChecksum") || input.contains("conditions"))
            return fail(error, QStringLiteral("文本协议不支持二进制大小端、校验或字节条件"));
        const QStringList allowedFrame = type == "text"
            ? QStringList{"encoding", "terminatorHex", "prefix", "suffix", "delimiter", "delimiterMode", "trimFields", "maxFrameBytes"}
            : QStringList{"encoding", "terminatorHex", "prefix", "suffix", "delimiter", "trimFields", "maxFrameBytes", "groupMode", "groupTimeoutMs", "duplicateKeyPolicy", "unknownKeyPolicy"};
        if (!keys(frame, allowedFrame, error)) return false;
        const QString encoding = frame.value("encoding").toString();
        if (encoding != "utf8" && encoding != "ascii") return fail(error, QStringLiteral("文本编码仅支持 utf8 或 ascii"));
        QByteArray ending;
        if (!hexAt(frame, "terminatorHex", ending, 16, true, error)
            || !text(frame, "prefix", 256, false, error) || !text(frame, "suffix", 256, false, error)) return false;
        if (maxFrame < ending.size()) return fail(error, QStringLiteral("文本最大帧长小于结束符长度"));
        if (!frame.contains("prefix")) frame.insert("prefix", QString());
        if (!frame.contains("suffix")) frame.insert("suffix", QString());
        if (type == "keyvalue") {
            if (!frame.contains("groupMode")) frame.insert("groupMode", "all-channels");
            if (!frame.contains("groupTimeoutMs")) frame.insert("groupTimeoutMs", 1000);
            if (!frame.contains("duplicateKeyPolicy")) frame.insert("duplicateKeyPolicy", "restart");
            if (!frame.contains("unknownKeyPolicy")) frame.insert("unknownKeyPolicy", "ignore");
            int timeout;
            if (frame.value("groupMode") != QJsonValue("all-channels") || !integer(frame, "groupTimeoutMs", 0, 600000, timeout, error)
                || (frame.value("duplicateKeyPolicy") != QJsonValue("restart") && frame.value("duplicateKeyPolicy") != QJsonValue("replace"))
                || (frame.value("unknownKeyPolicy") != QJsonValue("ignore") && frame.value("unknownKeyPolicy") != QJsonValue("reject-group")))
                return fail(error, error.isEmpty() ? QStringLiteral("键值组帧规则无效") : error);
        }
        const QString mode = type == "keyvalue" ? "literal" : frame.value("delimiterMode").toString();
        if (mode != "literal" && mode != "whitespace") return fail(error, QStringLiteral("delimiterMode 必须是 literal 或 whitespace"));
        if (mode == "literal") {
            if (!frame.value("delimiter").isString() || frame.value("delimiter").toString().isEmpty()
                || frame.value("delimiter").toString().size() > 16 || frame.value("delimiter").toString().contains(QChar::Null))
                return fail(error, QStringLiteral("字面分隔符必须为 1–16 个字符"));
        }
        else if (frame.contains("delimiter")) return fail(error, QStringLiteral("空白合并模式不接受字面分隔符"));
        if (!frame.contains("trimFields")) frame.insert("trimFields", true);
        if (!frame.value("trimFields").isBool()) return fail(error, QStringLiteral("trimFields 必须是布尔值"));
    } else {
        const bool lengthFramed = frame.value("mode") == QJsonValue("length"), delimiterFramed = frame.value("mode") == QJsonValue("delimiter");
        const bool variable = lengthFramed || delimiterFramed;
        if (!variable && frame.value("mode") != QJsonValue("fixed")) return fail(error, QStringLiteral("二进制帧 mode 必须是 fixed、length 或 delimiter"));
        if (array && !variable) return fail(error, QStringLiteral("动态数组仅适用于可变长度帧；定长协议请使用固定通道布局"));
        if (delimiterFramed && (!array || input.contains("headerChecksum"))) return fail(error, QStringLiteral("首尾分隔使用动态数组，不支持长度字段或帧头校验；数据校验可在帧尾前配置"));
        if (!keys(frame, delimiterFramed ? QStringList{"mode", "headerHex", "tailHex", "payloadOffset", "payloadEndOffset", "maxFrameBytes", "minFrameBytes"}
            : variable ? QStringList{"mode", "headerHex", "tailHex", "payloadOffset", array ? "payloadEndOffset" : "payloadBytes", "maxFrameBytes", "minFrameBytes", "lengthField"}
            : QStringList{"mode", "headerHex", "sizeBytes", "tailHex", "payloadOffset", "payloadBytes", "maxFrameBytes"}, error)) return false;
        if (!byteOrder(input.value("byteOrder").toString())) return fail(error, QStringLiteral("byteOrder 必须是 little 或 big"));
        QByteArray head, tail; int lengthOffset = 0, lengthWidth = 0;
        if (!hexAt(frame, "headerHex", head, 256, lengthFramed, error) || !hexAt(frame, "tailHex", tail, 256, false, error)) return false;
        if (head.isEmpty() && tail.isEmpty())
            return fail(error, QStringLiteral("二进制帧的帧头和帧尾不能同时为空，请至少设置一项以定位帧边界"));
        if (!integer(frame, "payloadOffset", 0, maxFrame, payloadOffset, error)) return false;
        if (array && (!integer(frame, "payloadEndOffset", tail.size(), maxFrame, payloadEndOffset, error)
            || payloadOffset + payloadEndOffset + minChannels * arrayWidth > maxFrame))
            return fail(error, error.isEmpty() ? QStringLiteral("最大帧长不足以容纳动态数组的最少通道数") : error);
        if (delimiterFramed) {
            if (!integer(frame, "minFrameBytes", 1, maxFrame, size, error)) return false;
        } else if (lengthFramed) {
            QJsonObject length; int unit, adjustment;
            if (!integer(frame, "minFrameBytes", 1, maxFrame, size, error)
                || !objectAt(frame, "lengthField", length, error)
                || !keys(length, {"offset", "type", "byteOrder", "unitBytes", "adjustment", "meaning"}, error)
                || !integer(length, "offset", 0, size, lengthOffset, error)) return false;
            const QString meaning = length.value("meaning").toString("custom");
            if (meaning != "custom") {
                if ((meaning != "payload-bytes" && meaning != "frame-bytes" && meaning != "elements") || (!array && meaning == "elements"))
                    return fail(error, QStringLiteral("长度含义无效；通道数量仅适用于动态数组"));
                const int expectedUnit = meaning == "elements" ? arrayWidth : 1;
                const int expectedAdjustment = meaning == "frame-bytes" ? 0 : array ? payloadOffset + payloadEndOffset : size - frame.value("payloadBytes").toInt();
                if ((length.contains("unitBytes") && length.value("unitBytes") != QJsonValue(expectedUnit))
                    || (length.contains("adjustment") && length.value("adjustment") != QJsonValue(expectedAdjustment)))
                    return fail(error, QStringLiteral("长度公式与所选长度含义不一致，请使用自动计算值或改为自定义公式"));
                length.insert("unitBytes", expectedUnit); length.insert("adjustment", expectedAdjustment);
            }
            if (!integer(length, "unitBytes", 1, MaxFrameBytes, unit, error)
                || !integer(length, "adjustment", -MaxFrameBytes, MaxFrameBytes, adjustment, error)) return false;
            const auto t = length.value("type").toString();
            lengthWidth = t == "uint8" ? 1 : t == "uint16" ? 2 : t == "uint32" ? 4 : 0;
            if (lengthWidth > 1 && !length.contains("byteOrder")) length.insert("byteOrder", input.value("byteOrder"));
            if (!lengthWidth || (lengthWidth > 1 && !byteOrder(length.value("byteOrder").toString()))
                || (lengthWidth == 1 && length.contains("byteOrder"))) return fail(error, QStringLiteral("长度字段只支持 uint8/uint16/uint32；多字节大小端必须有效，省略时继承协议默认值"));
            if (!within(lengthOffset, lengthWidth, size) || lengthOffset < head.size()
                || lengthOffset + lengthWidth > size - tail.size()) return fail(error, QStringLiteral("长度字段在最小帧中越界或与帧头/帧尾重叠"));
            // Reject impossible ranges before any receive-buffer allocation.
            const quint64 largest = (quint64(1) << (lengthWidth * 8)) - 1;
            const qint64 lower = qMax<qint64>(0, (qint64(size) - adjustment + unit - 1) / unit);
            const qint64 upper = (qint64(maxFrame) - adjustment) / unit;
            if (qint64(maxFrame) < adjustment || lower > upper || quint64(lower) > largest)
                return fail(error, QStringLiteral("长度计算公式无法得到允许范围内的帧长"));
            frame.insert("lengthField", length);
        } else if (!integer(frame, "sizeBytes", 1, maxFrame, size, error)) return false;
        if (array) {
            payloadSize = size - payloadOffset - payloadEndOffset;
            if (payloadSize < minChannels * arrayWidth) return fail(error, QStringLiteral("最小帧长不足以容纳动态数组的最少通道数"));
        } else if (!integer(frame, "payloadBytes", 1, size, payloadSize, error)) return false;
        if (!within(payloadOffset, payloadSize, size) || payloadOffset < head.size()
            || payloadOffset + payloadSize > size - tail.size()) return fail(error, QStringLiteral("数据区越界或与帧头/帧尾重叠"));
        if (lengthFramed && overlap(lengthOffset, lengthWidth, payloadOffset, payloadSize))
            return fail(error, QStringLiteral("长度字段与固定数据区重叠"));
        if (input.contains("checksum")) {
            QJsonObject checksum;
            if (!objectAt(input, "checksum", checksum, error)) return false;
            const QString algorithm = checksum.value("algorithm").toString();
            if (algorithm == "none") { if (!keys(checksum, {"algorithm"}, error)) return false; }
            else {
                const int width = ProtocolChecksum::width(algorithm);
                if (width < 1) return fail(error, QStringLiteral("未知校验算法"));
                if (!keys(checksum, {"algorithm", "range", "field"}, error)) return false;
                QJsonObject range, field; int start, length = 0, location, endOffset = 0;
                if (!objectAt(checksum, "range", range, error) || !objectAt(checksum, "field", field, error)
                    || !keys(range, {"offset", "length", "endOffset"}, error) || !keys(field, {"offset", "type", "byteOrder", "from"}, error)
                    || !integer(range, "offset", 0, size, start, error)
                    || !integer(field, "offset", 0, size, location, error)) return false;
                const bool dynamicRange = range.contains("endOffset"), fromEnd = field.value("from") == QJsonValue("end");
                if (delimiterFramed && (!fromEnd || !dynamicRange || start != 0 || location != width + tail.size()
                    || range.value("endOffset") != QJsonValue(width + tail.size())))
                    return fail(error, QStringLiteral("首尾分隔的数据校验必须从帧起点 0 开始，到紧贴帧尾之前的校验位结束，不含校验位和帧尾"));
                if (dynamicRange == range.contains("length")) return fail(error, QStringLiteral("校验范围必须且只能指定 length 或 endOffset"));
                if (field.contains("from") && !fromEnd && field.value("from") != QJsonValue("start"))
                    return fail(error, QStringLiteral("校验字段 from 必须是 start 或 end"));
                if (dynamicRange ? !integer(range, "endOffset", 0, size, endOffset, error) : !integer(range, "length", 1, size, length, error)) return false;
                if (field.value("type") != QJsonValue(width == 1 ? "uint8" : width == 2 ? "uint16" : "uint32")
                    || (width > 1 && !byteOrder(field.value("byteOrder").toString()))
                    || (width == 1 && field.contains("byteOrder"))) return fail(error, QStringLiteral("校验字段类型或大小端无效"));
                // All positions are affine in frame size. Check both ends and
                // require the same non-overlap ordering throughout the range.
                bool before = true, after = true;
                for (int frameSize : {size, variable ? maxFrame : size}) {
                    const int position = fromEnd ? frameSize - location : location;
                    const int count = dynamicRange ? frameSize - endOffset - start : length;
                    if (!within(start, count, frameSize) || !within(position, width, frameSize)
                        || overlap(position, width, payloadOffset, array ? frameSize - payloadOffset - payloadEndOffset : payloadSize)
                        || position < head.size() || position + width > frameSize - tail.size()
                        || (variable && overlap(position, width, lengthOffset, lengthWidth)))
                        return fail(error, QStringLiteral("校验范围/字段越界，或与数据区、长度字段、帧头/帧尾重叠"));
                    before = before && start + count <= position;
                    after = after && start >= position + width;
                }
                if (!before && !after) return fail(error, QStringLiteral("校验范围包含校验字段，或在变长帧中发生重叠"));
                if (variable && fromEnd && size - location < qMax(payloadOffset + payloadSize, lengthOffset + lengthWidth))
                    return fail(error, QStringLiteral("帧末定位的校验字段必须位于固定数据区及长度字段之后"));
            }
        }
        if (input.contains("headerChecksum")) {
            QJsonObject check, range, field; int start, count, position;
            if (!objectAt(input, "headerChecksum", check, error)
                || !keys(check, {"algorithm", "range", "field"}, error)) return false;
            const int width = ProtocolChecksum::width(check.value("algorithm").toString());
            if (!width || !objectAt(check, "range", range, error) || !objectAt(check, "field", field, error)
                || !keys(range, {"offset", "length"}, error) || !keys(field, {"offset", "type", "byteOrder"}, error)
                || !integer(range, "offset", 0, payloadOffset, start, error)
                || !integer(range, "length", 1, payloadOffset, count, error)
                || !integer(field, "offset", head.size(), payloadOffset, position, error))
                return fail(error, error.isEmpty() ? QStringLiteral("帧头校验配置无效") : error);
            if (!within(start, count, payloadOffset) || !within(position, width, payloadOffset)
                || overlap(start, count, position, width) || (variable && overlap(position, width, lengthOffset, lengthWidth))
                || (variable && (start > lengthOffset || start + count < lengthOffset + lengthWidth))
                || field.value("type") != QJsonValue(width == 1 ? "uint8" : width == 2 ? "uint16" : "uint32")
                || (width > 1 && !byteOrder(field.value("byteOrder").toString())) || (width == 1 && field.contains("byteOrder")))
                return fail(error, QStringLiteral("帧头校验必须在数据区前并覆盖完整长度字段，不能与长度字段重叠，且校验范围不能包含自身"));
            const auto body = input.value("checksum").toObject(), bodyField = body.value("field").toObject();
            const int bodyWidth = ProtocolChecksum::width(body.value("algorithm").toString());
            for (int frameSize : {size, variable ? maxFrame : size}) {
                const int bodyPosition = bodyField.value("from") == QJsonValue("end") ? frameSize - bodyField.value("offset").toInt() : bodyField.value("offset").toInt();
                if (bodyWidth && overlap(position, width, bodyPosition, bodyWidth)) return fail(error, QStringLiteral("帧头校验与数据校验字段重叠"));
            }
        }
        if (input.contains("conditions")) {
            if (!input.value("conditions").isArray() || input.value("conditions").toArray().size() > 16)
                return fail(error, QStringLiteral("conditions 必须是最多 16 项的数组"));
            QJsonArray normalized;
            for (auto item : input.value("conditions").toArray()) {
                if (!item.isObject()) return fail(error, QStringLiteral("帧条件必须是对象"));
                auto condition = item.toObject(); QByteArray bytes; int offset;
                if (!keys(condition, {"offset", "equalsHex"}, error) || !integer(condition, "offset", 0, size, offset, error)
                    || !hexAt(condition, "equalsHex", bytes, 256, true, error)) return false;
                if (!within(offset, bytes.size(), size)) return fail(error, QStringLiteral("帧条件越界"));
                normalized.append(condition);
            }
            result.insert("conditions", normalized);
        }
    }
    result.insert("frame", frame);
    if (!input.value("channels").isArray()) return fail(error, QStringLiteral("channels 必须是数组"));
    const auto channels = input.value("channels").toArray();
    if ((type != "text" && !array && channels.isEmpty()) || channels.size() > MaxChannels)
        return fail(error, type == "text" ? QStringLiteral("文本通道配置最多 256 项；可留空，自动读取所有数值列") : QStringLiteral("通道数必须是 1–256"));
    QSet<QString> ids, channelKeys; QSet<int> arrayIndices; QVector<QPair<int, int>> regions; QJsonArray normalized;
    for (int i = 0; i < channels.size(); ++i) {
        if (!channels[i].isObject()) return fail(error, QStringLiteral("通道 %1 必须是对象").arg(i + 1));
        auto channel = channels[i].toObject();
        // An omitted/blank channel name means the canonical default. Explicit
        // existing names (including the previous Chinese defaults) stay intact.
        if (!channel.contains("name") || (channel.value("name").isString() && channel.value("name").toString().trimmed().isEmpty()))
            channel.insert("name", QStringLiteral("Ch%1").arg(array ? channel.value("index").toInt() + 1 : i + 1));
        const QStringList allowed = type == "keyvalue" ? QStringList{"id", "name", "unit", "key", "scale", "bias", "min", "max", "note"}
            : type == "text" ? QStringList{"id", "name", "unit", "column", "scale", "bias", "min", "max", "note"}
            : array ? QStringList{"id", "name", "unit", "index", "scale", "bias", "min", "max", "note"}
            : QStringList{"id", "name", "unit", "offset", "type", "byteOrder", "scale", "bias", "min", "max", "note"};
        if (!keys(channel, allowed, error) || !text(channel, "id", 64, true, error)
            || !text(channel, "name", 64, true, error) || !text(channel, "unit", 32, false, error) || !text(channel, "note", 1024, false, error)) return false;
        const auto id = channel.value("id").toString();
        if (id != id.trimmed() || ids.contains(id)) return fail(error, QStringLiteral("通道 ID 重复或含首尾空格：%1").arg(id));
        ids.insert(id);
        if (!channel.contains("unit")) channel.insert("unit", QString());
        if (!channel.contains("scale")) channel.insert("scale", 1.0);
        if (!channel.contains("bias")) channel.insert("bias", 0.0);
        if (!finite(channel, "scale", error) || !finite(channel, "bias", error)) return false;
        for (const auto &key : {QStringLiteral("min"), QStringLiteral("max")})
            if (channel.contains(key) && !finite(channel, key, error)) return false;
        if (channel.contains("min") && channel.contains("max") && channel.value("min").toDouble() > channel.value("max").toDouble())
            return fail(error, QStringLiteral("通道范围 min 大于 max"));
        int offset;
        if (type == "keyvalue") {
            if (!text(channel, "key", 64, true, error)) return false;
            const auto key = channel.value("key").toString();
            if (key != key.trimmed() || channelKeys.contains(key) || key.contains(frame.value("delimiter").toString()) || key.contains('\r') || key.contains('\n'))
                return fail(error, QStringLiteral("通道键重复、含首尾空白、换行或键值分隔符：%1").arg(key));
            if (frame.value("encoding") == QJsonValue("ascii")) for (const auto c : key) if (c.unicode() > 127) return fail(error, "ASCII 协议的通道键必须为 ASCII 字符");
            channelKeys.insert(key);
        }
        else if (type == "text") { if (!integer(channel, "column", 0, 4095, offset, error)) return false; }
        else if (array) {
            if (!integer(channel, "index", 0, maxChannels - 1, offset, error)) return false;
            if (arrayIndices.contains(offset)) return fail(error, QStringLiteral("动态数组通道序号重复"));
            arrayIndices.insert(offset);
        }
        else {
            const int width = typeBytes(channel.value("type").toString());
            if (!width) return fail(error, QStringLiteral("未知或不支持的数据类型"));
            if (!integer(channel, "offset", 0, payloadSize, offset, error)) return false;
            if (!within(offset, width, payloadSize)) return fail(error, QStringLiteral("通道 %1 超出数据区").arg(i + 1));
            if (channel.contains("byteOrder") && !byteOrder(channel.value("byteOrder").toString())) return fail(error, QStringLiteral("通道大小端无效"));
            for (const auto &region : regions)
                if (overlap(offset, width, region.first, region.second)) return fail(error, QStringLiteral("通道字段重叠"));
            regions.append(qMakePair(offset, width));
        }
        normalized.append(channel);
    }
    result.insert("channels", normalized);
    ProtocolProfile profile;
    profile.id = result.value("id").toString(); profile.revision = revision;
    profile.name = result.value("name").toString(); profile.description = result.value("description").toString();
    profile.definition = result; output = profile; return true;
}

bool ProtocolProfileValidator::snapshot(const ProtocolSnapshot &input, ProtocolSnapshot &output, QString &error) {
    error.clear(); ProtocolSnapshot result;
    if (input.profiles.size() > MaxProfiles) return fail(error, QStringLiteral("协议库最多 128 个方案"));
    QSet<QString> ids, names;
    for (int i = 0; i < input.profiles.size(); ++i) {
        ProtocolProfile profile, original;
        if (!ProtocolProfileValidator::profile(input.profiles[i].definition, original, error)
            || !ProtocolProfileValidator::profile(standardizeBinary(original.definition, true), profile, error)) {
            error = QStringLiteral("方案 %1：%2").arg(i + 1).arg(error); return false;
        }
        if (ids.contains(profile.id) || names.contains(profile.name)) return fail(error, QStringLiteral("方案 ID 或名称重复：%1").arg(profile.name));
        ids.insert(profile.id); names.insert(profile.name); result.profiles.append(profile);
    }
    if (!selection(input.shared.toJson(), result.shared, error)) return false;
    if (result.shared.kind == "builtin" && result.shared.id == "builtin.amu4") {
        auto definition = keyValueTemplate(true); const auto id = definition.value("id").toString();
        if (!ids.contains(id)) {
            if (result.profiles.size() >= MaxProfiles) return fail(error, "旧 CBF/IR/RED 协议需要迁移，但协议库已满。请先导出并移除部分协议；原文件未修改。");
            QString name = "CBF/IR/RED"; int suffix = 0;
            while (names.contains(name)) name = QString("CBF/IR/RED_%1").arg(++suffix);
            definition.insert("name", name);
            auto preset = definition.value("devicePreset").toObject(); preset.insert("enabled", true); definition.insert("devicePreset", preset);
            auto frame = definition.value("frame").toObject(); frame.insert("groupTimeoutMs", 0); definition.insert("frame", frame);
            ProtocolProfile migrated; if (!profile(definition, migrated, error)) return false;
            result.profiles.append(migrated); ids.insert(id);
        } else {
            for (const auto &existing : result.profiles) if (existing.id == id && !existing.isKeyValue())
                return fail(error, "旧设备协议的迁移 ID 与其他类型协议冲突，未覆盖原协议。");
        }
        result.shared.kind = "custom"; result.shared.id = id; result.shared.bigEndian = false;
    }
    result.ble = result.shared; // Legacy in-memory alias, never independent.
    for (const auto &selected : {result.shared, result.ble})
        if (selected.kind == "custom" && !ids.contains(selected.id)) return fail(error, QStringLiteral("选择引用了不存在的方案：%1").arg(selected.id));
    output = result; return true;
}
bool ProtocolProfileValidator::decode(const QByteArray &bytes, bool storeDocument, ProtocolSnapshot &output, QString &error) {
    error.clear();
    if (bytes.isEmpty() || bytes.size() > MaxDocumentBytes || !boundedJson(bytes)) return fail(error, QStringLiteral("协议 JSON 为空、超过 2 MiB 或嵌套过深"));
    QJsonParseError parseError; const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return fail(error, QStringLiteral("协议 JSON 无效：%1").arg(parseError.errorString()));
    const auto root = document.object(); int version;
    if (!keys(root, storeDocument ? QStringList{"kind", "schemaVersion", "profiles", "selections"} : QStringList{"kind", "schemaVersion", "profiles"}, error)
        || !integer(root, "schemaVersion", SchemaVersion, SchemaVersion, version, error)) return false;
    if (root.value("kind") != QJsonValue(storeDocument ? "WaveLink.ProtocolStore" : "WaveLink.ProtocolProfiles"))
        return fail(error, QStringLiteral("文件不是预期的协议库/协议分享文件"));
    if (!root.value("profiles").isArray() || root.value("profiles").toArray().size() > MaxProfiles)
        return fail(error, QStringLiteral("profiles 必须是最多 128 项的数组"));
    ProtocolSnapshot result;
    for (auto item : root.value("profiles").toArray()) {
        if (!item.isObject()) return fail(error, QStringLiteral("方案必须是对象"));
        ProtocolProfile profile;
        if (!ProtocolProfileValidator::profile(item.toObject(), profile, error)) return false;
        result.profiles.append(profile);
    }
    if (storeDocument) {
        QJsonObject selections, shared;
        if (!objectAt(root, "selections", selections, error) || !keys(selections, {"shared", "ble"}, error)
            || !objectAt(selections, "shared", shared, error) || !selection(shared, result.shared, error)) return false;
        result.ble = result.shared;
    }
    return snapshot(result, output, error);
}
QByteArray ProtocolProfileValidator::encode(const ProtocolSnapshot &input, bool storeDocument, QString &error) {
    ProtocolSnapshot normalized;
    if (!snapshot(input, normalized, error)) return {};
    QJsonArray profiles;
    for (const auto &profile : normalized.profiles) profiles.append(profile.definition);
    QJsonObject root{{"kind", storeDocument ? "WaveLink.ProtocolStore" : "WaveLink.ProtocolProfiles"},
        {"schemaVersion", SchemaVersion}, {"profiles", profiles}};
    if (storeDocument) root.insert("selections", QJsonObject{{"shared", normalized.shared.toJson()}});
    const auto bytes = QJsonDocument(root).toJson(storeDocument ? QJsonDocument::Compact : QJsonDocument::Indented);
    if (bytes.size() > MaxDocumentBytes) { fail(error, QStringLiteral("协议 JSON 超过 2 MiB")); return {}; }
    return bytes;
}
} // namespace DataProtocols
