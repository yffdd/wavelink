#include "rangeslider.h"
#include <QEvent>
#include <QPainterPath>
#include <cmath>
#include <algorithm>

RangeSlider::RangeSlider(QWidget *parent)
    : QWidget(parent)
    , m_min(0.0)
    , m_max(100.0)
    , m_lowerValue(0.0)
    , m_upperValue(100.0)
    , m_activeHandle(None)
    , m_handleWidth(12)
    , m_handleHeight(40)
    , m_lastMousePos(0)
{
    setMinimumHeight(60);
    setMouseTracking(true);
}

void RangeSlider::setRelativeOffset(double offset)
{
    m_relativeOffset = offset;
    update();
}

void RangeSlider::setRange(double min, double max)
{
    if (min >= max) return;
    m_min = min;
    m_max = max;
    m_lowerValue = std::max(m_min, std::min(m_lowerValue, m_max));
    m_upperValue = std::max(m_min, std::min(m_upperValue, m_max));
    update();
}

void RangeSlider::setLowerValue(double val)
{
    setValues(val, m_upperValue);
}

void RangeSlider::setUpperValue(double val)
{
    setValues(m_lowerValue, val);
}

void RangeSlider::setValues(double lower, double upper)
{
    lower = std::max(m_min, std::min(lower, m_max));
    upper = std::max(m_min, std::min(upper, m_max));
    if (lower > upper) {
        std::swap(lower, upper);
    }
    
    if (m_lowerValue != lower || m_upperValue != upper) {
        m_lowerValue = lower;
        m_upperValue = upper;
        update();
        emit rangeChanged(m_lowerValue, m_upperValue);
    }
}

int RangeSlider::valueToPos(double val) const
{
    if (m_max <= m_min) return m_handleWidth / 2;
    int availableWidth = width() - m_handleWidth;
    double ratio = (val - m_min) / (m_max - m_min);
    return m_handleWidth / 2 + static_cast<int>(ratio * availableWidth);
}

double RangeSlider::posToValue(int pos) const
{
    int availableWidth = width() - m_handleWidth;
    if (availableWidth <= 0) return m_min;
    
    double ratio = static_cast<double>(pos - m_handleWidth / 2) / availableWidth;
    ratio = std::max(0.0, std::min(ratio, 1.0));
    return m_min + ratio * (m_max - m_min);
}

QRect RangeSlider::lowerHandleRect() const
{
    int pos = valueToPos(m_lowerValue);
    return QRect(pos - m_handleWidth / 2, (height() - m_handleHeight) / 2, m_handleWidth, m_handleHeight);
}

QRect RangeSlider::upperHandleRect() const
{
    int pos = valueToPos(m_upperValue);
    return QRect(pos - m_handleWidth / 2, (height() - m_handleHeight) / 2, m_handleWidth, m_handleHeight);
}

void RangeSlider::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange)
        update();
}

void RangeSlider::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // QWidget's text palette follows the application's QSS theme. Reading
    // its active group also keeps the theme stable when the slider is disabled.
    const QColor valueText = palette().color(QPalette::Active, QPalette::WindowText);
    const bool dark = valueText.lightness() > 128;
    const QColor groove = dark ? QColor("#3e3e42") : QColor("#e1e5ea");
    const QColor selection = dark ? QColor("#5cba62") : QColor("#0078d7");
    const QColor selectionText = dark ? QColor("#16331b") : QColor("#ffffff");
    const QColor limitText = dark ? QColor("#b8b8bf") : QColor("#555555");
    const QColor handle = dark ? QColor("#dcdcdc") : QColor("#ffffff");
    const QColor handleBorder = dark ? QColor("#636b74") : QColor("#64748b");

    int grooveHeight = 24;
    int grooveY = (height() - grooveHeight) / 2;
    QRect grooveRect(m_handleWidth / 2, grooveY, width() - m_handleWidth, grooveHeight);

    // Draw background groove
    painter.setPen(Qt::NoPen);
    painter.setBrush(groove);
    painter.drawRoundedRect(grooveRect, 4, 4);

    // Draw active region
    int lowerPos = valueToPos(m_lowerValue);
    int upperPos = valueToPos(m_upperValue);
    QRect activeRect(lowerPos, grooveY, upperPos - lowerPos, grooveHeight);
    painter.setBrush(selection);
    painter.drawRoundedRect(activeRect, 4, 4);

    // Draw text inside active region
    painter.setPen(selectionText);
    painter.setFont(QFont("Arial", 8));
    QString rangeText = QString("Range: %1").arg(m_upperValue - m_lowerValue, 0, 'f', 2);
    painter.drawText(activeRect, Qt::AlignCenter, rangeText);
    
    // Draw min max text
    painter.setPen(limitText);
    painter.drawText(QRect(4, 0, 80, height()), Qt::AlignLeft | Qt::AlignVCenter, QString::number(m_min - m_relativeOffset, 'f', 1));
    painter.drawText(QRect(width() - 84, 0, 80, height()), Qt::AlignRight | Qt::AlignVCenter, QString::number(m_max - m_relativeOffset, 'f', 1));

    // Draw handles
    painter.setPen(QPen(handleBorder, 1));
    painter.setBrush(handle);
    
    QRect lRect = lowerHandleRect();
    painter.drawRoundedRect(lRect, 2, 2);
    
    QRect uRect = upperHandleRect();
    painter.drawRoundedRect(uRect, 2, 2);

    // Draw handle values
    painter.setPen(valueText);
    painter.setFont(QFont("Arial", 8));
    QString lText = QString::number(m_lowerValue - m_relativeOffset, 'f', 2);
    QString uText = QString::number(m_upperValue - m_relativeOffset, 'f', 2);
    painter.drawText(QRect(lRect.x() - 20, 0, 40 + lRect.width(), grooveY), Qt::AlignCenter | Qt::AlignBottom, lText);
    painter.drawText(QRect(uRect.x() - 20, 0, 40 + uRect.width(), grooveY), Qt::AlignCenter | Qt::AlignBottom, uText);
}

void RangeSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_lastMousePos = event->pos().x();
        
        bool inLower = lowerHandleRect().contains(event->pos());
        bool inUpper = upperHandleRect().contains(event->pos());
        
        if (inLower && inUpper) {
            // Pick the one closer to the edge or just default to moving both
            m_activeHandle = Both;
        } else if (inLower) {
            m_activeHandle = Lower;
        } else if (inUpper) {
            m_activeHandle = Upper;
        } else {
            // Clicked in the groove
            double val = posToValue(event->pos().x());
            if (val < m_lowerValue) {
                m_activeHandle = Lower;
                setLowerValue(val);
            } else if (val > m_upperValue) {
                m_activeHandle = Upper;
                setUpperValue(val);
            } else {
                m_activeHandle = Both; // Move entire range
            }
        }
    }
}

void RangeSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (m_activeHandle != None) {
        int dx = event->pos().x() - m_lastMousePos;
        int availableWidth = width() - m_handleWidth;
        if (availableWidth <= 0) return;
        double dv = (dx / (double)availableWidth) * (m_max - m_min);
        
        if (m_activeHandle == Lower) {
            setLowerValue(m_lowerValue + dv);
        } else if (m_activeHandle == Upper) {
            setUpperValue(m_upperValue + dv);
        } else if (m_activeHandle == Both) {
            double range = m_upperValue - m_lowerValue;
            double newLower = m_lowerValue + dv;
            double newUpper = m_upperValue + dv;
            
            if (newLower < m_min) {
                newLower = m_min;
                newUpper = m_min + range;
            }
            if (newUpper > m_max) {
                newUpper = m_max;
                newLower = m_max - range;
            }
            setValues(newLower, newUpper);
        }
        
        m_lastMousePos = event->pos().x();
    }
}

void RangeSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_activeHandle = None;
    }
}

void RangeSlider::resizeEvent(QResizeEvent *)
{
    update();
}
