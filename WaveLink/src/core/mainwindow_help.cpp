#include "mainwindow.h"
#include <QTime>
#include <QProcess>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include "ffthelper.h"
#include "demodatagenerator.h"
#include <QOpenGLContext>
#include "debuglogger.h"
#include "commandpanelwindow.h"
#include "helpdialog.h"
#include <QSerialPortInfo>
#include <QMessageBox>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollBar>
#include <QScrollArea>
#include <QListWidget>
#include <QLineEdit>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFileDialog>
#include <QProgressBar>
#include <QTimer>
#include <QFile>
#include <QCoreApplication>
#include <QMenu>
#include <QSettings>
#include <QTextCodec>
#include <QColorDialog>
#include <QProgressDialog>
#include <QScreen>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTextDocument>
#include <QPalette>
#include <QEvent>
#include <QAbstractTextDocumentLayout>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QTextOption>
#include <QtMath>

namespace {
// Each expanded section fits its complete wrapped document. The surrounding
// QScrollArea owns scrolling, including wheel events over the body text.
class EngineProtocolBody final : public QTextBrowser {
public:
    explicit EngineProtocolBody(QWidget *parent) : QTextBrowser(parent) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this] { scheduleFit(); });
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QTextBrowser::resizeEvent(event);
        if (event->size().width() != event->oldSize().width()) scheduleFit();
    }
    void showEvent(QShowEvent *event) override { QTextBrowser::showEvent(event); scheduleFit(); }
    void changeEvent(QEvent *event) override {
        QTextBrowser::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
            event->type() == QEvent::ContentsRectChange) scheduleFit();
    }
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
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
class EngineProtocolHelpDialog : public QDialog {
public:
    explicit EngineProtocolHelpDialog(QWidget *parent) : QDialog(parent) { setObjectName("engineProtocolHelpDialog"); }
    void refreshTheme(bool force = false) {
        // Read a clean widget's APPLICATION palette, not this dialog's old
        // local stylesheet; otherwise a light/dark change could stay stuck.
        QTextBrowser probe; probe.ensurePolished();
        const bool dark = probe.palette().color(QPalette::Base).lightness() < 128;
        const QString window = dark ? "#1e1e1e" : "#f0f0f0", panel = dark ? "#252526" : "#ffffff";
        const QString text = dark ? "#dddddd" : "#333333", border = dark ? "#4d555c" : "#c5cdd4";
        const QString accent = dark ? "#70c8ff" : "#07689d", card = dark ? "#263947" : "#eaf4fb";
        const QString hover = dark ? "#314c60" : "#d9edf9", code = dark ? "#1a1d20" : "#edf1f5";
        const QString sheet = QString(
            "QDialog#engineProtocolHelpDialog, QWidget#engineProtocolContent, QWidget#engineProtocolViewport { background: %1; color: %2; }"
            "QScrollArea { background: %1; border: 1px solid %3; border-radius: 4px; }"
            "QToolButton[engineProtocolSection=\"true\"] { font-weight: bold; font-size: 14px; color: %4; background: %5; border: 1px solid %3; border-radius: 4px; padding: 8px 12px; text-align: left; }"
            "QToolButton[engineProtocolSection=\"true\"]:hover { background: %6; }"
            "QTextBrowser { border: none; background: %7; color: %2; padding: 10px; font-size: 13px; selection-background-color: #0078d7; selection-color: white; }"
            "QScrollBar:vertical { background: %1; width: 12px; margin: 0; } QScrollBar::handle:vertical { background: %3; min-height: 28px; border-radius: 5px; }"
            "QScrollBar:horizontal { background: %1; height: 12px; margin: 0; } QScrollBar::handle:horizontal { background: %3; min-width: 28px; border-radius: 5px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; } QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
            "QScrollBar::add-page, QScrollBar::sub-page { background: %1; }").arg(window, text, border, accent, card, hover, panel);
        if (!force && styleSheet() == sheet) return;
        m_applying = true;
        setStyleSheet(sheet);
        for (auto body : findChildren<QTextBrowser *>()) {
            const int vertical = body->verticalScrollBar()->value(), horizontal = body->horizontalScrollBar()->value();
            body->document()->setDefaultStyleSheet(QString("body { color: %1; } pre, .codeSample { background-color: %2; color: %1; } pre { white-space: pre-wrap; } code, pre { font-family: Consolas, 'Courier New', monospace; } a { color: %3; }").arg(text, code, accent));
            body->setHtml(body->property("engineProtocolHtml").toString());
            body->verticalScrollBar()->setValue(vertical); body->horizontalScrollBar()->setValue(horizontal);
        }
        m_applying = false;
    }
protected:
    void changeEvent(QEvent *event) override {
        QDialog::changeEvent(event);
        if (!m_applying && !m_queued && (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange)) {
            m_queued = true;
            QTimer::singleShot(0, this, [this] { m_queued = false; refreshTheme(); });
        }
    }
private:
    bool m_applying = false, m_queued = false;
};
}

void MainWindow::showProtocolHelpDialog()
{
    EngineProtocolHelpDialog dlg(this);
    dlg.setWindowTitle("数据引擎协议说明");
    dlg.setWindowFlags(dlg.windowFlags() | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    dlg.setMinimumSize(480, 320); dlg.setSizeGripEnabled(true);
    dlg.resize(700, 500);

    QVBoxLayout *mainLayout = new QVBoxLayout(&dlg);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(10);

    QScrollArea *scrollArea = new QScrollArea(&dlg);
    scrollArea->setObjectName("engineProtocolScroll"); scrollArea->viewport()->setObjectName("engineProtocolViewport");
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    

    QWidget *scrollContent = new QWidget(scrollArea);
    scrollContent->setObjectName("engineProtocolContent");
    QVBoxLayout *contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(10, 10, 10, 10);
    contentLayout->setSpacing(10);

    auto addFoldableSection = [&](const QString &title, const QString &htmlContent, bool expanded = false) {
        QToolButton *btn = new QToolButton(scrollContent);
        btn->setProperty("engineProtocolSection", true); btn->setObjectName("engineProtocolSection");
        btn->setText(title); btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        QTextBrowser *txt = new EngineProtocolBody(scrollContent);
        txt->setObjectName("engineProtocolBody"); txt->setProperty("engineProtocolHtml", htmlContent);
        txt->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
        txt->setHtml(htmlContent);
        txt->setVisible(expanded);

        QObject::connect(btn, &QToolButton::clicked, [btn, txt](){
            bool isVis = txt->isVisible();
            txt->setVisible(!isVis);
            btn->setArrowType(isVis ? Qt::RightArrow : Qt::DownArrow);
        });

        contentLayout->addWidget(btn);
        contentLayout->addWidget(txt);
    };

    // 1. Raw
    addFoldableSection("1. Raw (文本/Hex)", 
        "<p>最基础的字节流接收模式，仅将串口或网络收到的字节进行简单的 UTF-8 / GBK 字符串拼接，或者 Hex 十六进制显示。该模式不支持将数据绘制成波形，仅用于最基础的调试。</p>"
    );

    // 2. FireWater
    addFoldableSection("2. FireWater (CSV 文本)", 
        "<p>这是波形库最常用的标准纯文本协议，各大国产上位机（如野火、vofa+）均原生支持。下位机只需使用 <code>printf</code> 打印用逗号分隔的数字，并以 <code>\\n</code> 换行结尾即可。</p>"
        "<p><b>示例（3个通道数据）：</b></p>"
        "<p class='codeSample' style='padding: 5px; border-radius: 3px;'><code>printf(\"%.2f,%.2f,%.2f\\n\", ch1, ch2, ch3);</code></p>"
        "<p>发送到上位机的实际文本结果为：<code>1.23,4.56,7.89\\n</code></p>"
    );

    // 3. JustFloat
    addFoldableSection("3. JustFloat (二进制浮点数)", 
        "<p>适用于需要极高帧率或单片机算力不足以频繁进行 float 转 string 格式化 (printf) 的场景。下位机直接发送底层 4 字节的 float 内存数据，末尾附加 4 字节的特定帧尾。推荐使用 DMA 方式发送以降低 CPU 占用。</p>"
        "<p><b>协议格式：</b></p>"
        "<ul>"
        "<li>数据：<code>通道个数 * 4</code> (N 字节 IEEE754 小端浮点数)</li>"
        "<li>帧尾：<code>0x00 0x00 0x80 0x7F</code> (4 字节)</li>"
        "</ul>"
        "<p><b>示例（C 语言，发送多个通道数据至 VOFA+）：</b></p>"
        "<pre class='codeSample' style='padding: 8px; border-radius: 4px; overflow: auto;'><code>"
        "#define JUSTFLOAT_MAX_LEN 20<br>"
        "void JustFloat_send(float *data, uint8_t len, uint8_t mode) {<br>"
        "    if (data == NULL || len == 0 || len > JUSTFLOAT_MAX_LEN) return;<br>"
        "    __ALIGNED(4) static uint8_t uart_tx_buf[JUSTFLOAT_MAX_LEN * 8] = {0};<br>"
        "    uint16_t total_len = 0;<br>"
        "    if (mode == 0) {<br>"
        "        memcpy(uart_tx_buf, data, len * 4);<br>"
        "        total_len = len * 4;<br>"
        "        uart_tx_buf[total_len + 0] = 0x00;<br>"
        "        uart_tx_buf[total_len + 1] = 0x00;<br>"
        "        uart_tx_buf[total_len + 2] = 0x80;<br>"
        "        uart_tx_buf[total_len + 3] = 0x7f;<br>"
        "        total_len += 4;<br>"
        "    } else {<br>"
        "        uint8_t *p = uart_tx_buf;<br>"
        "        for (uint8_t i = 0; i < len; i++) {<br>"
        "            memcpy(p, &amp;data[i], 4); p += 4;<br>"
        "            *p++ = 0x00; *p++ = 0x00; *p++ = 0x80; *p++ = 0x7f;<br>"
        "        }<br>"
        "        total_len = len * 8;<br>"
        "    }<br>"
        "    UART_Transmit_Data(&amp;huart1, uart_tx_buf, total_len);<br>"
        "}"
        "</code></pre>"
    );

    addFoldableSection("4. 自定义协议（文本 / 二进制 / 多通道键值）",
        "<p>在数据协议区先选择<b>自定义协议</b>，再点击蓝色 <b>⚙ 管理自定义协议</b>按钮。新增可选择文本分隔、二进制固定长度、二进制长度字段、二进制首尾分隔或<b>多通道键值协议</b>，配置后生成样例、测试并保存。专用设备不再占用主界面的通用协议列表。</p>"
        "<p><b>多通道键值：</b>每行一个 <code>键:数值\\r\\n</code>，所有配置通道收到后组成一帧，按通道表顺序输出。键可以是数字或文本、区分大小写；未知键可忽略或拒绝并清空半帧。重复键可重启组帧（推荐）或用最新值替换，组帧超时可防止长时间前后的值拼在一起；已配置键的无效数值清空未完成帧。没有帧序号的键值流不能完全识别丢包，重要采集建议采用带序号/校验的二进制协议。</p>"
        "<p><b>CBF/IR/RED 模板：</b>协议列表右键<b>新增协议 → 新增模板协议 → CBF/IR/RED 设备模板</b>可快捷创建；新增对话框中也可选择键值类型及该模板。键 <b>0 → CBF（默认单位 MHz），1 → IR（AFE4400 LED1），2 → RED（AFE4400 LED2）</b>，例如 <code>0:123.456\\r\\n1:0.789\\r\\n2:5.432\\r\\n</code>。设备采样率默认 31 Hz，采用 TCP 服务端，默认端口 4970。模板按需创建，新安装列表仍为空；可像其他协议一样编辑、复制、导入和导出。</p>"
        "<p><b>设备预设可选：</b>模板的“启用TCP服务器预设”默认勾选，可按需关闭。未连接时应用协议可设置 TCP 服务器和端口（可修改），不会自动开始监听；可选择首次收到完整帧后自动分离视图。旧内置 CBF 配置自动迁移为该模板，保留原来的预设与组帧行为；协议 ID 和已选配置安全迁移，不交换数值。</p>"
        "<p>配置项名称及表头可悬停查看说明。HEX 字段仅接受 0–9、A–F 和分隔空白，小写自动显示为大写；含非法字符的粘贴不会替换原内容。每个字节填写两位，不使用 0x 前缀；保存时仍校验完整字节及帧格式。文本固定前缀、固定后缀可空，仍按所选编码填写文本。</p>"
        "<p><b>文本通道配置</b>只需填写需要自定义名称、单位或换算的列，列号从 0 开始。先按表格顺序输出已配置通道，再按原列顺序追加未配置列，默认名称 ChN（N = 原列号 + 1）、无单位，保留原始值；空表也自动读取全部列。配置 2 路时输入 4 列仍读取 4 路，测试和实际接收一致。每帧最多输出 256 路，空列、无效格式或数量超限拒绝整帧，不静默截断。CSV/WLR 每次录制按首个有效数值帧固定通道数；数量不符的帧只跳过录制并在底部红色累计提示，不影响实时展示，也不停止录制；再次录制重新锁定数量并清零。</p>"
        "<p>新增或留空通道名默认使用 Ch1、Ch2 等；已有名称保留。CSV、WLR 录制和波形导出保存对应名称，数据导入恢复文件名称，旧 WLR 无名称时使用 ChN。新建默认无校验、系数 1 和偏移 0。新建首尾分隔默认 JustFloat 帧尾 0000807F，其他类型默认结束符/帧尾 0D0A，也可选择预设或自定义；文本结束符必填，固定长度和首尾分隔的帧头、帧尾不能同时为空，长度字段类型必须有帧头。结构摘要可选中复制，配置冲突显示红色。最小/最大值校验换算后数值（边界允许），留空不限制；文本默认拒绝整帧，二进制默认超限通道 NaN、保留其他通道，也可手动选择处理方式；设备发送的 NaN 会保留为缺失采样点，同帧其他通道按配置解析。无效格式、无穷大（Inf）、换算溢出或帧结构与校验错误会拒绝整帧。</p>"
        "<p><b>二进制固定字段</b>按通道表内的数据区偏移、类型和大小端读取；可自动关联通道布局与数据区字节数。<b>动态数组</b>按实际数据区长度读取全部完整数值，通道表仅用于名称、单位、换算及范围等自定义，不限制实际通道数。新建通道名为 Ch1、Ch2；大小端的‘默认’继承协议设置。<b>固定字节条件</b>可筛选命令码等报文标记，全部匹配才接受，可留空，不代替校验。测试数值简洁展示，普通数值直接显示为 10、3.3 等，极大/极小值保留科学计数法，悬停可查看完整精度；不修改实际采集或录制值。</p>"
        "<p>右侧<b>协议名称</b>默认跟随当前打开项并测试未保存草稿；也可选其他协议的已保存版本，不切换中间编辑项或实际连接。切换测试协议或协议类型时，文本自动用文本格式，二进制自动用 HEX，并生成对应样例；也可手动切换格式。普通参数编辑保留手工样例，可点击<b>根据协议生成样例</b>更新。HEX 仅接受十六进制、小写自动大写，‘HEX 字节间空格’自动加显示空格；二进制文本表示使用 \\xHH。分包方式模拟整块、逐字节或随机分包，正常结果应相同；较长测试才出现停止按钮。测试选项及结果表头可悬停查看说明。</p>"
        "<p>主界面选择<b>自定义协议</b>或更换协议后会自动应用，通信线程确认后显示已应用。管理窗口只负责编辑、保存和测试，不提供应用按钮。没有协议时下方提示新建或导入，不能开启连接。串口、TCP 客户端/服务端和 BLE 均按连续字节流解码，协议字段名称和单位用于通道显示。</p>"
        "<p>管理窗口中的保存或导入在返回主界面后同步当前所选规则。结构切换涉及绘图历史时仍需确认，会清空旧半包及绘图历史，不删除磁盘录制。录制、发送或连接操作期间禁止切换；Demo 使用自身生成的数据。</p>"
        "<p>新增协议先选择文本分隔、二进制固定长度、长度字段、首尾分隔或多通道键值类型，新草稿立即显示在列表中并选中，名称随编辑同步。点击<b>保存协议</b>或按 <b>Ctrl+S</b> 保存；放弃新草稿会移除列表项，放弃已有协议的修改则恢复保存版本。配置顺序为协议名称、协议类型、协议说明。协议列表右键的<b>协议说明</b>展示该协议自己的说明。删除支持 Ctrl/Shift 多选；复制、重命名及排序位于右键菜单。有效备份用于协议库损坏后的恢复，仅加载失败时展示恢复入口。</p>"
        "<p>协议可独立导入/导出 JSON；菜单栏全局配置包含协议库和一个全软件共用的当前协议选择，串口、TCP 和 BLE 切换接口沿用同一选择。数值录制按本次首帧通道数固定，后续数量不符只跳过录制、底部红色累计提示，不影响实时展示；重录重新锁定并清零。数值文件仅保存通过规则验证的帧，并非全部原始字节；CSV/WLR 导入不再次执行协议换算。</p>"
        "<p>新建长度字段协议默认<b>动态数组</b>。长度字段可表示数据区字节数、整帧字节数，动态数组还可表示通道数量；系统根据布局自动计算总帧长修正，无需手动设置。数组支持 1–256 路，固定字段适合混合类型与明确偏移。数据校验统一从帧头起点 0 开始，覆盖到校验位之前，包含帧头、长度、数据和保留字节，不含校验位自身及帧尾；校验紧邻帧尾，无帧尾时位于整帧末尾。长度字段类型可选帧头校验保护头部与长度。提供 SUM8、XOR8、CRC8/SMBUS、CRC16/MODBUS、CCITT-FALSE、XMODEM 及 CRC32/ISO-HDLC，旁边的<b>说明 / C 代码</b>可查看参数、自检向量及示例。</p>"
        "<p><b>首尾分隔动态数组：</b>无长度字段，支持仅帧尾、仅帧头或首尾配对，但不能两者都为空。新建默认 N 个小端 float32 + <code>0000807F</code>，即 JustFloat 式配置；1–256 路、无校验。仅帧头时必须等到下一个帧头才输出上一帧，最后一帧暂存；生成样例会附加下一帧头。首尾都有时，新帧头可跳过未结束坏帧。数据区不完整或数量超限整段拒绝，校验紧贴帧尾（无帧尾时在本帧末尾）。通道表不限制动态数量，录制及导入导出沿用统一规则。<b>数据、保留/校验字节及跨字段组合不能与分隔标记相同</b>；没有转义，校验无法消除分隔歧义。无帧头的接收首段是否完整不能完全判断，从半个帧尾开始可能损失后续首帧，丢失字节无法恢复。可靠采集优先长度字段＋校验。</p>"
        "<p>CSV 录制及 WLR 转 CSV 的时间列为 <code>UnixTime(s;FS=...)</code>：以秒保存主机接收时刻的 Unix 时间戳，FS 是采样率参考值，不改变接收时间。旧 <code>HostReceiveTime(s;FS=...)</code> 和 <code>HostReceiveTime(s;Hint=...)</code> 文件仍可导入，并使用相同的重建时间轴规则。</p>"
    );
    contentLayout->addStretch();
    scrollArea->setWidget(scrollContent);
    mainLayout->addWidget(scrollArea);

    QPushButton *btnOk = new QPushButton("关闭", &dlg);
    btnOk->setObjectName("engineProtocolHelpClose");
    btnOk->setStyleSheet("QPushButton { background-color: #0e639c; color: #fff; padding: 6px 30px; font-weight: bold; border: none; border-radius: 4px; } QPushButton:hover { background-color: #1177bb; }");
    mainLayout->addWidget(btnOk, 0, Qt::AlignCenter);

    QObject::connect(btnOk, &QPushButton::clicked, &dlg, &QDialog::accept);
    dlg.refreshTheme(true);
    dlg.exec();
}


