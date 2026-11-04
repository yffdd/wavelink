#pragma once
#include "commandswitchconfig.h"
#include "panellayout.h"
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QLockFile>
#include <QDir>
#include <QUuid>
#include <QDateTime>

// Shared by local startup, independent imports and global configuration
// transactions. Compatibility must never depend on which route loaded a file.
namespace CommandPanelConfig {
constexpr int MaxBytes = 16 * 1024 * 1024;

inline bool readLayout(const QJsonObject &panel, PanelLayout::Layout &layout, QString &error)
{
    const auto value = panel.value("layoutRows");
    if (value.isUndefined()) {
        layout.reset();
        return true;
    }
    QVector<PanelLayout::Item> items;
    for (const auto &entry : panel.value("commands").toArray()) {
        const auto command = entry.toObject();
        items.append({command["id"].toString(), {}, command["widgetType"].toInt() == 1});
    }
    const int limit = panel.contains("columns") ? qBound(1, panel.value("columns").toInt(), PanelLayout::CommandMaxColumns) : 3;
    return PanelLayout::read(value, items, limit, layout, error);
}

inline bool decode(const QByteArray &bytes, QJsonArray &result, QString &error,
                   QStringList &changes, bool repair = false) {
    if (bytes.size() > MaxBytes) { error = "命令配置超过 16 MB，未加载。"; return false; }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isArray()) {
        error = QString("命令配置 JSON 无效（位置 %1）：%2").arg(parse.offset).arg(parse.errorString()); return false;
    }
    auto panels = doc.array(); int count = 0;
    if (panels.size() > 512) { error = "命令面板数量超过上限。"; return false; }
    for (int p = 0; p < panels.size(); ++p) {
        if (!panels[p].isObject()) { error = "命令面板必须是对象。"; return false; }
        auto panel = panels[p].toObject();
        if (panel.contains("commands") && !panel["commands"].isArray()) { error = "面板命令列表无效。"; return false; }
        auto commands = panel["commands"].toArray(); count += commands.size();
        if (count > 16384) { error = "命令总数超过上限。"; return false; }
        for (int c = 0; c < commands.size(); ++c) {
            if (!commands[c].isObject()) { error = "命令必须是对象。"; return false; }
            auto command = commands[c].toObject();
            if (command.contains("widgetType")) {
                const auto type = command["widgetType"];
                if (!type.isDouble() || type.toDouble() < 0 || type.toDouble() > 2 || type.toInt(-1) != type.toDouble()) {
                    error = "不支持的命令类型，未忽略其中的规则。"; return false;
                }
            }
            CommandSwitchConfig::normalize(command, repair, changes);
            QString reason;
            if (!CommandSliderConfig::validate(command, reason) || !CommandSwitchConfig::validate(command, reason)) {
                error = QString("面板“%1” / 命令“%2”：%3").arg(panel["name"].toString(), command["name"].toString(), reason); return false;
            }
            commands[c] = command;
        }
        panel["commands"] = commands;
        PanelLayout::Layout layout;
        if (!readLayout(panel, layout, error)) {
            error = QStringLiteral("面板“%1”：%2").arg(panel["name"].toString(), error);
            return false;
        }
        panels[p] = panel;
    }
    result = panels; return true;
}
inline bool read(const QString &path, QByteArray &bytes, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    if (file.size() > MaxBytes) { error = "命令配置超过 16 MB。"; return false; }
    bytes = file.readAll();
    if (file.error() != QFile::NoError) { error = file.errorString(); return false; }
    return true;
}
inline bool write(const QString &path, const QByteArray &bytes, QString &error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString(); return false;
    }
    return true;
}
inline bool preserve(const QString &path, const QByteArray &bytes, QString &backup, QString &error) {
    backup = path + ".before-repair-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
    return write(backup, bytes, error);
}
// Caller supplies the exact file it inspected. Check it again under the same
// lock used by normal writes; a repair never replaces another instance's edit.
inline bool repairFile(const QString &path, const QByteArray &expected, bool manual,
                       QByteArray &repaired, QString &report, QString &error, bool expectedExists = true) {
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0)) { error = "命令配置正在由其他实例保存，请稍后重试。"; return false; }
    if (QFile::exists(QFileInfo(path).dir().filePath("global-config-transaction.json"))) {
        error = "有未完成的全局配置恢复，请先重新启动软件完成恢复。"; return false;
    }
    QByteArray current;
    if (QFile::exists(path) != expectedExists) { error = "命令配置是否存在的状态已变化，请重新读取。"; return false; }
    if (expectedExists && !read(path, current, error)) return false;
    if (current != expected) { error = "命令配置已变化，请重新读取后再修复。"; return false; }
    QJsonArray panels; QStringList changes;
    bool fromBackup = false;
    if (!decode(current, panels, error, changes, manual)) {
        if (!manual) return false;
        QByteArray saved; QString backupError; changes.clear();
        if (!read(path + ".bak", saved, backupError) || !decode(saved, panels, backupError, changes)) {
            error += "\n当前文件无法自动修复，也没有可用的有效备份。原文件已保留，可导入已导出的配置。"; return false;
        }
        fromBackup = true;
    }
    repaired = QJsonDocument(panels).toJson(QJsonDocument::Indented);
    QString backup;
    if (expectedExists && !preserve(path, current, backup, error)) { error = "无法备份原文件，修复已取消：" + error; return false; }
    if (!write(path, repaired, error)) return false;
    report = (fromBackup ? QString("已从最近有效备份恢复面板。") : QString("命令配置已修复。"));
    if (!changes.isEmpty()) report += "\n" + changes.join("\n");
    if (expectedExists) report += "\n修复前的完整配置已备份到：\n" + backup;
    error.clear(); return true;
}
}
