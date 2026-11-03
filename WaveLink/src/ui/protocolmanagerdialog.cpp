#include "protocolmanagerdialog.h"
#include <QSpinBox>
#include "protocoleditorwidget.h"
#include "ProtocolParserFactory.h"
#include "ProtocolSampleGenerator.h"
#include "protocolsampleedit.h"
#include <QApplication>
#include <QScreen>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QScrollArea>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QSettings>
#include <QJsonArray>
#include <QTimer>
#include <QElapsedTimer>
#include <QCloseEvent>
#include <QInputDialog>
#include <QGroupBox>
#include <QFrame>
#include <QFormLayout>
#include <QMenu>
#include <QSet>
#include <QLineEdit>
#include <QShortcut>
#include <QTextBrowser>
#include <QTextDocument>
#include <QPalette>
#include <QEvent>
#include <QWindow>
#include <QSignalBlocker>
#include <QCheckBox>
#include <QTextOption>
#include <QLocale>
#include <cmath>
#include <limits>
#include <QUuid>

using namespace DataProtocols;
namespace {
// Preserve the theme's background/scrollbars; only diagnostics are red.
// Re-evaluate after inherited style/palette changes (including live themes).
class ProtocolDiagnosticsEdit : public QPlainTextEdit {
public:
    ProtocolDiagnosticsEdit() {
        setObjectName("protocolDiagnostics"); updateColor();
    }
protected:
    void changeEvent(QEvent *event) override {
        QPlainTextEdit::changeEvent(event);
        if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) && !m_pending) {
            m_pending = true;
            QTimer::singleShot(0, this, [this] { m_pending = false; updateColor(); });
        }
    }
private:
    void updateColor() {
        const QString color = palette().color(QPalette::Base).lightness() < 128 ? "#ff8b8b" : "#b42318";
        const auto style = QString("QPlainTextEdit#protocolDiagnostics { color: %1; }").arg(color);
        if (styleSheet() != style) setStyleSheet(style);
    }
    bool m_pending = false;
};
enum Decision { Cancel = 0, Save = 1, Discard = 2, Confirm = 3 };
QString readablePreviewNotation(const QString &text) {
    const int marker = text.indexOf('e', 0, Qt::CaseInsensitive);
    if (marker < 0) return text;
    bool ok = false;
    const int exponent = text.mid(marker + 1).toInt(&ok);
    if (!ok || exponent < -6 || exponent >= 12) return text;
    // Move the decimal point in the already-selected display string. Do not
    // round a second time or alter the parser/recording value.
    QString digits = text.left(marker), sign;
    if (digits.startsWith('-')) { sign = "-"; digits.remove(0, 1); }
    const int dot = digits.indexOf('.');
    const int position = (dot < 0 ? digits.size() : dot) + exponent;
    digits.remove('.');
    if (position <= 0) return sign + "0." + QString(-position, '0') + digits;
    if (position >= digits.size()) return sign + digits + QString(position - digits.size(), '0');
    return sign + digits.left(position) + "." + digits.mid(position);
}
QString previewNumber(double value, double raw, const QJsonObject &field, bool converted) {
    if (std::isnan(value)) return "NaN";
    const auto full = QString::number(value, 'g', QLocale::FloatingPointShortest);
    if (field.value("type") != QJsonValue("float32")) return readablePreviewNotation(full);
    const double scale = converted ? field.value("scale").toDouble(1) : 1;
    const double bias = converted ? field.value("bias").toDouble() : 0;
    if (scale == 0) return readablePreviewNotation(full); // The result is a double-precision bias alone.

    // A float32 promoted to double retains its binary quantization error.
    // Select the shortest displayed decimal consistent with that ORIGINAL
    // float's rounding interval, instead of dropping a fixed number of digits.
    // Invert scale/bias for the converted cell; do not re-round the actual
    // parsed, plotted or recorded value. When cancellation prevents a safe
    // inverse, retain the full double representation.
    const float source = static_cast<float>(raw);
    const float infinity = std::numeric_limits<float>::infinity();
    const double previous = std::nextafter(source, -infinity);
    const double next = std::nextafter(source, infinity);
    const double lower = std::isfinite(previous) ? (previous + raw) / 2 : raw - (next - raw) / 2;
    const double upper = std::isfinite(next) ? (next + raw) / 2 : raw + (raw - previous) / 2;
    for (int digits = 1; digits <= std::numeric_limits<double>::max_digits10; ++digits) {
        const auto text = QString::number(value, 'g', digits);
        bool ok = false; const double candidate = text.toDouble(&ok);
        const double original = (candidate - bias) / scale;
        // Strict bounds exclude halfway ties that may round to a neighbor.
        if (ok && std::isfinite(original) && original > lower && original < upper) return readablePreviewNotation(text);
    }
    return readablePreviewNotation(full);
}
struct UsageSection { QString title, content; };
const QVector<UsageSection> &usageSections() {
    static const QVector<UsageSection> sections{
        {"快速开始", QString::fromUtf8(R"(
            <p class='lead'>把设备的原始报文转换成数值通道，用于波形、分析和录制。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><b>新增协议 → 配置帧与通道 → 生成并测试样例 → 保存 → 主界面选择</b></td></tr></table>
            <h3>五种协议，按设备报文选择</h3>
            <ul><li><b>文本分隔：</b>例如 <code>DATA:1,2\r\n</code>，按结束符组帧、按分隔符读取数值列。</li>
            <li><b>二进制固定长度：</b>每帧总字节数相同，按通道类型和偏移读取。</li>
            <li><b>二进制长度字段：</b>长度字段决定每帧长度；默认动态数组，也可选择固定字段。</li><li><b>二进制首尾分隔：</b>用帧头和/或帧尾分隔动态数组；默认 JustFloat 式，无长度字段。</li>
            <li><b>多通道键值：</b>每行一个键和值，所有配置通道收到后组成一帧。可从通用模板或 CBF/IR/RED 设备模板创建。</li></ul>
            <h3>草稿与保存</h3>
            <p>新增协议立即选中，名称随编辑变化。点击<b>保存协议</b>或按 <b>Ctrl+S</b> 才会写入协议库。当前编辑项的测试使用未保存草稿；导出使用已保存版本。</p>
            <p>离开有修改的协议时，可选择保存、放弃或取消。放弃新草稿会移除该项；放弃已有协议的修改会恢复保存版本。</p>
            <p class='muted'>管理窗口不发送数据，也不直接切换设备协议。返回主界面后，在“自定义协议”下拉列表中选择要使用的协议；当前已选协议的保存修改会同步应用。</p>
        )")},
        {"文本分隔协议", QString::fromUtf8(R"(
            <p class='lead'>适合以逗号、空格或 Tab 分开的文本数值。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><code>DATA:25.34,3.30\r\n</code><br>前缀 DATA: · 分隔符 , · 结束符 0D0A</td></tr></table>
            <h3>配置顺序</h3><ol><li>选择 UTF-8 或 ASCII 文本编码。</li><li>设置固定前缀、固定后缀，均可留空；匹配后移除。</li>
            <li>选择字面分隔或空白合并。字面分隔保留空列；空白合并按连续空格/Tab 分列。</li><li>填写分隔符和结束符。结束符可下拉选择 <code>\r\n (0D0A)</code>、<code>\n (0A)</code>、<code>\r (0D)</code>、<code>\n\r (0A0D)</code>、NUL/ETX/EOT；右侧也可手动输入其他 HEX，自动大写。</li></ol>
            <p><b>结束符必须填写：</b>不提供“无”选项；清空时会提示，不能保存或测试。NUL 是实际的 <code>00</code> 字节，不是无结束符。设备发送内容必须带有相同结束符，以可靠处理串口/TCP 的拆包和粘包。</p>
            <h3>自动读取全部数值列</h3>
            <p>通道表只需填写要自定义名称、单位、换算或范围的列，<b>列号从 0 开始</b>，可留空表。配置通道先按表格顺序输出，未配置列按原列顺序追加，名称为 ChN，数值不换算。</p>
            <p>从二进制类型切换到文本类型时，字节偏移转为 <b>0、1、2…</b> 连续列号；连续布局的新增、删除和移动会自动重排。手动填写的非连续列号在加载或增删时保留，新通道使用首个未配置列，避免重复。</p>
            <p>例如只配置两列，收到四列数值时仍会读取全部四路。每帧最多 256 路；空列、非数值或数量超限拒绝整帧，不静默截断。</p>
            <p class='muted'>前缀、后缀和分隔符是文本，支持 \r、\n、\t、\\；结束符才是 HEX。选中“去除字段首尾空白”后，解析前清理每列两端空白。</p>
        )")},
        {"多通道键值协议与设备模板", QString::fromUtf8(R"(
            <p class='lead'>适合多行发送的数字键或文本键，例如 <code>0:1\r\n1:2\r\n</code>。</p>
            <h3>创建与映射</h3><p>新增协议时选择<b>多通道键值协议</b>，再选择通用模板或 <b>CBF/IR/RED 设备模板</b>。通用模板默认三路 Ch1、Ch2、Ch3；样例为 <code>0:0\r\n1:1\r\n2:2\r\n</code>。键值分隔符可为冒号、等号等文本；结束符必须填写。通道表中的键区分大小写、不能重复或含分隔符；最多 256 路。所有配置键都收到后，按通道表顺序输出一帧。移动通道只改变输出顺序，不改写键；未知键不会自动生成通道。</p>
            <h3>半帧处理</h3><p>重复键可选择<b>重新开始一帧</b>（推荐）或用最新值替换；未知键/非目标行可忽略或拒绝并清空未完成帧。组帧超时从首个有效键计时，默认 1000 ms；0 不限制。设备较慢时应调大。已配置键的无效数值、超长行或无效编码清空半帧；超限 NaN 模式仅标记该通道，保留其他值。接口重连或规则切换会清空未完成组。</p>
            <p class='muted'>没有帧序号的键值流无法完全识别漏行/丢包；要求严格对齐时建议设备增加帧序号，使用带长度及校验的二进制协议。</p>
            <h3>CBF/IR/RED 模板</h3><p>协议列表右键<b>新增协议 → 新增模板协议 → CBF/IR/RED 设备模板</b>可快捷创建。0 = CBF（默认单位 MHz），1 = IR（LED1），2 = RED（LED2），不交换数值。设备采样率默认 31 Hz，采用 TCP 服务端，默认端口 4970。模板可编辑、复制和独立分享；新安装列表不会自动塞入设备模板。模板的<b>启用TCP服务器预设</b>默认勾选，可按需关闭；未连接时应用协议会选择 TCP 服务器和可配置端口，不会自动开始监听。可选择首次完整帧后自动分离视图，之后仍可手动合并。旧内置 CBF 选择迁移为自定义模板，保留原来的接口预设、重复键替换和不限组帧时间规则。</p>
        )")},
        {"固定长度二进制帧", QString::fromUtf8(R"(
            <p class='lead'>适合总字节数固定、包含多种数值类型的报文。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><b>帧头 + 数据/保留字节 + 数据校验 + 帧尾</b><br>总帧长包括所有字节；帧头、帧尾可以分别留空，但不能同时为空。数据校验始终位于末尾帧尾之前。</td></tr></table>
            <h3>常用字节预设</h3><p>帧头可选 AA55、55AA、无帧头或自定义；帧尾可选 CRLF（0D0A）、LF（0A）、CR（0D）、JustFloat 小端（0000807F）、无帧尾或自定义。左侧选择后填入右侧 HEX，也可直接编辑；匹配预设时自动识别，否则显示自定义。“无”是空内容，不是 00 字节。</p>
            <p class='muted'>预设仅填写边界字节，不是完整协议模板；帧长、数据区、大小端及校验仍需与设备一致。打开已有协议按实际字节匹配，不覆盖已保存的内容。长度字段协议禁用“无帧头”，但允许无帧尾；手动清空必填帧头仍会标红提示。</p>
            <h3>帧与数据区</h3><p>设置总帧长、数据区起点和固定数据区字节数。帧内偏移从 0 开始；通道表的偏移<b>相对数据区起点</b>。全部通道必须在数据区内，不得重叠。</p>
            <h3>数据区与通道自动关联</h3><p>新建固定长度协议默认开启联动。新增、删除、移动通道或改变类型后，按真实类型宽度重算连续偏移，并更新数据区大小和帧长。例如 int16（2）+ float32（4），下一通道偏移为 6。</p>
            <p>增大数据区时，未配置的完整 4 字节补为 float32 通道；不足 4 字节视为保留字节。缩小时删除无法完整容纳的通道前会询问确认。特殊空隙可关闭联动、手动设置偏移。</p>
            <h3>固定字节条件</h3><p>用于筛选命令码、消息类型等。例如<b>完整帧</b>偏移 2 必须为 <code>01</code>。最多 16 项，全部匹配才接受；不需要筛选可留空，不代替校验。</p>
            <h3>分帧与错位恢复</h3><p>有帧头时按帧头与总帧长验证，有帧尾时也检查帧尾。仅帧尾且无数据校验时，按配置长度检查帧尾位置，长度不符的片段会被丢弃。若接收从半帧或杂字节开始，可能需要先跳过起点不明的首段。可使用帧头和数据校验提高错位恢复的可靠性。</p>
            <p>仅帧尾、无校验的定长协议在实时接收时，连续通过 3 帧才确认同步，从第 3 帧开始绘图和录制。坏帧或重连后重新确认。单帧样例可在此窗口核对格式。</p>
            <p class='muted'>HEX 字段只接受 0–9、A–F 和分隔空白，小写自动大写。若数据可能包含与边界完全相同的字节组合，建议同时配置帧头、帧尾及校验，降低误判风险；没有校验的协议无法绝对区分伪装成合法帧的错误数据。</p>
        )")},
        {"长度字段与动态数组", QString::fromUtf8(R"(
            <p class='lead'>发送多少个完整数值，就读取多少路；不依赖一次串口读取或 BLE 通知的大小。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><b>默认：</b>AA55 + uint16 数据区字节数 + N 个 float32 + 0D0A<br>小端、无校验、自动布局。默认总帧长 <b>T = 6 + N × 4</b>。</td></tr></table>
            <h3>动态数组（默认）</h3><p>统一选择数组数值类型；每帧数据区必须由完整数值组成。通道表只覆盖指定序号的名称、单位、换算和范围，不限制实际通道数。未配置项使用 Ch1、Ch2…，系数 1、偏移 0、无单位。</p>
            <p>默认允许 1–256 路，最大帧长 4096 字节。可调整通道上下限和帧长上限；软件单帧硬上限为 65536 字节。混合类型请选择<b>固定字段（按通道表提取）</b>。</p>
            <h3>长度字段含义</h3><ul><li><b>数据区字节数：</b>系统加上帧头、长度、保留、校验和帧尾开销。</li><li><b>整帧字节数：</b>长度值就是 T。</li><li><b>通道数量：</b>仅动态数组；系统乘以数组类型宽度再加开销。</li></ul>
            <p>无需填写长度修正。固定字段模式以“最小总帧长 − 固定数据区大小”计算开销，多出的附加字节参与校验，但不增加通道。最小、最大总帧长是接收允许范围，不是每帧固定长度。</p>
            <p>自动布局按<b>帧头 → 长度 → 头部保留 → 帧头校验 → 数据 → 尾部保留 → 数据校验 → 帧尾</b>排列。关闭后可手动设置数据区及长度字段位置，校验范围和长度修正仍由系统统一处理。</p>
            <p class='muted'>长度字段类型必须有帧头和长度字段；帧尾可空，填写时必须在实际帧末匹配。合法半包保留等待，无接收超时猜测；如果后方出现通过长度、边界、条件、校验和数值验证的完整帧，则跳过前方错位或不完整帧并恢复同步。单独出现标记字节不会触发切帧。建议开启帧头校验保护长度、数据校验保护整帧；无校验时，数据中完整嵌入另一段合法报文仍可能产生歧义。</p>
        )")},
        {"通道换算与录制", QString::fromUtf8(R"(
            <p class='lead'>把原始数值转换成有名称、有单位的分析通道。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><b>换算值 = 原始值 × 系数 + 换算偏移</b><br>新建通道默认 ChN、系数 1、偏移 0；名称留空保存时补默认名。单位仅用于显示。</td></tr></table>
            <h3>备注与数值输入</h3><p>通道表末尾的<b>备注</b>默认为空，可输入中英文、数字和符号，单行最多 1024 个字符；随协议保存、复制及独立/全局配置导入导出，不参与解析和数值换算。单位仍可自由输入。数据偏移、文本列号、数组序号只接受非负整数；系数、换算偏移及最小/最大值支持有限浮点数、负数和科学计数法，小数点使用英文句点，不接受中文。系数和换算偏移不可留空；键值协议的通道键仍支持文本。</p>
            <h3>最小值与最大值</h3><p>检查<b>换算后的数值</b>，包含边界，留空则不限制；不是波形 Y 轴范围。可选择拒绝整帧，或仅把超限通道标记为 NaN、保留其他通道。</p>
            <p>新建二进制、多通道键值协议默认 NaN 模式，文本分隔协议默认拒绝整帧。NaN 模式排在第一项，拒绝整帧排在第二项。设备发送的 NaN 表示缺失采样点，保留在原始和换算结果中，继续解析同帧其他通道。无穷大（Inf）、换算溢出、无效文本、帧结构或校验错误会拒绝整帧。</p>
            <h3>选择录制保存的数值</h3><p>通道表上方的<b>录制保存数据类型</b>对每个协议独立保存，并随协议及全局配置导入导出。<b>原始数值（默认）</b>保存系数、偏移及超限 NaN 替代前的真实读数；<b>换算后数值</b>保存“原始值 × 系数 + 换算偏移”，超限通道按设置保存为 NaN。未配置通道始终按系数 1、偏移 0 保留原值。两者都不包含界面数字滤波，适用于 CSV 和 WLR；格式/校验错误及拒绝整帧模式仍不生成录制帧。原始值是按类型和大小端解码的数值，不是报文字节抓包。</p>
            <p>此选择仅影响之后开始的录制，不修改已有文件。“保存数据”合并或转换录制时保留文件已有值，不再次换算。<b>导出当前波形</b>仍保存当前显示结果（含缩放、偏移和滤波）。CSV、WLR 及波形导出保留通道名称；导入恢复文件中的数值和名称，<b>不自动执行当前协议的系数和偏移</b>。测试数值简洁显示，悬停查看完整精度；显示舍入不改变文件数值。</p>
            <h3>显示与录制的通道数量</h3><p>动态数组和文本实时展示、解析测试仍可读取不同数量的通道，不增加协议配置项。CSV/WLR 每次录制固定为开始后首个有效数值帧的通道数；数量不符只跳过该帧的录制、不截取或补造通道、不影响展示，正常数量恢复后继续写入。底部状态栏红色累计显示“录制中 · 已跳过 N 帧异常数据”；停止后清除，再次录制重新锁定并清零。真实磁盘写入失败仍立即停止。</p>
            <p class='muted'>NaN 保留时间点和波形断点。历史变通道 CSV/WLR 文件仍可回放及合并导出，按最大数量补 NaN，不补 0、不沿用上一帧。波形历史上限只影响绘图历史；录制保存的是通过解析验证的接收数值帧。</p>
        )")},
        {"校验与帧结构", QString::fromUtf8(R"(
            <p class='lead'>固定长度、长度字段及首尾分隔使用相同的数据校验规则。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><b>从帧起点 0 开始，到数据校验位之前结束</b><br>包含帧头、长度、数据和保留字节；不包含校验位自身和帧尾。校验位紧贴帧尾之前，无帧尾时就是整帧末尾。</td></tr></table>
            <h3>只需选择算法</h3><p>支持无校验、SUM8、XOR8、CRC8/SMBUS、CRC16/MODBUS、CRC16/CCITT-FALSE、CRC16/XMODEM 和 CRC32/ISO-HDLC。范围、位置由系统计算，多字节长度及校验值继承默认大小端。</p>
            <p>可选帧头校验独立位于数据区之前，覆盖起点 0 到帧头校验位之前的头部和完整长度字段。数据校验仍保护末尾前的内容，包括帧头校验字节。</p>
            <p>点击<b>说明 / C 代码</b>查看算法参数、标准自检向量及可复制的 C 参考实现。算法名相似不代表参数一致，必须与设备发送方匹配。</p>
            <h3>实时结构摘要</h3><p>协议说明框下方第一行显示帧结构，后续显示帧长、数据区和长度公式，可选中复制。越界、重叠或帧长不足等冲突标红，未修正时不能保存或测试。</p>
            <p class='muted'><b>旧非统一规则会覆盖为新规则。</b>旧校验范围、位置、长度修正和独立大小端不再保留入口；名称、单位和换算保留。设备端若使用旧规则，需同步修改。</p>
        )")},
        {"协议测试", QString::fromUtf8(R"(
            <p class='lead'>测试不向设备发送、不写入录制，也不应用协议到连接。</p>
            <h3>自动选择格式与样例</h3><p>右侧默认跟随当前打开协议，使用未保存草稿；选择其他协议测试其已保存版本。切换测试协议或协议类型时，<b>文本协议自动用文本格式，二进制自动用 HEX</b>，并生成对应样例。配置无效或无法生成时清空不匹配的旧样例并提示原因。</p>
            <p>可以手动切换格式，切换只转换表示，保留相同字节；未补全的 HEX 半字节不能转换。普通参数编辑不会反复覆盖手工样例，修改后可点击<b>根据协议生成样例</b>更新。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><b>HEX：</b>仅 0–9、A–F，小写自动大写；“HEX 字节间空格”自动添加字节间显示空格，空格不属于数据。<br><b>文本：</b>支持 UTF-8、\r、\n、\t、\\、\xHH。二进制转换为文本时所有字节使用 \xHH，例如 \x40、\x55。</td></tr></table>
            <h3>生成和分包验证</h3><p>生成器按所选规则编码数值、长度、头尾、固定条件和校验，并通过真实解析器验证。手动生成失败时保留原输入；生成成功不证明实物设备配置正确。动态数组“样例通道数”仅控制生成，不限制接收。</p>
            <p>整块输入每批最多 4096 字节，逐字节每批 1 字节，随机分包每批 1–31 字节。只模拟接收边界，不模拟发送时序，正常结果应相同；统计显示实际分包次数和大小。</p>
            <h3>红色诊断与定位</h3><p>错误区显示样例的原始字节偏移（从 0 开始），并尽可能列出帧内位置、预期/实际字节、出错通道及列号。支持帧头/帧尾、前后缀/结束符、字节数量、长度字段、校验、固定条件、编码和数值范围诊断；疑似分隔符错误明确标为可能原因，不自动更改协议。</p>
            <p>样例结束时额外检查半帧及缺失键值通道。实际设备接收中的半包仍正常等待，不因测试诊断而丢弃。错误字节附近的 HEX 可复制；分包方式不改变字节偏移。没有帧序号或校验时，不能定位所有硬件丢字节/错误类型。</p>
            <p class='muted'>最多 256 KiB 原始样例字节。结果表最多 1000 项、诊断最多 100 条，全部已处理帧计入统计。较长测试进行时才显示“停止测试”，可取消剩余输入，保留已有结果。</p>
        )")},
        {"管理、导入导出与应用", QString::fromUtf8(R"(
            <p class='lead'>协议可独立分享，也可随软件全局配置一起保存。</p>
            <h3>列表快捷操作</h3><p>右键提供新建、复制、重命名、删除、导出、排序，以及当前协议自己的<b>协议说明</b>。复制放在原项下方，名称追加 _1，重名时用 _2、_3。Ctrl/Shift 可多选删除；全软件当前选择或正在使用的协议需先切换后删除。</p>
            <h3>独立协议文件</h3><p>可导出所选或全部协议（右键还可导出当前项）。导入支持追加、按 ID 更新、替换整库；未保存修改会先询问处理。独立协议文件不改变当前设备连接，也不发送数据。</p>
            <p>导出当前或所选协议时，默认文件名为 <code>WaveLink_Protocols_协议名称.json</code>；多选名称以 _ 连接。只有“导出全部”使用 <code>WaveLink_Protocols.json</code>。文件名中的路径及非法字符会自动替换，不改变协议名称。</p>
            <h3>全局配置与统一选择</h3><p>菜单栏“设置”中的导出配置包含完整协议库、<b>一个全局当前协议选择</b>及命令面板配置。导入可追加、按 ID 合并、覆盖或跳过协议库，是否恢复选择可单独决定；命令面板的覆盖、追加或跳过在下一步单独选择。</p>
            <p>串口、TCP 客户端/服务端、UDP 和 BLE 共用当前协议；切换接口沿用，重启保留。主界面选择自定义协议后自动应用，通信线程确认后才显示已应用；Demo 使用自身生成的数据，不另记协议。</p>
            <p>录制、发送或连接操作期间禁止切换。应用结构变化需要清理绘图历史时会确认，<b>不会删除磁盘录制文件</b>。无自定义协议时提示新建或导入，并阻止以空协议打开连接。</p>
            <h3>有效备份</h3><p>保存保留上一次有效协议库备份。加载失败时才展示恢复入口；恢复前保留原文件。这不是恢复出厂设置，不删除录制文件。</p>
            <p class='muted'>旧软件若不支持扩展协议会拒绝导入，不应降级解码。数值 CSV/WLR 导入是回放，不需要重新解析原始协议。</p>
        )")},
        {"首尾分隔与 JustFloat", QString::fromUtf8(R"(
            <p class='lead'>没有长度字段，按帧头和/或帧尾分隔动态数值数组。</p>
            <table class='card' width='100%' cellpadding='12'><tr><td><b>新建默认 JustFloat 式：</b>N 个 float32 小端 + <code>00 00 80 7F</code><br>无帧头、无长度、无校验；1–256 路，最大帧长 4096 字节。发送的是原始二进制字节，不是 HEX 字符串。</td></tr></table>
            <h3>三种分隔方式</h3><ul><li><b>仅帧尾：</b>以完整帧尾结束一帧，数据区在帧尾前；默认 JustFloat 属于此方式。</li>
            <li><b>仅帧头：</b>以本帧头开始，收到下一个帧头才确定本帧结束；下一个帧头不计入上一帧。最后一帧暂存等待，不按超时或一次接收的大小猜测。生成样例会自动附加下一帧头，测试会说明该边界正常保留。</li>
            <li><b>帧头＋帧尾：</b>两者必须配对；帧尾前遇到新的帧头时，拒绝不完整帧并从新帧头重新同步。</li></ul>
            <p><b>帧头、帧尾不能同时为空</b>；配置冲突在上方标红，并禁止保存和测试。支持常用预设和自定义 HEX。此类型只用动态数组；混合类型选固定长度或长度字段的固定字段模式。</p>
            <h3>数据、校验与录制</h3><p>自动布局为帧头 → 头部保留 → N 个同类型数值 → 尾部保留 → 数据校验 → 帧尾；没有长度和帧头校验。通道表只覆盖名称、单位、备注、换算及范围，不限制实际接收数量；未配置通道 ChN、系数 1、偏移 0。数据区必须是类型宽度的整数倍且满足通道数和帧长限制，否则整段拒绝，不截取部分数值。</p>
            <p>数据校验从帧起点 0 到校验位之前，包含保留字节；校验紧贴帧尾，无帧尾时位于本帧最后。录制原始/换算数值、协议独立导入导出和全局配置保存均适用；当前录制按首帧通道数固定，后续数量不符只跳过录制并累计提示，实时展示仍可变。逐字节或随机分包用于验证边界跨接收包仍正常。</p>
            <h3>必须了解的限制</h3><p><b>数据、保留字节、校验值以及跨字段组合不能与生效的分隔标记混淆</b>；此模式不提供字节转义，CRC 不能消除分隔歧义。任意二进制数据或可靠性要求高时，优先选择长度字段＋校验。</p>
            <p>无帧头时，从半帧开始接收的首段若恰好满足结构，无法判断是否完整；若从半个帧尾开始，可能要到后续完整帧尾才恢复。超长无帧头报文丢弃到完整帧尾再同步，不把后半段当作新帧。丢失的字节不能恢复。仅帧头方式的最后一帧需要后续边界，UDP 报文末尾也不会被当作隐式帧尾。</p>
        )")}
    };
    return sections;
}
class ProtocolUsageDialog final : public QDialog {
public:
    explicit ProtocolUsageDialog(QWidget *parent) : QDialog(parent) {
        setObjectName("protocolUsageDialog"); setWindowTitle("自定义协议 · 使用指南"); setSizeGripEnabled(true);
        setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) & ~Qt::WindowContextHelpButtonHint);
        const auto screen = parent && parent->windowHandle() ? parent->windowHandle()->screen() : QApplication::primaryScreen();
        const auto available = screen ? screen->availableGeometry().size() - QSize(40, 60) : QSize(1100, 780);
        setMinimumSize(QSize(640, 420).boundedTo(available)); resize(QSize(1040, 740).boundedTo(available));
        auto root = new QVBoxLayout(this); root->setContentsMargins(20, 18, 20, 16); root->setSpacing(12);
        auto title = new QLabel("自定义协议使用指南"); QFont titleFont = title->font(); titleFont.setPointSize(18); titleFont.setBold(true); title->setFont(titleFont); root->addWidget(title);
        auto subtitle = new QLabel("配置报文规则 · 验证解析结果 · 应用于波形与录制"); subtitle->setWordWrap(true); root->addWidget(subtitle);
        auto split = new QSplitter(Qt::Horizontal); split->setObjectName("protocolUsageColumns"); root->addWidget(split, 1);
        m_list = new QListWidget; m_list->setObjectName("protocolUsageList"); m_list->setMinimumWidth(170); m_list->setMaximumWidth(250);
        m_list->setStyleSheet("QListWidget#protocolUsageList { outline: 0; border: none; } QListWidget#protocolUsageList::item { padding: 8px; margin: 2px 0; border-radius: 4px; } QListWidget#protocolUsageList::item:selected { background-color: #1675b5; color: white; }");
        split->addWidget(m_list);
        m_body = new QTextBrowser; m_body->setObjectName("protocolUsageText"); m_body->setOpenExternalLinks(false);
        m_body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        QFont bodyFont = m_body->font(); bodyFont.setPointSize(qMax(10, bodyFont.pointSize())); m_body->setFont(bodyFont);
        m_body->document()->setDocumentMargin(18); split->addWidget(m_body); split->setStretchFactor(1, 1); split->setSizes({215, 780});
        for (int i = 0; i < usageSections().size(); ++i) m_list->addItem(QString("%1  %2").arg(i + 1, 2, 10, QChar('0')).arg(usageSections()[i].title));
        auto footer = new QHBoxLayout; auto tip = new QLabel("左侧选择主题 · 右侧可滚动阅读、选中复制"); tip->setWordWrap(true); footer->addWidget(tip, 1);
        auto close = new QPushButton("关闭"); close->setMinimumWidth(90); footer->addWidget(close); root->addLayout(footer);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        connect(m_list, &QListWidget::currentRowChanged, this, [this] { render(); }); m_list->setCurrentRow(0);
    }
protected:
    void changeEvent(QEvent *event) override {
        QDialog::changeEvent(event);
        if (m_body && (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange))
            QTimer::singleShot(0, this, [this] { render(); });
    }
private:
    void render() {
        if (!m_body || !m_list || m_list->currentRow() < 0) return;
        m_body->ensurePolished(); const auto &section = usageSections()[m_list->currentRow()];
        const bool dark = m_body->palette().color(QPalette::Base).lightness() < 128;
        const QString accent = dark ? "#70C8FF" : "#07689D", card = dark ? "#263947" : "#EAF4FB", muted = dark ? "#BAC6D0" : "#536575";
        // Native Windows scrollbars otherwise remain white in the dark theme.
        // Style only the bars, leaving the browser's text/base inherited so a
        // subsequent application theme change can still be detected correctly.
        const QString bars = QString("QScrollBar:vertical { background: %1; width: 12px; margin: 0; } QScrollBar::handle:vertical { background: %2; min-height: 28px; border-radius: 5px; } QScrollBar:horizontal { background: %1; height: 12px; margin: 0; } QScrollBar::handle:horizontal { background: %2; min-width: 28px; border-radius: 5px; } QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; } QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; } QScrollBar::add-page, QScrollBar::sub-page { background: %1; }")
            .arg(dark ? "#25282C" : "#EDF1F5", dark ? "#59636D" : "#ACBAC8");
        if (m_body->styleSheet() != bars) m_body->setStyleSheet(bars);
        const auto listStyle = QString("QListWidget#protocolUsageList { outline: 0; border: none; } QListWidget#protocolUsageList::item { padding: 8px; margin: 2px 0; border-radius: 4px; } QListWidget#protocolUsageList::item:selected { background-color: #1675b5; color: white; }") + bars;
        if (m_list->styleSheet() != listStyle) m_list->setStyleSheet(listStyle);
        m_body->document()->setDefaultStyleSheet(QString("h2 { color: %1; font-size: 18pt; margin-top: 0; } h3 { font-size: 12pt; margin-top: 20px; } p { margin-top: 8px; margin-bottom: 12px; } .lead { font-size: 11pt; } .muted { color: %2; } .card { background-color: %3; } code { font-family: Consolas, monospace; } li { margin-bottom: 6px; }").arg(accent, muted, card));
        m_body->setHtml("<h2>" + section.title + "</h2>" + section.content);
    }
    QListWidget *m_list = nullptr;
    QTextBrowser *m_body = nullptr;
};
struct ProtocolKind { QString name, hint, action; int type; };
// Editor, New dialog and list menu use the same type names.
const QVector<ProtocolKind> &protocolKinds() {
    static const QVector<ProtocolKind> kinds{
        {ProtocolEditorWidget::protocolTypeNames().at(0), "按结束符分帧，再按分隔符读取数值列。", "protocolMenuNewText", 0},
        {ProtocolEditorWidget::protocolTypeNames().at(1), "每帧总字节数固定，按偏移和数据类型读取字段。", "protocolMenuNewBinary", 1},
        {ProtocolEditorWidget::protocolTypeNames().at(2), "长度字段分帧，动态数组自动读取全部通道，也可选择固定字段。", "protocolMenuNewLength", 2},
        {ProtocolEditorWidget::protocolTypeNames().at(4), "按帧头和/或帧尾分隔动态数组，不需要长度字段，两者不能同时为空。仅帧头需等待下一个帧头；默认 JustFloat 小端。数据内不能包含分隔标记。", "protocolMenuNewDelimited", 4},
        {ProtocolEditorWidget::protocolTypeNames().at(3), "每行一个键和值，所有配置通道收到后组成一帧。支持数字/文本键，也可从 CBF/IR/RED 设备模板创建。", "protocolMenuNewKeyValue", 3}};
    return kinds;
}
int ask(QWidget *parent, const QString &title, const QString &text, bool draft, const QStringList &names = {}) {
    // Use the same themed/resizable Qt dialog on all platforms. Qt 5.14's
    // native QMessageBox Windows system menu also requires a native window.
    QDialog dialog(parent); dialog.setObjectName("protocolConfirm"); dialog.setWindowTitle(title); dialog.resize(names.isEmpty() ? 520 : 650, names.isEmpty() ? 220 : 300); dialog.setSizeGripEnabled(true);
    auto layout = new QVBoxLayout(&dialog); auto message = new QLabel(text); message->setTextFormat(Qt::PlainText); message->setWordWrap(true); layout->addWidget(message, names.isEmpty() ? 1 : 0);
    if (!names.isEmpty()) {
        dialog.setMinimumWidth(560);
        auto list = new QPlainTextEdit; list->setObjectName("protocolDeleteNames"); list->setReadOnly(true);
        list->setWordWrapMode(QTextOption::WrapAnywhere);
        QFont font = list->font(); font.setBold(true); font.setPointSizeF(qMax(9.0, font.pointSizeF()) + 2); list->setFont(font);
        list->setStyleSheet("QPlainTextEdit#protocolDeleteNames { border: 2px solid #d94a54; border-radius: 5px; padding: 8px; font-weight: bold; }");
        QStringList numbered; for (int i = 0; i < names.size(); ++i) numbered << QString("%1. %2").arg(i + 1).arg(names[i]); list->setPlainText(numbered.join('\n'));
        list->setMinimumHeight(78); list->setMaximumHeight(250); layout->addWidget(list, 1);
        auto warning = new QLabel("仅删除上面列出的协议，未选中的协议不会删除。"); warning->setWordWrap(true); layout->addWidget(warning);
    }
    auto row = new QHBoxLayout; layout->addLayout(row);
    const QVector<QPair<int, QString>> choices = draft ? QVector<QPair<int, QString>>{{Save, "保存并继续"}, {Discard, "放弃修改"}, {Cancel, "取消"}}
        : QVector<QPair<int, QString>>{{Confirm, "确认"}, {Cancel, "取消"}};
    for (const auto &choice : choices) {
        auto button = new QPushButton(choice.second); button->setObjectName(QStringLiteral("protocolDecision%1").arg(choice.first)); button->setAutoDefault(false); row->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, choice] { dialog.done(choice.first); });
        if (choice.first == Cancel) { button->setDefault(true); button->setFocus(); }
        if (!names.isEmpty() && choice.first == Confirm) { button->setText("删除协议"); button->setStyleSheet("QPushButton { background-color: #a63842; color: white; border: 1px solid #d94a54; border-radius: 4px; padding: 6px 16px; } QPushButton:hover { background-color: #be4651; }"); }
    }
    return dialog.exec();
}
}
ProtocolManagerDialog::ProtocolManagerDialog(const QString &path, QWidget *parent, const QString &selectedProfileId) : QDialog(parent), m_store(path) {
    setWindowTitle("自定义数据协议 · 管理与测试"); setObjectName("protocolManagerDialog");
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint); setSizeGripEnabled(true);
    auto screen = QApplication::primaryScreen(); resize(screen ? QSize(1440, 860).boundedTo(screen->availableGeometry().size() - QSize(60, 100)) : QSize(1440, 860));
    auto root = new QVBoxLayout(this);
    auto note = new QLabel("配置接收数据的解码规则，可独立导入导出。测试不会发送数据；返回主界面后，选中的自定义协议自动应用。"); note->setWordWrap(true); root->addWidget(note);
    auto split = new QSplitter(Qt::Horizontal); split->setObjectName("protocolColumns"); root->addWidget(split, 1);
    auto left = new QWidget; auto ll = new QVBoxLayout(left); ll->setContentsMargins(0, 0, 0, 0);
    auto leftSplit = new QSplitter(Qt::Vertical); leftSplit->setObjectName("protocolLeftSplit"); ll->addWidget(leftSplit);
    auto listArea = new QWidget; auto listLayout = new QVBoxLayout(listArea); listLayout->setContentsMargins(0, 0, 0, 0);
    auto listTitle = new QLabel("协议列表"); listTitle->setStyleSheet("font-weight: bold;"); listLayout->addWidget(listTitle);
    m_list = new QListWidget; m_list->setObjectName("protocolList"); m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setMinimumSize(150, 100); m_list->setTextElideMode(Qt::ElideRight); listLayout->addWidget(m_list, 1);
    // Leave normal background/text to the application's theme stylesheet.
    // palette(base) can still be the native white palette in a dark theme.
    m_list->setStyleSheet("QListWidget#protocolList { outline: 0; }"
        "QListWidget#protocolList::item { padding-left: 10px; margin: 1px; border: 1px solid transparent; border-radius: 3px; }"
        "QListWidget#protocolList::item:hover:!selected { border: 1px solid #00b45f; }"
        "QListWidget#protocolList::item:selected { background-color: #00D26A; color: white; }");
    m_list->setContextMenuPolicy(Qt::CustomContextMenu); connect(m_list, &QListWidget::customContextMenuRequested, this, &ProtocolManagerDialog::showListMenu);
    leftSplit->addWidget(listArea);
    auto management = new QGroupBox("协议管理"); auto ml = new QVBoxLayout(management);
    auto managementScroll = new QScrollArea; managementScroll->setObjectName("protocolManagementScroll"); managementScroll->setWidgetResizable(true);
    managementScroll->setFrameShape(QFrame::NoFrame); managementScroll->setMinimumHeight(160); managementScroll->setWidget(management); leftSplit->addWidget(managementScroll);
    ml->setSpacing(10);
    auto create = new QPushButton("新增协议…"), remove = new QPushButton("删除协议");
    create->setObjectName("protocolNew"); remove->setObjectName("protocolDelete"); ml->addWidget(create); ml->addWidget(remove);
    auto import = new QPushButton("导入协议…"); import->setObjectName("protocolImport"); ml->addWidget(import);
    auto exportSelected = new QPushButton("导出所选…"), exportAll = new QPushButton("导出全部…");
    exportSelected->setObjectName("protocolExportSelected"); exportAll->setObjectName("protocolExportAll"); ml->addWidget(exportSelected); ml->addWidget(exportAll);
    auto help = new QPushButton("使用说明"); help->setObjectName("protocolHelp"); ml->addWidget(help); ml->addStretch();
    leftSplit->setCollapsible(0, false); leftSplit->setCollapsible(1, false); leftSplit->setStretchFactor(0, 1); leftSplit->setSizes({430, 310});
    split->addWidget(left);
    auto middle = new QWidget; auto middleLayout = new QVBoxLayout(middle); middleLayout->setContentsMargins(0, 0, 0, 0);
    auto editorTitle = new QLabel("协议配置"); editorTitle->setStyleSheet("font-weight: bold;"); middleLayout->addWidget(editorTitle);
    auto scroll = new QScrollArea; scroll->setObjectName("protocolEditorScroll"); scroll->setWidgetResizable(true);
    auto editorArea = new QWidget; auto editorLayout = new QVBoxLayout(editorArea); editorLayout->setContentsMargins(0, 0, 0, 0); editorLayout->setSpacing(0);
    // A normal box layout excludes the hidden editor from size calculations,
    // keeping the empty-library welcome page comfortable on short screens.
    m_welcome = new QWidget; m_welcome->setObjectName("protocolWelcome"); auto welcomeLayout = new QVBoxLayout(m_welcome); welcomeLayout->setContentsMargins(18, 18, 18, 18); welcomeLayout->addStretch();
    auto card = new QFrame; card->setObjectName("protocolWelcomeCard"); card->setStyleSheet("QFrame#protocolWelcomeCard { border: 1px solid palette(mid); border-radius: 8px; }");
    auto cardLayout = new QVBoxLayout(card); cardLayout->setContentsMargins(20, 20, 20, 20); cardLayout->setSpacing(16);
    m_welcomeTitle = new QLabel; m_welcomeTitle->setObjectName("protocolWelcomeTitle"); m_welcomeTitle->setWordWrap(true); m_welcomeTitle->setStyleSheet("font-size: 18px; font-weight: bold;"); cardLayout->addWidget(m_welcomeTitle);
    m_welcomeText = new QLabel; m_welcomeText->setObjectName("protocolWelcomeText"); m_welcomeText->setWordWrap(true); cardLayout->addWidget(m_welcomeText);
    auto steps = new QLabel("1. 选择协议类型\n2. 配置帧结构与数值通道\n3. 生成样例并解析测试\n4. 保存后返回主界面选择使用"); steps->setObjectName("protocolWelcomeSteps"); steps->setWordWrap(true); cardLayout->addWidget(steps);
    auto welcomeNew = new QPushButton("＋ 新增协议"); welcomeNew->setObjectName("protocolWelcomeNew");
    welcomeNew->setStyleSheet("QPushButton#protocolWelcomeNew { background-color: #1675b5; color: white; border: 1px solid #3396d6; border-radius: 4px; padding: 8px; font-weight: bold; } QPushButton#protocolWelcomeNew:hover { background-color: #218bc9; } QPushButton#protocolWelcomeNew:disabled { background-color: #555555; color: #bbbbbb; border-color: #666666; }");
    auto welcomeImport = new QPushButton("已有协议文件？导入协议"); welcomeImport->setObjectName("protocolWelcomeImport"); cardLayout->addWidget(welcomeNew); cardLayout->addWidget(welcomeImport);
    connect(welcomeNew, &QPushButton::clicked, create, &QPushButton::click); connect(welcomeImport, &QPushButton::clicked, import, &QPushButton::click);
    welcomeLayout->addWidget(card); welcomeLayout->addStretch(); editorLayout->addWidget(m_welcome);
    m_editor = new ProtocolEditorWidget; editorLayout->addWidget(m_editor); scroll->setWidget(editorArea); middleLayout->addWidget(scroll, 1);
    auto save = new QPushButton("保存协议"); save->setObjectName("protocolSave"); save->setToolTip("保存当前协议（Ctrl+S），不会切换设备正在使用的协议。");
    save->setStyleSheet("QPushButton#protocolSave { background-color: #1675b5; color: white; border: 1px solid #3396d6; border-radius: 4px; padding: 7px 16px; font-weight: bold; }"
        "QPushButton#protocolSave:hover { background-color: #218bc9; } QPushButton#protocolSave:pressed { background-color: #125888; }"
        "QPushButton#protocolSave:disabled { background-color: #555555; color: #bbbbbb; border-color: #666666; }");
    auto row = new QHBoxLayout; row->addWidget(save); middleLayout->addLayout(row); split->addWidget(middle);
    auto shortcut = new QShortcut(QKeySequence::Save, this); shortcut->setObjectName("protocolSaveShortcut");
    connect(shortcut, &QShortcut::activated, save, &QPushButton::click);
    auto testArea = new QWidget; auto tl = new QVBoxLayout(testArea); tl->setContentsMargins(2, 2, 2, 2);
    auto testTitle = new QLabel("协议测试（不会向设备发送）"); testTitle->setStyleSheet("font-weight: bold;"); tl->addWidget(testTitle);
    m_previewProtocol = new QComboBox; m_previewProtocol->setObjectName("protocolPreviewProtocol");
    m_previewProtocol->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_previewProtocol->setMinimumContentsLength(10);
    m_inputMode = new QComboBox; m_inputMode->setObjectName("protocolInputMode"); m_inputMode->addItems({"文本（转义符生效）", "HEX 字节"});
    m_chunkMode = new QComboBox; m_chunkMode->setObjectName("protocolChunkMode"); m_chunkMode->addItems({"整块输入", "逐字节", "随机分包"});
    m_generateSample = new QPushButton("根据协议生成样例"); m_generateSample->setObjectName("protocolGenerateSample"); m_test = new QPushButton("解析测试"); m_test->setObjectName("protocolTest");
    // Keep the two primary test actions distinct from ordinary management
    // buttons, with readable text in both application themes.
    auto accentAction = [](QPushButton *button, const QString &normal, const QString &hover, const QString &pressed) {
        const QString selector = "QPushButton#" + button->objectName();
        button->setStyleSheet(QString(
            "%1 { background-color: %2; color: #ffffff; border: 2px solid %2; border-radius: 4px; padding: 7px 12px; font-weight: bold; }"
            "%1:hover { background-color: %3; border-color: %3; }"
            "%1:pressed { background-color: %4; border-color: %4; }"
            "%1:focus { border-color: #b9dfff; }"
            "%1:disabled { background-color: #687580; color: #eef1f4; border-color: #687580; }")
            .arg(selector, normal, hover, pressed));
    };
    accentAction(m_generateSample, "#176ea6", "#1b7ab2", "#125781");
    accentAction(m_test, "#187545", "#1d8150", "#105b35");
    m_cancelTest = new QPushButton("停止测试"); m_cancelTest->setObjectName("protocolCancelTest"); m_cancelTest->setEnabled(false); m_cancelTest->hide();
    auto testOptions = new QFormLayout; testOptions->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto testOption = [testOptions](const QString &caption, QComboBox *field, const QString &help) {
        auto label = new QLabel(caption); label->setObjectName(field->objectName() + "Label"); label->setToolTip(help); field->setToolTip(help); testOptions->addRow(label, field);
    };
    testOption("协议名称", m_previewProtocol, "默认选择中间正在打开的协议，并使用当前未保存的编辑内容。也可选择其他协议的已保存版本测试；不会切换中间编辑项、保存草稿或改变设备连接。切换协议或协议类型会自动选择文本/HEX，并生成对应样例，替换上一协议的输入；普通配置编辑不会覆盖手工样例。");
    testOption("样例格式", m_inputMode, "文本：支持 UTF-8、\\r、\\n、\\t、\\\\ 及 \\xHH 原始字节转义。二进制协议生成样例或从 HEX 转成文本时，每个字节统一显示为 \\xHH，例如 \\x40、\\x55，帧尾显示为 \\x0D\\x0A；手动输入的普通字符和短转义仍兼容。文本协议保留可读文字。HEX：仅 0–9、A–F，小写自动大写，每字节两位，不写 0x。切换格式保留原始字节；半个 HEX 字节需先补全。不会改变协议类型。");
    testOption("分包方式", m_chunkMode, "整块输入：以最多 4 KiB 批次处理；逐字节：每次输入一个字节；随机分包：每次输入 1–31 字节。用于检查拆包/粘包处理，不模拟发送时序或向设备发送数据。");
    m_arrayCount = new QSpinBox; m_arrayCount->setObjectName("protocolSampleArrayCount"); m_arrayCount->setRange(1, 256); m_arrayCount->setValue(2); m_arrayCount->setKeyboardTracking(false);
    m_arrayCountLabel = new QLabel("样例通道数"); m_arrayCountLabel->setObjectName("protocolSampleArrayCountLabel");
    const QString countHelp = "仅用于生成动态数组测试样例，不限制真实接收的通道数。选择 1、2、16 等数量，软件按当前类型、边界、长度含义（如有）及校验设置生成有效报文；半包和粘包的结果应相同。";
    m_arrayCount->setToolTip(countHelp); m_arrayCountLabel->setToolTip(countHelp); testOptions->addRow(m_arrayCountLabel, m_arrayCount);
    tl->addLayout(testOptions);
    m_chunkHint = new QLabel; m_chunkHint->setObjectName("protocolChunkHint"); m_chunkHint->setWordWrap(true); tl->addWidget(m_chunkHint);
    m_spaceHex = new QCheckBox("HEX 字节间空格"); m_spaceHex->setObjectName("protocolSpaceHex"); m_spaceHex->setChecked(true);
    m_spaceHex->setToolTip("仅改变 HEX 显示：系统每两位自动分隔，空格不属于数据。取消后连续显示，字节内容不变，不需要手动加空格。"); tl->addWidget(m_spaceHex);
    tl->addWidget(m_generateSample);
    m_input = new ProtocolSampleEdit; m_input->setObjectName("protocolSample"); m_input->setMinimumHeight(100); m_input->setMaximumHeight(180); tl->addWidget(m_input);
    m_input->setToolTip("输入待解析的原始报文，按上方样例格式解释。最多 256 KiB 原始字节；可包含多帧、半包或错误帧。测试只作用于所选测试协议，不会写入录制或向设备发送。");
    m_generateSample->setToolTip("为右侧选中协议生成一帧样例；仅帧头分隔时附加下一帧头作为结束边界。当前协议使用草稿，其他协议使用已保存版本。按类型/大小端编码、补长度/头尾/固定条件并计算校验，先用解析器验证。失败保留原样例；不修改协议、不保存、不发送。复杂冲突条件需手动构造。");
    m_test->setToolTip("用所选测试协议解析样例。当前编辑协议使用草稿，其他协议使用已保存版本；展示原始值、换算值和拒绝原因，不保存或应用到设备。");
    m_cancelTest->setToolTip("停止剩余样例处理，保留已经产生的测试结果；不影响实际设备接收、发送或录制。");
    row = new QHBoxLayout; row->addWidget(m_test); row->addWidget(m_cancelTest); tl->addLayout(row);
    m_stats = new QLabel("尚未测试"); m_stats->setObjectName("protocolStats"); m_stats->setTextFormat(Qt::PlainText); m_stats->setWordWrap(true);
    m_stats->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_stats->setTextInteractionFlags(Qt::TextSelectableByMouse); tl->addWidget(m_stats);
    m_stats->setToolTip("显示本次测试的协议、处理字节、有效帧、拒绝帧、校验失败及剩余半包。统计覆盖全部已处理输入，未处理完或剩余半包不代表完整有效帧。");
    m_results = new QTableWidget(0, 6); m_results->setObjectName("protocolResults"); m_results->setMinimumHeight(120); m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setHorizontalHeaderLabels({"帧", "字段名称", "单位", "原始数值", "换算后数值", "字段编号"}); m_results->setColumnHidden(5, true);
    m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); m_results->horizontalHeader()->setStretchLastSection(true); tl->addWidget(m_results, 1);
    const QStringList resultHints{"有效报文的帧序号，一帧中的不同通道共享此序号。", "已配置的通道名称；文本未配置列自动命名为ChN（N 为原列号 + 1）。", "配置的显示单位，不进行自动单位换算；文本未配置列无单位。", "从报文直接读取的数值，尚未应用系数和换算偏移。按来源精度简洁展示：float32 避免扩展到 double 后的长尾，文本/float64 保留双精度。悬停单元格查看完整精度。", "原始值 × 系数 + 换算偏移；文本未配置列保持原值。float32 按原始字段精度简洁展示，不修改真实换算值；悬停可查看完整精度。只有通过范围验证的有效帧才展示。", "通道的稳定标识；文本自动通道也有独立标识以保存显示设置。"};
    for (int i = 0; i < resultHints.size(); ++i) m_results->horizontalHeaderItem(i)->setToolTip(resultHints[i]);
    m_diagnostics = new ProtocolDiagnosticsEdit; m_diagnostics->setReadOnly(true); m_diagnostics->setMinimumHeight(90); m_diagnostics->setMaximumHeight(170); tl->addWidget(m_diagnostics);
    m_diagnostics->setPlaceholderText("错误与样例结束检查显示在这里；字节偏移从 0 开始，范围 [起点, 终点) 不包含终点。可选中复制。");
    m_diagnostics->setToolTip("红色显示最多 100 条诊断：样例绝对字节偏移从 0 开始，帧内位置相对本帧；预期/实际 HEX、通道/列号及建议。疑似原因会明确标注；样例结束时检查缺少帧头、结束符、字节和键值通道。实际接收仍等待半包，不自动补数据；可复制文本。");
    // All columns scroll independently; a long rule set cannot grow the
    // outer window or push the test controls off a short/high-DPI screen.
    auto previewScroll = new QScrollArea; previewScroll->setObjectName("protocolPreviewScroll"); previewScroll->setWidgetResizable(true); previewScroll->setWidget(testArea);
    split->addWidget(previewScroll); for (int i = 0; i < 3; ++i) split->setCollapsible(i, false);
    split->setStretchFactor(0, 0); split->setStretchFactor(1, 2); split->setStretchFactor(2, 1); split->setSizes({210, 700, 480});
    m_status = new QLabel; m_status->setObjectName("protocolSaveStatus"); m_status->setWordWrap(true); m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); root->addWidget(m_status);
    m_recoveryButton = new QPushButton("协议库加载失败：从有效备份恢复…"); m_recoveryButton->setObjectName("protocolRecoverBackup");
    m_recoveryButton->hide(); root->addWidget(m_recoveryButton);
    row = new QHBoxLayout; auto close = new QPushButton("关闭"); row->addStretch(); row->addWidget(close); root->addLayout(row);
    connect(help, &QPushButton::clicked, this, [this] { ProtocolUsageDialog dialog(this); dialog.exec(); });
    // No auto-default Enter action while a user is editing fields/samples.
    for (auto button : findChildren<QPushButton *>()) button->setAutoDefault(false);
    m_previewTimer = new QTimer(this); m_previewTimer->setInterval(0); connect(m_previewTimer, &QTimer::timeout, this, &ProtocolManagerDialog::previewStep);
    connect(m_test, &QPushButton::clicked, this, &ProtocolManagerDialog::startPreview); connect(m_cancelTest, &QPushButton::clicked, this, &ProtocolManagerDialog::stopPreview);
    connect(m_generateSample, &QPushButton::clicked, this, &ProtocolManagerDialog::generateSample);
    connect(m_inputMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ProtocolManagerDialog::changeInputMode);
    connect(m_spaceHex, &QCheckBox::toggled, m_input, &ProtocolSampleEdit::setSpacedHex);
    connect(m_chunkMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ProtocolManagerDialog::updateSampleControls);
    connect(m_input, &ProtocolSampleEdit::invalidInput, this, [this] {
        const QString error = "HEX 只接受 0–9、A–F（小写自动大写）；非法输入未插入。每字节两位，空格由系统处理，最多 256 KiB。";
        m_stats->setText(error); m_diagnostics->setPlainText("【输入错误】" + error);
    });
    updateSampleControls();
    connect(m_editor, &ProtocolEditorWidget::edited, this, [this] { m_dirty = true; syncDraftTitle(); m_status->setText("协议有未保存的修改；右侧可选择测试协议，当前编辑项使用草稿，其他项和导出使用已保存版本。"); if (m_testing) stopPreview(); updateSampleControls(); });
    connect(m_previewProtocol, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        syncSampleProtocol();
    });
    connect(m_list, &QListWidget::currentRowChanged, this, &ProtocolManagerDialog::selectRow);
    connect(create, &QPushButton::clicked, this, &ProtocolManagerDialog::chooseNewProfile);
    connect(save, &QPushButton::clicked, this, [this] { saveDraft(); }); connect(close, &QPushButton::clicked, this, &ProtocolManagerDialog::reject);
    connect(remove, &QPushButton::clicked, this, &ProtocolManagerDialog::deleteSelectedProfiles);
    connect(import, &QPushButton::clicked, this, &ProtocolManagerDialog::importFile);
    connect(exportSelected, &QPushButton::clicked, this, [this] { exportFile(1); }); connect(exportAll, &QPushButton::clicked, this, [this] { exportFile(2); });
    connect(m_recoveryButton, &QPushButton::clicked, this, &ProtocolManagerDialog::restoreBackup);
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"); QString error;
    if (!m_store.load(error, ProtocolProfileStore::legacySelections(main, advanced))) {
        m_status->setText("协议库加载失败：" + error + "。请恢复备份或处理原文件；未覆盖原文件。"); m_editor->setEnabled(false); updateEditorState(); m_recoveryButton->show();
        refreshPreviewProtocols(true);
    } else {
        // Opening the manager only selects an editor/test target. It never
        // changes the active parser or persists another acquisition choice.
        const auto selection = m_store.snapshot().shared;
        refresh(!selectedProfileId.isEmpty() ? selectedProfileId : selection.kind == "custom" ? selection.id : QString());
    }
}
void ProtocolManagerDialog::chooseNewProfile() {
    if (!m_store.ready()) return;
    QDialog dialog(this); dialog.setObjectName("protocolNewDialog"); dialog.setWindowTitle("新增协议 · 选择类型"); dialog.resize(520, 210);
    auto layout = new QVBoxLayout(&dialog); auto label = new QLabel("请选择要创建的协议类型："); layout->addWidget(label);
    auto type = new QComboBox; type->setObjectName("protocolNewType");
    for (const auto &kind : protocolKinds()) type->addItem(kind.name, kind.type); layout->addWidget(type);
    auto templateBox = new QComboBox; templateBox->setObjectName("protocolNewTemplate");
    templateBox->addItem("通用键值模板", false); templateBox->addItem("CBF/IR/RED 设备模板", true); layout->addWidget(templateBox);
    auto hint = new QLabel; hint->setWordWrap(true); layout->addWidget(hint, 1);
    auto updateHint = [type, hint, templateBox] { hint->setText(protocolKinds().at(type->currentIndex()).hint); templateBox->setVisible(protocolKinds().at(type->currentIndex()).type == 3); };
    connect(type, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, updateHint); updateHint();
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); buttons->setObjectName("protocolNewButtons");
    buttons->button(QDialogButtonBox::Ok)->setText("创建协议"); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto kind = protocolKinds().at(type->currentIndex()); newProfile(kind.type, kind.type == 3 && templateBox->currentData().toBool());
}
void ProtocolManagerDialog::copyCurrentProfile() {
    if (!leaveDraft() || m_currentId.isEmpty()) return; QString error, id;
    if (m_guard && !m_guard(m_store.snapshot(), error)) { report("复制失败", error); return; }
    if (!m_store.copyProfile(m_currentId, id, error)) report("复制失败", error); else refresh(id);
}
void ProtocolManagerDialog::renameCurrentProfile() {
    if (!leaveDraft() || m_currentId.isEmpty()) return;
    for (const auto &p : m_store.snapshot().profiles) if (p.id == m_currentId) {
        // Own a value before entering a nested dialog (saving rebuilds lists).
        auto definition = p.definition; bool ok = false;
        auto name = QInputDialog::getText(this, "重命名方案", "新名称", QLineEdit::Normal, p.name, &ok);
        if (ok) { definition.insert("name", name); QString id, error; if (!allowProfile(definition, error) || !m_store.saveProfile(definition, id, error)) report("重命名失败", error); else refresh(id); } break;
    }
}
void ProtocolManagerDialog::deleteSelectedProfiles() {
    QSet<QString> ids; QStringList names;
    for (auto item : m_list->selectedItems()) { ids.insert(item->data(Qt::UserRole).toString()); names << item->text(); }
    if (ids.isEmpty()) { m_status->setText("请先选择要删除的协议，可使用 Ctrl/Shift 多选。"); return; }
    if (!leaveDraft()) return;
    // Discarding a new draft already removed its temporary list item. Never
    // delete the old selection that becomes current after discarding it.
    QSet<QString> savedIds; names.clear();
    for (const auto &p : m_store.snapshot().profiles) if (ids.contains(p.id)) { savedIds.insert(p.id); names << p.name; }
    ids = savedIds;
    if (ids.isEmpty()) return;
    if (ask(this, "删除选中的协议", QString("确定删除以下 %1 个协议？").arg(ids.size()), false, names) != Confirm) return;
    QString error; auto candidate = m_store.snapshot();
    for (const auto &selection : {candidate.shared, candidate.ble}) if (selection.kind == "custom" && ids.contains(selection.id)) {
        report("删除失败", "所选协议是全软件当前使用的数据协议。请先切换到其他协议，再删除；本次没有删除任何协议。"); return;
    }
    for (int i = candidate.profiles.size() - 1; i >= 0; --i) if (ids.contains(candidate.profiles[i].id)) candidate.profiles.remove(i);
    // Validate and persist the whole deletion once: never partially delete
    // a multi-selection if one item is protected or saving fails.
    if ((m_guard && !m_guard(candidate, error)) || !m_store.commit(candidate, error)) report("删除失败", error);
    else { refresh(); m_status->setText(QString("已删除所选 %1 个协议，其他协议已保留。").arg(ids.size())); }
}
void ProtocolManagerDialog::moveCurrentProfile(int delta) {
    if (!leaveDraft() || m_currentId.isEmpty()) return; QString error;
    if ((m_guard && !m_guard(m_store.snapshot(), error)) || !m_store.moveProfile(m_currentId, delta, error)) m_status->setText(error); else refresh(m_currentId);
}
void ProtocolManagerDialog::showProfileDescription() {
    if (m_currentId.isEmpty()) return;
    const QString description = m_editor->findChild<QPlainTextEdit *>("protocolDescription")->toPlainText();
    report(m_editor->findChild<QLineEdit *>("protocolName")->text() + " · 协议说明",
           description.trimmed().isEmpty() ? "此协议尚未填写协议说明。可在中间“协议配置 → 协议说明”中填写并保存。" : description);
}
void ProtocolManagerDialog::restoreBackup() {
    if (!leaveDraft()) return;
    if (ask(this, "恢复协议库备份", "将协议库恢复到上一次有效保存的内容，而非出厂默认值。\n当前文件会另存保留，不影响任何录制文件。是否继续？", false) != Confirm) return;
    QString error; QByteArray bytes; ProtocolSnapshot candidate;
    if (!ProtocolProfileStore::readFile(m_store.path() + ".bak", bytes, error) || !ProtocolProfileValidator::decode(bytes, true, candidate, error) ||
        (m_guard && !m_guard(candidate, error)) || !m_store.restoreBackup(error)) report("恢复失败", error);
    else { m_recoveryButton->hide(); refresh(); m_status->setText("已恢复备份；恢复前文件已另存保留。"); }
}
void ProtocolManagerDialog::showListMenu(const QPoint &pos) {
    auto item = m_list->itemAt(pos); const QString id = item ? item->data(Qt::UserRole).toString() : QString();
    QStringList selectedIds;
    if (item && item->isSelected()) for (auto selected : m_list->selectedItems()) selectedIds << selected->data(Qt::UserRole).toString();
    if (item && id != m_currentId) {
        // Switching away from a draft must resolve Save/Discard/Cancel.
        selectRow(m_list->row(item));
        // A cancelled draft prompt must not act on a different protocol.
        if (m_currentId != id) return;
    }
    if (selectedIds.size() > 1) for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setSelected(selectedIds.contains(m_list->item(i)->data(Qt::UserRole).toString()));
    int row = -1;
    for (int i = 0; i < m_store.snapshot().profiles.size(); ++i) if (m_store.snapshot().profiles[i].id == id) row = i;
    const bool selected = row >= 0 && m_store.ready();
    QMenu menu(this); menu.setObjectName("protocolListMenu");
    auto action = [this](QMenu *target, const QString &text, const QString &name, bool enabled, std::function<void()> run) {
        auto a = target->addAction(text); a->setObjectName(name); a->setEnabled(enabled);
        connect(a, &QAction::triggered, this, [run] { run(); });
        return a;
    };
    auto create = menu.addMenu("新增协议"); create->setObjectName("protocolMenuNew"); create->setEnabled(m_store.ready());
    for (const auto &kind : protocolKinds()) action(create, kind.name, kind.action, m_store.ready(), [this, kind] { newProfile(kind.type); });
    auto templates = create->addMenu("新增模板协议"); templates->setObjectName("protocolMenuNewTemplates");
    action(templates, "CBF/IR/RED 设备模板", "protocolMenuNewCbf", m_store.ready(), [this] { newProfile(3, true); });
    action(&menu, "复制协议", "protocolMenuCopy", selected, [this] { copyCurrentProfile(); });
    action(&menu, "重命名", "protocolMenuRename", selected, [this] { renameCurrentProfile(); });
    const bool currentItem = !id.isEmpty() && id == m_currentId && m_store.ready();
    action(&menu, "删除所选协议", "protocolMenuDelete", currentItem, [this] { deleteSelectedProfiles(); });
    menu.addSeparator();
    action(&menu, "导出当前协议…", "protocolMenuExport", selected, [this] { exportFile(0); });
    action(&menu, "导入协议…", "protocolMenuImport", m_store.ready(), [this] { importFile(); });
    menu.addSeparator();
    action(&menu, "上移", "protocolMenuUp", selected && row > 0, [this] { moveCurrentProfile(-1); });
    action(&menu, "下移", "protocolMenuDown", selected && row + 1 < m_store.snapshot().profiles.size(), [this] { moveCurrentProfile(1); });
    menu.addSeparator(); action(&menu, "协议说明", "protocolMenuDescription", currentItem, [this] { showProfileDescription(); });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}
void ProtocolManagerDialog::report(const QString &title, const QString &message) {
    QDialog dialog(this); dialog.setObjectName("protocolInfoDialog"); dialog.setWindowTitle(title); dialog.setSizeGripEnabled(true); dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowMinMaxButtonsHint); dialog.resize(600, 300);
    auto layout = new QVBoxLayout(&dialog); auto text = new QPlainTextEdit; text->setObjectName("protocolInfoText"); text->setReadOnly(true); text->setPlainText(message); layout->addWidget(text);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok); connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); layout->addWidget(buttons); dialog.exec();
}
bool ProtocolManagerDialog::saveDraft() {
    ProtocolProfile p; QString error, id;
    if (!m_editor->isEnabled()) return false;
    // Ctrl+S must commit a spin-box/table editor that still has focus, just
    // like clicking Save does, before collecting and validating the values.
    findChild<QPushButton *>("protocolSave")->setFocus(Qt::OtherFocusReason);
    if (!m_editor->profile(p, error) || !allowProfile(p.definition, error) || !m_store.saveProfile(p.definition, id, error)) { report("保存失败", error); return false; }
    m_dirty = false; refresh(id); m_status->setText("协议已保存，重新打开后保留；没有改变当前设备连接或实时解析规则。"); return true;
}
void ProtocolManagerDialog::syncDraftTitle() {
    const QString name = m_editor->findChild<QLineEdit *>("protocolName")->text();
    const QString description = m_editor->findChild<QPlainTextEdit *>("protocolDescription")->toPlainText();
    for (int i = 0; i < m_list->count(); ++i) if (m_list->item(i)->data(Qt::UserRole).toString() == m_currentId) {
        auto item = m_list->item(i); item->setText(name.isEmpty() ? "未命名协议" : name);
        item->setToolTip(item->text() + (m_dirty ? "（未保存）" : "") + "\n" + description); break;
    }
    refreshPreviewProtocols(false);
}
void ProtocolManagerDialog::refreshPreviewProtocols(bool followCurrent) {
    const QString wanted = followCurrent ? m_currentId : m_previewProtocol->currentData().toString();
    QSignalBlocker blocker(m_previewProtocol); m_previewProtocol->clear();
    // Resolve IDs, not row numbers: copy/import/sorting and an unsaved draft
    // can all change list positions without changing the selected protocol.
    for (int row = 0; row < m_list->count(); ++row) {
        auto item = m_list->item(row); const QString id = item->data(Qt::UserRole).toString();
        m_previewProtocol->addItem(item->text() + (id == m_currentId ? "（当前编辑）" : ""), id);
        m_previewProtocol->setItemData(row, id == m_currentId ? "使用中间当前编辑的草稿，含未保存修改。" : "使用此协议的已保存版本，不切换中间编辑项。", Qt::ToolTipRole);
    }
    int index = m_previewProtocol->findData(wanted);
    if (index < 0) index = m_previewProtocol->findData(m_currentId);
    m_previewProtocol->setCurrentIndex(index >= 0 ? index : (m_previewProtocol->count() ? 0 : -1));
    m_previewProtocol->setEnabled(m_store.ready() && m_previewProtocol->count() && !m_testing);
    m_test->setEnabled(m_store.ready() && m_previewProtocol->count() && !m_testing);
    m_generateSample->setEnabled(m_store.ready() && m_previewProtocol->count() && !m_testing);
    syncSampleProtocol();
}
bool ProtocolManagerDialog::allowProfile(const QJsonObject &definition, QString &error) const {
    ProtocolProfile p;
    if (!ProtocolProfileValidator::profile(definition, p, error)) return false;
    if (!m_guard) return true;
    auto candidate = m_store.snapshot(); bool found = false;
    for (auto &old : candidate.profiles) if (old.id == p.id) { old = p; found = true; break; }
    if (!found) candidate.profiles.append(p);
    return m_guard(candidate, error);
}
bool ProtocolManagerDialog::leaveDraft() {
    if (!m_dirty) return true;
    auto answer = ask(this, "尚未保存的协议", "保存当前草稿后继续？\n放弃新协议会移除该草稿；放弃已有协议的修改会恢复保存版本。", true);
    if (answer == Cancel) return false;
    if (answer == Save) return saveDraft();
    m_dirty = false;
    if (m_newDraft) {
        const QString returnId = m_draftReturnId; refresh(returnId);
    } else if (!m_currentId.isEmpty()) {
        for (const auto &p : m_store.snapshot().profiles) if (p.id == m_currentId) { m_editor->setProfile(p); break; }
        syncDraftTitle();
    } else { m_editor->setEnabled(false); updateEditorState(); }
    return true;
}
void ProtocolManagerDialog::updateEditorState() {
    const bool editing = m_editor->isEnabled(), ready = m_store.ready();
    m_editor->setVisible(editing); m_welcome->setVisible(!editing);
    findChild<QPushButton *>("protocolSave")->setEnabled(editing);
    findChild<QPushButton *>("protocolWelcomeNew")->setEnabled(ready);
    findChild<QPushButton *>("protocolWelcomeImport")->setEnabled(ready);
    findChild<QLabel *>("protocolWelcomeSteps")->setVisible(ready);
    m_welcomeTitle->setText(ready ? "欢迎使用自定义协议" : "协议库暂不可用");
    m_welcomeText->setText(ready ? "这里还没有协议。请新增协议，按照设备的数据格式完成配置；已有的协议文件也可以直接导入。"
        : "已有协议库读取失败，原文件没有被覆盖。请先从下方恢复有效备份，或检查协议库文件。不要把读取失败当作首次使用。");
}
void ProtocolManagerDialog::refresh(const QString &selectId) {
    QStringList selectedIds;
    for (auto item : m_list->selectedItems()) selectedIds << item->data(Qt::UserRole).toString();
    stopPreview(); m_refreshing = true; m_list->clear(); int selected = -1;
    for (const auto &p : m_store.snapshot().profiles) {
        auto item = new QListWidgetItem(p.name, m_list); item->setData(Qt::UserRole, p.id); item->setToolTip(p.name + "\n" + p.description);
        item->setSizeHint(QSize(0, 44));
        if (p.id == selectId) selected = m_list->count() - 1;
    }
    m_currentId.clear(); m_draftReturnId.clear(); m_newDraft = false; m_lastRow = -1; m_dirty = false; m_refreshing = false;
    m_editor->setEnabled(m_store.ready() && m_list->count() > 0);
    updateEditorState();
    if (m_list->count()) {
        m_list->setCurrentRow(selected >= 0 ? selected : 0);
        if (selectedIds.size() > 1) for (int i = 0; i < m_list->count(); ++i)
            if (selectedIds.contains(m_list->item(i)->data(Qt::UserRole).toString())) m_list->item(i)->setSelected(true);
    }
    else m_status->setText("尚无协议。可新增协议，或导入协议文件。");
    refreshPreviewProtocols(true);
}
void ProtocolManagerDialog::selectRow(int row) {
    if (m_refreshing || row < 0) return;
    auto item = m_list->item(row); if (!item) return;
    const auto id = item->data(Qt::UserRole).toString();
    if (id == m_currentId) return;
    if (!leaveDraft()) { m_refreshing = true; m_list->setCurrentRow(m_lastRow); m_refreshing = false; return; }
    // Saving a draft may rebuild the list; resolve by ID after that rebuild.
    stopPreview(); m_currentId = id;
    for (const auto &p : m_store.snapshot().profiles) if (p.id == id) {
        m_editor->setEnabled(true); updateEditorState(); m_editor->setProfile(p); m_dirty = false;
        for (int i = 0; i < m_list->count(); ++i) if (m_list->item(i)->data(Qt::UserRole).toString() == id) {
            m_lastRow = i; m_refreshing = true; m_list->setCurrentRow(i); m_refreshing = false; break;
        }
        m_status->setText("已保存协议。修改后点击“保存协议”或按 Ctrl+S；解析测试使用当前编辑内容。"); break;
    }
    refreshPreviewProtocols(true);
}
void ProtocolManagerDialog::newProfile(int type, bool cbfTemplate) {
    const bool keyValue = type == 3, delimiterFramed = type == 4, binary = type == 1 || type == 2 || delimiterFramed, variable = type == 2 || delimiterFramed;
    if (!leaveDraft() || !m_store.ready()) return;
    if (m_store.snapshot().profiles.size() >= 128) { report("无法新增协议", "协议库已达到 128 个协议的上限，请先删除不再需要的协议。"); return; }
    stopPreview(); ProtocolProfile p; QString error;
    auto d = keyValue ? ProtocolProfileValidator::keyValueTemplate(cbfTemplate) : delimiterFramed ? ProtocolEditorWidget::delimitedArrayExample() : variable ? ProtocolEditorWidget::dynamicArrayExample() : ProtocolEditorWidget::example(binary);
    if (keyValue) d.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto channels = d.value("channels").toArray();
    for (int i = 0; i < channels.size(); ++i) {
        auto channel = channels[i].toObject(); if (!cbfTemplate) channel.insert("name", QStringLiteral("Ch%1").arg(i + 1));
        channel.insert("scale", 1); channel.insert("bias", 0); if (!cbfTemplate) channel.insert("unit", ""); channels[i] = channel;
    }
    if (binary) {
        if (!variable) d.insert("autoChannelLayout", true);
        d.insert("outOfRangePolicy", "nan-channel");
        d.insert("checksum", QJsonObject{{"algorithm", "none"}});
        auto frame = d.value("frame").toObject();
        if (!variable) frame.insert("sizeBytes", 10);
        d.insert("frame", frame);
        if (!variable) d.insert("description", "默认无校验、无缩放。样例 HEX：AA 55 E6 09 33 33 53 40 0D 0A（原始值 2534、约 3.3）。");
    }
    d.insert("channels", channels); // Only new defaults change, never existing/imported names.
    d.insert("name", keyValue ? cbfTemplate ? "CBF/IR/RED" : "多通道键值协议" : delimiterFramed ? "二进制首尾分隔协议" : variable ? "二进制长度字段帧协议" : binary ? "二进制固定长度帧协议" : "文本分隔协议");
    QString base = d.value("name").toString(), name = base; int suffix = 0;
    auto exists = [&](const QString &name) { for (const auto &p : m_store.snapshot().profiles) if (p.name == name) return true; return false; };
    while (exists(name)) name = base + QStringLiteral("_%1").arg(++suffix); d.insert("name", name);
    if (!ProtocolProfileValidator::profile(d, p, error)) { report("创建失败", error); return; }
    m_draftReturnId = m_currentId; m_currentId = p.id; m_newDraft = true;
    m_editor->setEnabled(true); updateEditorState(); m_editor->setProfile(p); m_dirty = true;
    m_refreshing = true;
    auto item = new QListWidgetItem(p.name, m_list); item->setData(Qt::UserRole, p.id); item->setSizeHint(QSize(0, 44));
    m_lastRow = m_list->count() - 1; m_list->setCurrentRow(m_lastRow, QItemSelectionModel::ClearAndSelect);
    m_list->scrollToItem(item); m_refreshing = false; syncDraftTitle(); refreshPreviewProtocols(true);
    updateSampleControls();
    m_status->setText("新协议尚未保存。可先修改字段并测试，确认后点击“保存协议”或按 Ctrl+S。");
}
bool ProtocolManagerDialog::importBytes(const QByteArray &bytes, ProtocolImportMode mode, bool accepted, QString &error) {
    ProtocolImportPreview preview;
    if (!m_store.ready()) { error = "协议库尚未就绪"; return false; }
    if (!ProtocolProfileStore::prepareImport(m_store.snapshot(), bytes, false, mode, false, preview, error)) return false;
    if (m_guard && !m_guard(preview.snapshot, error)) return false;
    if (!accepted) return true;
    if (!m_store.commit(preview.snapshot, error)) return false;
    refresh(preview.snapshot.profiles.isEmpty() ? QString() : preview.snapshot.profiles.last().id); return true;
}
void ProtocolManagerDialog::importFile() {
    if (!leaveDraft()) return;
    const auto path = QFileDialog::getOpenFileName(this, "导入自定义数据协议", {}, "协议方案 (*.json)"); if (path.isEmpty()) return;
    QByteArray bytes; QString error;
    if (!ProtocolProfileStore::readFile(path, bytes, error)) { report("导入失败", error); return; }
    QDialog dialog(this); dialog.setWindowTitle("协议导入预览"); dialog.resize(640, 380);
    auto layout = new QVBoxLayout(&dialog); auto mode = new QComboBox; mode->addItems({"追加为新方案（保留已有库）", "按 ID 更新/合并", "替换整个自定义协议库"}); layout->addWidget(mode);
    auto previewText = new QPlainTextEdit; previewText->setReadOnly(true); layout->addWidget(previewText);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText("确认导入"); layout->addWidget(buttons);
    auto update = [&, this] {
        ProtocolImportPreview p;
        const bool ok = m_store.ready() && ProtocolProfileStore::prepareImport(m_store.snapshot(), bytes, false, static_cast<ProtocolImportMode>(mode->currentIndex()), false, p, error) && (!m_guard || m_guard(p.snapshot, error));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
        QString text = ok ? QStringLiteral("新增 %1 个，更新 %2 个，移除旧库 %3 个。\n导入后不会自动应用到当前设备。\n\n导入后的协议列表：\n").arg(p.added).arg(p.updated).arg(p.removed) : error;
        if (ok) for (const auto &profile : p.snapshot.profiles) text += profile.name + "\n"; previewText->setPlainText(text);
    };
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, update); update();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (!importBytes(bytes, static_cast<ProtocolImportMode>(mode->currentIndex()), true, error)) report("导入失败", error);
    else m_status->setText("协议导入完成，已自动保存。");
}
void ProtocolManagerDialog::exportFile(int mode) {
    QStringList ids;
    if (mode == 0 && !m_currentId.isEmpty()) ids << m_currentId;
    else if (mode == 1) for (int i = 0; i < m_list->count(); ++i) if (m_list->item(i)->isSelected()) ids << m_list->item(i)->data(Qt::UserRole).toString();
    if (!leaveDraft()) return;
    for (int i = ids.size() - 1; i >= 0; --i) {
        bool saved = false; for (const auto &p : m_store.snapshot().profiles) if (p.id == ids[i]) { saved = true; break; }
        if (!saved) ids.removeAt(i);
    }
    if ((mode != 2 && ids.isEmpty()) || m_store.snapshot().profiles.isEmpty()) { m_status->setText("请先保存并选择要导出的方案。"); return; }
    const auto path = QFileDialog::getSaveFileName(this, "导出自定义协议", m_store.defaultExportFileName(ids), "自定义协议 (*.json)"); if (path.isEmpty()) return;
    QString error; if (!m_store.exportProfiles(path, ids, error)) report("导出失败", error); else m_status->setText("协议已导出：" + path);
}
bool ProtocolManagerDialog::previewProfile(ProtocolProfile &p, QString &error) const {
    const QString id = m_previewProtocol->currentData().toString(); bool ready = false;
    if (!id.isEmpty() && id == m_currentId && m_editor->isEnabled()) ready = m_editor->profile(p, error);
    else if (!id.isEmpty()) for (const auto &saved : m_store.snapshot().profiles) if (saved.id == id) { p = saved; ready = true; break; }
    if (!ready && error.isEmpty()) error = "请先新建协议或在右侧选择可用的测试协议。";
    return ready;
}
void ProtocolManagerDialog::updateSampleControls() {
    const bool hex = m_inputMode->currentIndex() == 1;
    m_spaceHex->setVisible(hex);
    bool array = false; ProtocolProfile selected; QString error;
    if (previewProfile(selected, error)) {
        array = selected.isDynamicArray();
        if (array) { const auto layout = selected.definition.value("array").toObject(); m_arrayCount->setRange(layout.value("minChannels").toInt(), layout.value("maxChannels").toInt()); }
    } else if (m_previewProtocol->currentData().toString() == m_currentId && m_editor->isEnabled()) array = m_editor->isDynamicArrayProtocol();
    m_arrayCount->setVisible(array); m_arrayCountLabel->setVisible(array); m_arrayCount->setEnabled(!m_testing);
    m_input->setPlaceholderText(hex ? "HEX 字节：每字节两位，自动大写/分隔。\n点击“根据协议生成样例”生成所选协议的有效报文。"
        : binarySampleProtocol() ? "二进制文本：使用 \\xHH 逐字节表示，如 \\xAA\\x55。\n点击“根据协议生成样例”；格式切换保留相同字节。"
        : "文本：按当前协议的前后缀、分隔符和结束符填写，转义符生效。\n点击“根据协议生成样例”；格式切换保留相同字节。");
    const QStringList hints{"整块输入：一次最多 4096 字节。", "逐字节：每次 1 字节，验证半包能否逐步组帧。", "随机分包：每次 1–31 字节，验证不同拆包边界。"};
    m_chunkHint->setText(hints.value(m_chunkMode->currentIndex()) + "正常情况下解析结果应相同；完成后显示实际分包次数和大小。仅模拟接收，不发送数据。");
}
bool ProtocolManagerDialog::binarySampleProtocol() const {
    const auto id = m_previewProtocol->currentData().toString();
    // Formatting must work even while an unrelated draft field is invalid.
    // Follow the right-hand test selection, not always the middle editor.
    if (!id.isEmpty() && id == m_currentId && m_editor->isEnabled()) return m_editor->isBinaryProtocol();
    for (const auto &profile : m_store.snapshot().profiles)
        if (profile.id == id) return profile.definition.value("type") == QJsonValue("binary");
    return false;
}
void ProtocolManagerDialog::syncSampleProtocol() {
    const auto id = m_previewProtocol->currentData().toString(); QString kind;
    if (!id.isEmpty() && id == m_currentId && m_editor->isEnabled()) {
        kind = m_editor->findChild<QComboBox *>("protocolType")->currentData().toString();
        if (kind == "length") kind += m_editor->isDynamicArrayProtocol() ? ".array" : ".fields";
    } else for (const auto &p : m_store.snapshot().profiles) if (p.id == id) {
        kind = p.isKeyValue() ? "keyvalue" : p.definition.value("type") == QJsonValue("text") ? "text" : p.definition.value("frame").toObject().value("mode").toString();
        if (kind == "length") kind += p.isDynamicArray() ? ".array" : ".fields";
        break;
    }
    const auto context = id + "|" + kind;
    if (context == m_sampleProtocolContext) { updateSampleControls(); return; }
    m_sampleProtocolContext = context; stopPreview(); m_results->setRowCount(0); m_diagnostics->clear();
    const bool binary = binarySampleProtocol();
    { const QSignalBlocker block(m_inputMode); m_inputMode->setCurrentIndex(binary ? 1 : 0); }
    // A protocol change is not a format conversion: do not reinterpret the
    // previous protocol's text (or an incomplete HEX byte) as a new sample.
    m_inputRepresentation = binary ? 1 : 0; m_input->setHexMode(false); m_input->clear(); m_input->setHexMode(binary);
    updateSampleControls(); ProtocolProfile selected; QByteArray bytes; QString error;
    if (id.isEmpty()) { m_stats->setText("请先新增或选择测试协议。"); return; }
    if (!previewProfile(selected, error) || !ProtocolSampleGenerator::generate(selected, bytes, error, selected.isDynamicArray() ? m_arrayCount->value() : 0)) {
        m_stats->setText("已切换为" + QString(binary ? " HEX 字节" : "文本") + "格式。当前配置无法自动生成样例：" + error + "\n修正配置后可点击“根据协议生成样例”，或手动输入报文。"); return;
    }
    m_input->setPlainText(ProtocolSampleEdit::displayBytes(bytes, binary, m_spaceHex->isChecked(), binary));
    m_stats->setText(QString("已按“%1”自动选择%2格式，生成一帧 %3 字节样例并验证。可手动切换格式或编辑样例；不会向设备发送。")
        .arg(selected.name, binary ? " HEX 字节" : "文本").arg(bytes.size()));
}
void ProtocolManagerDialog::changeInputMode(int mode) {
    if (mode == m_inputRepresentation) { updateSampleControls(); return; }
    QByteArray bytes; QString error;
    const bool valid = m_inputRepresentation == 1 ? ProtocolProfileValidator::parseHex(m_input->toPlainText(), bytes)
        : ProtocolSampleEdit::textBytes(m_input->toPlainText(), bytes, error);
    if (!valid) {
        const QSignalBlocker block(m_inputMode); m_inputMode->setCurrentIndex(m_inputRepresentation);
        m_stats->setText("无法转换样例格式：" + (error.isEmpty() ? "HEX 每字节需要两位，请先补全半个字节。原输入未改变。" : error)); return;
    }
    m_input->setHexMode(false); m_input->setPlainText(ProtocolSampleEdit::displayBytes(bytes, mode == 1, m_spaceHex->isChecked(), binarySampleProtocol()));
    m_input->setHexMode(mode == 1); m_inputRepresentation = mode; updateSampleControls();
}
void ProtocolManagerDialog::generateSample() {
    m_generateSample->setFocus(Qt::OtherFocusReason); ProtocolProfile profile; QByteArray bytes; QString error;
    if (!previewProfile(profile, error) || !ProtocolSampleGenerator::generate(profile, bytes, error, profile.isDynamicArray() ? m_arrayCount->value() : 0)) { report("无法生成测试样例", error); return; }
    stopPreview(); m_results->setRowCount(0); m_diagnostics->clear();
    m_input->setPlainText(ProtocolSampleEdit::displayBytes(bytes, m_inputRepresentation == 1, m_spaceHex->isChecked(), profile.definition.value("type") == QJsonValue("binary")));
    m_stats->setText(QString("已为“%1”生成一帧 %2 字节样例，并通过当前规则验证。可点击“解析测试”；不会向设备发送，也不证明真实设备协议正确。").arg(profile.name).arg(bytes.size()));
}
void ProtocolManagerDialog::startPreview() {
    stopPreview(); ProtocolProfile p; QString error;
    auto fail = [this](const QString &title, const QString &message) {
        m_results->setRowCount(0); m_diagnostics->setPlainText("【输入/配置错误】" + title + "：" + message);
        m_stats->setText("测试未开始：" + title + "，详见下方红色诊断。"); report(title, message);
    };
    // Commit a table/spin editor before reading a current draft snapshot.
    m_test->setFocus(Qt::OtherFocusReason);
    if (!previewProfile(p, error)) { fail("无法解析测试", error); return; }
    const auto input = m_input->toPlainText();
    if (input.size() > 4 * 256 * 1024 + 64) { fail("样例超限", "样例输入超过限额，请缩小至 256 KiB 字节数据。"); return; }
    QByteArray bytes;
    if (m_inputMode->currentIndex() == 1) {
        if (!ProtocolProfileValidator::parseHex(input, bytes)) { fail("HEX 格式无效", "仅接受十六进制字节和空白；每个字节需两个 HEX 字符，请补全末尾半个字节。"); return; }
    } else {
        if (!ProtocolSampleEdit::textBytes(input, bytes, error)) { fail("文本样例无效", error); return; }
    }
    if (bytes.isEmpty() || bytes.size() > 256 * 1024) { fail("样例超限或为空", "请输入 1–256 KiB 的原始数据。"); return; }
    m_parser.reset(qobject_cast<CustomProtocolParser *>(ProtocolParserFactory::createCustom(p, nullptr, error)));
    if (!m_parser) { fail("创建解析器失败", error); return; }
    m_testProfile = p; m_testBytes = bytes; m_buffer.clear(); m_position = 0; m_random = 0x57a1; m_diagnosticCount = 0;
    m_packetCount = m_minChunk = m_maxChunk = 0;
    m_results->setRowCount(0); m_diagnostics->clear();
    m_stats->setText("测试协议：" + p.name + "\n正在解析样例…");
    connect(m_parser.get(), &CustomProtocolParser::frameDecoded, this, [this](const QVector<double> &raw, const QVector<double> &values) {
        const auto channels = m_testProfile.outputChannels(raw.size());
        for (int i = 0; i < raw.size() && m_results->rowCount() < 1000; ++i) {
            auto channel = channels[i].toObject();
            if (m_testProfile.isDynamicArray()) channel.insert("type", m_testProfile.definition.value("array").toObject().value("type"));
            int row = m_results->rowCount(); m_results->insertRow(row);
            // Display follows the source field's precision, not the double
            // container used by the parser. Full values remain in tooltips.
            const QStringList data{QString::number(m_parser->stats().validFrames), channel.value("name").toString(), channel.value("unit").toString(),
                previewNumber(raw[i], raw[i], channel, false), previewNumber(values[i], raw[i], channel, true), channel.value("id").toString()};
            for (int col = 0; col < data.size(); ++col) {
                auto item = new QTableWidgetItem(data[col]);
                if (col == 3 || col == 4) item->setToolTip("完整精度：" + QString::number(col == 3 ? raw[i] : values[i], 'g', 17)
                    + (channel.value("type") == QJsonValue("float32") ? "\nfloat32 字段按原始有效精度简洁展示；实际解析和录制数值未舍入。" : QString()));
                if ((col == 3 || col == 4) && std::isnan(raw[i])) {
                    item->setToolTip("设备发送了 NaN。此通道保留缺失采样点，同帧其他通道继续解析。");
                } else if (col == 4 && std::isnan(values[i])) {
                    item->setToolTip("换算结果超出此通道的最小/最大范围，已标记为 NaN。此帧其他通道仍保留，可在左侧查看原始数值。");
                }
                m_results->setItem(row, col, item);
            }
        }
    });
    connect(m_parser.get(), &CustomProtocolParser::frameRejected, this, [this](quint64 offset, const QString &reason) {
        if (m_diagnosticCount >= 100) return;
        const int start = qMax(0, int(qMin(offset, quint64(m_testBytes.size()))) - 4);
        const auto context = m_testBytes.mid(start, 16);
        appendDiagnostic(QStringLiteral("【解析错误】字节偏移 %1：%2\n  附近输入 [%3, %4) HEX：%5")
            .arg(offset).arg(reason).arg(start).arg(start + context.size()).arg(QString::fromLatin1(context.toHex(' ').toUpper())));
    });
    m_testing = true; m_test->setEnabled(false); m_cancelTest->setEnabled(true); m_cancelTest->hide();
    const quint64 epoch = ++m_previewEpoch;
    // Small samples finish immediately: avoid briefly flashing a useless
    // stop button. A delayed callback must not belong to a newer test run.
    QTimer::singleShot(100, this, [this, epoch] { if (m_testing && epoch == m_previewEpoch) m_cancelTest->show(); });
    m_input->setEnabled(false); m_inputMode->setEnabled(false); m_chunkMode->setEnabled(false); m_previewProtocol->setEnabled(false); m_generateSample->setEnabled(false); m_spaceHex->setEnabled(false); m_previewTimer->start();
    m_arrayCount->setEnabled(false);
}
void ProtocolManagerDialog::appendDiagnostic(const QString &message) {
    if (m_diagnosticCount < 100) { ++m_diagnosticCount; m_diagnostics->appendPlainText(message); }
}
void ProtocolManagerDialog::previewStep() {
    if (!m_testing || !m_parser) return;
    QElapsedTimer budget; budget.start();
    do {
        if (m_position >= m_testBytes.size()) break;
        m_random = m_random * 1664525u + 1013904223u;
        const int requested = m_chunkMode->currentIndex() == 1 ? 1 : m_chunkMode->currentIndex() == 2 ? int(m_random % 31) + 1 : 4096;
        const int chunk = qMin(requested, m_testBytes.size() - m_position);
        ++m_packetCount; m_minChunk = m_minChunk ? qMin(m_minChunk, chunk) : chunk; m_maxChunk = qMax(m_maxChunk, chunk);
        m_buffer += m_testBytes.mid(m_position, chunk); m_position += chunk;
        const int consumed = m_parser->processData(m_buffer.constData(), m_buffer.size()); m_buffer.remove(0, consumed);
    } while (budget.elapsed() < 8);
    const auto s = m_parser->stats();
    m_stats->setText("测试协议：" + m_testProfile.name + "\n" + QStringLiteral("输入 %1/%2 字节 · 有效 %3 帧 · 拒绝 %4 帧 · 校验失败 %5 · 丢弃 %6 字节 · 剩余半包 %7 字节 · 超限标记 %8 个值\n表格最多显示 1000 个字段结果，诊断最多 100 条；统计覆盖全部已处理输入。").arg(m_position).arg(m_testBytes.size()).arg(s.validFrames).arg(s.rejectedFrames).arg(s.checksumFailures).arg(s.discardedBytes).arg(m_buffer.size()).arg(s.outOfRangeValues));
    m_stats->setText(m_stats->text() + QString("\n模拟分包：%1 · %2 次 · 每包 %3–%4 字节（不是发送次数）").arg(m_chunkMode->currentText()).arg(m_packetCount).arg(m_minChunk).arg(m_maxChunk));
    if (m_position >= m_testBytes.size()) {
        const auto finalDiagnostics = m_parser->testInputDiagnostics(m_testBytes, m_buffer);
        for (const auto &message : finalDiagnostics) appendDiagnostic("【样例结束检查】" + message);
        QByteArray nextHeader;
        const auto frame = m_testProfile.definition.value("frame").toObject();
        if (m_testProfile.isDelimiterFramed() && frame.value("tailHex").toString().isEmpty())
            ProtocolProfileValidator::parseHex(frame.value("headerHex").toString(), nextHeader);
        const bool waitingHeaderOnly = s.validFrames && !nextHeader.isEmpty() && m_buffer == nextHeader;
        if (waitingHeaderOnly) m_stats->setText(m_stats->text() + "\n仅帧头模式：末尾下一帧头已保留，等待下一帧数据（正常边界，不是错误）。");
        const bool issues = s.rejectedFrames || s.discardedBytes || (!m_buffer.isEmpty() && !waitingHeaderOnly) || !finalDiagnostics.isEmpty();
        m_stats->setText(m_stats->text() + (s.validFrames == 0 ? "\n结论：未得到完整有效帧，请查看下方红色诊断。"
            : issues ? "\n结论：存在异常，已保留有效结果；请查看下方红色诊断。" : "\n结论：全部样例解析完成，未发现异常。"));
        m_previewTimer->stop(); m_testing = false; m_test->setEnabled(true); m_cancelTest->setEnabled(false); m_cancelTest->hide();
        m_input->setEnabled(true); m_inputMode->setEnabled(true); m_chunkMode->setEnabled(true);
        m_previewProtocol->setEnabled(true); m_generateSample->setEnabled(true); m_spaceHex->setEnabled(true);
        m_arrayCount->setEnabled(true);
    }
}
void ProtocolManagerDialog::stopPreview() {
    if (m_arrayCount) m_arrayCount->setEnabled(true);
    if (m_previewTimer) m_previewTimer->stop();
    if (m_testing) { m_testing = false; m_stats->setText(m_stats->text() + "\n测试已停止。"); }
    if (m_test) m_test->setEnabled(m_store.ready() && m_previewProtocol && m_previewProtocol->count()); if (m_cancelTest) { m_cancelTest->setEnabled(false); m_cancelTest->hide(); }
    if (m_input) m_input->setEnabled(true); if (m_inputMode) m_inputMode->setEnabled(true); if (m_chunkMode) m_chunkMode->setEnabled(true);
    if (m_previewProtocol) m_previewProtocol->setEnabled(m_store.ready() && m_previewProtocol->count()); if (m_generateSample) m_generateSample->setEnabled(m_store.ready() && m_previewProtocol->count()); if (m_spaceHex) m_spaceHex->setEnabled(true);
}
void ProtocolManagerDialog::closeEvent(QCloseEvent *event) {
    if (!leaveDraft()) { event->ignore(); return; } stopPreview(); event->accept();
}
void ProtocolManagerDialog::reject() {
    if (!leaveDraft()) return; stopPreview(); QDialog::reject();
}
