#ifndef PLOTSCROLLAREA_H
#define PLOTSCROLLAREA_H

#include <QScrollArea>
#include <QWidget>
#include <QMainWindow>
#include <QDockWidget>
#include <QEvent>
#include <QTimer>

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
        setPlotSizing(count, m_manualHeight, m_measurementsVisible);
    }

    static int automaticStackMinimumHeight(int detachedCount)
    {
        return detachedCount >= 3 ? (detachedCount + 1) * 240 : 0;
    }

    void setPlotSizing(int count, bool manualHeight, bool measurementsVisible)
    {
        m_detachedCount = count;
        m_manualHeight = manualHeight;
        m_measurementsVisible = measurementsVisible;
        setVerticalScrollBarPolicy(!manualHeight && count >= 3 ? Qt::ScrollBarAlwaysOn :
            (manualHeight || measurementsVisible ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff));
        updateStackMinimum();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == widget() && event->type() == QEvent::LayoutRequest &&
            (m_measurementsVisible || m_manualHeight) && !m_updateQueued) {
            m_updateQueued = true;
            QTimer::singleShot(0, this, [this] {
                m_updateQueued = false;
                updateStackMinimum();
            });
        }
        return QScrollArea::eventFilter(watched, event);
    }

private:
    void updateStackMinimum()
    {
        if (!widget()) return;
        int height = m_manualHeight ? 0 : automaticStackMinimumHeight(m_detachedCount);
        if (m_measurementsVisible || m_manualHeight)
            height = qMax(height, widget()->minimumSizeHint().height());
        if (widget()->minimumHeight() != height) widget()->setMinimumHeight(height);
    }

    int m_detachedCount = 0;
    bool m_manualHeight = false;
    bool m_measurementsVisible = false;
    bool m_updateQueued = false;
};

#endif
