#include "helpdialog.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QFontDatabase>
#include <QListWidget>
#include <QToolButton>
#include <QDebug>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QWheelEvent>
#include <QtMath>

static void settleGuide()
{
    // Reflow after show/resize can change the outer scrollbar width once.
    for (int i = 0; i < 8; ++i) qApp->processEvents();
}

static int lightnessAt(const QPixmap &pixmap, int x, int y)
{
    const auto scale = pixmap.devicePixelRatioF();
    return pixmap.toImage().pixelColor(qRound(x * scale), qRound(y * scale)).lightness();
}

static bool verifyTheme(bool dark, const QString &outputDir)
{
    // Reproduce the application-level theme inherited by the dialog.
    qApp->setStyleSheet(dark
        ? "QWidget { background-color:#1e1e1e; color:#d4d4d4; }"
        : "QWidget { background-color:#f0f0f0; color:#333333; }");
    HelpDialog dialog(dark);
    dialog.resize(840, 560);
    dialog.show();
    qApp->processEvents();

    QListWidget *sidebar = dialog.findChild<QListWidget*>();
    if (!sidebar) return false;

    sidebar->setCurrentRow(0);
    qApp->processEvents();
    const QPixmap overview = dialog.grab();
    overview.save(outputDir + (dark ? "/help_overview_dark.png" : "/help_overview_light.png"));

    sidebar->setCurrentRow(1); settleGuide();
    auto guideScroll = dialog.findChild<QScrollArea *>("helpGuideScroll");
    auto expandAll = dialog.findChild<QPushButton *>("helpGuideExpandAll");
    auto collapseAll = dialog.findChild<QPushButton *>("helpGuideCollapseAll");
    if (!guideScroll || !expandAll || !collapseAll) return false;
    QList<QToolButton *> sections; QList<QTextBrowser *> bodies;
    for (int i = 1; i <= 8; ++i) {
        auto toggle = dialog.findChild<QToolButton *>(QString("helpGuideToggle%1").arg(i));
        auto body = dialog.findChild<QTextBrowser *>(QString("helpGuideBody%1").arg(i));
        if (!toggle || !body || !toggle->isCheckable() || toggle->isChecked() || !body->isHidden()
            || toggle->arrowType() != Qt::RightArrow || body->toPlainText().isEmpty()
            || body->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff) return false;
        sections.append(toggle); bodies.append(body);
    }
    dialog.grab().save(outputDir + (dark ? "/help_guide_collapsed_dark.png" : "/help_guide_collapsed_light.png"));
    sections[0]->click(); sections[2]->click(); settleGuide();
    if (!bodies[0]->isVisible() || !bodies[2]->isVisible() || !bodies[1]->isHidden()
        || sections[0]->arrowType() != Qt::DownArrow) return false;
    sections[0]->click(); settleGuide();
    if (!bodies[0]->isHidden() || !bodies[2]->isVisible()) return false; // Independent cards, not an accordion.
    expandAll->click(); settleGuide();
    for (auto section : sections) if (!section->isChecked()) return false;
    for (auto body : bodies) if (body->isHidden()) return false;
    for (const auto &size : {QSize(720, 480), QSize(1080, 760), QSize(840, 560)}) {
        dialog.resize(size); settleGuide();
        for (auto body : bodies) {
            if (body->viewport()->height() < qCeil(body->document()->documentLayout()->documentSize().height())
                || body->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff) return false;
        }
        if (guideScroll->verticalScrollBar()->maximum() <= 0 || guideScroll->horizontalScrollBar()->maximum() != 0) return false;
    }
    collapseAll->click(); settleGuide();
    for (auto section : sections) if (section->isChecked()) return false;
    for (auto body : bodies) if (!body->isHidden()) return false;
    sections[2]->click(); settleGuide(); guideScroll->verticalScrollBar()->setValue(0);
    const auto guide = dialog.grab(); guide.save(outputDir + (dark ? "/help_guide_expanded_dark.png" : "/help_guide_expanded_light.png"));
    // Content survives toggles and the existing theme replacement pass.
    if (!bodies[0]->toPlainText().contains(QStringLiteral("自定义数据协议"))
        || !bodies[5]->toPlainText().contains(QStringLiteral("WaveLink_Cache"))) return false;
    const auto interaction = bodies[1]->toPlainText(), recording = bodies[5]->toPlainText();
    if (!interaction.contains("AA 32 30") || !interaction.contains("先立即发送一次") ||
        !interaction.contains("不自动过期（0 ms）") || !interaction.contains("含未启用项") ||
        !interaction.contains("ASCII 英文字母") || !interaction.contains("最多保留 2000 条") ||
        !recording.contains("所有内置数值协议及自定义协议") ||
        !recording.contains("不停止录制、不截取或补造通道") || !recording.contains("Raw 文本/HEX 不生成")) return false;
    const int guideMargin = lightnessAt(guide, 200, 310);
    if (dark ? guideMargin > 90 : guideMargin < 180) return false;

    sidebar->setCurrentRow(2); settleGuide();
    auto download = dialog.findChild<QTextBrowser *>("helpDownloadLinks");
    if (!download || !download->openExternalLinks()
        || !download->toPlainText().contains("https://xiales.lanzouu.com/b007unma2b（提取码：0000）")
        || !download->toPlainText().contains("https://github.com/yffdd/wavelink/releases")
        || !download->toHtml().contains("href=\"https://github.com/yffdd/wavelink/releases\"")) return false;
    dialog.grab().save(outputDir + (dark ? "/help_download_dark.png" : "/help_download_light.png"));

    sidebar->setCurrentRow(3);
    qApp->processEvents();
    auto currentRelease = dialog.findChild<QToolButton *>("changelogCurrentRelease");
    auto currentText = dialog.findChild<QTextBrowser *>("changelogCurrentReleaseText");
    auto previousText = dialog.findChild<QTextBrowser *>("changelogV09Text");
    auto v010Text = dialog.findChild<QTextBrowser *>("changelogV010Text");
    auto v011Text = dialog.findChild<QTextBrowser *>("changelogV011Text");
    auto v012Text = dialog.findChild<QTextBrowser *>("changelogV012Text");
    auto v013Text = dialog.findChild<QTextBrowser *>("changelogV013Text");
    if (!currentRelease || !currentText || !previousText || !v010Text || !v011Text || !v012Text || !v013Text ||
        !currentRelease->text().contains("V" + QCoreApplication::applicationVersion()) ||
        !currentRelease->text().contains(QStringLiteral("当前版本")) ||
        !currentText->isVisible() || !previousText->isHidden() ||
        !currentText->toPlainText().contains(QStringLiteral("视图测量")) ||
        !currentText->toPlainText().contains(QStringLiteral("按需加载")) ||
        !currentText->toPlainText().contains(QStringLiteral("背景颜色")) ||
        !v013Text->isHidden() || !v013Text->toPlainText().contains(QStringLiteral("状态监视面板")) ||
        !v013Text->toPlainText().contains(QStringLiteral("接收文本监视")) ||
        !v013Text->toPlainText().contains(QStringLiteral("全局配置集成")) ||
        !v012Text->isHidden() || !v012Text->toPlainText().contains(QStringLiteral("UDP 数据接口")) ||
        !v012Text->toPlainText().contains(QStringLiteral("自定义协议")) ||
        !v012Text->toPlainText().contains(QStringLiteral("数据存储")) ||
        !v011Text->isHidden() || !v011Text->toPlainText().contains(QStringLiteral("自定义协议帧")) ||
        !v011Text->toPlainText().contains(QStringLiteral("自动切换文本/HEX")) ||
        !v010Text->isHidden() || !v010Text->toPlainText().contains(QStringLiteral("BLE 低功耗蓝牙")) ||
        !v010Text->toPlainText().contains(QStringLiteral("串口硬件流控")) ||
        !previousText->toPlainText().contains(QStringLiteral("录制与数据安全"))) return false;
    currentRelease->click();
    if (!currentText->isHidden()) return false;
    currentRelease->click();
    if (!currentText->isVisible()) return false;
    // Historical sections must never regain the "latest version" label.
    for (auto button : dialog.findChildren<QToolButton *>()) {
        if (button == currentRelease) continue;
        if (button->objectName().startsWith("helpGuideToggle")) continue;
        button->click();
        if (button->text().contains(QStringLiteral("当前版本"))) return false;
        button->click();
        if (button->text().contains(QStringLiteral("当前版本"))) return false;
    }
    auto changelogScroll = dialog.findChild<QScrollArea *>("helpChangelogScroll");
    if (!changelogScroll) return false;
    const auto releases = changelogScroll->findChildren<QToolButton *>();
    const auto releaseBodies = changelogScroll->findChildren<QTextBrowser *>();
    if (releases.size() != 14 || releaseBodies.size() != 14) return false;
    for (auto release : releases) if (release != currentRelease) release->click();
    for (const auto &size : {QSize(720, 480), QSize(1080, 760), QSize(840, 560)}) {
        dialog.resize(size); settleGuide();
        for (auto body : releaseBodies) {
            if (!body->isVisible()
                || body->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff
                || body->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff
                || body->viewport()->height() < qCeil(body->document()->documentLayout()->documentSize().height())) {
                qCritical() << "Changelog body clipped" << body->objectName() << size;
                return false;
            }
        }
        if (changelogScroll->verticalScrollBar()->maximum() <= 0
            || changelogScroll->horizontalScrollBar()->maximum() != 0) return false;
    }
    changelogScroll->verticalScrollBar()->setValue(0);
    QWheelEvent wheel(QPointF(12, 12), currentText->viewport()->mapToGlobal(QPoint(12, 12)),
                      QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(currentText->viewport(), &wheel); settleGuide();
    if (changelogScroll->verticalScrollBar()->value() <= 0) {
        qCritical() << "Changelog wheel did not scroll the shared page";
        return false;
    }
    changelogScroll->verticalScrollBar()->setValue(0);
    dialog.grab().save(outputDir + (dark ? "/help_changelog_expanded_dark.png" : "/help_changelog_expanded_light.png"));
    for (auto release : releases) if (release != currentRelease) release->click();
    settleGuide();
    const QPixmap changelog = dialog.grab();
    changelog.save(outputDir + (dark ? "/help_changelog_dark.png" : "/help_changelog_light.png"));

    const int overviewMargin = lightnessAt(overview, 190, 300);
    const int changelogMargin = lightnessAt(changelog, 205, 300);
    const int changelogScrollbar = lightnessAt(changelog, 812, 450);
    if (dark)
        return overviewMargin < 90 && changelogMargin < 90 && changelogScrollbar < 130;
    return overviewMargin > 180 && changelogMargin > 180 && changelogScrollbar > 120;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) QApplication::setFont(QFont(families.first(), 9));
    QCoreApplication::setApplicationVersion(QStringLiteral(WAVELINK_VERSION));
    const QString outputDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(outputDir);
    if (!verifyTheme(false, outputDir) || !verifyTheme(true, outputDir)) {
        qCritical() << "Help dialog version, changelog or theme verification failed";
        return 1;
    }
    qInfo() << "PASS: download links, shared changelog scrolling, full-height reflow and both themes";
    return 0;
}
