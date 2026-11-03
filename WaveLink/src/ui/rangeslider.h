#ifndef RANGESLIDER_H
#define RANGESLIDER_H

#include <QWidget>
#include <QMouseEvent>
#include <QPainter>

class RangeSlider : public QWidget
{
    Q_OBJECT
public:
    explicit RangeSlider(QWidget *parent = nullptr);

    double minimum() const { return m_min; }
    double maximum() const { return m_max; }
    double lowerValue() const { return m_lowerValue; }
    double upperValue() const { return m_upperValue; }

public slots:
    void setRange(double min, double max);
    void setRelativeOffset(double offset);
    void setLowerValue(double val);
    void setUpperValue(double val);
    void setValues(double lower, double upper);

signals:
    void rangeChanged(double lower, double upper);

protected:
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    double m_relativeOffset = 0;
    double m_min;
    double m_max;
    double m_lowerValue;
    double m_upperValue;
    
    enum Handle { None, Lower, Upper, Both };
    Handle m_activeHandle;
    int m_handleWidth;
    int m_handleHeight;
    
    int m_lastMousePos;
    
    QRect lowerHandleRect() const;
    QRect upperHandleRect() const;
    int valueToPos(double val) const;
    double posToValue(int pos) const;
};

#endif // RANGESLIDER_H
