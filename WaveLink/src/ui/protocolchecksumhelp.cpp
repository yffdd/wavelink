#include "protocolchecksumhelp.h"
#include "ProtocolChecksum.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QTextBrowser>
#include <QPushButton>
#include <QApplication>
#include <QClipboard>
#include <QTextDocument>
#include <QPalette>

void showProtocolChecksumHelp(QWidget *parent, const QString &selectedAlgorithm) {
    QDialog dialog(parent); dialog.setObjectName("protocolChecksumHelpDialog");
    dialog.setWindowTitle("校验算法说明与 C 参考代码");
    dialog.setWindowFlags((dialog.windowFlags() | Qt::WindowMinMaxButtonsHint) & ~Qt::WindowContextHelpButtonHint);
    dialog.setMinimumSize(760, 440); dialog.resize(1000, 720); dialog.setSizeGripEnabled(true);
    auto layout = new QVBoxLayout(&dialog); auto split = new QSplitter(Qt::Horizontal); layout->addWidget(split, 1);
    auto list = new QListWidget; list->setObjectName("protocolChecksumHelpList"); list->setMinimumWidth(230); split->addWidget(list);
    auto body = new QTextBrowser; body->setObjectName("protocolChecksumHelpText"); body->setOpenExternalLinks(true); split->addWidget(body);
    // Keep references readable under every application theme; do not inherit
    // QTextDocument's hard-coded blue links on a dark background.
    body->ensurePolished();
    body->document()->setDefaultStyleSheet(QString("a { color: %1; text-decoration: underline; } pre { font-family: Consolas, 'Courier New', monospace; }")
        .arg(body->palette().color(QPalette::Text).name()));
    split->setStretchFactor(1, 1); split->setSizes({250, 720});
    int selected = 0;
    for (const auto &preset : DataProtocols::ProtocolChecksum::presets()) {
        auto item = new QListWidgetItem(preset.name, list); item->setData(Qt::UserRole, preset.id); item->setToolTip(preset.parameters);
        if (preset.id == selectedAlgorithm) selected = list->count() - 1;
    }
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [list, body] {
        if (auto item = list->currentItem()) body->setHtml(DataProtocols::ProtocolChecksum::detailedHelp(item->data(Qt::UserRole).toString()));
    }); list->setCurrentRow(selected);
    auto actions = new QHBoxLayout; layout->addLayout(actions);
    auto copy = new QPushButton("复制当前算法 C 代码"); copy->setObjectName("protocolChecksumCopyCode"); actions->addWidget(copy); actions->addStretch();
    auto close = new QPushButton("关闭"); actions->addWidget(close);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [list] {
        if (auto item = list->currentItem()) QApplication::clipboard()->setText(DataProtocols::ProtocolChecksum::cExample(item->data(Qt::UserRole).toString()));
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept); dialog.exec();
}
