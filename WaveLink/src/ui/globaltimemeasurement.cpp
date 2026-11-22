#include "globaltimemeasurement.h"
#include "qcustomplot.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>
#include <cmath>

namespace {
QString secondsText(double time)
{
    QString text = QString::number(time, 'f', 9);
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text == "-0" ? QStringLiteral("0") : text;
}

QString durationText(double duration)
{
    if (duration > 0 && duration < .001) return secondsText(duration * 1e6) + QStringLiteral(" µs");
    if (duration > 0 && duration < 1) return secondsText(duration * 1000) + QStringLiteral(" ms");
    return secondsText(duration) + QStringLiteral(" s");
}

void setTimePosition(QCPItemPosition *position)
{
    position->setTypeX(QCPItemPosition::ptPlotCoords);
    position->setTypeY(QCPItemPosition::ptAxisRectRatio);
}
}

GlobalTimeMeasurement::GlobalTimeMeasurement(QWidget *parent, std::function<QStringList()> channels,
    std::function<double(int, double)> snap, std::function<void(double, double)> locate)
    : QObject(parent), m_bar(new QWidget(parent)), m_readout(new QLabel(m_bar)),
      m_channels(std::move(channels)), m_snap(std::move(snap)), m_locate(std::move(locate))
{
    m_bar->setObjectName("globalTimeMeasurementBar");
    m_bar->hide();
    auto layout = new QHBoxLayout(m_bar);
    layout->setContentsMargins(10, 2, 10, 2);
    m_readout->setObjectName("globalTimeReadout");
    m_readout->setWordWrap(true);
    m_readout->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_readout->setToolTip(QStringLiteral("A / B 使用数据时间。实时波形的横轴以最新时刻为零点。"));
    layout->addWidget(m_readout, 1);

    auto edit = new QToolButton(m_bar);
    edit->setObjectName("globalTimeEdit");
    edit->setText(QStringLiteral("精确输入…"));
    edit->setToolTip(QStringLiteral("设置全局 A / B 时间与采样点吸附"));
    connect(edit, &QToolButton::clicked, this, &GlobalTimeMeasurement::editPositions);
    layout->addWidget(edit);

    auto locateButton = new QToolButton(m_bar);
    locateButton->setObjectName("globalTimeLocate");
    locateButton->setText(QStringLiteral("定位区间"));
    connect(locateButton, &QToolButton::clicked, this, [this] {
        m_locate(qMin(m_time[0], m_time[1]), qMax(m_time[0], m_time[1]));
    });
    layout->addWidget(locateButton);

    auto close = new QToolButton(m_bar);
    close->setObjectName("globalTimeClose");
    close->setText(QStringLiteral("关闭"));
    connect(close, &QToolButton::clicked, this, [this] { setEnabled(false); });
    layout->addWidget(close);
}

void GlobalTimeMeasurement::addPlot(QCustomPlot *plot)
{
    Overlay overlay;
    overlay.interval = new QCPItemRect(plot);
    overlay.interval->setObjectName("globalTimeInterval");
    overlay.interval->setPen(Qt::NoPen);
    overlay.interval->setBrush(QColor(213, 142, 43, 42));
    overlay.interval->setLayer("grid");
    overlay.interval->setSelectable(false);
    setTimePosition(overlay.interval->topLeft);
    setTimePosition(overlay.interval->bottomRight);
    for (int i = 0; i < 2; ++i) {
        const QColor color(i == 0 ? "#a66116" : "#177d80");
        auto line = overlay.lines[i] = new QCPItemLine(plot);
        line->setObjectName(i == 0 ? "globalTimeCursorA" : "globalTimeCursorB");
        line->setPen(QPen(color, 2));
        line->setLayer("overlay");
        line->setSelectable(false);
        setTimePosition(line->start);
        setTimePosition(line->end);
        auto label = overlay.labels[i] = new QCPItemText(plot);
        label->setText(i == 0 ? QStringLiteral("全局 A") : QStringLiteral("全局 B"));
        label->setFont(plot->font());
        label->setColor(Qt::white);
        label->setBrush(color);
        label->setPadding(QMargins(4, 2, 4, 2));
        label->setPositionAlignment(Qt::AlignTop | Qt::AlignHCenter);
        label->position->setType(QCPItemPosition::ptAbsolute);
        label->setLayer("overlay");
        label->setSelectable(false);
    }
    m_overlays.insert(plot, overlay);
    layoutOverlay(plot, overlay);
    plot->installEventFilter(this);
    connect(plot, &QCustomPlot::afterLayout, this, [this, plot] {
        if (m_enabled) layoutOverlay(plot, m_overlays.value(plot));
    });
    connect(plot, &QObject::destroyed, this, [this, plot] { m_overlays.remove(plot); });
}

void GlobalTimeMeasurement::appendMenu(QMenu *menu, QCustomPlot *plot)
{
    menu->addSeparator();
    auto action = menu->addAction(QStringLiteral("全局时间测量"));
    action->setObjectName("globalTimeMeasurementAction");
    action->setCheckable(true);
    action->setChecked(m_enabled);
    connect(action, &QAction::triggered, this, [this, source = QPointer<QCustomPlot>(plot)](bool checked) {
        setEnabled(checked, source);
    });
}

void GlobalTimeMeasurement::setEnabled(bool enabled, QCustomPlot *source)
{
    if (m_enabled == enabled || (enabled && !source)) return;
    m_enabled = enabled;
    if (enabled) {
        const auto range = source->xAxis->range();
        m_time[0] = snapTime(range.lower + range.size() * .25);
        m_time[1] = snapTime(range.lower + range.size() * .75);
    } else {
        stopDragging();
        if (m_hoverPlot) m_hoverPlot->setCursor(Qt::ArrowCursor);
        m_hoverPlot.clear();
    }
    m_bar->setVisible(enabled);
    updateReadoutAndPlots();
}

void GlobalTimeMeasurement::resetData()
{
    m_referenceChannel = -1;
    setEnabled(false);
}

double GlobalTimeMeasurement::snapTime(double time) const
{
    return m_referenceChannel < 0 ? time : m_snap(m_referenceChannel, time);
}

void GlobalTimeMeasurement::layoutOverlay(QCustomPlot *plot, const Overlay &overlay) const
{
    const auto range = plot->xAxis->range();
    const double lower = qMax(range.lower, qMin(m_time[0], m_time[1]));
    const double upper = qMin(range.upper, qMax(m_time[0], m_time[1]));
    overlay.interval->setVisible(m_enabled && lower < upper);
    if (overlay.interval->visible()) {
        overlay.interval->topLeft->setCoords(lower, 0);
        overlay.interval->bottomRight->setCoords(upper, 1);
    }

    const auto bounds = plot->axisRect()->rect();
    QRectF previousLabel;
    for (int i = 0; i < 2; ++i) {
        const bool visible = m_enabled && range.contains(m_time[i]);
        auto line = overlay.lines[i];
        line->setVisible(visible);
        line->start->setCoords(m_time[i], 0);
        line->end->setCoords(m_time[i], 1);
        auto label = overlay.labels[i];
        label->setVisible(false);
        if (!visible) continue;
        const QFontMetrics metrics(label->font());
        const double halfWidth = (metrics.horizontalAdvance(label->text()) + 8) / 2.0;
        const int height = metrics.height() + 4;
        if (bounds.right() - bounds.left() < 2 * halfWidth || bounds.height() < height + 3) continue;
        const double x = qBound(bounds.left() + halfWidth, plot->xAxis->coordToPixel(m_time[i]), bounds.right() - halfWidth);
        double y = bounds.top() + 3;
        QRectF box(x - halfWidth, y, 2 * halfWidth, height);
        if (box.intersects(previousLabel)) { y = previousLabel.bottom() + 2; box.moveTop(y); }
        if (box.bottom() > bounds.bottom()) continue;
        label->position->setCoords(x, y);
        label->setVisible(true);
        previousLabel = box;
    }
}

void GlobalTimeMeasurement::updateReadoutAndPlots()
{
    if (m_enabled) m_readout->setText(QStringLiteral("全局测量　A：%1 s　B：%2 s　Δt：%3")
        .arg(secondsText(m_time[0]), secondsText(m_time[1]), durationText(std::abs(m_time[1] - m_time[0]))));
    for (auto it = m_overlays.cbegin(); it != m_overlays.cend(); ++it) {
        auto plot = it.key();
        layoutOverlay(plot, it.value());
        if (plot->isVisible() && !plot->visibleRegion().isEmpty()) plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void GlobalTimeMeasurement::editPositions()
{
    QDialog dialog(m_bar);
    dialog.setObjectName("globalTimeSettings");
    dialog.setWindowTitle(QStringLiteral("全局时间测量"));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto form = new QFormLayout(&dialog);
    QDoubleSpinBox *inputs[2];
    for (int i = 0; i < 2; ++i) {
        auto input = inputs[i] = new QDoubleSpinBox(&dialog);
        input->setObjectName(i == 0 ? "globalTimeA" : "globalTimeB");
        input->setDecimals(9);
        input->setRange(-1e12, 1e12);
        input->setSuffix(QStringLiteral(" s"));
        input->setKeyboardTracking(false);
        input->setValue(m_time[i]);
        form->addRow(i == 0 ? QStringLiteral("A 数据时间") : QStringLiteral("B 数据时间"), input);
    }
    auto snap = new QCheckBox(QStringLiteral("吸附采样点"), &dialog);
    snap->setObjectName("globalTimeSnap");
    form->addRow(snap);
    auto channel = new QComboBox(&dialog);
    channel->setObjectName("globalTimeReference");
    channel->addItems(m_channels());
    channel->setCurrentIndex(qBound(0, m_referenceChannel, qMax(0, channel->count() - 1)));
    snap->setEnabled(channel->count() > 0);
    snap->setChecked(m_referenceChannel >= 0 && m_referenceChannel < channel->count());
    channel->setEnabled(snap->isChecked());
    connect(snap, &QCheckBox::toggled, channel, &QWidget::setEnabled);
    form->addRow(QStringLiteral("参考通道"), channel);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted || !m_enabled) return;
    m_referenceChannel = snap->isChecked() ? channel->currentIndex() : -1;
    for (int i = 0; i < 2; ++i) m_time[i] = snapTime(inputs[i]->value());
    updateReadoutAndPlots();
}

void GlobalTimeMeasurement::stopDragging()
{
    auto plot = m_dragPlot;
    m_dragPlot.clear();
    m_dragCursor = -1;
    if (plot && QWidget::mouseGrabber() == plot) plot->releaseMouse();
}

bool GlobalTimeMeasurement::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_enabled) return false;
    auto plot = static_cast<QCustomPlot *>(watched);
    const auto type = event->type();
    if (type == QEvent::UngrabMouse || type == QEvent::Hide) {
        if (m_dragPlot == plot) stopDragging();
    }
    if (type == QEvent::Leave || type == QEvent::Hide) {
        if (m_hoverPlot == plot) { plot->setCursor(Qt::ArrowCursor); m_hoverPlot.clear(); }
    }
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove &&
        type != QEvent::MouseButtonRelease && type != QEvent::MouseButtonDblClick) return false;

    auto mouse = static_cast<QMouseEvent *>(event);
    const auto bounds = plot->axisRect()->rect();
    if (m_dragPlot == plot && (type == QEvent::MouseMove || type == QEvent::MouseButtonRelease)) {
        if (type == QEvent::MouseButtonRelease && mouse->button() != Qt::LeftButton) return false;
        const double pixel = qBound(double(bounds.left()), mouse->localPos().x(), double(bounds.right()));
        m_time[m_dragCursor] = snapTime(plot->xAxis->pixelToCoord(pixel));
        updateReadoutAndPlots();
        if (type == QEvent::MouseButtonRelease) stopDragging();
        return true;
    }
    if (type == QEvent::MouseButtonRelease || (type == QEvent::MouseMove && mouse->buttons() != Qt::NoButton)) return false;
    int hit = -1;
    double closest = 8;
    if (bounds.contains(mouse->pos())) {
        const auto range = plot->xAxis->range();
        for (int i = 0; i < 2; ++i) if (range.contains(m_time[i])) {
            const double distance = std::abs(plot->xAxis->coordToPixel(m_time[i]) - mouse->localPos().x());
            if (distance < closest) { hit = i; closest = distance; }
        }
    }
    if (type == QEvent::MouseMove) {
        if (hit >= 0) {
            plot->setCursor(Qt::SplitHCursor);
            m_hoverPlot = plot;
            return true;
        }
        if (m_hoverPlot == plot) { plot->setCursor(Qt::ArrowCursor); m_hoverPlot.clear(); }
    } else if (hit >= 0 && mouse->button() == Qt::LeftButton) {
        m_dragPlot = plot;
        m_dragCursor = hit;
        if (QApplication::platformName() != "offscreen") plot->grabMouse();
        return true;
    }
    return false;
}
