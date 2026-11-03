#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QFutureWatcher>
#include <QSet>
#include <functional>
#include "qcustomplot.h"
#include "scopemeasurements.h"

class WaveformGraph;
class QComboBox;
class QCheckBox;
class QLabel;
class QTableWidget;
class QToolButton;
class QMenu;

// One controller per plot: no acquisition objects and no shared cursor state.
class PlotMeasurementController : public QObject {
    Q_OBJECT
public:
    enum CursorMode { NoCursors, TimeCursors, AmplitudeCursors, BothCursors };
    PlotMeasurementController(QCustomPlot *plot, QWidget *host, std::function<QString(int)> unitResolver, std::function<double()> timeResolver = {});
    QWidget *panel() const { return m_panel; }
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    void appendMenu(QMenu *menu);
    void setLatestTime(double time) { m_latestTime = time; }
    void setPreferenceKey(const QString &key);
    void reloadPreferences() { setPreferenceKey(m_preferenceKey); }
    void resetData();
    void refresh();
    ScopeMeasurements::Result lastResult() const { return m_result; }
signals:
    void activated();
    void visibilityChanged(bool visible);
    void measurementsUpdated();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void syncChannels();
    void initializeCursors();
    void updateCursors();
    void layoutLabels();
    void render();
    void updateResultsGeometry();
    void invalidate();
    void savePreferences();
    void resetCursors();
    void editPositions();
    void editThresholds();
    void populateProjects(QMenu *menu);
    double snapTime(double value) const;
    double valueAt(double time) const;
    bool hasTimeCursors() const;
    bool hasAmplitudeCursors() const;

    QPointer<QCustomPlot> m_plot;
    QPointer<WaveformGraph> m_source;
    std::function<QString(int)> m_unitResolver;
    std::function<double()> m_timeResolver;
    QWidget *m_panel = nullptr, *m_details = nullptr;
    QComboBox *m_channels = nullptr, *m_mode = nullptr, *m_range = nullptr;
    QCheckBox *m_snap = nullptr;
    QTableWidget *m_results = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_fold = nullptr;
    QTimer m_timer, m_resultsLayoutTimer;
    QFutureWatcher<ScopeMeasurements::Result> m_watcher;
    ScopeMeasurements::Options m_options;
    ScopeMeasurements::Result m_result;
    QSet<QString> m_projects;
    QCPItemLine *m_timeLines[2] = {}, *m_amplitudeLines[2] = {};
    QCPItemText *m_timeLabels[2] = {}, *m_amplitudeLabels[2] = {};
    QCPItemRect *m_gate = nullptr;
    double m_time[2] = {}, m_amplitude[2] = {}, m_latestTime = 0;
    double m_workLower = 0, m_workUpper = 0;
    double m_resultLower = 0, m_resultUpper = 0;
    bool m_enabled = false, m_initialized = false, m_loading = false, m_stampValid = false;
    int m_selectedChannel = -1, m_drag = -1, m_lastCount = -1, m_originalMinimumHeight = 0;
    quint64 m_version = 0, m_workVersion = 0, m_lastPrefixRevision = 0;
    const QCPGraphDataContainer *m_lastData = nullptr;
    QCPRange m_lastRange;
    QString m_preferenceKey, m_problem;
};
