#pragma once
#include "commandsliderconfig.h"
#include <QStringList>

// Shared validation for panel imports and atomic whole-application imports.
// QtCore only: color strings are checked by the widget when displayed.
namespace CommandSwitchConfig {
// Missing optional metadata has an unambiguous default. Missing payloads do
// not: retain a disabled placeholder, never invent an ON/OFF device command.
inline bool normalize(QJsonObject &object, bool repair, QStringList &changes) {
    if (object.value("widgetType").toInt(0) != 2) return false;
    const auto before = object;
    bool pending = object.value("switchNeedsRepair").toBool();
    if (!object.contains("switchDefaultState")) object["switchDefaultState"] = 0;
    const auto initial = object["switchDefaultState"];
    if (repair && (!initial.isDouble() || (initial.toDouble() != 0 && initial.toDouble() != 1))) {
        object["switchDefaultState"] = 0; pending = true;
    }
    if (!object.contains("switchStates") || (repair && (!object["switchStates"].isArray() || object["switchStates"].toArray().size() != 2))) {
        QJsonArray recovered;
        const auto original = object["switchStates"].toArray();
        for (int i = 0; i < 2; ++i) recovered.append(i < original.size() ? original[i] : QJsonObject());
        object["switchStates"] = recovered; pending = true;
    }
    if (object["switchStates"].isArray() && object["switchStates"].toArray().size() == 2) {
        auto states = object["switchStates"].toArray();
        for (int i = 0; i < 2; ++i) {
            if (!states[i].isObject() && !repair) continue;
            auto state = states[i].toObject();
            if (!state.contains("payload")) { state["payload"] = ""; pending = true; }
            if (!state.contains("annotation")) state["annotation"] = i ? "开启" : "关闭";
            if (!state.contains("color")) state["color"] = "";
            if (repair) {
                for (const QString &key : {QString("payload"), QString("annotation"), QString("color")})
                    if (!state[key].isString()) { state[key] = ""; pending = true; }
                if (state["payload"].toString().isEmpty() || (object.value("isHex").toBool() && !CommandSliderConfig::validHex(state["payload"].toString()))) pending = true;
            }
            states[i] = state;
        }
        object["switchStates"] = states;
    }
    if (pending) { object["switchNeedsRepair"] = true; object["autoSend"] = false; }
    if (before == object) return false;
    changes.append(QString("%1：%2").arg(object["name"].toString(), pending ? "开关配置待补全，已禁止发送" : "已兼容旧版开关配置"));
    return true;
}
inline bool validate(const QJsonObject &object, QString &error) {
    if (object["widgetType"].toInt(0) != 2) return true;
    const auto initial = object["switchDefaultState"];
    if (!initial.isDouble() || (initial.toDouble() != 0 && initial.toDouble() != 1)) {
        error = "开关默认状态只能为 0 或 1。"; return false;
    }
    const auto states = object["switchStates"];
    if (!states.isArray() || states.toArray().size() != 2) {
        error = "开关命令必须配置且只能配置两个状态。"; return false;
    }
    if (object.contains("isHex") && !object["isHex"].isBool()) {
        error = "开关 HEX 格式设置无效。"; return false;
    }
    if (object.contains("switchNeedsRepair") && !object["switchNeedsRepair"].isBool()) {
        error = "开关修复标记无效。"; return false;
    }
    const bool pending = object["switchNeedsRepair"].toBool();
    for (int i = 0; i < 2; ++i) {
        const auto value = states.toArray()[i]; const auto state = value.toObject();
        if (!value.isObject() || !state["payload"].isString() || (!pending && state["payload"].toString().isEmpty())
            || !state["annotation"].isString() || !state["color"].isString()) {
            error = QString("状态 %1 的命令不能为空，命令、注释和颜色必须为字符串。").arg(i); return false;
        }
        if (!pending && object["isHex"].toBool() && !CommandSliderConfig::validHex(state["payload"].toString())) {
            error = QString("状态 %1 的 HEX 命令必须由完整字节组成，仅接受 0–9、A–F 和空白。").arg(i); return false;
        }
    }
    if (object["appendIndex"].toInt(0) == 5 && object["isHex"].toBool()
        && !object["customAppendPayload"].toString().trimmed().isEmpty()
        && !CommandSliderConfig::validHex(object["customAppendPayload"].toString())) {
        error = "开关命令的自定义追加必须是完整的 HEX 字节。"; return false;
    }
    return true;
}
}
