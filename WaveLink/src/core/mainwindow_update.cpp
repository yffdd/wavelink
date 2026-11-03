#include "mainwindow.h"
#include "updatemanager.h"
#include "versiondialog.h"
#include "updatebackup.h"
#include "commandpanelwindow.h"
#include "statusmonitorwindow.h"
#include "ProtocolConfigTransfer.h"
#include <QStandardPaths>
#include <QStatusBar>
#include <QMessageBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QUuid>

void MainWindow::initializeUpdates() {
    m_updates = new UpdateManager(this); m_updates->setObjectName("applicationUpdates");
    auto notice = new QPushButton; notice->setObjectName("updateAvailableNotice"); notice->hide();
    notice->setToolTip(QStringLiteral("打开版本信息查看更新；不会自动安装或打断采集录制。"));
    statusBar()->addPermanentWidget(notice); connect(notice, &QPushButton::clicked, this, &MainWindow::showVersionInfo);
    connect(m_updates, &UpdateManager::updateFound, this, [this, notice] {
        notice->setText(QStringLiteral("发现更新 v%1").arg(m_updates->release().version)); notice->show();
    });
    connect(m_updates, &UpdateManager::changed, this, [this, notice] {
        QSettings settings("WaveLink", "WaveLink");
        if (settings.value("Update/IgnoredVersion").toString() == m_updates->release().version || m_updates->state() == UpdateManager::State::Current) notice->hide();
    });
    // No network in test mode, no startup error popup, at most once per day.
    QTimer::singleShot(10000, this, [this] {
        QSettings settings("WaveLink", "WaveLink");
        const auto last = settings.value("Update/LastCheckUtc").toDateTime();
        if (!QStandardPaths::isTestModeEnabled() && settings.value("Update/AutoCheck", true).toBool() &&
            (!last.isValid() || last > QDateTime::currentDateTimeUtc() || last.secsTo(QDateTime::currentDateTimeUtc()) >= 86400)) m_updates->check(true);
    });
}
void MainWindow::showVersionInfo() {
    if (!m_updates) return;
    VersionDialog dialog(m_updates, [this] { QTimer::singleShot(0, this, &MainWindow::installDownloadedUpdate); }, this);
    dialog.exec();
}
void MainWindow::installDownloadedUpdate() {
    if (!m_updates || m_updates->state() != UpdateManager::State::Ready) return;
    // The full installer upgrades an installed copy, not an IDE/portable tree.
    bool installed = false;
    for (const auto format : {QSettings::Registry32Format, QSettings::Registry64Format})
        for (const auto hive : {QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("HKEY_CURRENT_USER")}) {
            QSettings uninstall(hive + "\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{9F5B2F99-4A73-4D95-8F32-8D13F2716E34}_is1", format);
            auto path = uninstall.value("InstallLocation").toString();
            if (!path.isEmpty() && QDir(path).canonicalPath().compare(QDir(QCoreApplication::applicationDirPath()).canonicalPath(), Qt::CaseInsensitive) == 0) installed = true;
        }
    if (!installed) {
        QMessageBox::information(this, QStringLiteral("请使用安装版升级"), QStringLiteral("当前运行目录不是已登记的 WaveLink 安装目录。为避免覆盖开发或便携目录，请通过手动下载入口运行正式安装包；本次不会退出软件。")); return;
    }
    auto busy = [this] { return isPortOpen || protocolChangeBlocked() ||
        (m_connectionState != ConnectionState::Disconnected && m_connectionState != ConnectionState::Error); };
    if (busy()) {
        QMessageBox::information(this, QStringLiteral("更新已就绪"), QStringLiteral("安装不会自动中断采集录制。请先结束录制、停止发送并断开设备，然后从“帮助 → 版本信息”继续安装。")); return;
    }
    if (QMessageBox::question(this, QStringLiteral("退出并安装更新"), QStringLiteral("将备份系统设置、自定义协议、命令及状态面板，然后安全退出并安装新版。\n不会删除录制缓存和已保存的数据；请先保存其他编辑窗口中的修改。\n\n是否继续？"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes || busy()) return;
    if (!m_protocolStore || !m_protocolStore->ready()) {
        QMessageBox::warning(this, QStringLiteral("无法安全备份"), QStringLiteral("协议库尚未就绪，请先恢复配置后再更新。")); return;
    }
    saveConfig(); if (m_statusWindow) m_statusWindow->savePreferences();
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString backup, error;
    if (!UpdateBackup::create(m_protocolStore->snapshot(), data, backup, error)) {
        QMessageBox::critical(this, QStringLiteral("备份失败，未开始更新"), error.isEmpty() ? QStringLiteral("无法创建配置备份目录。") : error); return;
    }
    if (!m_updates->launchInstaller(QCoreApplication::applicationFilePath(), backup, error)) {
        QMessageBox::critical(this, QStringLiteral("未开始更新"), error + QStringLiteral("\n配置备份：") + backup); return;
    }
    close(); // Normal closeEvent saves state and shuts down the acquisition workers.
}
