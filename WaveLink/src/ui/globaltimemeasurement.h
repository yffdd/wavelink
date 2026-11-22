#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class QLabel;
class QMenu;
class QWidget;
class QCustomPlot;
class QCPItemLine;
class QCPItemRect;
class QCPItemText;

class GlobalTimeMeasurement final : public QObject
{
public:
    GlobalTimeMeasurement(QWidget *parent, std::function<QStringList()> channels,
        std::function<double(int, double)> snap, std::function<void(double, double)> locate);
    QWidget *bar() const { return m_bar; }
    void addPlot(QCustomPlot *plot);
    void appendMenu(QMenu *menu, QCustomPlot *plot);
    void resetData();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Overlay {
        QCPItemRect *interval = nullptr;
        QCPItemLine *lines[2] = {};
        QCPItemText *labels[2] = {};
    };

    void setEnabled(bool enabled, QCustomPlot *source = nullptr);
    void updateReadoutAndPlots();
    void layoutOverlay(QCustomPlot *plot, const Overlay &overlay) const;
    void editPositions();
    void stopDragging();
    double snapTime(double time) const;

    QWidget *m_bar;
    QLabel *m_readout;
    std::function<QStringList()> m_channels;
    std::function<double(int, double)> m_snap;
    std::function<void(double, double)> m_locate;
    QHash<QCustomPlot *, Overlay> m_overlays;
    QPointer<QCustomPlot> m_dragPlot, m_hoverPlot;
    double m_time[2] = {0, 0};
    int m_dragCursor = -1;
    int m_referenceChannel = -1;
    bool m_enabled = false;
};
