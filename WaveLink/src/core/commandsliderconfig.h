#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

// QtCore-only validation shared by panel imports and global-config
// transactions. Never write a list that the command UI cannot load safely.
namespace CommandSliderConfig {
constexpr int MaxItems = 256;
inline bool validHex(const QString &text, bool complete = true) {
    int digits = 0;
    for (QChar ch : text) {
        if (ch.isSpace()) continue;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
        ++digits;
    }
    return !complete || (digits > 0 && digits % 2 == 0);
}
inline bool validate(const QJsonObject &object, QString &error) {
    for (const auto &key : {QString("sliderMode"), QString("defaultListIndex")}) if (object.contains(key)) {
        const auto value = object[key]; const int limit = key == "sliderMode" ? 1 : MaxItems - 1;
        if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > limit || value.toInt(-1) != value.toDouble()) {
            error = "滑条模式或默认列表编号无效。"; return false;
        }
    }
    if (object.contains("sliderValues") && (!object["sliderValues"].isArray() || object["sliderValues"].toArray().size() > MaxItems)) {
        error = "滑条参数列表必须是最多 256 项的数组。"; return false;
    }
    const auto values = object["sliderValues"].toArray();
    for (const auto &value : values) if (!value.isString()) { error = "滑条列表的每一项必须是字符串。"; return false; }
    if (object["widgetType"].toInt(0) != 1 || object["sliderMode"].toInt(0) != 1) return true;
    if (values.isEmpty()) { error = "列表模式至少需要一项参数，请先添加列表项。"; return false; }
    if (object["defaultListIndex"].toInt(0) >= values.size()) { error = "请选择有效的默认列表项。"; return false; }
    for (int i = 0; i < values.size(); ++i) {
        if (values[i].toString().isEmpty()) { error = QString("编号 %1 的参数内容不能为空。").arg(i); return false; }
        if (object["isHex"].toBool() && !validHex(values[i].toString())) {
            error = QString("编号 %1 必须填写完整的 HEX 字节，每个字节两位，仅接受 0–9、A–F 和空白。").arg(i); return false;
        }
    }
    return true;
}
}
