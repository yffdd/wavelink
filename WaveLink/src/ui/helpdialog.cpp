#include <QFile>
#include <QFileDialog>
#include <QTimer>
#include "helpdialog.h"
#include "debuglogger.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QListWidgetItem>
#include <QDateTime>
#include <QToolButton>
#include <QScrollArea>
#include <QClipboard>
#include <QApplication>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QtMath>
#include <QStylePainter>
#include <QStyleOptionToolButton>

namespace {
// Native arrows avoid missing triangle glyphs in Windows fonts. Draw the
// label left-aligned while keeping QToolButton's normal keyboard/click states.
class GuideSectionButton final : public QToolButton {
public:
    explicit GuideSectionButton(QWidget *parent) : QToolButton(parent) { setToolButtonStyle(Qt::ToolButtonTextBesideIcon); }
protected:
    void paintEvent(QPaintEvent *) override {
        QStylePainter painter(this); QStyleOptionToolButton option; initStyleOption(&option);
        const auto direction = arrowType(); option.text.clear(); option.arrowType = Qt::NoArrow;
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
        QStyleOption arrow; arrow.rect = QRect(12, (height() - 16) / 2, 16, 16); arrow.state = option.state; arrow.palette = option.palette;
        style()->drawPrimitive(direction == Qt::DownArrow ? QStyle::PE_IndicatorArrowDown : QStyle::PE_IndicatorArrowRight, &arrow, &painter, this);
        painter.setPen(option.palette.color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
        const auto textRect = rect().adjusted(36, 0, -12, 0);
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, textRect.width()));
    }
};
// Guide cards grow to their wrapped document height. Only the surrounding
// page scrolls, so long instructions never get trapped in a nested viewport.
class GuideTextBrowser final : public QTextBrowser {
public:
    explicit GuideTextBrowser(QWidget *parent) : QTextBrowser(parent) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { scheduleFit(); });
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QTextBrowser::resizeEvent(event);
        if (event->size().width() != event->oldSize().width()) scheduleFit();
    }
    void showEvent(QShowEvent *event) override { QTextBrowser::showEvent(event); scheduleFit(); }
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
    void changeEvent(QEvent *event) override {
        QTextBrowser::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange || event->type() == QEvent::ContentsRectChange) scheduleFit();
    }
private:
    void scheduleFit() {
        if (m_scheduled || m_fitting) return;
        m_scheduled = true;
        QTimer::singleShot(0, this, [this] {
            m_scheduled = false; m_fitting = true;
            const int chrome = height() - viewport()->height();
            document()->setTextWidth(qMax(1, viewport()->width()));
            setFixedHeight(qMax(40, qCeil(document()->documentLayout()->documentSize().height()) + chrome + 2));
            m_fitting = false;
        });
    }
    bool m_scheduled = false, m_fitting = false;
};
}

HelpDialog::HelpDialog(bool darkTheme, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("帮助中心");
    setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    resize(840, 560);
    setMinimumSize(720, 480);
    
    setupUi();
    applyTheme(darkTheme);

    connect(&DebugLogger::instance(), &DebugLogger::logAdded, this, &HelpDialog::onLogAdded);
    connect(&DebugLogger::instance(), &DebugLogger::logsCleared, this, &HelpDialog::onLogsCleared);
}

void HelpDialog::applyTheme(bool darkTheme)
{
    const QString window = darkTheme ? QStringLiteral("#1e1e1e") : QStringLiteral("#f4f4f4");
    const QString panel = darkTheme ? QStringLiteral("#252526") : QStringLiteral("#ffffff");
    const QString panelAlt = darkTheme ? QStringLiteral("#1a1a1a") : QStringLiteral("#f7f7f7");
    const QString text = darkTheme ? QStringLiteral("#d4d4d4") : QStringLiteral("#2b2b2b");
    const QString muted = darkTheme ? QStringLiteral("#aaaaaa") : QStringLiteral("#555555");
    const QString border = darkTheme ? QStringLiteral("#3c3c3c") : QStringLiteral("#c9c9c9");
    const QString accent = darkTheme ? QStringLiteral("#00ffcc") : QStringLiteral("#007a66");
    const QString blue = darkTheme ? QStringLiteral("#00aaff") : QStringLiteral("#006a9e");

    setStyleSheet(QStringLiteral(
        "QDialog { background-color: %1; color: %2; }"
        "QWidget[helpPage=\"true\"] { background-color: %1; color: %2; }"
        "QWidget[changelogContent=\"true\"] { background-color: %1; color: %2; }"
        "QWidget[helpGuideContent=\"true\"] { background-color: %1; color: %2; }"
        "QLabel { color: %2; background: transparent; }"
        "QScrollArea { background-color: %1; border: 1px solid %3; border-radius: 4px; }"
        "QScrollArea > QWidget > QWidget { background-color: %1; }"
        "QScrollBar:vertical { background-color: %6; width: 14px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background-color: %3; min-height: 28px; border-radius: 6px; margin: 2px; }"
        "QScrollBar::handle:vertical:hover { background-color: %5; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; background: none; border: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background-color: %6; }"
        "QPushButton { background-color: %4; color: %2; border: 1px solid %3; padding: 5px 12px; border-radius: 3px; }"
        "QPushButton:hover { border-color: %5; }"
    ).arg(window, text, border, panel, blue, panelAlt));

    m_sidebarList->setStyleSheet(QStringLiteral(
        "QListWidget { background-color: %1; color: %2; border: none; font-size: 16px; font-weight: bold; outline: 0; }"
        "QListWidget::item { height: 48px; padding-left: 20px; border-left: 4px solid transparent; }"
        "QListWidget::item:hover { background-color: %3; }"
        "QListWidget::item:selected { background-color: %3; color: %4; border-left: 4px solid %4; }"
    ).arg(panelAlt, text, panel, accent));
    m_stackedWidget->setStyleSheet(QStringLiteral("background-color: %1; color: %2;").arg(window, text));

    for (int index = 0; index < m_stackedWidget->count(); ++index) {
        QWidget *page = m_stackedWidget->widget(index);
        page->setObjectName(QStringLiteral("HelpDialogPage"));
        page->setAttribute(Qt::WA_StyledBackground, true);
        page->setStyleSheet(QStringLiteral(
            "QWidget#HelpDialogPage { background-color: %1; color: %2; }").arg(window, text));
    }

    const QList<QWidget*> changelogContents = findChildren<QWidget*>(
        QString(), Qt::FindChildrenRecursively);
    for (QWidget *widget : changelogContents) {
        if (widget->property("changelogContent").toBool() || widget->property("helpGuideContent").toBool()) {
            const bool guide = widget->property("helpGuideContent").toBool();
            widget->setObjectName(guide ? QStringLiteral("HelpGuideContent") : QStringLiteral("HelpChangelogContent"));
            widget->setAttribute(Qt::WA_StyledBackground, true);
            widget->setStyleSheet(QStringLiteral(
                "QWidget#%1 { background-color: %2; color: %3; }").arg(widget->objectName(), window, text));
        }
    }

    const QList<QScrollArea*> scrollAreas = findChildren<QScrollArea*>();
    for (QScrollArea *area : scrollAreas) {
        area->setStyleSheet(QStringLiteral(
            "QScrollArea { border: 1px solid %1; background-color: %2; border-radius: 4px; }"
            "QScrollArea > QWidget > QWidget { background-color: %2; }"
            "QScrollBar:vertical { background-color: %3; width: 14px; margin: 0; border: none; }"
            "QScrollBar::handle:vertical { background-color: %1; min-height: 28px; border-radius: 6px; margin: 2px; }"
            "QScrollBar::handle:vertical:hover { background-color: %4; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; background: none; border: none; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background-color: %3; }"
        ).arg(border, window, panelAlt, blue));
        area->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        area->viewport()->setStyleSheet(QStringLiteral("background-color: %1;").arg(window));
    }

    const QList<QTextBrowser*> browsers = findChildren<QTextBrowser*>();
    for (QTextBrowser *browser : browsers) {
        browser->setStyleSheet(QStringLiteral(
            "QTextBrowser { border: 1px solid %1; background-color: %2; color: %3; padding: 10px; font-size: 15px; }"
            "QScrollBar:vertical { background: %4; }"
        ).arg(border, panel, text, window));

        if (!darkTheme) {
            QString html = browser->toHtml();
            const QList<QPair<QString, QString>> replacements = {
                {QStringLiteral("#00ffcc"), accent}, {QStringLiteral("#00aaff"), blue},
                {QStringLiteral("#dddddd"), text}, {QStringLiteral("#d0d0d0"), text},
                {QStringLiteral("#cccccc"), text}, {QStringLiteral("#aaaaaa"), muted},
                {QStringLiteral("#888888"), muted}, {QStringLiteral("#ffffff"), text},
                {QStringLiteral("#333333"), border}, {QStringLiteral("#121212"), panel},
                {QStringLiteral("#1a1a1a"), panel}, {QStringLiteral("#1e1e1e"), window},
                {QStringLiteral("#252830"), panelAlt}, {QStringLiteral("#ff5555"), QStringLiteral("#b3261e")},
                {QStringLiteral("#ffaa00"), QStringLiteral("#8a5200")}
            };
            for (const auto &replacement : replacements) {
                html.replace(replacement.first, replacement.second, Qt::CaseInsensitive);
            }
            browser->setHtml(html);
        }
    }

    const QList<QToolButton*> toolButtons = findChildren<QToolButton*>();
    for (QToolButton *button : toolButtons) {
        const bool currentRelease = button->text().contains(QStringLiteral("当前版本"));
        button->setStyleSheet(QStringLiteral(
            "QToolButton { font-weight: bold; font-size: 16px; color: %1; background-color: %2; border: 1px solid %3; border-radius: 4px; padding: 8px 12px; text-align: left; }"
            "QToolButton:hover { background-color: %4; }"
        ).arg(currentRelease ? accent : blue, currentRelease ? panelAlt : panel,
              currentRelease ? blue : border, panelAlt));
    }

    const QList<QLabel*> labels = findChildren<QLabel*>();
    for (QLabel *label : labels) {
        if (!label->styleSheet().isEmpty()) {
            label->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 16px; color: %1;").arg(accent));
        }
    }

    const QList<QPushButton*> pushButtons = findChildren<QPushButton*>();
    for (QPushButton *button : pushButtons) {
        if (button == m_btnClearLogs) {
            button->setStyleSheet(darkTheme
                ? QStringLiteral("background-color: #552222; color: #ffaaaa; border: 1px solid #ff5555; padding: 4px 12px; border-radius: 3px; font-weight: bold;")
                : QStringLiteral("background-color: #fff0f0; color: #8f2525; border: 1px solid #d15b5b; padding: 4px 12px; border-radius: 3px; font-weight: bold;"));
        } else if (button->text().contains(QStringLiteral("复制"))) {
            button->setStyleSheet(darkTheme
                ? QStringLiteral("background-color: #225522; color: #aaffaa; border: 1px solid #55ff55; padding: 4px 12px; border-radius: 3px; font-weight: bold;")
                : QStringLiteral("background-color: #edf8ef; color: #17663a; border: 1px solid #49a96d; padding: 4px 12px; border-radius: 3px; font-weight: bold;"));
        }
    }

    if (m_debugTextBrowser) {
        m_debugTextBrowser->setStyleSheet(QStringLiteral(
            "background-color: %1; color: %2; font-family: Consolas, Monaco, monospace; font-size: 12px; border: 1px solid %3; padding: 8px;"
        ).arg(panel, darkTheme ? QStringLiteral("#00ffaa") : QStringLiteral("#006b55"), border));
    }
}

void HelpDialog::setupUi()
{
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Left sidebar (WeChat-style navigation)
    m_sidebarList = new QListWidget(this);
    m_sidebarList->setFixedWidth(180);
    m_sidebarList->setFocusPolicy(Qt::NoFocus);
    m_sidebarList->setStyleSheet(
        "QListWidget {"
        "   background-color: #2b2e36;"
        "   color: #c0c7eb;"
        "   border: none;"
        "   font-size: 16px;"
        "   font-weight: bold;"
        "   outline: 0;"
        "}"
        "QListWidget::item {"
        "   height: 48px;"
        "   padding-left: 20px;"
        "   border-left: 4px solid transparent;"
        "}"
        "QListWidget::item:hover {"
        "   background-color: #353842;"
        "   color: #ffffff;"
        "}"
        "QListWidget::item:selected {"
        "   background-color: #3e424e;"
        "   color: #00ffcc;"
        "   border-left: 4px solid #00ffcc;"
        "}"
    );

    new QListWidgetItem("软件介绍", m_sidebarList);
    new QListWidgetItem("使用说明", m_sidebarList);
    new QListWidgetItem("软件下载", m_sidebarList);
    new QListWidgetItem("更新日志", m_sidebarList);
    new QListWidgetItem("程序调试", m_sidebarList);

    // Right stacked content
    m_stackedWidget = new QStackedWidget(this);
    m_stackedWidget->setStyleSheet("background-color: #1e1e1e; color: #e0e0e0;");

    m_stackedWidget->addWidget(createOverviewPage());
    m_stackedWidget->addWidget(createGuidePage());
    m_stackedWidget->addWidget(createDownloadPage());
    m_stackedWidget->addWidget(createChangelogPage());
    m_stackedWidget->addWidget(createDebugPage());

    for (int index = 0; index < m_stackedWidget->count(); ++index) {
        m_stackedWidget->widget(index)->setProperty("helpPage", true);
    }

    mainLayout->addWidget(m_sidebarList);
    mainLayout->addWidget(m_stackedWidget);

    connect(m_sidebarList, &QListWidget::currentRowChanged, this, &HelpDialog::onSidebarItemChanged);
    m_sidebarList->setCurrentRow(0);
}

void HelpDialog::onSidebarItemChanged(int currentRow)
{
    if (currentRow < 0 || currentRow >= m_stackedWidget->count()) return;
    m_stackedWidget->setCurrentIndex(currentRow);
    if (currentRow == 4 && m_debugTextBrowser) {
        m_debugTextBrowser->setPlainText(DebugLogger::instance().getLogs().join("\n"));
        m_debugTextBrowser->moveCursor(QTextCursor::End);
    }
}

QWidget* HelpDialog::createOverviewPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(25, 25, 25, 25);

    QTextBrowser *browser = new QTextBrowser(page);
    browser->setOpenExternalLinks(true);
    browser->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    browser->setStyleSheet("border: none; background: transparent; font-size: 15px; line-height: 1.6; color: #aaaaaa;");
    
    QString html = QString::fromUtf8(R"(
        <h2 style='color: #00ffcc; margin-bottom: 10px;'>WaveLink 信号采集与波形分析系统</h2>
        <p style='font-size: 16px; color: #cccccc;'>
            <b>WaveLink</b> 是一款专为工业测量、信号处理及嵌入式串口调试打造的高性能上位机系统。
            软件集成了串口、TCP、UDP 与 BLE 低功耗蓝牙数据收发、Demo 波形、多通道波形显示、数字滤波、FFT 频谱分析和数据录制功能。
        </p>
        <hr style='border: 1px solid #333333; margin: 15px 0;'/>
        <h3 style='color: #00aaff;'>核心优势与特性</h3>
        <ul style='color: #aaaaaa; line-height: 1.8;'>
            <li><b>多接口与多协议：</b>支持串口、TCP 客户端、TCP 服务端、UDP 用户数据报、BLE 低功耗蓝牙和 Demo 数据；可按数据格式选择 Raw、FireWater、JustFloat 或自定义协议；专用 CBF/IR/RED 可从多通道键值模板创建。</li>
            <li><b>波形与分析：</b>支持多通道显示、独立视图、数字滤波、FFT 频谱和信号特征测量。</li>
            <li><b>录制与回放：</b>提供 CSV 兼容录制和 WLR 高速录制，并支持导入历史数据。</li>
            <li><b>灵活布局：</b>面板可折叠或调整大小，支持数据收发模式与恢复默认布局。</li>
            <li><b>设备交互：</b>命令交互面板管理按钮、滑条和开关命令；状态监视面板可解析通道数值或设备文本反馈，支持实时状态与事件提示。</li>
            <li><b>双主题：</b>支持深色和浅色主题切换。</li>
        </ul>
    
        <br>
        <hr style='border: 1px dashed #333333; margin: 15px 0;'/>
        <p style='color: #888888; font-size: 15px;'><b>开发平台：</b>本软件基于 <b>C++</b> 与 <b>Qt</b> 框架构建，采用 CMake 管理工程。</p>
    )");
    browser->setHtml(html);
    layout->addWidget(browser);
    return page;
}

QWidget* HelpDialog::createGuidePage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 20, 20, 20); layout->setSpacing(12);
    auto title = new QLabel("WaveLink 使用指南", page); title->setStyleSheet("font-weight: bold; font-size: 16px; color: #00ffcc;"); layout->addWidget(title);
    auto intro = new QLabel("点击标题展开或收起。建议按“选择接口与协议 → 建立连接 → 查看波形或文本 → 按需录制”的顺序操作。", page);
    intro->setWordWrap(true); layout->addWidget(intro);
    auto actions = new QHBoxLayout; auto expand = new QPushButton("全部展开"), collapse = new QPushButton("全部收起");
    expand->setObjectName("helpGuideExpandAll"); collapse->setObjectName("helpGuideCollapseAll");
    actions->addWidget(expand); actions->addWidget(collapse); actions->addStretch(); layout->addLayout(actions);
    auto scroll = new QScrollArea(page); scroll->setObjectName("helpGuideScroll"); scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); layout->addWidget(scroll, 1);
    auto content = new QWidget; content->setProperty("helpGuideContent", true);
    auto cards = new QVBoxLayout(content); cards->setContentsMargins(10, 10, 10, 10); cards->setSpacing(10);
    scroll->setWidget(content);

    QString html = R"(
<h2 style='color: #00ffcc;'>WaveLink 使用指南</h2>
<p style='color: #cccccc;'>建议按“选择接口与协议 → 建立连接 → 查看波形或文本 → 按需录制”的顺序操作。</p>
<hr style='border: 1px solid #333333;'/>

<h4 style='color: #00aaff;'>1. 选择数据接口与协议</h4>
<p style='color: #cccccc;'>
在左侧【参数设置】中选择数据接口：<b>串口</b>、<b>TCP 客户端</b>、<b>TCP 服务端</b>、<b>UDP 用户数据报</b>、<b>BLE 低功耗蓝牙</b>或<b>Demo 数据</b>。串口模式需选择端口，并确认波特率、数据位、校验位和停止位与设备一致；首次使用的波特率预设为 115200。TCP / UDP 模式按界面填写地址和端口；Demo 模式可设置波形类型、通道数等参数。<br><br>
<b>串口 DTR / RTS：</b>位于串口参数末尾、打开串口按钮上方。默认不勾选，勾选表示置位、取消表示释放，设置自动保存；未连接时供下次打开使用，已连接时在后台应用。这是手动控制信号，不是 RTS/CTS 硬件流控，普通数据收发未必需要开启。部分开发板会因此复位或进入下载模式，请按设备说明使用。驱动无法确认控制信号时会显示可复制提示，对应项呈半选并在当前连接中禁用，不会仅因不支持控制线就停止接收或录制；设备真正掉线时仍执行正常的异常关闭。<br><br>
Windows 下，串口句柄打开后先合并应用 DTR/RTS、释放 Break，再报告连接成功；关闭前再次核对请求状态，不恢复打开前的旧设置。相同且已确认的状态不重复切换，设置未确认时不盲目改用逐个切换。取消勾选不代表能阻止驱动在打开/关闭句柄瞬间产生电平变化，无法保证所有自动下载电路完全不复位，仍需实机核对。ESP32 等设备普通调试应使用无流控，按需释放 DTR/RTS。<br><br>
<b>串口流控：</b>位于“停止位”下方，可选“无流控（默认）”或“硬件流控（RTS/CTS）”。设置自动保存，只能在未连接时修改。硬件流控需要转换器、设备和 RTS/CTS 接线支持；RTS 改由驱动自动控制，切回无流控后恢复原来的手动设置。CTS 未就绪时显示等待和待发送大小，接收、录制继续；待发送队列上限为 256 KB 或 1024 条发送请求，满后拒绝新命令并停止命令循环。文件发送按实际串口写入完成推进，期间禁止混入其他命令，最后一段写完才显示完成。“取消待发送”或“停止发送”会丢弃软件队列中的数据，不会自动重发；仅在软件队列等待时可保留连接，已有未完成写入进入驱动时则安全关闭串口并停止录制，以可复制弹窗明确提示。已提交到驱动或硬件的少量字节仍可能到达，无法撤回；写入完成不等于设备业务确认。普通三线调试一般选择无流控。<br><br>
<b>串口 Break：</b>位于 DTR/RTS 下方，仅连接成功且驱动支持时可用。点击【发送】按设定毫秒数短时保持发送线，到时自动释放；勾选【保持】则持续保持，取消勾选或点击【释放】结束。每次启动和重新连接均默认释放，只记住时长，不保存开启状态。Break 不是发送 0x00，普通文本/波形调试不需要开启；执行期间禁用普通发送并停止命令循环，文件发送期间不能开启，结束后不会自动重发命令。健康连接的接收和录制继续运行；驱动不支持会提示并禁用 Break，无法确认释放时安全关闭串口。部分设备可能因此复位，本功能不是精确定时的 LIN/DMX 协议发送。各控件可悬停查看说明。<br><br>
<b>BLE 低功耗蓝牙：</b>扫描并选择设备，首次连接在快速配置中选择接收（Notify/Indicate）和发送特征，也可先仅接收候选通道。候选按属性和结构推荐，需核对实际用途，不会发送探测命令；已知固件升级服务不参与推荐。写入方式自动适配，服务树、模板和分包参数位于默认收起的高级配置；无法应用时显示具体原因。需配对时先在 Windows 蓝牙设置中配对；经典蓝牙虚拟 COM 口仍使用串口接口。默认 Raw 文本/Hex 调试；数值数据可选择匹配的 FireWater、JustFloat 或自定义协议，使用波形分析及 CSV/WLR 录制。录制保留原始通知接收时间，跨包帧使用组帧完成那次通知的时间；批量数据可在导入时重建时间轴。停止录制会在后台完成文件收尾，期间不能再次录制、保存录制文件或清空缓存。发送源文件在独立后台进程中打开和分段读取，读取失败/超时只停止文件任务，不主动中断仍健康的 BLE 接收和录制；准备期间显示等待状态。文件默认按 20 字节 / 10 ms 分包节流，高级参数应用后按实际生效值发送；发送期间不接受其他命令或参数修改。最后一包确认前不显示完成，进度表示 GATT 完成/提交，并非外设业务确认。接收积压超限会报错并断开；取消/掉线不会自动重发排队数据，已提交的最后一包可能到达，结算前不能开始新发送。<br><br>
<b>BLE 配置与模板：</b>“收发配置…”区分当前已确认配置与下次连接配置，保存不会在线替换订阅；修改下次通道配置后重新连接需确认并核对特征。成功配置会按设备记忆，后续服务结构、UUID 与属性一致且发送参数无需风险确认时快捷应用；旧配置、重复 UUID、服务变化及需风险确认的参数仍打开配置窗口，不自动发送业务命令。录制、文件发送或连接操作期间只读。自定义模板支持保存、JSON 导入/导出，不包含设备身份或临时句柄；导入及同名覆盖需确认。Nordic UART 模板仅预选唯一匹配的 UUID，不按设备名称猜测通道，仍需确认透传用途。HC-08 / E104-BT52 暂无实机验证，需手动核对实际 UUID。<br><br>
<b>BLE 高级发送控制：</b>在“收发配置…”中展开高级区域，设置分包 1–512 字节、每包完成后的最小间隔 0–1000 ms，默认仍为 20 字节 / 10 ms。界面区分设置值与实际生效值；实际分包受协商 MTU−3 与后端上限限制，能力未知时最多 20 字节。Windows 上限不是模块缓冲容量，增大分包或降低间隔需每次确认风险；0 ms 仍只保留一个在途 GATT 写入，不并发。发送空闲时可仅应用发送参数到当前连接，确认成功后自动保存，不改变 UUID/订阅。模板不保存提速授权或协商 MTU。能力增大不自动提速；能力降低时停止待发数据并提供可复制提示，接收和录制继续，不自动重发。重新确认合适参数后需手动开始新发送；更快参数仍需要真实模块连续序号测试。<br><br>
<b>BLE 连接诊断：</b>每秒刷新并支持复制，默认隐藏设备身份和错误路径；可主动勾选完整信息。分别显示通知、Rx/Tx、解析帧、录制写入已接纳和仅显示丢帧；仅显示丢帧不代表录制丢帧，录制已接纳可能尚未刷写。阶段耗时包含系统等待，未知 MTU/适配器详情不猜测，Tx 不表示设备业务确认。<br><br>
<b>TCP 客户端：</b>远端 IP/端口填写实际设备地址（也支持主机名）和监听端口，不能将 0.0.0.0 用作远端 IP。本机 IP 默认 0.0.0.0，由系统选择出口地址；本机端口默认 0，表示自动分配。可指定本机已有 IP 和未占用端口，连接后显示实际本机端点。指定本机端口并使用主机名时，0.0.0.0 选择 IPv4，:: 选择 IPv6。<br><br>
<b>TCP 服务端：</b>监听 IP 支持手动输入和下拉选择。默认 0.0.0.0 监听所有本机 IPv4 地址，127.0.0.1 只接受本机连接；指定网卡地址时只在该地址监听，IPv6 可手动输入 :: 或具体地址。默认监听端口 4970。服务器开启后需等待客户端接入才能发送。所有网络参数自动保存，支持全局配置导入导出；导入不会自动连接。连接或监听期间参数锁定，绑定失败不会自动改用其他 IP/端口。设备 TCP 直接连接，不使用系统 HTTP/SOCKS 代理，也不修改系统代理设置；监听全部网卡时请注意防火墙与局域网访问权限。<br><br>
<b>UDP 用户数据报：</b>接口位于 TCP 服务端之后、BLE 之前。本机 IP 默认 0.0.0.0（监听所有本机 IPv4 地址），端口默认 4970；端口 0 自动分配，打开后显示实际端口。设备应发送到电脑实际网卡 IP，而不是 0.0.0.0。仅接收时远端 IP 留空、远端端口设为 0；发送时须同时设置远端单播 IP 和 1–65535 端口。勾选“仅接收远端数据”后过滤源 IP 和端口，未设远端时接收全部来源。本版仅单播，不支持主机名、广播或组播；IPv6 可手动输入，本机与远端须同类型。<br><br>
UDP 状态为“监听中”，不代表远端在线。每包独立解析，一包可含多帧，但不跨包拼接残帧；文本须带结束符，多通道键值须一包包含完整组。支持文本/HEX、命令与定时发送、绘图和 CSV/WLR 录制。一次发送一包，最多 65507 字节，不自动拆包；建议控制包长避免分片。UDP 可能丢包、乱序或重复，录制只保存实际接收且解析成功的数据；重要采集建议加序号、校验或改用 TCP。发送计数不是远端确认，本版不提供 UDP 文件发送。参数独立自动保存并参与配置导入导出，重启/导入不自动监听。打开期间参数锁定，收发错误会停止相关操作，不自动换端口或重发。请核对防火墙 UDP 入站权限。<br><br>
数据协议须与输入内容匹配：普通文本调试选 <b>Raw（文本/Hex）</b>；逗号分隔的数值行选 <b>FireWater（CSV 文本）</b>；二进制浮点数据选 <b>JustFloat</b>，并检查大小端；多行键值数据选 <b>自定义协议 → 多通道键值协议</b>，CBF/IR/RED 可从设备模板创建。悬停“数据接口”可查看各接口用途，下拉项也有简要提示；连接区选项名称和输入控件可悬停查看说明。不确定格式时，可先参考协议旁的“?”说明。不要把普通文本当作浮点波形解析。
<br><br><b>自定义数据协议：</b>先选择【自定义协议】，再点击蓝色【⚙ 管理自定义协议 → 新增协议】（选择内置协议时不显示管理按钮），先选择文本、二进制固定长度、长度字段、首尾分隔或多通道键值类型，再配置字段、系数及校验，解析样例并保存。主界面选择“自定义协议”或更换协议时自动应用，等待通信线程确认后显示已应用；没有可用协议时提示新建或导入，并阻止开启连接。长度字段帧根据所选长度含义和帧结构自动计算总帧长，无需手动设置修正或校验范围；新建长度字段及首尾分隔协议默认动态数组，按实际数据区自动计算通道数；混合类型可选择固定字段。提供 SUM8、XOR8、CRC8/SMBUS、CRC16/MODBUS、CCITT-FALSE、XMODEM、CRC32/ISO-HDLC，具体参数在编辑器展示。串口、TCP 和 BLE 共用规则；所有接口共用并保存同一个当前协议。管理窗口保存、独立导入后，在返回主界面时同步当前所选规则；录制、发送或连接操作期间禁止切换。结构变化涉及绘图历史时仍需确认，磁盘录制文件保留。通道按字段名称及单位显示；只有通过验证的数值帧参与绘图和录制，CSV/WLR 回放不再次执行换算。Demo 不使用自定义接收解码。
<br><br>协议管理采用三栏布局：左上协议列表、左下管理操作，中间配置，右侧测试；可拖动分隔线调整宽度，内容过长可滚动。管理区保留新增、删除、导入、导出所选、导出全部及使用说明；删除可用 Ctrl/Shift 多选。选择“无校验”时隐藏算法摘要，统一规则说明始终展示；范围、位置和长度修正由系统计算，不再手动配置。使用说明为可缩放的分类阅读窗口，支持主题适配、滚动和复制。列表右键提供复制、重命名、排序等快捷操作和当前协议的“协议说明”；副本紧跟原项，名称自动追加 _1、_2 等后缀，不自动选择副本用于采集。有效备份用于协议库损坏后的恢复，仅加载失败时展示恢复入口，恢复前文件会另存保留。命令交互面板的左侧列表同样支持右键复制面板，副本保留命令及布局，不自动启动定时发送。
<br><br>新增协议会立即出现在列表并选中，名称随编辑同步更新。类型可选文本分隔、二进制固定长度、二进制长度字段、二进制首尾分隔或<b>多通道键值协议</b>；键值类型可选择通用模板或 <b>CBF/IR/RED 设备模板</b>，协议列表右键【新增协议 → 新增模板协议】也可快捷创建，不会默认加入新用户的空列表。键值协议按行读取键和值，全部配置通道收到后组帧，按表格顺序输出；提供重复键、未知键及组帧超时处理。CBF 模板键 0、1、2 对应 CBF（默认单位 MHz）、IR/LED1、RED/LED2；设备采样率默认 31 Hz，采用 TCP 服务端，默认端口 4970。“启用TCP服务器预设”默认勾选，可按需关闭；未连接时应用协议会设置 TCP 服务器和端口，不会自动开始监听，可选择首次完整帧后自动分离视图。旧内置 CBF 选择自动迁移到自定义模板并保留原预设。没有帧序号的键值流不能完全识别丢包。点击蓝色【保存协议】或 <code>Ctrl+S</code> 保存；未保存的草稿不写入协议库，放弃新草稿会移除临时项。回到主界面选择协议后自动应用。
<br><br>文本配置按固定前缀（可空）、固定后缀（可空）、分隔方式、分隔符、结束符排列。空协议库展示欢迎提示，可直接新增或导入协议。结束符可下拉选择 CRLF、LF、CR、LFCR、NUL、ETX、EOT，也可在右侧手动输入 HEX；必须填写，不提供“无”选项，清空时提示补充。NUL 对应实际 00 字节。配置项名称及表头均可悬停查看说明。HEX 结束符、二进制帧头/帧尾、固定字节条件只接受十六进制字符及分隔空白，小写自动转为大写；含非法字符的粘贴不改变原内容。每个字节填写两位，不使用 <code>0x</code> 前缀；保存时仍校验完整字节及帧格式。文本前缀、后缀和分隔符仍按所选编码填写，不受 HEX 限制。
<br><br><b>文本通道配置</b>只需配置需要自定义名称、单位或换算的列，列号从 0 开始。先按表格顺序输出已配置通道，再按原列顺序追加未配置列，默认 ChN（N = 原列号 + 1）、无单位、原始值；通道表可留空。配置 2 路时输入 4 列仍读取 4 路，测试和接收一致，最多 256 路。空列、无效值或数量超限拒绝整帧，不静默截断；CSV/WLR 每次录制固定为开始后首个有效数值帧的通道数，数量不符只跳过该帧、不停止录制，也不截取或补造数据；实时显示仍按接收通道展示。底部状态栏红色累计显示“录制中 · 已跳过 N 帧异常数据”；停止后清除，再次录制重新确定数量并清零。真实写入失败仍停止提示。
<br><br><b>二进制通道配置</b>按字段偏移、类型和大小端读取。新建固定长度二进制默认勾选【数据区与通道自动关联】：新增/删除/移动通道或改变类型，按真实字节宽度重算连续偏移并更新固定数据区大小；例如 int16 + float32 后是 6。增大数据区，完整的未配置 4 字节补为 float32（ChN、系数 1、无单位）；不足 4 字节标明为保留字节，不猜测类型。缩小会删除无法完整容纳的通道，先确认，可取消；最多 256 路，不能静默丢弃更多完整字段。帧长和末尾校验位置同步调整；选择设备的长度字段含义，单位和修正值由系统计算。已有/导入的特殊布局保持手动模式与原偏移，需要联动时主动勾选，可关闭联动手动配置空隙。通道表完整向下展开，使用配置区整体滚动条，最大化不拉开配置项。
<br><br>新建/新增通道名为 Ch1、Ch2 等；名称留空保存补默认名，已有名称保留。CSV、WLR 和波形导出保存名称，导入恢复；旧 WLR 无名称使用 ChN。新建默认无校验、系数 1、偏移 0，首尾分隔默认 JustFloat 帧尾 0000807F，其他类型默认结束符/帧尾为 0D0A；已有通道名称、换算和超限选择保留，旧校验布局及长度修正统一覆盖。说明框下方实时显示帧长及结构，可选中复制。最小/最大值校验换算结果，不限制绘图 Y 轴；新建全部二进制及多通道键值协议均默认【超限通道标记为 NaN，其他通道保留】，文本分隔协议默认拒绝整帧，已有选择保持不变。NaN 模式排在第一项，拒绝整帧排在第二项。超限通道在绘图/分析及当前波形导出中记为 NaN；<b>录制选择原始数值时保留真实读数，选择换算后数值时保存 NaN</b>；设备发送的 NaN 表示缺失采样点，原始录制和换算录制均会保留。同帧其他通道按配置解析。拒绝整帧模式触发、收到无穷大（Inf）、换算溢出、无效格式或帧结构与校验错误时，该帧不会录制。大小端的“默认”继承协议设置。<b>固定字节条件</b>筛选命令码等标记，例如完整帧偏移 2 为 <code>01</code>；全部匹配才接受，可留空，不代替校验。
<br><br><b>通道备注与输入限制</b>通道表末尾的【备注】默认为空，可输入中英文、数字和符号（单行，最多 1024 个字符），随协议保存、复制及独立/全局配置导入导出，不参与解析和换算。单位仍可自由输入。数据偏移、文本列号、数组序号只接受非负整数；系数、换算偏移及最小/最大值只接受有限数值，支持负数、小数及科学计数法，小数点使用英文句点，不接受中文。最小/最大值可留空，系数和换算偏移不可留空；键值协议的【通道键】仍允许文本。
<br><br><b>帧头与帧尾预设</b>左侧下拉选择常用字节，右侧 HEX 始终可编辑。帧头提供 AA55、55AA、无帧头及自定义；帧尾提供 CRLF（0D0A）、LF（0A）、CR（0D）、JustFloat 小端（0000807F）、无帧尾及自定义。输入匹配预设时自动识别，否则显示自定义；“无”是空内容，不是 00。长度字段协议禁用“无帧头”。预设只填写边界字节，不代替完整协议模板，仍须核对帧长、数据区、大小端和校验；已有协议加载时按实际内容匹配，不覆盖原字节。
<br><br><b>二进制分帧与错位恢复</b>固定长度帧的帧头和帧尾可分别留空，但不能同时为空；同时为空时上方帧结构摘要标红，并禁止保存和解析测试。有帧头时寻找帧头并验证总帧长，配置帧尾时也检查帧尾。仅帧尾且无数据校验时，按总帧长检查帧尾位置，丢弃长度不符的片段并恢复同步。若从半帧或杂字节开始，首段可能随同步过程丢弃。长度字段帧必须有帧头和长度字段，帧尾可空；接收从半帧开始或出现损坏帧时重新寻找后续有效帧。正常半包保留等待；前方长度看似合法但内容不完整时，后续完整帧通过配置的边界、校验、条件和数值验证后可恢复同步。缺失的原始字节无法恢复，不补造数据。建议启用校验降低错误数据恰好符合帧结构的误判风险。
<br><br><b>首尾分隔动态数组：</b>没有长度字段，支持仅帧头、仅帧尾或首尾配对，至少设置一项。新建默认 JustFloat 式：N 个 float32 小端 + <code>0000807F</code>，1–256 路、无校验，按原始二进制字节发送。仅帧头需收到下一个帧头才结束上一帧，最后一帧暂存；样例生成自动附加下一帧头，测试显示正常等待提示。首尾都有时，新帧头可重新同步未结束帧。通道表只覆盖元数据/换算/范围，不限制实际数量；数据区须为类型宽度的整数倍，超长或数量超限整段拒绝。数据校验在帧尾前，无帧尾时在本帧末尾；无长度和帧头校验。<b>数据、保留/校验字节及跨字段组合不能包含相同分隔标记</b>，此模式不提供转义，CRC 不能消除切帧歧义；无帧头的首段完整性无法完全判断，从半个帧尾开始可能损失后续首帧。超长无帧头报文丢弃到下一完整帧尾后再同步，不会截取后半段伪造有效数据。可靠性要求高或任意二进制内容请用长度字段＋校验。独立协议及全局配置、CSV/WLR 录制与回放均支持新模式；UDP 不跨数据报拼接半帧，仅帧头需在同一数据报提供下一帧头。
<br><br><b>长度与校验说明</b>长度含义只需选择数据区字节数、整帧字节数，动态数组还可选择通道数量；总帧长修正由系统根据帧结构计算，不再手动配置。固定字段模式的最小总帧长包含固定数据区及头尾开销。所有数据校验固定从帧起点 0 开始，包含帧头、长度、保留和数据字节，到校验位之前结束；校验位紧贴帧尾之前，无帧尾时位于整帧末尾。不包含校验位自身及帧尾，多字节校验值跟随默认大小端。校验算法下面会直接展示规则；算法参数和 C99 示例见【说明 / C 代码】。可选帧头校验单独保护数据区之前的头部和长度，不是整帧数据校验。
<br><br>右侧【协议名称】默认跟随当前项，使用未保存草稿；选择其他项使用已保存版本，不切换编辑项、不保存、不改变连接。切换测试协议或类型会自动选择文本/HEX并生成对应样例，配置无效时清空旧样例并提示；仍可手动切换格式，普通参数编辑不会覆盖手工样例。【根据协议生成样例】替代样例文件入口，按所选规则补数值、长度、帧头尾及校验，通过实际解析器验证后才替换输入，失败保留原输入；生成成功不证明设备规则正确。格式切换转换当前样例表示、保留字节；文本支持 UTF-8、<code>\r、\n、\t、\\、\xHH</code>，HEX 仅合法字符且自动大写。勾选【HEX 字节间空格】每两位自动加显示空格，关闭后连续显示，空格不计入数据，无需手动输入；半个字节需补全。分包模拟整块/逐字节/随机接收边界，正确结果应一样，统计显示次数与大小，不是发送。【停止测试】仅处理大样例时显示，可保留已处理结果；小样例通常瞬间完成。选项及表头支持悬停说明，数值简洁展示，悬停查看完整精度，不修改采集或录制数值。
<br><br>二进制协议生成样例或从 HEX 转为文本时，全部字节统一显示为 <code>\xHH</code>，例如 <code>\x40、\x55</code>，帧尾为 <code>\x0D\x0A</code>，不混入可打印字符。文本协议保留可读文字与短转义；以前的混合字符/转义样例仍可解析。仅改变展示，不修改原始字节。
<br><br><b>解析错误诊断</b>在测试区下方以红色显示，尽可能指出原始样例字节偏移（从 0 开始）、帧内位置、预期/实际 HEX、出错通道或列号，并提示核对边界、分隔符、长度、校验、固定条件、编码或数值范围；疑似原因明确标为“可能”，不自动更改协议。样例结束还会检查未完成帧和缺失键值通道；这只用于离线测试，真实接收仍正常等待半包。最多展示 100 条，可选中复制。
<br><br>摘要第一行展示【帧结构】，随后展示总帧长与长度含义；内容可选中复制。尺寸不足、字段重叠、长度或数据配置冲突时摘要变红，并阻止保存和测试。新建变长协议默认为动态数组；【固定字段（按通道表提取）】只输出表中配置的数值。旧的非统一校验/长度规则在协议库加载时覆盖为新规则，并安全保存，不再提供旧规则配置入口。所有数据接口共用一个当前数据协议，串口、TCP、UDP、BLE 切换后沿用同一个选择；Demo 仍使用自身生成的数据，但不另行记忆协议。
</p>

<h4 style='color: #00aaff;'>2. 连接、收发与命令交互</h4>
<p style='color: #cccccc;'>
设置完成后点击连接按钮；Demo 模式使用【启动 Demo】。文本发送统一使用 UTF-8，接收区的 UTF-8/GBK 选项只影响接收显示，不改变发送编码；HEX 模式按实际字节发送。底部【数据收发】面板可查看接收内容、输入并发送文本/Hex，或发送文件。串口占用、设备断开等错误会提示；若串口打不开，请先关闭其他占用该端口的软件。<br><br>
<b>发送文件：</b>选择器默认显示所有文件，支持图片、文档、压缩包等任意类型。串口、TCP、BLE 均按文件原始字节分段发送，不受发送框 HEX/追加换行设置影响；.hex 文本也按原文发送，不自动解码。不附加文件名、长度、校验或保存指令，接收端须自行约定文件传输协议和保存方式。UDP 和 Demo 不提供文件发送。进度条右侧显示预计剩余时间（时:分:秒），根据最近实际写出/提交的字节速度估算；测速不足或长时间停滞时显示 --:--:--，完成、停止、失败或断开后显示 00:00:00。发送完成不等于接收端已校验或保存。<br><br>
<b>定时发送：</b>位于【清空发送】前，按输入的毫秒周期发送当前发送框内容，沿用 HEX 和追加换行设置。默认 1000 ms，第一个周期结束时首次发送；运行中修改周期会重新计时。周期自动保存并随全局配置导入导出，开启状态不保存。断开、切换接口、清空发送内容、Break、取消发送、开始文件发送或队列拒绝时自动停止，重连不自动恢复。停止后不再提交新数据，已排队字节仍可能发出，必要时使用“取消待发送”；软件周期不保证精确设备到达间隔。<br><br>
<b>保存接收文本：</b>【保存接收】保存点击时文本框中显示的内容，默认 TXT 文件名带年月日、时分秒和毫秒后缀。使用带 BOM 的 UTF-8，保留当前中文、HEX、时间戳和换行；空文本框会提示，取消保存不生成文件。选择文件期间新接收的内容不加入本次保存。只保存当前显示历史，不是持续日志或原始字节录制。<br><br>
<b>复制接收：</b>与【保存接收】、【显示缓存】位于接收区域第二行，一键复制接收文本框的全部内容，保留当前中文、HEX、时间戳和换行，不改变接收内容或选区。文本框为空时会提示，并保留剪贴板原有内容。缓存说明可悬停【显示缓存】标签或下拉框查看。<br><br>
<b>接收显示缓存：</b>默认最近 1000 行，可选 1000 / 2000 / 5000 行；默认最多 5 万字符、每 100 ms 批量刷新。行数按换行分段计算（含末尾空行），自动折行不额外计数；字符上限保护没有换行的二进制/HEX 显示。通过【设置 → 系统设置 → 数据收发】或接收区【接收设置】，字符上限以万字符设置（1–25 万，仅整数），建议使用安装默认值 5 万字符（50,000 字符）；刷新间隔可下拉选择 50、100（默认）、150、200、250、300、500、1000 ms，高速接收建议保持默认或降低缓存。达到任一上限只移除最旧内容，不整框清空；降低上限立即裁剪，提高上限不恢复旧内容。所有选项随配置保存及导入导出，仅限制显示及保存/复制接收的历史，不影响协议解析、状态监视、波形或录制。完整数据请使用持续录制。<br><br>
从【设备交互 → 命令交互面板】或【工具 → 命令交互面板】打开，可管理常用命令。命令左键操作保持发送/切换，短按右键打开菜单，长按右键约 0.7 秒进入编辑。菜单前部按类型提供发送一次、开始/停止定时、开关切换或指定状态、滑条当前值或恢复默认并发送，列表模式还可选择编号对应值发送；显式发送允许重发相同值，开关仍需发送入口接纳后更新显示。打开菜单或编辑不发送，定时命令的发送一次不改变定时状态。命令按钮开始定时时先立即发送一次，再按周期循环；数据收发区的定时发送则在一个周期结束后首次发送。停止仅停止后续提交，已排队的字节仍可能到达。菜单同时保留编辑、新增、复制、前移、后移和删除；详细用法见面板自己的“使用说明”。左侧面板列表的项目和空白处均支持右键菜单，可新增、导入和导出所有面板；单项操作仅针对右键的项目，空白处置灰且不改变原选择，没有面板时不能导出全部。
<br>重新打开软件后，命令交互面板会恢复上次选中的面板；仅当没有记忆记录或对应面板已不存在时，才打开第一个面板。改名、调整顺序不影响记忆；没有面板时展示欢迎提示。
<br>所有命令的“说明”（原“备注”）均不参与发送；鼠标悬停在按钮、滑条或开关命令上只显示说明内容，未填写时提示“暂无说明”。多行和特殊字符按原文展示，提示框适配浅白、暗黑主题。旧命令和面板备注自动作为说明读取，并随面板和软件整体配置导入导出。面板名称和说明统一在右上角【面板设置】中修改；左下角【导出所有】导出全部面板，列表右键仍可单独导出。
<br>悬停说明按当前屏幕可用空间自动换行，长串连续字符同样折行；能容纳时完整展示，过长时显示前部预览并提示右键编辑查看完整说明。原始说明完整保留，编辑窗口可滚动查看全部内容，提示框不会无限增大。
<br>旧版开关配置会先备份再自动兼容；命令正文缺失时保留为“待补全 · 禁止发送”，其他面板可正常使用，右键编辑并补全两个状态的命令后保存即可。无法读取配置时可点击窗口上方的“一键修复”：先备份原文件，优先修复当前配置，必要时恢复最近有效备份。修复前原文件单独保留，正常保存也维护上一份有效配置；没有可用数据时不会用空面板覆盖原文件。独立导入和全局配置导入使用相同兼容规则，已缺失且没有备份的命令内容须重新填写。
<br><br><b>开关命令：</b>与普通按钮尺寸相同，采用滑块式外观，只有状态 0/1，每个状态单独设置命令、状态名称和颜色（可选透明）；默认状态名称为关闭/开启，可自定义为模式 A/B 等。点击发送另一状态的命令，只有当前接口成功接纳发送后才切换显示；未连接、Break 占用、文件发送冲突或队列已满时状态不变。HEX 与追加方式共用，HEX 只限制命令及自定义追加；状态名称、说明可任意输入且不发送。开关不定时轮换，启动、重连、导入或切换主题/面板都不自动发送。保存默认状态而非运行状态，重启或重新载入后展示默认状态；配置参与面板及软件整体导入导出。<b>显示状态仅表示软件请求，不代表设备确认执行。</b>
<br><br>滑条命令支持常规模式和列表模式。常规模式按范围、步进和小数位生成数值文本；列表模式从自定义参数列表中选择，编号自动从 0 开始、步进固定为 1，外部滑条及右侧输入框显示编号，实际发送对应参数内容。列表可增删、上移和下移；默认值在顶部下拉选择，移动时保留所选参数，删除默认项时选择邻近项。最多 256 项，超过 10 项使用列表内部滚动条。HEX 列表仅接受完整的十六进制字节并自动大写，发送实际字节而非编号；格式切换会同步转换列表，无法无损转换时保留 HEX 并提示。常规滑条的 HEX 只将前缀/追加内容按字节解析，变化数值仍是十进制文本，并不自动编码为二进制整数或浮点数；例如 HEX 前缀 AA、数值 20（小数位 0）发送 AA 32 30。需要离散二进制参数时可用列表模式填写字节。文本命令不自动解释键入的反斜杠转义，实际换行请用追加设置或 Ctrl+Enter / Alt+Enter。列表、顺序、默认编号和滑条模式均自动保存，也随面板和软件整体配置导入导出；旧配置按常规模式加载。颜色选择框使用中文，可点击“拾取屏幕颜色”下方的“使用透明按钮”恢复默认透明样式，保存后生效。
</p>

<p style='color: #cccccc;'><b>状态监视：</b>从【设备交互 → 状态监视面板】或【工具 → 状态监视面板】打开。先新增面板，再添加监视项；数值来源支持原始值（默认）或换算值，提供状态映射、位段和阈值判断；映射/位段要求所选 8/16/32 位范围内的无符号整数，含小数或负数时应使用阈值规则；接收文本来源支持自定义完整匹配、前缀匹配、编码和结束符，并非只支持 ACK。文本监视独立读取 RX 字节，不受接收显示开关、HEX 显示或显示缓存上限影响。完整/前缀匹配按规则表顺序取首个命中，不是正则或任意位置关键词搜索；忽略大小写仅作用于 ASCII 英文字母，去除首尾空白只处理空格和 TAB。UDP 可按整报文匹配，但不会自动剥除 CR/LF，带结束符的报文可选结束符分帧。Demo 不提供文本反馈。<br><br>
<b>监视模式：</b>在解析来源之后选择“实时状态（默认）”或“事件提示”。事件提示适合单帧标记，后台逐帧捕获，新建默认保持至手动清除或下一事件（0 ms），也可选择定时清除；后续背景数据不覆盖提示，新事件立即替换提示。事件模式不使用生效延迟（防抖）或恢复回差，避免漏掉瞬时事件。卡片显示触发/屏蔽次数，右键可清除提示，不删除历史或清零计数；重连、改变影响解析的配置或重启后重新累计；仅改名称、说明、颜色等展示信息不因此重置当前监视状态。重复触发可选择进入匹配状态时触发（背景/未匹配后重新等待进入）或每次匹配均触发。<br><br>
<b>事件屏蔽：</b>最多配置 64 条屏蔽条件（含未启用项），任一启用条件命中即静默。支持固定数值、包含两端的数值区间，文本来源还支持固定文本。默认数值背景 0 可编辑、删除或取消勾选。数值条件作用于所选原始/换算值、位段提取前；浮点数建议使用区间。文本遵循分帧、编码、空白和大小写设置，数值类条件要求整条正文可解析为有限十进制数。屏蔽/未匹配内容不覆盖或延长事件提示，不影响录制、绘图及其他项；缺少通道、NaN/Inf、数据超时及断线仍提示异常。事件模式的“数据静默超时”按合法输入判断来源活跃（含屏蔽/未匹配内容），与提示保持时间独立。模式、条件均参与独立及全局配置导入导出，旧配置仍默认实时状态。<br><br>
卡片显示有效状态、等待、超时、断线等质量信息；有效状态的严重程度以独立标识展示：提示为蓝色、警告为橙色、错误为红色，“无”不显示标识，不自动发送命令或弹窗。顶部面板总开关与面板设置同步，重新启用时保留各监视项原有的启停设置，不影响设备连接或录制。左键启停此监视项，短按右键打开菜单，右键长按编辑，也可拖动调整顺序；不会断开设备连接。编辑中 Ctrl+S 只保存、不关闭，左侧“保存监视项”保存并关闭，其下方“关闭编辑”不保存未提交的修改；规则增删和排序支持 Ctrl+Z。在“面板设置”中统一选择锁定协议及结构，或跟随当前协议；所有通道监视项继承面板绑定，编辑监视项只选择来源通道及规则，两种方式均按序号而非名称读取。新面板默认锁定当前有效数值协议，Raw 等无数值协议时默认跟随。锁定时可从“系统协议 / 自定义协议”分组选择，JustFloat 大小端分别列出，Raw 无数值通道、不可选；选择后显示对应协议的通道名称，但不切换主界面采集协议。旧面板绑定一致时自动迁移，存在不同绑定则保留原行为并提示到面板设置统一；旧结构不会静默更新。数值绑定不匹配时不影响接收文本监视；协议列表超过 10 项可滚动。实时数值监视的“生效延迟（防抖）”要求条件持续满足，不是提示时长；阈值区间默认左闭右开，可分别设置包含边界，不允许重叠。新建监视项的“状态有效期”默认不自动过期（0 ms），已有设置保持不变；事件模式中显示为“数据静默超时”，新建同样默认关闭。超时为 0 不会掩盖断线、来源不匹配或无效数据。事件的“提示保持时间”另行控制提示时长，默认至手动清除或下一事件；均支持预设及手动输入。新增或修改后可生成样例、解析测试，解析测试与真实接收隔离，不会向设备发送。关闭监视窗口不停止后台监视；停用面板或监视项才停止对应判断。状态监视不自动发送命令、不更改命令开关，也不代表设备已执行某条发送请求。<br><br>
配置自动保存在本机，支持面板独立导入/导出、上次选择和窗口位置记忆；有效备份可恢复，读取失败不以空配置覆盖原文件。最近状态记录在内存中最多保留 2000 条，默认记录变化；文本实时项可选择记录每次匹配，事件模式也可逐次触发。筛选仅影响显示，导出包含全部保留记录；清空记录不清零触发/屏蔽次数，也不清除卡片提示。记录不跨软件重启保留，不是长期设备日志。软件设置菜单中的<b>全局配置</b>包含全部状态面板、规则、共享文本来源和窗口偏好；可单独选择覆盖、追加、跳过，旧文件无状态块时保留现有内容。协议追加时同步映射导入监视项的绑定，缺失或结构不符的绑定不会自动跟随通道。配置导入失败会回滚，中断事务下次启动恢复。二进制/文本混合流必须有设备约定的可靠边界，不能从任意二进制中截取关键词作为成功反馈。</p>

<h4 style='color: #00aaff;'>3. 波形、通道与独立视图</h4>
<p style='color: #cccccc;'>
主视图中左键拖动可平移，滚轮可缩放时间轴；关闭【Y 轴自适应】后，可按住 <code>Ctrl</code> 滚轮调整 Y 轴。波形双击用于测量，不是恢复 Y 轴自适应。点击【跳转至最新时刻】可回到实时数据；主视图下方时间范围条最右侧的【>>】作用相同。新会话默认 X 跨度为 10 秒，底部选中范围与主视图跨度同步；可拖动范围或修改跨度查看历史，范围受当前保留的绘图历史约束。<br><br>
<b>视图内示波器测量：</b>主视图和独立视图右键【测量】，开启底部可折叠测量栏。拖动 A/B 竖线读取时间差及对应通道数值，拖动 Y1/Y2 横线读取幅值差，也可在设置中精确输入位置。各视图独立选择通道、光标、当前可见范围或 A—B 区间；【区间统计】支持最小值、最大值、峰峰值、平均值、RMS 和总体标准差，【自动测量】支持频率、周期、正/负占空比、正/负脉宽、上升/下降时间。测量项目可选，结果表可滚动并复制。阈值默认 10%/50%/90%，可设置自动/手动高低电平、迟滞和周期容差。周期测量至少需要两个稳定完整周期且每周期至少 8 个采样间隔；数据不足、边沿无法解析时显示 --，不跨无效点或时间缺口拼接。1/Δt 是间隔倒数，仅当光标相隔一个完整周期时代表频率。使用当前保留的完整波形采样点，滤波开启时测量滤波后的值，时间精度取决于数据时间轴和采样率；单次超过 200 万点时请缩小范围。光标固定于数据时间，移出视窗后可重置；设置自动保存，光标坐标仅在当前数据会话内保留。右侧勾选仅控制主视图，独立视图持续显示其中的曲线。<br><br>
右侧【数据通道】可勾选通道显隐，颜色块可修改曲线颜色，通道勾选状态会保存。右键通道可分离到新视图；独立视图右键可【合并回主视图】，也可用【视图位置上移】或【视图位置下移】与相邻独立视图交换位置。独立视图较多时，使用绘图区右侧滚动条查看，不必挤压底部面板。<br><br>
主视图右键可【清空波形】【导入数据】【导出当前波形】，独立视图不提供这三项。主视图右键清空和右侧清空按钮都会先弹窗确认，确认后清除所有视图的历史波形和测量结果；取消保留数据，已保存的文件不受影响，连接和录制继续。导入与原有入口相同；导出与右侧按钮相同，保存所有通道当前保留的波形历史。
</p>

<h4 style='color: #00aaff;'>4. 绘图历史与完整录制的区别</h4>
<p style='color: #cccccc;'>
【数据通道 → 实时历史上限】仅限制实时采集时主视图和独立视图合计保留的<b>绘图数据</b>，默认 64 MB；达到上限后清理最旧的实时显示历史。离线导入默认展示完整时间范围，小文件完整加载，大文件自动按需浏览，全部采样点仍可访问；修改实时上限、滤波和分离视图均不会截掉文件里的数据。点击【暂停绘图】可暂停波形更新，按钮会变为【继续绘图（已暂停）】。数据接收和磁盘录制继续运行。<br><br>
<b>离线完整加载上限：</b>在【系统设置 → 绘图显示】设置，默认 256 MB，范围 16–4096 MB，并随全局配置保存及导入导出。按“采样行数 ×（通道数＋1 个时间列）× 8 字节”估算原始数值，同组分卷合计计算。实际额度还受可用内存 1/8 的保守保护约束；无法读取系统内存时保护额度为 128 MB。滤波、绘图和索引仍需额外内存，此值不是程序总内存的精确上限。<br><br>
<b>大文件按需浏览：</b>超过额度后在本机缓存路径建立临时浏览缓存，内存只保留索引和当前显示区间；概览保留峰谷，放大后读取原始采样点，界面会区分“峰谷概览”和“原始采样点”。光标、测量及分析读取原始数据，超出单次分析点数限制时提示缩小范围，不使用概览点代替计算；导出会流式读取整份离线数据。滤波按原始顺序连续计算，翻页不重置滤波。首次扫描及滤波需要时间和额外磁盘空间，可取消；正常清空波形、切换数据或退出时清理临时缓存。文件格式、缓存写入或索引额度失败时，保留当前波形及原文件，提供具体原因、调整建议和“绘图显示设置”入口。<br><br>
【导出当前波形】只导出当前仍保留的曲线显示结果，包含已应用的自定义通道缩放、偏移和滤波；它不是原始数据的完整录制。要保留长时间采集的原始数据，请使用【开始录制】。绘图历史上限与下方的磁盘缓存上限互不相同。<br><br>
缓存路径仅在本机自动保存，不随全局配置导出；导入配置时保留当前电脑的路径，旧配置中携带的缓存路径也会忽略。新电脑未设置时使用本机默认目录，可在【数据存储】中选择其他路径；缓存上限、录制模式等其他设置仍正常导入、导出。
</p>

<h4 style='color: #00aaff;'>5. 数字滤波、频谱与信号分析</h4>
<p style='color: #cccccc;'>
右侧【数字滤波】提供陷波、高通和低通选项；按信号特征勾选并设置截止频率。底部【频谱分析】可选择分析通道、采样率和 FFT 点数，必要时勾选【去除 0Hz】以便观察非直流频率成分。【信号分析】显示所选通道的统计或信号特征。若波形或频谱明显异常，先核对数据协议和采样率，再调整分析参数。
</p>

<h4 style='color: #00aaff;'>6. 录制格式与磁盘缓存</h4>
<p style='color: #cccccc;'>
在左侧【数据存储】选择缓存路径和录制模式：<b>兼容模式（CSV）</b>便于其他软件直接读取；<b>高速模式（WLR）</b>适合高采样率、多通道或长时间采集，可在 WaveLink 中导入，也可用【保存数据】转为 CSV。自定义协议的通道表上方可选择<b>录制保存数据类型</b>：<b>原始数值（默认）</b>保存系数、偏移及超限 NaN 替代前的读数；<b>换算后数值</b>保存“原始值 × 系数 + 换算偏移”，并保留超限处理产生的 NaN。未配置通道按系数 1、偏移 0 保留原值。该选择按协议保存，随独立协议和全局配置导入导出；旧协议默认原始值，仅影响之后开始的录制。两种数值类型都不含界面数字滤波，与 CSV/WLR 格式无关。原始值是按字段类型与大小端解码的数值，不是报文字节抓包。例：收到 2534、系数 0.01，波形显示 25.34，原始录制保存 2534，换算录制保存 25.34。先连接设备或启动 Demo，并应用能输出数值通道的协议后才能录制；Raw 文本/HEX 不生成 CSV/WLR 数值录制，请勿将“保存接收”的有限显示快照当成持续日志。两种格式均按约 1 GB 自动分卷，异常退出留下的临时录制文件会在下次启动时尝试恢复，写入失败会停止录制并提示。<br><br>
<b>录制通道数量：</b>所有内置数值协议及自定义协议，均以本次录制开始后首个有效数值帧的通道数为固定数量。后续数量不符仅跳过该帧，不停止录制、不截取或补造通道，实时通道显示仍可随接收变化；底部状态栏用红字累计提示“录制中 · 已跳过 N 帧异常数据”。重新开始录制时重新确定数量并清零。磁盘写入失败、磁盘安全空间不足或设备断开等仍可能停止录制；正常解析失败的帧本来就不会生成数值录制。<br><br>
录制文件保存在所选路径下的 <code>WaveLink_Cache</code> 文件夹。【缓存上限】检查该文件夹大小，也用作录制磁盘的最低安全剩余空间，<b>不是绘图内存上限</b>。缓存超限不会自动删除：下次开始录制时可选择保留并继续、清空并继续，或打开文件夹自行处理；关闭提示框则取消本次录制。【录前清空】只清空当前显示的波形，不删除已有录制文件。<br><br>
【清空缓存】先显示路径、数量、大小和影响，只有点击“确认清空”才删除本次列出的磁盘缓存文件，取消/Esc/关闭均不清理。录制、分卷及未恢复的临时录制文件可能包含重要数据，删除无法撤销，请先另存。不会清空当前波形或绘图内存，不删除文件夹外的另存数据，不递归清理子目录。录制中或被其他实例使用时阻止清理；确认期间新增或变化的文件保留。
</p>

<h4 style='color: #00aaff;'>7. 保存、导入与时间轴</h4>
<p style='color: #cccccc;'>
停止录制后使用【保存数据】可将完整录制（含分卷）导出为 CSV，文件已有数值保持不变、不再次换算；【导入数据】可读取 CSV 或 WLR 回放与分析，直接使用文件中的原始值或换算值，不自动套用当前协议的缩放和偏移，不修改已有文件。录制时保存主机原始接收时间；导入 WaveLink 录制文件时，会依据接收批次和帧序号重建较均匀的显示时间轴，以避免批量接收造成波形阶梯化。此重建是估算，不能替代设备自身提供的精确采样时间戳。
<br>CSV 时间列为 <code>UnixTime(s;FS=采样率)</code>，保存以秒为单位的 Unix 主机接收时间。FS 是重建时间轴的参考采样率，不代表实测的设备采样率；旧版 <code>HostReceiveTime(s;FS=...)</code> 和 <code>HostReceiveTime(s;Hint=...)</code> 文件同样支持导入。
<br>录制与导出使用可恢复原数值的精度，不受界面小数位数影响。导入后，自动频谱与滤波使用文件时间轴对应的采样率；修改采样率不会重写已有波形的时间戳。取消导入或发现坏行时保留当前波形；数据超过安全内存预算时会明确停止导入，不会截断后报告成功。重新连接时退出历史文件的分析状态，防止旧文件覆盖新接收的数据。
</p>

<h4 style='color: #00aaff;'>8. 布局、主题与配置</h4>
<p style='color: #cccccc;'>【绘图显示 → 波形线宽】可选 1、2、3 个实际屏幕像素，默认推荐 1 像素，高 DPI 下不自动变粗。统一作用于主视图、独立视图和波形图片外观，不改变原始数值、CSV/WLR 录制、数值导出及分析精度。粗线更醒目，但高频、密集波形、多通道或高 DPI 下可能增加刷新、拖动和缩放的负担；卡顿时建议恢复 1 像素或降低目标刷新率。设置会记忆并随全局配置导入导出。</p>
<p style='color: #cccccc;'>
顶部【数据收发模式】只保留参数设置与数据收发面板，再次点击恢复切换前的布局；每次进入时接收区与发送区默认按 5:1 展开。面板可折叠、拖动调整；【恢复默认布局】会重置所有面板布局。主题可在顶部或【设置】中切换；【设置 → 系统设置】提供左侧分类、右侧详细设置：“数据收发”管理接收显示行数、字符上限与刷新间隔；“绘图显示”下拉选择波形目标刷新率：5、10、15、20、25、30（默认）、40、50、60 帧/秒。“界面设置”可设置恢复默认布局时下方数据收发、频谱分析和信号分析区域的高度占比：默认 30%（上方约 70%），范围 20%–50%；按当前窗口工作区计算，不含菜单、工具栏和状态栏，实际高度受控件最小尺寸限制。保存此选项不立即重排当前布局，点击【恢复默认布局】或首次没有已保存布局时应用；不改变数据收发模式内部接收/发送分割比例。该选项也随全局配置导入导出。这些设置不改变设备采样率、Y 轴自适应或录制数据。点击【应用】或 Ctrl+S 保存且不关闭，点击【保存并关闭】保存后退出；【恢复本页默认】仅修改当前页草稿，应用后生效。设置菜单还可导出、导入软件配置。
全局配置包含设备接口参数、接收显示缓存等设置、完整自定义协议库及协议选择、命令面板和状态面板；面板的独立 JSON 不包含设备接口参数或完整协议库，整体迁移应使用全局配置。运行中的连接、定时发送和监视状态/事件记录不会作为自动运行任务恢复。协议导入可追加、按 ID 更新、替换或跳过，是否恢复选择可单独决定。全局导入需先断开连接并停止录制/发送；写入失败会尝试恢复原配置，中断导入在下次启动时先恢复或提示处理。
<br><br>【帮助 → 版本信息】集中显示当前版本、检查更新、更新说明与下载进度。系统设置的【软件更新】可关闭启动后自动检查（默认开启，每天最多一次），此选项随全局配置导入导出。没有配置发布地址或发布公钥时明确提示，并保留手动下载入口；不会连接示例服务器。下载在后台进行，不自动安装；可取消或关闭版本窗口后继续下载。安装前需结束录制、停止发送并断开设备，确认后自动备份全局配置和协议/命令/状态面板原文件，再安全退出并启动安装包。备份位于应用数据目录的 UpdateBackups 子目录，WaveLink_Config.ini 可用全局配置导入恢复；更新不清空录制缓存和已保存的数据。签名、大小或摘要验证失败时阻止更新；安装失败保留日志和备份，不自动回滚程序。
</p>
)";
    // Use the existing numbered headings as the single source of guide text;
    // adding a new section automatically gives it the same collapsible card.
    const QRegularExpression section("<h4[^>]*>([^<]+)</h4>\\s*([\\s\\S]*?)(?=<h4\\b|$)");
    auto matches = section.globalMatch(html); QList<QToolButton *> toggles;
    while (matches.hasNext()) {
        const auto match = matches.next(); const QString heading = match.captured(1);
        const int index = toggles.size() + 1;
        auto toggle = new GuideSectionButton(content); toggle->setObjectName(QString("helpGuideToggle%1").arg(index));
        toggle->setProperty("helpGuideTitle", heading); toggle->setCheckable(true);
        toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto body = new GuideTextBrowser(content); body->setObjectName(QString("helpGuideBody%1").arg(index));
        body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
        body->setHtml(match.captured(2)); body->hide();
        auto update = [toggle, body, heading](bool open) {
            toggle->setText(heading); toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            toggle->setToolTip(open ? "点击收起此项说明" : "点击展开此项说明");
            toggle->setAccessibleDescription(open ? "已展开，点击收起" : "已收起，点击展开");
            body->setVisible(open);
        };
        connect(toggle, &QToolButton::toggled, body, update); update(false);
        cards->addWidget(toggle); cards->addWidget(body); toggles.append(toggle);
    }
    cards->addStretch();
    connect(expand, &QPushButton::clicked, page, [toggles] { for (auto toggle : toggles) toggle->setChecked(true); });
    connect(collapse, &QPushButton::clicked, page, [toggles] { for (auto toggle : toggles) toggle->setChecked(false); });
    return page;
}

QWidget* HelpDialog::createDownloadPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(25, 25, 25, 25);
    layout->setSpacing(15);

    QLabel *lblTitle = new QLabel("WaveLink 最新编译发布包下载", page);
    lblTitle->setStyleSheet("font-weight: bold; font-size: 16px; color: #00ffcc;");
    lblTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(lblTitle);

    QTextBrowser *browser = new QTextBrowser(page);
    browser->setObjectName("helpDownloadLinks");
    browser->setOpenExternalLinks(true);
    browser->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    browser->setStyleSheet("border: 1px solid #333333; background-color: #121212; border-radius: 6px; padding: 15px; font-size: 16px; line-height: 1.8; color: #aaaaaa;");

    QString html = R"(
        <p style='color: #aaaaaa; margin-bottom: 15px;'>
            本页面提供 WaveLink 系统的最新可执行编译发布包。
        </p>
        <div style='background-color: #252830; padding: 15px; border-radius: 5px; border: 1px solid #00aaff;'>
            <p style='margin: 0; font-size: 16px; color: #ffffff;'>
                <b>蓝奏云下载：</b><br/>
                <a href='https://xiales.lanzouu.com/b007unma2b' style='color: #00ffcc; font-size: 15px; font-weight: bold;'>https://xiales.lanzouu.com/b007unma2b</a><span style='color: #ffaa00;'>（提取码：0000）</span>
            </p>
            <p style='margin-top: 10px; margin-bottom: 0; font-size: 16px; color: #ffffff;'>
                <b>GitHub Releases 下载：</b><br/>
                <a href='https://github.com/yffdd/wavelink/releases' style='color: #00ffcc; font-size: 15px; font-weight: bold;'>https://github.com/yffdd/wavelink/releases</a>
            </p>
        </div>
    )";
    browser->setHtml(html);
    layout->addWidget(browser);
    return page;
}

QWidget* HelpDialog::createChangelogPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(page);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(12);

    QLabel *lblTitle = new QLabel("版本更新日志 (Changelog)", page);
    lblTitle->setStyleSheet("font-weight: bold; font-size: 16px; color: #00ffcc;");
    lblTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(lblTitle);

    QScrollArea *scrollArea = new QScrollArea(page);
    scrollArea->setObjectName("helpChangelogScroll");
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: 1px solid #333333; background-color: #121212; border-radius: 4px; }");

    QWidget *scrollContent = new QWidget(scrollArea);
    scrollContent->setProperty("changelogContent", true);
    QVBoxLayout *contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(10, 10, 10, 10);
    contentLayout->setSpacing(10);

    // The current version number comes from CMake via QApplication metadata.
    const QString currentVersion = QCoreApplication::applicationVersion();
    const QString currentReleaseText = QString::fromUtf8("V%1 (当前版本) - 2026-11").arg(currentVersion);

    // --- Current release (Expanded) ---
    QToolButton *btnCurrentRelease = new QToolButton(scrollContent);
    btnCurrentRelease->setObjectName(QStringLiteral("changelogCurrentRelease"));
    btnCurrentRelease->setText(QString::fromUtf8("▼  ") + currentReleaseText);
    btnCurrentRelease->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtCurrentRelease = new GuideTextBrowser(scrollContent);
    txtCurrentRelease->setObjectName(QStringLiteral("changelogCurrentReleaseText"));
    txtCurrentRelease->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtCurrentRelease->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[视图测量]</b> 新增示波器式时间/幅值光标、区间统计与自动测量，支持频率、周期、占空比、脉宽及上升/下降时间。</li>
            <li><b>[波形显示]</b> 修复悬停读数及标记越界、独立视图通道隐藏和移除异常，改善测量栏显示。</li>
            <li><b>[离线浏览]</b> 导入数据完整保留，不受实时历史上限限制；大文件按需加载，可配置完整加载额度，并提供导入失败时的调整建议。</li>
            <li><b>[绘图与菜单]</b> 主视图新增背景颜色设置、导入和导出入口；主视图和独立视图背景可分别自定义，清空波形置于菜单末尾并增加确认。</li>
            <li><b>[配置与布局]</b> 配置迁移保留本机缓存路径；菜单调整为文件、视图、工具、设置、帮助，优化绘图设置说明位置。</li>
            <li><b>[面板布局]</b> 命令交互与状态监视面板新增布局编辑器，支持自动排列、自定义分行、卡片跨行拖动和整行排序。每行最多列数与面板设置同步，命令滑条独占一行。布局按面板保存，支持随面板复制及配置导入导出。</li>
        </ol>
    )"));
    contentLayout->addWidget(btnCurrentRelease);
    contentLayout->addWidget(txtCurrentRelease);

    // --- V0.13 (Collapsed) ---
    auto btnV013 = new QToolButton(scrollContent); btnV013->setObjectName("changelogV013Release");
    btnV013->setText(QString::fromUtf8("▶  V0.13 (历史版本) - 2026-10"));
    btnV013->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto txtV013 = new GuideTextBrowser(scrollContent); txtV013->setObjectName("changelogV013Text");
    txtV013->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV013->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[状态监视面板]</b> 新增主窗口与工具菜单入口，支持面板列表、状态卡片、说明、排序及选择记忆。</li>
            <li><b>[实时状态处理]</b> 接入串口、TCP、UDP、BLE 与 Demo 数值数据，支持超时、断线及通道异常检测，保留有限状态变化记录。</li>
            <li><b>[接收文本监视]</b> 新增自定义完整/前缀匹配核心，支持文本分帧、编码检查、超时与异常恢复，接入串口、TCP、UDP 和 BLE。</li>
            <li><b>[配置与解析测试]</b> 支持数值/文本规则编辑、共享文本来源、离线解析测试、本地保存、独立导入导出和备份恢复；完成全局配置集成、协议 ID 映射及中断事务恢复。</li>
            <li><b>[在线更新]</b> 版本信息集中检查更新和下载安装包，支持发布签名验证、安装前配置备份与安全退出；发布服务未配置时保留手动下载。</li>
        </ol>
    )"));
    txtV013->hide(); contentLayout->addWidget(btnV013); contentLayout->addWidget(txtV013);
    connect(btnV013, &QToolButton::clicked, [btnV013, txtV013] {
        const bool visible = txtV013->isVisible(); txtV013->setVisible(!visible);
        btnV013->setText(QString::fromUtf8(!visible ? "▼  V0.13 (历史版本) - 2026-10" : "▶  V0.13 (历史版本) - 2026-10"));
    });

    // --- V0.12 (Collapsed) ---
    auto btnV012 = new QToolButton(scrollContent); btnV012->setObjectName("changelogV012Release");
    btnV012->setText(QString::fromUtf8("▶  V0.12 (历史版本) - 2026-10"));
    btnV012->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto txtV012 = new GuideTextBrowser(scrollContent); txtV012->setObjectName("changelogV012Text");
    txtV012->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    txtV012->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[UDP 数据接口]</b> 新增 UDP 数据收发与波形采集支持。</li>
            <li><b>[自定义协议]</b> 优化协议配置、数据解析及测试体验。</li>
            <li><b>[数据存储]</b> 支持录制原始数值或换算后数值，完善数据保存与导入。</li>
        </ol>
    )"));
    txtV012->hide(); contentLayout->addWidget(btnV012); contentLayout->addWidget(txtV012);
    connect(btnV012, &QToolButton::clicked, [btnV012, txtV012] {
        const bool visible = txtV012->isVisible(); txtV012->setVisible(!visible);
        btnV012->setText(QString::fromUtf8(!visible ? "▼  V0.12 (历史版本) - 2026-10" : "▶  V0.12 (历史版本) - 2026-10"));
    });

    // --- V0.11 (Collapsed) ---
    auto btnV011 = new QToolButton(scrollContent); btnV011->setObjectName("changelogV011Release");
    btnV011->setText(QString::fromUtf8("▶  V0.11 (历史版本) - 2026-10"));
    btnV011->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto txtV011 = new GuideTextBrowser(scrollContent); txtV011->setObjectName("changelogV011Text");
    txtV011->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV011->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[自定义协议帧]</b> 新增文本分隔、二进制固定长度及可变长度协议配置，支持动态数组、通道命名与换算、范围检查和独立协议导入导出。</li>
            <li><b>[统一帧规则]</b> 自动计算长度修正，统一末尾校验及大小端，实时显示帧结构并提示配置冲突；所有数据接口共用当前协议选择。</li>
            <li><b>[解析测试]</b> 支持按协议生成样例、自动切换文本/HEX、模拟分包和拒绝诊断；完善中文通道名称保存及导入显示。</li>
            <li><b>[使用体验]</b> 优化三栏协议管理、配置布局与主题适配，更新分类使用指南及校验算法 C 参考代码。</li>
        </ol>
    )"));
    txtV011->hide(); contentLayout->addWidget(btnV011); contentLayout->addWidget(txtV011);
    connect(btnV011, &QToolButton::clicked, [btnV011, txtV011] {
        const bool visible = txtV011->isVisible(); txtV011->setVisible(!visible);
        btnV011->setText(QString::fromUtf8(!visible ? "▼  V0.11 (历史版本) - 2026-10" : "▶  V0.11 (历史版本) - 2026-10"));
    });

    // --- V0.10 (Collapsed) ---
    auto btnV010 = new QToolButton(scrollContent); btnV010->setObjectName("changelogV010Release");
    btnV010->setText(QString::fromUtf8("▶  V0.10 (历史版本) - 2026-10"));
    btnV010->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto txtV010 = new GuideTextBrowser(scrollContent); txtV010->setObjectName("changelogV010Text");
    txtV010->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV010->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[BLE 低功耗蓝牙]</b> 新增 BLE 接口，支持设备扫描、服务与收发特征配置、文本/Hex 调试、波形分析和数据录制，并提供配置模板与连接诊断。</li>
            <li><b>[BLE 高级发送]</b> 支持分包大小与发送间隔设置，完善文件发送进度、取消及异常处理。</li>
            <li><b>[串口硬件流控]</b> 新增 RTS/CTS 硬件流控，默认无流控，设置自动保存；提供 CTS 等待提示、待发送队列保护和安全取消。</li>
            <li><b>[串口控制信号]</b> 新增 DTR/RTS 手动控制及 Break 短时发送、持续保持与释放；每次启动和重连默认释放 Break。</li>
        </ol>
    )"));
    txtV010->hide(); contentLayout->addWidget(btnV010); contentLayout->addWidget(txtV010);
    connect(btnV010, &QToolButton::clicked, [btnV010, txtV010] {
        const bool visible = txtV010->isVisible(); txtV010->setVisible(!visible);
        btnV010->setText(QString::fromUtf8(!visible ? "▼  V0.10 (历史版本) - 2026-10" : "▶  V0.10 (历史版本) - 2026-10"));
    });

    // --- V0.9 (Collapsed) ---
    QToolButton *btnV09 = new QToolButton(scrollContent);
    btnV09->setText(QString::fromUtf8("▶  V0.9 (历史版本) - 2026-09"));
    btnV09->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV09 = new GuideTextBrowser(scrollContent);
    txtV09->setObjectName(QStringLiteral("changelogV09Text"));
    txtV09->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV09->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #d0d0d0; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV09->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[录制与数据安全]</b> 增加磁盘空间检查、1 GB 自动分卷、异常录制文件恢复及写入失败保护，避免录制状态与实际文件不同步。</li>
            <li><b>[高速录制]</b> 新增 WLR 高速二进制录制模式，并保留 CSV 兼容模式；两种格式均可直接导入和导出。</li>
            <li><b>[时间轴重建]</b> 录制文件保留原始主机接收时间和连续帧序号，导入时自动识别批量传输并重建均匀波形时间轴。</li>
            <li><b>[绘图与缓存]</b> 优化高采样率、多通道绘图性能，增加独立波形历史上限，并完善缓存超限处理和录制提示。</li>
        </ol>
    )"));
    txtV09->setVisible(false);

    contentLayout->addWidget(btnV09);
    contentLayout->addWidget(txtV09);

    // --- V0.8 (Collapsed) ---
    QToolButton *btnV08 = new QToolButton(scrollContent);
    btnV08->setText(QString::fromUtf8("▶  V0.8 (历史版本) - 2026-09"));
    btnV08->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV08->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV08 = new GuideTextBrowser(scrollContent);
    txtV08->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV08->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #d0d0d0; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV08->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[数据收发模式]</b> 新增专用数据收发布局，支持一键切换、布局记忆，并在每次进入时将接收区与发送区恢复为 5:1。</li>
            <li><b>[面板布局]</b> 重构停靠面板折叠功能；左右面板可缩为竖排标题条，上下方面板可缩为紧凑标题栏，并支持状态恢复。</li>
            <li><b>[串口稳定性]</b> 增强串口占用、设备断开及访问失败等异常处理，采用可复制错误弹窗并安全关闭通信流程。</li>
            <li><b>[数据缓存]</b> 默认磁盘缓存上限调整为 500 MB；未连接时提示无法开始录制。缓存超限后不再自动清空，开始录制时可选择保留、清空或打开缓存文件夹自行处理。</li>
            <li><b>[高速录制]</b> 新增 WLR 二进制录制模式，与 CSV 兼容模式自由切换；两种格式均支持 1 GB 分卷、磁盘保护和异常恢复，WLR 可直接导入或转换为 CSV。</li>
            <li><b>[绘图性能]</b> 优化多通道数据批量处理和绘图刷新，统一限制曲线历史内存，减少高采样率下的界面卡顿。显示限流不影响录制，波形导出仅包含当前保留的数据。</li>
            <li><b>[独立视图]</b> 分离视图达到 3 个时支持纵向滚动，不再挤压底部面板；新增右键合并回主视图，修复恢复默认布局后的排列问题，并自动保存通道勾选状态。</li>
            <li><b>[工程维护]</b> 拆分主窗口模块职责，降低后续功能升级和问题排查的风险。</li>
        </ol>
    )"));
    txtV08->setVisible(false);

    contentLayout->addWidget(btnV08);
    contentLayout->addWidget(txtV08);

    // --- Section 1: V0.7 (Collapsed) ---
    QToolButton *btnV07 = new QToolButton(scrollContent);
    btnV07->setText(QString::fromUtf8("▶  V0.7 (历史版本) - 2026-08"));
    btnV07->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV07->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV07 = new GuideTextBrowser(scrollContent);
    txtV07->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV07->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #d0d0d0; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV07->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[界面布局]</b> 优化界面布局方式，改善窗口尺寸变化、面板排列以及底部分析区域的显示效果。</li>
        </ol>
    )"));
    txtV07->setVisible(false);

    contentLayout->addWidget(btnV07);
    contentLayout->addWidget(txtV07);

    // --- Section 1: V0.6 (Collapsed) ---
    QToolButton *btnV06 = new QToolButton(scrollContent);
    btnV06->setText(QString::fromUtf8("▶  V0.6 (历史版本) - 2026-08"));
    btnV06->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV06->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV06 = new GuideTextBrowser(scrollContent);
    txtV06->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV06->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #d0d0d0; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV06->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #dddddd;'>
            <li><b>[配置管理]</b> 新增了系统全局配置的导出与导入功能，方便配置迁移与备份。</li>
            <li><b>[命令交互]</b> 新增了命令交互面板，支持快捷命令的编辑、分组、导出与批量导入，提高调试效率。</li>
            <li><b>[数据发送]</b> 在串口数据发送终端中新增了发送文件的功能，支持大块数据的下发。</li>
            <li><b>[信号分析]</b> 优化了频谱分析的频域范围显示功能，观测频谱细节更加直观。</li>
        </ol>
    )"));
    txtV06->setVisible(false);

    contentLayout->addWidget(btnV06);
    contentLayout->addWidget(txtV06);

    // --- Section 1: V0.5 (Collapsed) ---
    QToolButton *btnV05 = new QToolButton(scrollContent);
    btnV05->setText(QString::fromUtf8("▶  V0.5 (历史版本) - 2026-08"));
    btnV05->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV05->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV05 = new GuideTextBrowser(scrollContent);
    txtV05->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV05->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #aaaaaa; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV05->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #aaaaaa;'>
            <li><b>[界面与视图]</b> 优化初始主视图体验，时间窗口及底部时间滑条默认跨度扩大至10秒；每次导入离线数据及更改“Y轴小数位数”后，主视图“Y跨度”将自动智能刷新。</li>
            <li><b>[串行通信与驱动]</b> 优化硬件检测逻辑，当系统未检测到可用串口时提供明确缺省提示；新增识别蓝牙设备及其对应COM端口映射关系；菜单栏集成 CH341 串口驱动的一键下载。</li>
            <li><b>[数据接收与调试]</b> 增强数据收发终端的功能，新增“时间戳显示”功能（精确至毫秒）；将 LDC16xx 线圈参数计算工具以独立窗口的形式集成至顶部工具栏。</li>
            <li><b>[信号分析算法]</b> 深度修正并升级了脑电（EEG）频带能量的数学模型，补全了 FFT 功率谱绝对能量的归一化系数，现已能精准输出标准量级的绝对功率值。</li>
            <li><b>[内置 Demo 引擎]</b> 全新重磅上线医疗级“Demo数据”离线模拟引擎！全面支持 FireWater 与 JustFloat 双协议原生推流封装；支持 10+ 种波形发生器，不仅包含正弦、方波、脉冲等函数波，更支持以数学高精度合成 ECG(心电)、EMG(肌电)、EEG(脑电)、EOG(眼电)、PPG(脉搏波)、CBF(脑血流) 等高度逼真的医用生理信号；支持精准叠加 50Hz 工频干扰与高斯白噪声。</li>
        </ol>
    )"));
    txtV05->setVisible(false);

    contentLayout->addWidget(btnV05);
    contentLayout->addWidget(txtV05);

    // --- Section 0: V0.4 (Expanded) ---
    QToolButton *btnV04 = new QToolButton(scrollContent);
    btnV04->setText(QString::fromUtf8("▶  V0.4 (历史版本) - 2026-07"));
    btnV04->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV04->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV04 = new GuideTextBrowser(scrollContent);
    txtV04->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV04->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #d0d0d0; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV04->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #aaaaaa;'>
            <li><b>[UI 布局与主题]</b> 全面优化停靠面板的布局逻辑及命名规范（原“信号测量”更名为“信号分析”）；修复白色主题模式下面板标题及字体颜色显示异常的问题，提升视觉一致性。新增顶部菜单栏，统一视图、工具及帮助快捷入口，移除悬浮按钮上的多余字母标识。</li>
            <li><b>[信号分析]</b> 深度优化特征测量算法与数据计算方式，确保多通道统计数据的高精度与实时性；升级特征提示（ToolTip）功能，提供更直观的数据释义。</li>
            <li><b>[频谱分析]</b> 优化 FFT 频谱分析仪的交互体验，完善拖拽与缩放功能，支持动态消除直流分量，进一步提升频域观测的便捷性。</li>
            <li><b>[数据处理与存储]</b> 重构底层波形绘图缓存逻辑，显著降低高并发数据流下的内存占用；新增完备的波形数据存储功能，支持历史缓存限制、全通道数据录制与离线 CSV 文件导入回放。</li>
            <li><b>[控制台交互]</b> 优化数据收发（串口控制台）面板功能，提升命令收发稳定性；重构主视图底部全局时间滑条动作逻辑，增强波形回溯的平滑度。</li>
        </ol>
    )"));
    txtV04->setVisible(false);

    // --- Section 1: V0.3 (Collapsed) ---
    QToolButton *btnV03 = new QToolButton(scrollContent);
    btnV03->setText(QString::fromUtf8("▶  V0.3 (历史版本) - 2026-07"));
    btnV03->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV03->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    
    QTextBrowser *txtV03 = new GuideTextBrowser(scrollContent);
    txtV03->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV03->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #aaaaaa; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV03->setHtml(QString::fromUtf8(R"(
<ol style='margin-left: -15px; line-height: 1.8; color: #aaaaaa;'>
<li><b>[硬件通信]</b> 新增 TCP 网络服务端模式与连接管理；新增下位机硬件连接协议与代码模板的动态折叠帮助页。完美修复 TCP 模式下数据流无法写入录制文件的问题。</li>
<li><b>[波形渲染]</b> 采用革命性的分级渲染引擎架构：主视图启用 OpenGL 硬件加速承载数百万点密度的极限刷新，分离视图无缝切换纯 CPU 高精度绘制，彻底根除跨屏幕多窗口场景下的 0xc0000005 显卡缓存越界崩溃与波形克隆串位问题。</li>
<li><b>[自适应引擎]</b> 支持各个单通道分离窗口的可见性独立开关及 Y 轴自适应联动触发；取消特定设备通道的强行倍率放大机制，现在直接支持原生捕捉和撑满百万分之一级别的微小生理信号。</li>
<li><b>[信号特征测量]</b> 特征测量面板的“平均值、有效值、峰峰值”等多维指标精度已全面接管，随波形视图全局设置实时同步动态小数位数。</li>
<li><b>[系统日志]</b> 进一步提升软件防崩溃监控等级，修正 QApplication 在退出过程中的生命周期析构顺序，消灭内存泄漏并确保所有异常日志落盘。</li>
</ol>
)"));
    txtV03->setVisible(false);

    // --- Section 2: V0.2 (Collapsed) ---
    QToolButton *btnV024 = new QToolButton(scrollContent);
    btnV024->setText(QString::fromUtf8("▶  V0.2 (历史版本) - 2026-07"));
    btnV024->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV024->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV024 = new GuideTextBrowser(scrollContent);
    txtV024->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV024->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #aaaaaa; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV024->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #aaaaaa;'>
            <li><b>[滤波算法]</b> 修正数字滤波算法的底层计算架构，彻底解决实时采集模式下高通滤波归零以及级联失效的问题。</li>
            <li><b>[频谱辅助]</b> 频谱分析仪新增高通、低通、陷波的垂直截断频率虚线，并在右上角自动生成彩色图例标识。</li>
            <li><b>[参数限制]</b> 根据奈奎斯特采样定理，自动将滤波器允许配置的最高频率限制为采样率的一半以内。</li>
            <li><b>[串口通信]</b> 优化了串口意外断开时的容错机制，增加硬件物理连接断开的明确提示，并确保 UI 状态正确复位。</li>
            <li><b>[使用体验]</b> 升级更新日志为折叠卡片样式；修复了导出与导入数据时无法记忆并自动定位到上次保存路径的问题。</li>
            <li><b>[工程规范]</b> 整体重构并规范了整个软件源代码的目录结构，按核心、UI、硬件通信、DSP算法等模块进行细化分类。</li>
        </ol>
    )"));
    txtV024->setVisible(false);

    // --- Section 3: V0.1 (Collapsed) ---
    QToolButton *btnV01 = new QToolButton(scrollContent);
    btnV01->setText(QString::fromUtf8("▶  V0.1 (初始发布版) - 2026-07"));
    btnV01->setStyleSheet("QToolButton { font-weight: bold; font-size: 16px; color: #00aaff; background-color: #1e1e1e; border: 1px solid #333333; border-radius: 4px; padding: 8px 12px; text-align: left; } QToolButton:hover { background-color: #2a2a2a; }");
    btnV01->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QTextBrowser *txtV01 = new GuideTextBrowser(scrollContent);
    txtV01->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    txtV01->setStyleSheet("border: none; background-color: #1a1a1a; padding: 10px; font-size: 15px; line-height: 1.6; color: #aaaaaa; selection-background-color: #0078d7; selection-color: #ffffff;");
    txtV01->setHtml(QString::fromUtf8(R"(
        <ol style='margin-left: -15px; line-height: 1.8; color: #aaaaaa;'>
            <li><b>[波形采集]</b> 支持多通道波形高帧率实时采集、GPU 硬件渲染与动态坐标轴自适应。</li>
            <li><b>[协议解析]</b> 内置 RAW 原始字节、FireWater (CSV) 标准通讯协议及 JustFloat 浮点解析器。</li>
            <li><b>[模块布局]</b> 实现支持自由拖拽、悬浮分离与多视角并排停靠的 QDockWidget 架构。</li>
            <li><b>[数字滤波]</b> 内置 2阶/4阶 巴特沃斯低通、高通、带通滤波及 50Hz 工频陷波器。</li>
            <li><b>[数据与主题]</b> 支持实时 CSV 数据文件录制与暗黑/亮色双主题动态无缝切换。</li>
        </ol>
    )"));
    txtV01->setVisible(false);

    connect(btnCurrentRelease, &QToolButton::clicked, [btnCurrentRelease, txtCurrentRelease, currentReleaseText](){
        const bool isVisible = txtCurrentRelease->isVisible();
        txtCurrentRelease->setVisible(!isVisible);
        btnCurrentRelease->setText((!isVisible ? QString::fromUtf8("▼  ") : QString::fromUtf8("▶  ")) + currentReleaseText);
    });

    connect(btnV09, &QToolButton::clicked, [btnV09, txtV09](){
        bool isVisible = txtV09->isVisible();
        txtV09->setVisible(!isVisible);
        btnV09->setText(!isVisible ? QString::fromUtf8("▼  V0.9 (历史版本) - 2026-09") : QString::fromUtf8("▶  V0.9 (历史版本) - 2026-09"));
    });

    connect(btnV08, &QToolButton::clicked, [btnV08, txtV08](){
        bool isVisible = txtV08->isVisible();
        txtV08->setVisible(!isVisible);
        btnV08->setText(!isVisible ? QString::fromUtf8("▼  V0.8 (历史版本) - 2026-09") : QString::fromUtf8("▶  V0.8 (历史版本) - 2026-09"));
    });

    connect(btnV06, &QToolButton::clicked, [btnV06, txtV06](){
        bool isVisible = txtV06->isVisible();
        txtV06->setVisible(!isVisible);
        btnV06->setText(!isVisible ? QString::fromUtf8("▼  V0.6 (历史版本) - 2026-08") : QString::fromUtf8("▶  V0.6 (历史版本) - 2026-08"));
    });

    connect(btnV07, &QToolButton::clicked, [btnV07, txtV07](){
        bool isVisible = txtV07->isVisible();
        txtV07->setVisible(!isVisible);
        btnV07->setText(!isVisible ? QString::fromUtf8("▼  V0.7 (历史版本) - 2026-08") : QString::fromUtf8("▶  V0.7 (历史版本) - 2026-08"));
    });

    connect(btnV05, &QToolButton::clicked, [btnV05, txtV05](){
        bool isVisible = txtV05->isVisible();
        txtV05->setVisible(!isVisible);
        btnV05->setText(!isVisible ? QString::fromUtf8("▼  V0.5 (历史版本) - 2026-08") : QString::fromUtf8("▶  V0.5 (历史版本) - 2026-08"));
    });

    connect(btnV04, &QToolButton::clicked, [btnV04, txtV04](){
        bool isVisible = txtV04->isVisible();
        txtV04->setVisible(!isVisible);
        btnV04->setText(!isVisible ? QString::fromUtf8("▼  V0.4 (历史版本) - 2026-07") : QString::fromUtf8("▶  V0.4 (历史版本) - 2026-07"));
    });

    connect(btnV03, &QToolButton::clicked, [btnV03, txtV03](){
        bool isVisible = txtV03->isVisible();
        txtV03->setVisible(!isVisible);
        btnV03->setText(!isVisible ? QString::fromUtf8("▼  V0.3 (历史版本) - 2026-07") : QString::fromUtf8("▶  V0.3 (历史版本) - 2026-07"));
    });

    connect(btnV024, &QToolButton::clicked, [btnV024, txtV024](){
        bool isVisible = txtV024->isVisible();
        txtV024->setVisible(!isVisible);
        btnV024->setText(!isVisible ? QString::fromUtf8("▼  V0.2 (历史版本) - 2026-07") : QString::fromUtf8("▶  V0.2 (历史版本) - 2026-07"));
    });

    connect(btnV01, &QToolButton::clicked, [btnV01, txtV01](){
        bool isVisible = txtV01->isVisible();
        txtV01->setVisible(!isVisible);
        btnV01->setText(!isVisible ? QString::fromUtf8("▼  V0.1 (初始发布版) - 2026-07") : QString::fromUtf8("▶  V0.1 (初始发布版) - 2026-07"));
    });

    contentLayout->addWidget(btnV04);
    contentLayout->addWidget(txtV04);
    contentLayout->addWidget(btnV03);
    contentLayout->addWidget(txtV03);
    contentLayout->addWidget(btnV024);
    contentLayout->addWidget(txtV024);
    contentLayout->addWidget(btnV01);
    contentLayout->addWidget(txtV01);
    contentLayout->addStretch();

    scrollArea->setWidget(scrollContent);
    mainLayout->addWidget(scrollArea);
    return page;
}

QWidget* HelpDialog::createDebugPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 20, 20, 20);

    QLabel *lblTitle = new QLabel("程序运行异常与诊断日志 (最大保留 1000 行)", page);
    lblTitle->setStyleSheet("font-weight: bold; font-size: 16px; color: #00ffcc;");
    lblTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    
    m_btnClearLogs = new QPushButton(QString::fromUtf8("一键清空日志"), page);
    m_btnClearLogs->setStyleSheet("background-color: #552222; color: #ffaaaa; border: 1px solid #ff5555; padding: 4px 12px; border-radius: 3px; font-weight: bold;");
    
    QPushButton *btnCopyLogs = new QPushButton(QString::fromUtf8("一键复制所有日志"), page);
    btnCopyLogs->setStyleSheet("background-color: #225522; color: #aaffaa; border: 1px solid #55ff55; padding: 4px 12px; border-radius: 3px; font-weight: bold;");

    QTimer *easterEggTimer = new QTimer(page);
    easterEggTimer->setSingleShot(true);
    
    connect(easterEggTimer, &QTimer::timeout, this, [this](){
        QString targetResource = "";
        if (QFile::exists(":/source.zip")) {
            targetResource = ":/source.zip";
        } else if (QFile::exists(":/WaveLink.zip")) {
            targetResource = ":/WaveLink.zip";
        }
        
        if (targetResource.isEmpty()) {
            return;
        }
        const QString defaultName = QString("WaveLink_Source_V%1.zip")
                                        .arg(QCoreApplication::applicationVersion());
        QString savePath = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出核心源代码"), defaultName, "Zip Files (*.zip)");
        if (!savePath.isEmpty()) {
            QFile srcFile(targetResource);
            if (srcFile.exists() && srcFile.open(QIODevice::ReadOnly)) {
                QFile destFile(savePath);
                if (destFile.open(QIODevice::WriteOnly)) {
                    destFile.write(srcFile.readAll());
                    destFile.close();
                    QMessageBox::information(this, QString::fromUtf8("成功"), QString::fromUtf8("源代码导出成功，去查收吧！"));
                }
                srcFile.close();
            } else {
                QMessageBox::warning(this, QString::fromUtf8("失败"), QString::fromUtf8("未找到源代码资源，可能未被打包进程序中。"));
            }
        }
    });

    connect(m_btnClearLogs, &QPushButton::pressed, this, [easterEggTimer](){
        easterEggTimer->start(10000);
    });

    connect(m_btnClearLogs, &QPushButton::released, this, [this, easterEggTimer](){
        if (easterEggTimer->isActive()) {
            easterEggTimer->stop();
            this->onClearLogsClicked();
        }
    });

    connect(btnCopyLogs, &QPushButton::clicked, this, [this](){
        if (!m_debugTextBrowser->toPlainText().isEmpty()) {
            QApplication::clipboard()->setText(m_debugTextBrowser->toPlainText());
            QMessageBox::information(this, QString::fromUtf8("成功"), QString::fromUtf8("已将所有日志内容复制到剪贴板。"));
        } else {
            QMessageBox::information(this, QString::fromUtf8("提示"), QString::fromUtf8("当前没有日志可复制。"));
        }
    });

    QHBoxLayout *buttonsLayout = new QHBoxLayout();
    buttonsLayout->addStretch();
    buttonsLayout->addWidget(btnCopyLogs);
    buttonsLayout->addWidget(m_btnClearLogs);

    m_debugTextBrowser = new QTextBrowser(page);
    m_debugTextBrowser->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    m_debugTextBrowser->setStyleSheet("background-color: #121212; color: #00ffaa; font-family: Consolas, Monaco, monospace; font-size: 12px; border: 1px solid #333333; padding: 8px;");

    layout->addLayout(buttonsLayout);
    layout->addWidget(lblTitle);
    layout->addWidget(m_debugTextBrowser);

    return page;
}

void HelpDialog::onClearLogsClicked()
{
    if (m_debugTextBrowser->toPlainText().trimmed().isEmpty()) {
        return;
    }
    
    if (QMessageBox::question(this, QString::fromUtf8("确认"), QString::fromUtf8("确认要清空当前所有日志记录吗？"), QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
        DebugLogger::instance().clearLogs();
    }
}

void HelpDialog::onLogAdded(const QString &formattedMessage)
{
    if (m_debugTextBrowser) {
        m_debugTextBrowser->append(formattedMessage);
    }
}

void HelpDialog::onLogsCleared()
{
    if (m_debugTextBrowser) {
        m_debugTextBrowser->clear();
    }
}
