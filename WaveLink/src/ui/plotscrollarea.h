#ifndef PLOTSCROLLAREA_H
#define PLOTSCROLLAREA_H

#include <QScrollArea>
#include <QWidget>
#include <QMainWindow>
#include <QDockWidget>

// restoreState() only knows docks present when its snapshot was saved. Later
// plots must be explicitly restacked, rather than left in Qt's fallback layout.
inline void stackPlotDocksVertically(QMainWindow *window, const QList<QDockWidget *> &docks)
{
    if (!window || docks.isEmpty()) return;
    for (auto dock : docks) window->removeDockWidget(dock);
    QDockWidget *previous = nullptr;
    for (auto dock : docks) {
        dock->setFloating(false);
        if (previous) window->splitDockWidget(previous, dock, Qt::Vertical);
        else window->addDockWidget(Qt::TopDockWidgetArea, dock);
        dock->show();
        previous = dock;
    }
}

// Keep the nested dock layout's minimum height inside the viewport. It must
// never become the outer main window's minimum height (or move bottom docks).
class PlotScrollArea : public QScrollArea
{
public:
    explicit PlotScrollArea(QWidget *parent = nullptr) : QScrollArea(parent)
    {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        setMinimumSize(0, 0);
    }

    QSize minimumSizeHint() const override { return QSize(0, 0); }
    QSize sizeHint() const override { return QSize(640, 400); }

    void setDetachedViewCount(int count)
    {
        m_detachedCount = count;
        const bool scroll = count >= 3;
        setVerticalScrollBarPolicy(scroll ? Qt::ScrollBarAlwaysOn : m_measurementsVisible ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        if (widget()) {
            // Include the main view. DPI-aware logical pixels, not screen pixels.
            widget()->setMinimumHeight(scroll ? (count + 1) * 240 : 0);
            widget()->updateGeometry();
        }
    }
    void setMeasurementBarsVisible(bool visible) {
        if (m_measurementsVisible == visible) return;
        m_measurementsVisible = visible; setDetachedViewCount(m_detachedCount);
    }
private:
    int m_detachedCount = 0;
    bool m_measurementsVisible = false;
};

#endif
