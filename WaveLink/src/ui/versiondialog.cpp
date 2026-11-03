#include "versiondialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QCoreApplication>
#include <QFrame>

VersionDialog::VersionDialog(UpdateManager *manager, std::function<void()> install, QWidget *parent)
    : QDialog(parent), m_manager(manager) {
    setObjectName("versionInfoDialog"); setWindowTitle(QStringLiteral("版本信息"));
    setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) & ~Qt::WindowContextHelpButtonHint);
    resize(720, 600); setMinimumSize(520, 440); setSizeGripEnabled(true);
    auto body = new QVBoxLayout(this); body->setSpacing(12); body->setContentsMargins(20, 20, 20, 20);
    // Keep the original About presentation first. The manual address and
    // extraction code remain visible even without an online update service.
    auto current = new QLabel; current->setObjectName("versionSummary");
    current->setTextFormat(Qt::RichText); current->setWordWrap(true);
    current->setTextInteractionFlags(Qt::TextBrowserInteraction); current->setOpenExternalLinks(true);
    current->ensurePolished();
    const auto linkColor = current->palette().color(QPalette::WindowText).lightness() > 128 ? "#65bfff" : "#006a9e";
    current->setText(QStringLiteral(
        "<table border='0' cellpadding='0' cellspacing='5'>"
        "<tr><td valign='middle'><img src=':/app.ico' width='28' height='28'></td>"
        "<td valign='middle'><h2>WaveLink 信号采集与波形分析系统</h2></td></tr></table>"
        "<table border='0' cellpadding='2' cellspacing='0'>"
        "<tr><td><b>Version:&nbsp;&nbsp;</b></td><td>v%1</td></tr>"
        "<tr><td><b>Copyright (C):&nbsp;&nbsp;</b></td><td>xiales</td></tr>"
        "<tr><td><b>蓝奏云下载:&nbsp;&nbsp;</b></td><td><a style='color:%2;' href='https://xiales.lanzouu.com/b007unma2b'>https://xiales.lanzouu.com/b007unma2b</a>（提取码：0000）</td></tr>"
        "<tr><td><b>GitHub 下载:&nbsp;&nbsp;</b></td><td><a style='color:%2;' href='https://github.com/yffdd/wavelink/releases'>https://github.com/yffdd/wavelink/releases</a></td></tr></table>"
    ).arg(QCoreApplication::applicationVersion().toHtmlEscaped(), QString::fromLatin1(linkColor)));
    current->setToolTip(QStringLiteral("点击下载地址可在浏览器中打开；版本信息、地址和提取码均可选中复制。"));
    body->addWidget(current);
    auto description = new QLabel(QStringLiteral("基于 Qt5.14.2 构建的轻量化数据采集与波形渲染平台。"));
    description->setWordWrap(true); body->addWidget(description);
    auto divider = new QFrame; divider->setFrameShape(QFrame::HLine); divider->setFrameShadow(QFrame::Sunken); body->addWidget(divider);
    auto heading = new QLabel(QStringLiteral("在线更新")); heading->setObjectName("versionUpdateHeading");
    auto font = heading->font(); font.setBold(true); heading->setFont(font); body->addWidget(heading);
    m_status = new QLabel; m_status->setObjectName("updateStatus"); m_status->setWordWrap(true); m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); body->addWidget(m_status);
    m_release = new QLabel; m_release->setObjectName("updateRelease"); m_release->setWordWrap(true); body->addWidget(m_release);
    m_notes = new QPlainTextEdit; m_notes->setObjectName("updateReleaseNotes"); m_notes->setReadOnly(true); m_notes->setPlaceholderText(QStringLiteral("检查到新版本后，在这里展示该版本的更新说明。")); body->addWidget(m_notes, 1);
    m_progress = new QProgressBar; m_progress->setObjectName("updateProgress"); m_progress->setRange(0, 100); body->addWidget(m_progress);
    auto actions = new QHBoxLayout;
    m_check = new QPushButton(QStringLiteral("检查更新")); m_check->setObjectName("checkForUpdates");
    m_download = new QPushButton(QStringLiteral("下载更新")); m_download->setObjectName("downloadUpdate");
    m_cancel = new QPushButton(QStringLiteral("取消下载")); m_cancel->setObjectName("cancelUpdate");
    m_install = new QPushButton(QStringLiteral("退出并安装")); m_install->setObjectName("installUpdate");
    m_ignore = new QPushButton(QStringLiteral("忽略此版本")); m_ignore->setObjectName("ignoreUpdate");
    for (auto button : {m_check, m_download, m_cancel, m_install, m_ignore}) { actions->addWidget(button); button->setMinimumHeight(32); }
    for (auto button : {m_check, m_download, m_install}) button->setStyleSheet("QPushButton { background:#147dad; color:white; padding:6px 12px; border-radius:4px; } QPushButton:disabled { background:#808890; color:#e0e0e0; }");
    body->addLayout(actions);
    auto note = new QLabel(QStringLiteral("自动检查可在“设置 → 系统设置 → 软件更新”中关闭。下载不会停止采集或录制；安装前需结束录制并断开设备，自动备份配置后再退出。")); note->setWordWrap(true); body->addWidget(note);
    auto footer = new QHBoxLayout;
    auto close = new QPushButton(QStringLiteral("关闭")); footer->addStretch(); footer->addWidget(close); body->addLayout(footer);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_check, &QPushButton::clicked, manager, [manager] { manager->check(); });
    connect(m_download, &QPushButton::clicked, manager, &UpdateManager::download);
    connect(m_cancel, &QPushButton::clicked, manager, &UpdateManager::cancel);
    connect(m_ignore, &QPushButton::clicked, this, [this] { m_manager->ignoreVersion(); reject(); });
    connect(m_install, &QPushButton::clicked, this, [this, install] { accept(); install(); });
    connect(manager, &UpdateManager::changed, this, &VersionDialog::refresh);
    connect(manager, &UpdateManager::progress, this, [this](qint64 bytes, qint64 total) { m_progress->setValue(total > 0 ? int(bytes * 100 / total) : 0); });
    refresh();
}
void VersionDialog::refresh() {
    const auto state = m_manager->state(); const bool busy = state == UpdateManager::State::Checking || state == UpdateManager::State::Downloading;
    m_status->setText(m_manager->message()); m_status->setStyleSheet(state == UpdateManager::State::Error ? "color:#d64545;" : QString());
    const auto release = m_manager->release();
    const bool showRelease = state == UpdateManager::State::Available || state == UpdateManager::State::Downloading || state == UpdateManager::State::Ready;
    m_release->setText(showRelease ? QStringLiteral("新版本：v%1 · %2 · 安装包 %3 MB").arg(release.version, release.published.toLocalTime().toString("yyyy-MM-dd")).arg(release.size / 1048576.0, 0, 'f', 1) : QString());
    m_notes->setPlainText(showRelease ? release.notes : QString());
    m_check->setEnabled(!busy && state != UpdateManager::State::Ready);
    m_download->setVisible(state == UpdateManager::State::Available);
    m_install->setVisible(state == UpdateManager::State::Ready);
    m_ignore->setVisible(state == UpdateManager::State::Available);
    m_cancel->setVisible(busy); m_cancel->setText(state == UpdateManager::State::Checking ? QStringLiteral("取消检查") : QStringLiteral("取消下载"));
    m_progress->setVisible(state == UpdateManager::State::Downloading || state == UpdateManager::State::Ready);
    if (state == UpdateManager::State::Ready) m_progress->setValue(100);
}
