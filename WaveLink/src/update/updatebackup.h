#pragma once
#include "ProtocolConfigTransfer.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QUuid>

namespace UpdateBackup {
// Caller flushes UI preferences first. Nothing is removed or overwritten;
// failure leaves partial backup files for diagnosis and blocks installation.
inline bool create(const DataProtocols::ProtocolSnapshot &snapshot, const QString &data,
                   QString &backup, QString &error) {
    backup = QDir(data).filePath("UpdateBackups/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(backup)) { error = QStringLiteral("无法创建配置备份目录。"); return false; }
    if (!DataProtocols::ProtocolConfigTransfer::exportFile(QDir(backup).filePath("WaveLink_Config.ini"), snapshot, QDir(data).filePath("cmd_panels.json"), error)) return false;
    for (const auto &name : QDir(data).entryList({"custom_protocols*", "cmd_panels*", "status_panels*"}, QDir::Files | QDir::NoSymLinks)) {
        if (name.endsWith(".lock")) continue;
        if (!QFile::copy(QDir(data).filePath(name), QDir(backup).filePath(name))) {
            error = QStringLiteral("无法备份配置文件：%1\n已完成的备份保留在：%2").arg(name, backup); return false;
        }
    }
    return true;
}
}
