#include "plotmeasurementcontroller.h"
#include "waveformgraph.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMenu>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QDoubleValidator>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QSemaphore>
#include <QSettings>
#include <QStyle>
#include <QtConcurrent>
#include <cmath>
#include <algorithm>

namespace {
struct Metric { const char *id; const char *name; const char *unit; const char *hint; };
const Metric metrics[] = {
    {"minimum", "最小值", "", "所选范围内有效采样点的最小值。"},
    {"maximum", "最大值", "", "所选范围内有效采样点的最大值。"},
    {"peakToPeak", "峰峰值", "", "最大值减去最小值。"},
    {"mean", "平均值", "", "有效采样点的算术平均值。"},
    {"rms", "有效值 RMS", "", "有效采样点平方平均值的平方根，包含直流分量。"},
    {"standardDeviation", "标准差", "", "总体标准差，分母为有效采样点数 N。"},
    {"frequency", "频率", "Hz", "完整上升沿周期的中位数之倒数；至少两个稳定完整周期，每周期至少 8 个采样间隔。"},
    {"period", "周期", "s", "相邻上升沿中参考电平交点间隔的中位数。"},
    {"positiveDuty", "正占空比", "%", "完整周期内高电平持续时间之和 / 周期时间之和。"},
    {"negativeDuty", "负占空比", "%", "100% 减去正占空比。"},
    {"positiveWidth", "正脉宽", "s", "上升沿至下降沿中参考电平交点间隔的中位数，可测量单个完整脉冲。"},
    {"negativeWidth", "负脉宽", "s", "下降沿至下一上升沿中参考电平交点间隔的中位数。"},
    {"riseTime", "上升时间", "s", "从低参考电平上升到高参考电平，默认 10%–90%。单采样间隔内的跳变显示 --。"},
    {"fallTime", "下降时间", "s", "从高参考电平下降到低参考电平，默认 90%–10%。单采样间隔内的跳变显示 --。"}
};
double metricValue(const ScopeMeasurements::Result &r, const QString &id) {
    if (id == "minimum") return r.minimum; if (id == "maximum") return r.maximum;
    if (id == "peakToPeak") return r.peakToPeak; if (id == "mean") return r.mean;
    if (id == "rms") return r.rms; if (id == "standardDeviation") return r.standardDeviation;
    if (id == "frequency") return r.frequency; if (id == "period") return r.period;
    if (id == "positiveDuty") return r.positiveDuty; if (id == "negativeDuty") return r.negativeDuty;
    if (id == "positiveWidth") return r.positiveWidth; if (id == "negativeWidth") return r.negativeWidth;
    if (id == "riseTime") return r.riseTime; if (id == "fallTime") return r.fallTime;
    return ScopeMeasurements::unavailable();
}
QString number(double value, const QString &unit = {}) {
    if (!std::isfinite(value)) return QStringLiteral("--");
    return QString::number(value == 0 ? 0 : value, 'g', 8) + (unit.isEmpty() ? QString() : " " + unit);
}
QSemaphore &analysisSlots() { static QSemaphore permits(2); return permits; }
constexpr int MaximumSamples = 2000000;
}

PlotMeasurementController::PlotMeasurementController(QCustomPlot *plot, QWidget *host, std::function<QString(int)> unitResolver, std::function<double()> timeResolver)
    : QObject(plot), m_plot(plot), m_unitResolver(std::move(unitResolver)), m_timeResolver(std::move(timeResolver)) {
    setObjectName("plotMeasurementController");
    m_originalMinimumHeight = plot->minimumHeight();
    m_panel = new QWidget(host); m_panel->setObjectName("plotMeasurementPanel");
    m_panel->setMinimumWidth(0);
    auto layout = new QVBoxLayout(m_panel); layout->setContentsMargins(4, 2, 4, 2); layout->setSpacing(2);
    auto header = new QHBoxLayout(); header->setSpacing(4); layout->addLayout(header);
    m_fold = new QToolButton(); m_fold->setObjectName("scopeFold"); m_fold->setText("测量");
    m_fold->setCheckable(true); m_fold->setChecked(true); m_fold->setArrowType(Qt::DownArrow);
    m_fold->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); header->addWidget(m_fold);
    m_channels = new QComboBox(); m_channels->setObjectName("scopeChannel"); m_channels->setMinimumWidth(0);
    m_channels->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_channels->setToolTip("测量通道：仅列出本视图中显示的波形，按通道身份区分同名曲线。"); header->addWidget(m_channels, 1);
    m_mode = new QComboBox(); m_mode->setObjectName("scopeCursorMode");
    m_mode->addItems({"关闭光标", "时间光标", "幅值光标", "时间＋幅值"}); m_mode->setCurrentIndex(TimeCursors);
    m_mode->setToolTip("拖动 A/B 竖线测量时间，拖动 Y1/Y2 横线测量幅值；也可在设置中精确输入位置。"); header->addWidget(m_mode);
    auto settings = new QToolButton(); settings->setText("设置"); settings->setObjectName("scopeSettings"); header->addWidget(settings);
    auto close = new QToolButton(); close->setText("×"); close->setObjectName("scopeClose"); close->setToolTip("关闭本视图测量"); header->addWidget(close);
    m_details = new QWidget(); auto body = new QVBoxLayout(m_details); body->setContentsMargins(0, 0, 0, 0); body->setSpacing(2); layout->addWidget(m_details);
    auto controls = new QHBoxLayout(); controls->setSpacing(4); body->addLayout(controls);
    controls->addWidget(new QLabel("范围："));
    m_range = new QComboBox(); m_range->setObjectName("scopeRange"); m_range->addItems({"当前可见范围", "A—B 区间"});
    m_range->setToolTip("仅使用范围内当前保留的实际采样点；滤波开启时测量当前滤波后的波形。");
    m_range->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); controls->addWidget(m_range, 1);
    m_snap = new QCheckBox("吸附采样点"); m_snap->setObjectName("scopeSnap"); m_snap->setChecked(true); controls->addWidget(m_snap);
    auto reset = new QToolButton(); reset->setText("重置光标"); reset->setObjectName("scopeReset"); controls->addWidget(reset);
    auto copy = new QToolButton(); copy->setText("复制结果"); copy->setObjectName("scopeCopy"); controls->addWidget(copy); controls->addStretch();
    m_results = new QTableWidget(); m_results->setObjectName("scopeResults"); m_results->setColumnCount(4);
    m_results->setHorizontalHeaderLabels({"测量项", "数值", "测量项", "数值"});
    m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_results->verticalHeader()->hide();
    m_results->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_results->setWordWrap(false); body->addWidget(m_results);
    m_resultsLayoutTimer.setSingleShot(true);
    connect(&m_resultsLayoutTimer, &QTimer::timeout, this, &PlotMeasurementController::updateResultsGeometry);
    m_results->installEventFilter(this); m_results->horizontalHeader()->installEventFilter(this);
    m_resultsLayoutTimer.start(0);
    m_status = new QLabel("暂无数据"); m_status->setObjectName("scopeStatus"); m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); m_status->setMaximumHeight(38); body->addWidget(m_status);
    m_projects = {"peakToPeak", "mean", "rms", "frequency", "period", "positiveDuty"};
    m_gate = new QCPItemRect(plot); m_gate->setPen(Qt::NoPen); m_gate->setBrush(QColor(50, 150, 240, 24)); m_gate->setLayer("grid"); m_gate->setVisible(false);
    m_gate->topLeft->setTypeX(QCPItemPosition::ptPlotCoords); m_gate->topLeft->setTypeY(QCPItemPosition::ptAxisRectRatio);
    m_gate->bottomRight->setTypeX(QCPItemPosition::ptPlotCoords); m_gate->bottomRight->setTypeY(QCPItemPosition::ptAxisRectRatio);
    for (int i = 0; i < 2; ++i) {
        const QColor color = i ? QColor("#e065d7") : QColor("#299fda");
        m_timeLines[i] = new QCPItemLine(plot); m_amplitudeLines[i] = new QCPItemLine(plot);
        m_timeLines[i]->setObjectName(i ? "scopeTimeCursorB" : "scopeTimeCursorA");
        m_amplitudeLines[i]->setObjectName(i ? "scopeAmplitudeCursorB" : "scopeAmplitudeCursorA");
        for (auto line : {m_timeLines[i], m_amplitudeLines[i]}) { line->setPen(QPen(color, 1, Qt::DashLine)); line->setVisible(false); line->setSelectable(false); }
        m_timeLines[i]->start->setTypeX(QCPItemPosition::ptPlotCoords); m_timeLines[i]->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
        m_timeLines[i]->end->setTypeX(QCPItemPosition::ptPlotCoords); m_timeLines[i]->end->setTypeY(QCPItemPosition::ptAxisRectRatio);
        m_amplitudeLines[i]->start->setTypeX(QCPItemPosition::ptAxisRectRatio); m_amplitudeLines[i]->start->setTypeY(QCPItemPosition::ptPlotCoords);
        m_amplitudeLines[i]->end->setTypeX(QCPItemPosition::ptAxisRectRatio); m_amplitudeLines[i]->end->setTypeY(QCPItemPosition::ptPlotCoords);
        m_timeLabels[i] = new QCPItemText(plot); m_amplitudeLabels[i] = new QCPItemText(plot);
        m_timeLabels[i]->setText(i ? "B" : "A"); m_amplitudeLabels[i]->setText(i ? "Y2" : "Y1");
        for (auto text : {m_timeLabels[i], m_amplitudeLabels[i]}) {
            text->setColor(Qt::white); text->setBrush(color.darker(160)); text->setPadding(QMargins(3, 1, 3, 1));
            text->setFont(QFont("Arial", 9, QFont::Bold)); text->setPositionAlignment(Qt::AlignTop | Qt::AlignLeft);
            text->position->setType(QCPItemPosition::ptAbsolute); text->setVisible(false); text->setSelectable(false);
        }
    }
    m_panel->hide(); plot->installEventFilter(this);
    connect(plot, &QCustomPlot::afterLayout, this, &PlotMeasurementController::layoutLabels);
    connect(m_channels, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (m_loading) return; m_selectedChannel = m_channels->currentData().isValid() ? m_channels->currentData().toInt() : -1;
        m_source.clear(); m_initialized = false; invalidate(); syncChannels(); savePreferences(); refresh();
    });
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { if (!m_loading) {
        if (m_range->currentIndex() == 1 && !hasTimeCursors()) m_range->setCurrentIndex(0);
        invalidate(); updateCursors(); render(); savePreferences();
    } });
    connect(m_range, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_loading) return;
        if (index == 1 && !hasTimeCursors()) m_mode->setCurrentIndex(hasAmplitudeCursors() ? BothCursors : TimeCursors);
        invalidate(); updateCursors(); savePreferences(); refresh();
    });
    connect(m_snap, &QCheckBox::toggled, this, [this] { if (!m_loading) { if (m_initialized) for (auto &time : m_time) time = snapTime(time); invalidate(); updateCursors(); render(); savePreferences(); } });
    connect(m_fold, &QToolButton::toggled, this, [this](bool expanded) { m_details->setVisible(expanded); m_fold->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow); savePreferences(); });
    connect(close, &QToolButton::clicked, this, [this] { setEnabled(false); });
    connect(reset, &QToolButton::clicked, this, &PlotMeasurementController::resetCursors);
    connect(settings, &QToolButton::clicked, this, [this, settings] { QMenu menu; menu.setObjectName("scopeSettingsMenu"); appendMenu(&menu); menu.exec(settings->mapToGlobal(QPoint(0, settings->height()))); });
    connect(copy, &QToolButton::clicked, this, [this] {
        QString text = "通道\t" + m_channels->currentText() + "\n范围\t" + m_range->currentText() + "\n";
        for (int row = 0; row < m_results->rowCount(); ++row) for (int col : {0, 2}) {
            auto name = m_results->item(row, col), value = m_results->item(row, col + 1);
            if (name && value && !name->text().isEmpty()) text += name->text() + "\t" + value->text() + "\n";
        }
        text += m_status->toolTip(); QApplication::clipboard()->setText(text);
    });
    m_timer.setInterval(250); connect(&m_timer, &QTimer::timeout, this, &PlotMeasurementController::refresh);
    connect(&m_watcher, &QFutureWatcher<ScopeMeasurements::Result>::finished, this, [this] {
        if (m_workVersion == m_version && m_enabled) {
            m_result = m_watcher.result(); m_resultLower = m_workLower; m_resultUpper = m_workUpper;
            m_problem.clear(); render(); emit measurementsUpdated();
        }
    });
}

bool PlotMeasurementController::hasTimeCursors() const { return m_mode->currentIndex() == TimeCursors || m_mode->currentIndex() == BothCursors; }
bool PlotMeasurementController::hasAmplitudeCursors() const { return m_mode->currentIndex() == AmplitudeCursors || m_mode->currentIndex() == BothCursors; }

void PlotMeasurementController::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled; m_panel->setVisible(enabled); m_drag = -1;
    m_plot->setMinimumHeight(enabled ? qMax(160, m_originalMinimumHeight) : m_originalMinimumHeight);
    if (QWidget::mouseGrabber() == m_plot) m_plot->releaseMouse();
    if (enabled) { emit activated(); m_timer.start(); syncChannels(); initializeCursors(); }
    else { m_timer.stop(); ++m_version; m_stampValid = false; }
    updateCursors(); savePreferences(); emit visibilityChanged(enabled);
    if (enabled) refresh();
}
void PlotMeasurementController::invalidate() {
    ++m_version; m_stampValid = false; m_result = {}; m_problem.clear();
    render();
}
void PlotMeasurementController::syncChannels() {
    if (!m_plot) return;
    QList<WaveformGraph *> available;
    for (int i = 0; i < m_plot->graphCount(); ++i) {
        auto graph = dynamic_cast<WaveformGraph *>(m_plot->graph(i));
        if (graph && graph->visible()) available.append(graph);
    }
    bool same = available.size() == m_channels->count();
    QStringList labels;
    for (auto graph : available) {
        int duplicates = 0; for (auto other : available) if (other->name() == graph->name()) ++duplicates;
        labels.append(graph->name() + (duplicates > 1 ? QString(" [Ch%1]").arg(graph->channelIndex() + 1) : QString()));
    }
    if (same) for (int i = 0; i < available.size(); ++i)
        if (m_channels->itemData(i).toInt() != available[i]->channelIndex() || m_channels->itemText(i) != labels[i]) { same = false; break; }
    if (!same) {
        const QSignalBlocker blocker(m_channels); m_channels->clear();
        for (int i = 0; i < available.size(); ++i) {
            m_channels->addItem(labels[i], available[i]->channelIndex());
            m_channels->setItemData(m_channels->count() - 1, labels[i], Qt::ToolTipRole);
        }
        const int restored = m_channels->findData(m_selectedChannel);
        m_channels->setCurrentIndex(restored >= 0 ? restored : available.isEmpty() ? -1 : 0);
    }
    auto source = m_channels->currentIndex() < 0 ? nullptr : WaveformGraph::forChannel(m_plot, m_channels->currentData().toInt());
    if (m_source != source) {
        m_drag = -1; if (QWidget::mouseGrabber() == m_plot) m_plot->releaseMouse();
        m_source = source; m_selectedChannel = source ? source->channelIndex() : -1;
        m_initialized = false; invalidate();
    }
    m_channels->setToolTip(m_channels->currentText() + "\n仅列出本视图中显示的通道；测量设置在各视图之间独立。");
}
double PlotMeasurementController::snapTime(double time) const {
    if (!m_snap->isChecked() || !m_source || m_source->dataCount() == 0) return time;
    if (m_source->hasOfflineSource()) {
        QVector<QCPGraphData> points; QString error;
        if (!m_source->nearbySamples(time, points, error)) return time;
        return std::min_element(points.cbegin(), points.cend(), [time](const QCPGraphData &a, const QCPGraphData &b) { return std::abs(a.key - time) < std::abs(b.key - time); })->key;
    }
    const auto data = m_source->data(); auto next = data->findBegin(time, false);
    if (next == data->constEnd()) return (data->constEnd() - 1)->key;
    if (next == data->constBegin()) return next->key;
    const auto previous = next - 1;
    return std::abs(previous->key - time) <= std::abs(next->key - time) ? previous->key : next->key;
}
double PlotMeasurementController::valueAt(double time) const {
    if (!m_source || m_source->dataCount() == 0) return ScopeMeasurements::unavailable();
    if (m_source->hasOfflineSource()) {
        QVector<QCPGraphData> points; QString error;
        if (!m_source->nearbySamples(time, points, error)) return ScopeMeasurements::unavailable();
        auto next = std::lower_bound(points.cbegin(), points.cend(), time, [](const QCPGraphData &p, double t) { return p.key < t; });
        if (next != points.cend() && next->key == time) return next->value;
        if (next == points.cbegin() || next == points.cend()) return ScopeMeasurements::unavailable();
        const auto before = next - 1;
        if (!std::isfinite(before->value) || !std::isfinite(next->value) || next->key <= before->key) return ScopeMeasurements::unavailable();
        if (before != points.cbegin() && before->key > (before - 1)->key && next->key - before->key > 5 * (before->key - (before - 1)->key)) return ScopeMeasurements::unavailable();
        return before->value + (next->value - before->value) * (time - before->key) / (next->key - before->key);
    }
    const auto data = m_source->data(); auto next = data->findBegin(time, false);
    if (next != data->constEnd() && next->key == time) return std::isfinite(next->value) ? next->value : ScopeMeasurements::unavailable();
    if (next == data->constBegin() || next == data->constEnd()) return ScopeMeasurements::unavailable();
    const auto previous = next - 1;
    if (!std::isfinite(previous->value) || !std::isfinite(next->value) || next->key <= previous->key) return ScopeMeasurements::unavailable();
    // A visible gap is not a valid interpolation interval.
    if (previous != data->constBegin()) {
        const double dt = previous->key - (previous - 1)->key;
        if (dt > 0 && next->key - previous->key > dt * 5) return ScopeMeasurements::unavailable();
    }
    return previous->value + (next->value - previous->value) * ((time - previous->key) / (next->key - previous->key));
}
void PlotMeasurementController::initializeCursors() {
    if (m_initialized || !m_plot || !m_source || m_source->dataCount() == 0) return;
    const auto data = m_source->data();
    double lower = qMax(m_plot->xAxis->range().lower, data->constBegin()->key);
    double upper = qMin(m_plot->xAxis->range().upper, (data->constEnd() - 1)->key);
    if (lower > upper) { lower = m_plot->xAxis->range().lower; upper = m_plot->xAxis->range().upper; }
    m_time[0] = snapTime(lower * .75 + upper * .25); m_time[1] = snapTime(lower * .25 + upper * .75);
    const auto y = m_plot->yAxis->range(); m_amplitude[0] = y.lower + y.size() * .25; m_amplitude[1] = y.lower + y.size() * .75;
    m_initialized = true; updateCursors();
}
void PlotMeasurementController::resetCursors() { m_initialized = false; initializeCursors(); invalidate(); updateCursors(); refresh(); }
void PlotMeasurementController::resetData() {
    if (QWidget::mouseGrabber() == m_plot) m_plot->releaseMouse();
    m_initialized = false; m_drag = -1; invalidate(); updateCursors();
}
void PlotMeasurementController::updateCursors() {
    if (!m_plot) return;
    const bool ready = m_enabled && m_initialized && m_source && m_source->visible() && m_source->dataCount() > 0;
    for (int i = 0; i < 2; ++i) {
        m_timeLines[i]->setVisible(ready && hasTimeCursors()); m_timeLabels[i]->setVisible(ready && hasTimeCursors());
        m_amplitudeLines[i]->setVisible(ready && hasAmplitudeCursors()); m_amplitudeLabels[i]->setVisible(ready && hasAmplitudeCursors());
        m_timeLines[i]->start->setCoords(m_time[i], 0); m_timeLines[i]->end->setCoords(m_time[i], 1);
        m_amplitudeLines[i]->start->setCoords(0, m_amplitude[i]); m_amplitudeLines[i]->end->setCoords(1, m_amplitude[i]);
    }
    m_gate->setVisible(ready && m_range->currentIndex() == 1 && hasTimeCursors());
    m_gate->topLeft->setCoords(qMin(m_time[0], m_time[1]), 0); m_gate->bottomRight->setCoords(qMax(m_time[0], m_time[1]), 1);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}
void PlotMeasurementController::layoutLabels() {
    if (!m_plot || !m_enabled || !m_initialized) return;
    const QRectF bounds = QRectF(m_plot->axisRect()->rect()).adjusted(2, 2, -2, -2);
    for (int i = 0; i < 2; ++i) {
        auto place = [&](QCPItemText *text, QPointF point, bool inRange) {
            if (!inRange || bounds.isEmpty()) { text->setVisible(false); return; }
            text->setVisible(true); text->position->setPixelPosition(point);
            const QRectF rect(text->topLeft->pixelPosition(), text->bottomRight->pixelPosition());
            const double x = qBound(bounds.left(), rect.left(), qMax(bounds.left(), bounds.right() - rect.width()));
            const double y = qBound(bounds.top(), rect.top(), qMax(bounds.top(), bounds.bottom() - rect.height()));
            text->position->setPixelPosition(point + QPointF(x - rect.left(), y - rect.top()));
        };
        place(m_timeLabels[i], QPointF(m_plot->xAxis->coordToPixel(m_time[i]) + 3, bounds.bottom() - 22 - i * 2),
              hasTimeCursors() && m_source && m_source->dataCount() && m_plot->xAxis->range().contains(m_time[i]));
        place(m_amplitudeLabels[i], QPointF(bounds.right() - 28, m_plot->yAxis->coordToPixel(m_amplitude[i]) + 3),
              hasAmplitudeCursors() && m_source && m_source->dataCount() && m_plot->yAxis->range().contains(m_amplitude[i]));
    }
}

void PlotMeasurementController::refresh() {
    if (!m_enabled || !m_plot || !m_panel->isVisible()) return;
    if (m_timeResolver) m_latestTime = m_timeResolver();
    syncChannels();
    if (!m_source || m_source->dataCount() == 0) {
        if (m_initialized || m_result.samples) resetData();
        m_problem = m_source ? "当前通道没有数据。" : "本视图没有已显示的可测量通道。";
        render(); return;
    }
    initializeCursors(); render();
    if (m_plot->visibleRegion().isEmpty() || m_watcher.isRunning()) return;
    const QCPRange range = m_range->currentIndex() == 1 ? QCPRange(qMin(m_time[0], m_time[1]), qMax(m_time[0], m_time[1])) : m_plot->xAxis->range();
    const auto data = m_source->data();
    if (m_stampValid && m_lastData == data.data() && m_lastCount == data->size() &&
        m_lastPrefixRevision == data->prefixRevision() && m_lastRange == range) return;
    if (!analysisSlots().tryAcquire()) return;
    QVector<QCPGraphData> original; QString readError;
    if (!m_source->readExact(range.lower, range.upper, MaximumSamples, original, readError)) {
        analysisSlots().release(); m_result = {}; m_problem = readError;
        render(); return;
    }
    QVector<ScopeMeasurements::Sample> samples;
    try {
        samples.reserve(original.size());
        for (const auto &point : original) samples.append({point.key, point.value});
    } catch (...) {
        analysisSlots().release(); m_result = {}; m_problem = "测量内存不足，请缩小范围。"; render(); return;
    }
    m_stampValid = true; m_lastData = data.data(); m_lastCount = data->size(); m_lastPrefixRevision = data->prefixRevision(); m_lastRange = range;
    m_workLower = range.lower; m_workUpper = range.upper; m_workVersion = m_version; const auto options = m_options;
    m_watcher.setFuture(QtConcurrent::run([samples = std::move(samples), options]() {
        struct Permit { ~Permit() { analysisSlots().release(); } } permit;
        try { return ScopeMeasurements::calculate(samples, options); }
        catch (...) { ScopeMeasurements::Result result; result.timingStatus = "测量计算资源不足，请缩小范围。"; return result; }
    }));
}

void PlotMeasurementController::render() {
    if (!m_enabled) return;
    struct Value { QString id, name, text, hint; double value; };
    QVector<Value> values; const QString unit = m_source && m_unitResolver ? m_unitResolver(m_source->channelIndex()) : QString();
    const auto add = [&](QString id, QString name, double value, QString units, QString hint = {}) {
        values.append({id, name, number(value, units), hint, value});
    };
    const double missing = ScopeMeasurements::unavailable();
    if (hasTimeCursors()) {
        const double a = m_initialized ? valueAt(m_time[0]) : missing, b = m_initialized ? valueAt(m_time[1]) : missing;
        const double delta = m_initialized ? std::abs(m_time[1] - m_time[0]) : missing;
        add("timeA", "T_A", m_initialized ? m_time[0] - m_latestTime : missing, "s", "相对最新时刻的时间；拖动竖线或在光标位置窗口精确输入。");
        add("timeB", "T_B", m_initialized ? m_time[1] - m_latestTime : missing, "s");
        add("deltaTime", "Δt", delta, "s"); add("reciprocal", "间隔倒数 1/Δt", delta > 0 ? 1 / delta : missing, "Hz", "这是时间间隔的倒数；只有光标相隔一个完整周期时才代表信号频率。");
        add("valueA", "Y(T_A)", a, unit); add("valueB", "Y(T_B)", b, unit);
        add("deltaValue", "ΔY（B−A）", b - a, unit);
    }
    if (hasAmplitudeCursors()) {
        add("amplitudeA", "Y1", m_initialized ? m_amplitude[0] : missing, unit);
        add("amplitudeB", "Y2", m_initialized ? m_amplitude[1] : missing, unit);
        add("deltaAmplitude", "ΔY（Y2−Y1）", m_initialized ? m_amplitude[1] - m_amplitude[0] : missing, unit);
    }
    for (const auto &metric : metrics) if (m_projects.contains(metric.id))
        add(metric.id, QString::fromUtf8(metric.name), metricValue(m_result, metric.id), *metric.unit ? QString::fromUtf8(metric.unit) : unit, QString::fromUtf8(metric.hint));
    m_results->setRowCount((values.size() + 1) / 2);
    for (int i = 0; i < m_results->rowCount() * 2; ++i) {
        const int row = i / 2, col = (i % 2) * 2;
        for (int offset = 0; offset < 2; ++offset) if (!m_results->item(row, col + offset)) m_results->setItem(row, col + offset, new QTableWidgetItem());
        auto name = m_results->item(row, col), value = m_results->item(row, col + 1);
        if (i >= values.size()) { name->setText({}); value->setText({}); name->setData(Qt::UserRole, {}); value->setData(Qt::UserRole, {}); continue; }
        const auto &v = values[i]; name->setText(v.name); name->setData(Qt::UserRole, v.id); name->setToolTip(v.hint);
        value->setText(v.text); value->setData(Qt::UserRole, v.value); value->setToolTip(v.hint + "\n完整数值：" + (std::isfinite(v.value) ? QString::number(v.value, 'g', 17) : "--"));
    }
    m_resultsLayoutTimer.start(0);
    QString status = m_problem;
    if (status.isEmpty()) {
        status = QString("有效点 %1 · 完整周期 %2").arg(m_result.samples).arg(m_result.cycles);
        if (m_result.invalid || m_result.discontinuous) status += " · 含无效点或时间断点";
        if (!m_result.samples) status += " · " + (m_result.timingStatus.isEmpty() ? QString("等待计算") : m_result.timingStatus);
        else if (!std::isfinite(m_result.frequency)) status += " · 周期测量不可用（悬停查看原因）";
    }
    m_status->setText(status);
    m_status->setToolTip(status + QString("\n实际计算范围：%1 至 %2 s（相对最新时刻）\n").arg(number(m_resultLower - m_latestTime), number(m_resultUpper - m_latestTime)) + m_result.timingStatus +
        QString("\n低/高电平：%1 / %2；参考：%3% / %4% / %5%。\n仅测量当前保留的波形数据；时间精度取决于数据时间轴与采样率设置，不跨断点拼接周期。")
            .arg(number(m_result.lowLevel, unit), number(m_result.highLevel, unit)).arg(m_options.lowReference * 100).arg(m_options.middleReference * 100).arg(m_options.highReference * 100));
}

void PlotMeasurementController::populateProjects(QMenu *menu) {
    auto all = menu->addAction("选择全部项目");
    connect(all, &QAction::triggered, this, [this] { for (const auto &metric : metrics) m_projects.insert(metric.id); render(); savePreferences(); });
    auto none = menu->addAction("取消全部项目（保留光标读数）");
    connect(none, &QAction::triggered, this, [this] { m_projects.clear(); render(); savePreferences(); });
    menu->addSeparator();
    for (const auto &metric : metrics) {
        auto action = menu->addAction(QString::fromUtf8(metric.name)); action->setData(metric.id);
        action->setCheckable(true); action->setChecked(m_projects.contains(metric.id)); action->setToolTip(QString::fromUtf8(metric.hint));
        connect(action, &QAction::toggled, this, [this, id = QString(metric.id)](bool checked) {
            if (checked) m_projects.insert(id); else m_projects.remove(id);
            render(); savePreferences();
        });
    }
    menu->setToolTipsVisible(true);
}
void PlotMeasurementController::appendMenu(QMenu *menu) {
    syncChannels();
    QMenu *measure = menu->objectName() == "scopeSettingsMenu" ? menu : menu->addMenu("测量");
    measure->setObjectName("scopeMeasurementMenu");
    auto enabled = measure->addAction("显示测量栏"); enabled->setCheckable(true); enabled->setChecked(m_enabled);
    connect(enabled, &QAction::toggled, this, &PlotMeasurementController::setEnabled);
    connect(measure->addAction("区间统计"), &QAction::triggered, this, [this] {
        setEnabled(true); m_mode->setCurrentIndex(TimeCursors); m_range->setCurrentIndex(1);
        m_projects = {"minimum", "maximum", "peakToPeak", "mean", "rms", "standardDeviation"};
        render(); savePreferences(); refresh();
    });
    connect(measure->addAction("自动测量"), &QAction::triggered, this, [this] {
        setEnabled(true); m_mode->setCurrentIndex(NoCursors); m_range->setCurrentIndex(0);
        m_projects = {"frequency", "period", "positiveDuty", "negativeDuty", "positiveWidth", "negativeWidth", "riseTime", "fallTime"};
        render(); savePreferences(); refresh();
    });
    auto cursors = measure->addMenu("光标模式");
    for (int i = 0; i < m_mode->count(); ++i) {
        auto action = cursors->addAction(m_mode->itemText(i)); action->setCheckable(true); action->setChecked(m_mode->currentIndex() == i);
        connect(action, &QAction::triggered, this, [this, i] { setEnabled(true); m_mode->setCurrentIndex(i); });
    }
    auto ranges = measure->addMenu("测量范围");
    for (int i = 0; i < m_range->count(); ++i) {
        auto action = ranges->addAction(m_range->itemText(i)); action->setCheckable(true); action->setChecked(m_range->currentIndex() == i);
        connect(action, &QAction::triggered, this, [this, i] { setEnabled(true); m_range->setCurrentIndex(i); });
    }
    populateProjects(measure->addMenu("测量项目"));
    auto snap = measure->addAction("吸附采样点"); snap->setCheckable(true); snap->setChecked(m_snap->isChecked());
    connect(snap, &QAction::toggled, this, [this](bool checked) { m_snap->setChecked(checked); });
    measure->addSeparator();
    connect(measure->addAction("精确设置光标位置…"), &QAction::triggered, this, [this] { setEnabled(true); editPositions(); });
    connect(measure->addAction("自动测量阈值设置…"), &QAction::triggered, this, &PlotMeasurementController::editThresholds);
    connect(measure->addAction("重置光标至可见范围"), &QAction::triggered, this, [this] { setEnabled(true); resetCursors(); });
    connect(measure->addAction("清除测量"), &QAction::triggered, this, [this] { setEnabled(false); resetData(); });
}
void PlotMeasurementController::editPositions() {
    initializeCursors();
    QDialog dialog(m_panel); dialog.setObjectName("scopePositionDialog"); dialog.setWindowTitle("光标位置"); dialog.setSizeGripEnabled(true);
    auto layout = new QVBoxLayout(&dialog); auto form = new QFormLayout(); layout->addLayout(form);
    const double zero = m_latestTime; QDoubleSpinBox *inputs[4];
    const QString unit = m_source && m_unitResolver ? m_unitResolver(m_source->channelIndex()) : QString();
    const QStringList labels{"T_A（s）", "T_B（s）", "Y1" + (unit.isEmpty() ? QString() : "（" + unit + "）"), "Y2" + (unit.isEmpty() ? QString() : "（" + unit + "）")};
    for (int i = 0; i < 4; ++i) {
        inputs[i] = new QDoubleSpinBox(); inputs[i]->setObjectName("scopePosition" + QString::number(i));
        inputs[i]->setLocale(QLocale::c()); inputs[i]->setDecimals(9); inputs[i]->setRange(-1e12, 1e12);
        inputs[i]->setValue(i < 2 ? m_time[i] - zero : m_amplitude[i - 2]); form->addRow(labels[i], inputs[i]);
    }
    auto hint = new QLabel("T_A / T_B 相对于打开此窗口时的最新时刻。\n开启吸附时，时间位置取最近的实际采样点。光标位置仅在当前数据会话内保留。");
    hint->setWordWrap(true); layout->addWidget(hint);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("确定"); buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(460, 280);
    if (dialog.exec() == QDialog::Accepted) {
        for (int i = 0; i < 2; ++i) { m_time[i] = snapTime(inputs[i]->value() + zero); m_amplitude[i] = inputs[i + 2]->value(); }
        m_initialized = true; invalidate(); updateCursors(); refresh();
    }
}
void PlotMeasurementController::editThresholds() {
    QDialog dialog(m_panel); dialog.setObjectName("scopeThresholdDialog"); dialog.setWindowTitle("自动测量阈值设置"); dialog.setSizeGripEnabled(true);
    auto layout = new QVBoxLayout(&dialog); auto form = new QFormLayout(); layout->addLayout(form);
    auto automatic = new QCheckBox("自动估计波形低电平和高电平"); automatic->setObjectName("scopeAutomaticLevels"); automatic->setChecked(m_options.automaticLevels); form->addRow(automatic);
    QLineEdit *levels[2];
    for (int i = 0; i < 2; ++i) {
        levels[i] = new QLineEdit(QString::number(i ? m_options.highLevel : m_options.lowLevel, 'g', 17));
        levels[i]->setObjectName(i ? "scopeHighLevel" : "scopeLowLevel");
        auto validator = new QDoubleValidator(levels[i]); validator->setLocale(QLocale::c()); validator->setNotation(QDoubleValidator::ScientificNotation);
        levels[i]->setValidator(validator); levels[i]->setEnabled(!automatic->isChecked()); form->addRow(i ? "高电平" : "低电平", levels[i]);
    }
    connect(automatic, &QCheckBox::toggled, &dialog, [=](bool checked) { levels[0]->setEnabled(!checked); levels[1]->setEnabled(!checked); });
    QDoubleSpinBox *references[5];
    const QStringList labels{"低参考（%）", "中参考（%）", "高参考（%）", "中参考两侧迟滞（%）", "周期变化容差（%）"};
    const double values[]{m_options.lowReference, m_options.middleReference, m_options.highReference, m_options.hysteresis, m_options.periodTolerance};
    for (int i = 0; i < 5; ++i) {
        references[i] = new QDoubleSpinBox(); references[i]->setObjectName("scopeReference" + QString::number(i));
        references[i]->setDecimals(2); references[i]->setRange(i < 3 ? .01 : i == 3 ? 0 : 1, i < 3 ? 99.99 : i == 3 ? 20 : 50);
        references[i]->setValue(values[i] * 100); form->addRow(labels[i], references[i]);
    }
    auto hint = new QLabel("上升/下降时间使用低、高参考电平；周期与脉宽使用中参考电平。\n交点在相邻有效采样点间线性插值，不跨数据断点。单采样间隔内跳变无法解析真实边沿时间，将显示 --。");
    hint->setWordWrap(true); layout->addWidget(hint);
    auto error = new QLabel(); error->setObjectName("scopeThresholdError"); error->setWordWrap(true); layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("保存"); buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto options = m_options; options.automaticLevels = automatic->isChecked();
        bool validLow = true, validHigh = true;
        if (!options.automaticLevels) {
            options.lowLevel = QLocale::c().toDouble(levels[0]->text(), &validLow); options.highLevel = QLocale::c().toDouble(levels[1]->text(), &validHigh);
        }
        options.lowReference = references[0]->value() / 100; options.middleReference = references[1]->value() / 100;
        options.highReference = references[2]->value() / 100; options.hysteresis = references[3]->value() / 100; options.periodTolerance = references[4]->value() / 100;
        QString problem;
        if (!validLow || !validHigh || !ScopeMeasurements::validate(options, problem)) { error->setText(problem.isEmpty() ? "请填写有效的有限数值。" : problem); return; }
        m_options = options; invalidate(); savePreferences(); dialog.accept(); refresh();
    });
    dialog.resize(510, 440); dialog.exec();
}

void PlotMeasurementController::setPreferenceKey(const QString &key) {
    m_preferenceKey = key; if (key.isEmpty()) return;
    m_loading = true; QSettings settings("WaveLink", "WaveLink"); settings.beginGroup(key);
    const bool enabled = settings.value("Enabled", false).toBool();
    m_selectedChannel = qBound(-1, settings.value("Channel", -1).toInt(), 255);
    m_mode->setCurrentIndex(qBound(0, settings.value("CursorMode", TimeCursors).toInt(), 3));
    m_range->setCurrentIndex(qBound(0, settings.value("Range", 0).toInt(), 1));
    if (m_range->currentIndex() == 1 && !hasTimeCursors()) m_mode->setCurrentIndex(TimeCursors);
    m_snap->setChecked(settings.value("Snap", true).toBool());
    m_fold->setChecked(settings.value("Expanded", true).toBool()); m_details->setVisible(m_fold->isChecked());
    m_fold->setArrowType(m_fold->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    if (settings.contains("Projects")) {
        const auto projects = settings.value("Projects").toStringList(); m_projects.clear();
        for (const auto &metric : metrics) if (projects.contains(metric.id)) m_projects.insert(metric.id);
    }
    ScopeMeasurements::Options options;
    options.automaticLevels = settings.value("AutomaticLevels", true).toBool();
    options.lowLevel = settings.value("LowLevel", 0).toDouble(); options.highLevel = settings.value("HighLevel", 1).toDouble();
    options.lowReference = settings.value("LowReference", .1).toDouble(); options.middleReference = settings.value("MiddleReference", .5).toDouble();
    options.highReference = settings.value("HighReference", .9).toDouble(); options.hysteresis = settings.value("Hysteresis", .02).toDouble();
    options.periodTolerance = settings.value("PeriodTolerance", .2).toDouble(); QString error;
    m_options = ScopeMeasurements::validate(options, error) ? options : ScopeMeasurements::Options{};
    m_loading = false; m_source.clear(); m_initialized = false; invalidate();
    setEnabled(enabled); if (enabled) refresh();
}
void PlotMeasurementController::savePreferences() {
    if (m_loading || m_preferenceKey.isEmpty()) return;
    QSettings settings("WaveLink", "WaveLink"); settings.beginGroup(m_preferenceKey);
    settings.setValue("Enabled", m_enabled); settings.setValue("Channel", m_selectedChannel);
    settings.setValue("CursorMode", m_mode->currentIndex()); settings.setValue("Range", m_range->currentIndex());
    settings.setValue("Snap", m_snap->isChecked()); settings.setValue("Expanded", m_fold->isChecked());
    QStringList projects = m_projects.values(); std::sort(projects.begin(), projects.end()); settings.setValue("Projects", projects);
    settings.setValue("AutomaticLevels", m_options.automaticLevels); settings.setValue("LowLevel", m_options.lowLevel); settings.setValue("HighLevel", m_options.highLevel);
    settings.setValue("LowReference", m_options.lowReference); settings.setValue("MiddleReference", m_options.middleReference); settings.setValue("HighReference", m_options.highReference);
    settings.setValue("Hysteresis", m_options.hysteresis); settings.setValue("PeriodTolerance", m_options.periodTolerance);
}

void PlotMeasurementController::updateResultsGeometry() {
    // Include the theme's header padding and the current font rather than
    // fixing pixel heights, which clips Chinese text and larger UI fonts.
    auto header = m_results->horizontalHeader();
    header->ensurePolished();
    const int headerHeight = qMax(header->sizeHint().height(), header->fontMetrics().height() +
        2 * header->style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, header));
    header->setMinimumHeight(headerHeight);
    m_results->resizeRowsToContents();
    const int fallbackRowHeight = qMax(m_results->verticalHeader()->minimumSectionSize(), m_results->fontMetrics().height() + 4);
    const auto heightForRows = [&](int count) {
        int height = headerHeight + 2 * m_results->frameWidth();
        for (int row = 0; row < count; ++row)
            height += row < m_results->rowCount() ? m_results->rowHeight(row) : fallbackRowHeight;
        return height;
    };
    m_results->setMinimumHeight(heightForRows(3));
    m_results->setMaximumHeight(heightForRows(4));
    m_panel->updateGeometry();
}

bool PlotMeasurementController::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == m_results || watched == m_results->horizontalHeader()) &&
        (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
         event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::Show))
        m_resultsLayoutTimer.start(0);
    if (watched != m_plot || !m_enabled || !m_initialized || !m_source || !m_source->dataCount()) return QObject::eventFilter(watched, event);
    const auto nearest = [&](const QPointF &point) {
        int selected = -1; double best = 9;
        for (int i = 0; i < 4; ++i) {
            if ((i < 2 && !hasTimeCursors()) || (i >= 2 && !hasAmplitudeCursors())) continue;
            const double distance = i < 2 ? std::abs(point.x() - m_plot->xAxis->coordToPixel(m_time[i])) :
                                           std::abs(point.y() - m_plot->yAxis->coordToPixel(m_amplitude[i - 2]));
            if (distance < best) { selected = i; best = distance; }
        }
        return selected;
    };
    const auto move = [&](int index, const QPointF &point) {
        const auto rect = m_plot->axisRect()->rect();
        if (index < 2) m_time[index] = snapTime(m_plot->xAxis->pixelToCoord(qBound(double(rect.left()), point.x(), double(rect.right()))));
        else m_amplitude[index - 2] = m_plot->yAxis->pixelToCoord(qBound(double(rect.top()), point.y(), double(rect.bottom())));
        invalidate(); updateCursors(); render();
    };
    if (event->type() == QEvent::MouseButtonPress) {
        auto mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && m_plot->axisRect()->rect().contains(mouse->pos())) {
            m_drag = nearest(mouse->localPos());
            if (m_drag >= 0) { if (QApplication::platformName() != "offscreen") m_plot->grabMouse(); return true; }
        }
    } else if (event->type() == QEvent::MouseMove) {
        auto mouse = static_cast<QMouseEvent *>(event);
        if (m_drag >= 0) { move(m_drag, mouse->localPos()); return true; }
        const int hit = m_plot->axisRect()->rect().contains(mouse->pos()) ? nearest(mouse->localPos()) : -1;
        m_plot->setCursor(hit < 0 ? Qt::ArrowCursor : hit < 2 ? Qt::SplitHCursor : Qt::SplitVCursor);
    } else if (event->type() == QEvent::MouseButtonRelease && m_drag >= 0) {
        move(m_drag, static_cast<QMouseEvent *>(event)->localPos()); m_drag = -1;
        if (QWidget::mouseGrabber() == m_plot) m_plot->releaseMouse(); refresh(); return true;
    } else if (event->type() == QEvent::MouseButtonDblClick && (hasTimeCursors() || hasAmplitudeCursors())) {
        auto mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && m_plot->axisRect()->rect().contains(mouse->pos())) {
            int index = nearest(mouse->localPos());
            if (index < 0) {
                if (hasTimeCursors()) index = std::abs(mouse->localPos().x() - m_plot->xAxis->coordToPixel(m_time[0])) < std::abs(mouse->localPos().x() - m_plot->xAxis->coordToPixel(m_time[1])) ? 0 : 1;
                else index = std::abs(mouse->localPos().y() - m_plot->yAxis->coordToPixel(m_amplitude[0])) < std::abs(mouse->localPos().y() - m_plot->yAxis->coordToPixel(m_amplitude[1])) ? 2 : 3;
            }
            move(index, mouse->localPos()); refresh(); return true;
        }
    } else if (event->type() == QEvent::Leave && m_drag < 0) m_plot->setCursor(Qt::ArrowCursor);
    return QObject::eventFilter(watched, event);
}
