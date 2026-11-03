#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QComboBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextEdit>
#include <QPlainTextEdit>
#include "systempreferences.h"
#include <QCheckBox>
#include <QElapsedTimer>
#include "rangeslider.h"
#include <QQueue>
#include <QTime>
#include "thememanager.h"
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QMap>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSplitter>
#include <QMenu>
#include <QAction>
#include <QSettings>
#include "serialhandler.h"
#include "ble/blestreamworker.h"
#include "networkhandler.h"
#include "dataprocessor.h"
#include "ProtocolParser.h"
#include "ProtocolProfileStore.h"
#include <memory>
#include "qcustomplot.h"
#include "ffthelper.h"
#include "filter.h"
#include "csvrecordingwriter.h"
#include "wlrrecordingwriter.h"
#include "recordingchannelguard.h"
#include "filetransfereta.h"
#include "offlinewavestore.h"
#include <QStackedWidget>
#include <QLineEdit>
#include <QStringList>
#include "ble/blehostclient.h"

class RightAlignedTicker : public QCPAxisTicker
{
public:
    RightAlignedTicker() : m_latestTime(0) {}
    void setLatestTime(double t) { m_latestTime = t; }
    QString getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision) override {
        double relativeTick = tick - m_latestTime;
        // If it's extremely close to 0, show 0 to avoid -0.00
        if (qAbs(relativeTick) < 1e-6) relativeTick = 0.0;
        return QString::number(relativeTick, 'f', 2);
    }
protected:
    QVector<double> createTickVector(double tickStep, const QCPRange &range) override {
        QVector<double> result;
        qint64 firstStep = qFloor((range.lower - range.upper) / tickStep);
        qint64 lastStep = 0; // Anchor to range.upper
        for (qint64 i = firstStep; i <= lastStep; ++i) {
            result.append(range.upper + i * tickStep);
        }
        return result;
    }
private:
    double m_latestTime;
};
#include "qcustomplot.h"

enum class SignalType {
    General,
    Noise50Hz,
    ECG,
    EEG,
    EMG,
    EOG,
    PPG,
    CBF
};

struct PhysioResult {
    QMap<QString, QString> metrics;
};
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr, SerialHandler *serialBackend = nullptr);
    ~MainWindow();

private slots:
    void onRefreshPorts();
    void onOpenCloseConnection();
    void onInterfaceChanged(int index);
    void showProtocolHelpDialog();
    void showCustomProtocols();
    void initializeProtocols();
    void refreshProtocolChoices();
    void restoreProtocolUi();
    void requestProtocolApply();
    void queueProtocolAutoApply();
    void handleCustomProfileChanged();
    void finishProtocolApply(quint64 request, bool success, const QString &error);
    void updateProtocolControls();
    bool protocolChangeBlocked() const;
    bool protocolUiMatchesApplied() const;
    bool protocolHasNumericData() const;
    void clearProtocolPlots();
    QString channelDisplayName(int channel) const;
    QString channelUnit(int channel) const;
    QString channelSettingKey(int channel, const QString &property) const;
    void showCommandPanel();
    void showStatusMonitor();
    void scrollToLatest();
    bool sendPanelData(const QByteArray &data, bool isAuto = false);
    void onExportConfig();
    void onImportConfig();
    void showSystemSettings();
    void showVersionInfo();
    void initializeUpdates();
    void installDownloadedUpdate();
    void onClearReceive();
    void flushReceiveText();
    void onSaveReceive();
    void onCopyReceive();
    void onSend();
    void onConsolePeriodicSendToggled(bool enabled);
    void onConsolePeriodicSendTimer();
    void onOpenSendFile();
    void onStartSendFile();
    void onStopSendFile();
    void onFileSendTimer();
    void refreshFileSendEta();
    
    void handleSerialDataReceived(const QByteArray &data);
    void handleSerialOpenFinished(quint64 requestId, bool success, const QString &errorMsg, int errorCode);
    void handleSerialError(const QString &errorMsg, int errorCode);
    void onSerialControlLinesChanged();
    void handleSerialControlLinesFinished(quint64 requestId, int dtrState, int rtsState, const QString &warning);
    void handleSerialBreakChanged(quint64 requestId, bool active, bool pulse, bool supported, const QString &warning);
    void handleSerialSendStatus(quint64 requestId, qint64 pendingBytes, int ctsState);
    void handleSerialSendCancelled(quint64 requestId, bool disconnected);
    void cancelSerialSend();
    void handleNetworkConnectionAttemptFinished(bool success, const QString &errorMsg);
    void handleRecordingStartFinished(bool success, const QString &errorMsg);
    void handleRecordingError(const QString &errorMsg);
    void handleNetworkError(const QString &errorMsg);
    void handleNetworkStateChanged(bool isConnected);
    void handleClientConnected(const QString &clientIp);
    void handleClientDisconnected();
    void equalizePlotHeights();
    
    void onSpectrumTimer();
    void onMeasureTimer();
    void updateSpectrumChannelMenu();
    
    bool eventFilter(QObject *watched, QEvent *event) override;
    
    void handleProtocolChanged(int index);
    void handleChannelsUpdated(const QVector<double> &data);
    void updateChannelSelectionSummary();
    void onPlotTimer();
    void onRateTimer(); // New rate timer
    void onXAxisRangeChanged(const class QCPRange &newRange);
    void onDockTopLevelChanged(bool floating);
    void updateDockButtons();
    void setDockCollapsed(QDockWidget *dock, bool collapsed,
                          bool restoreExpandedSize = true);
    QStringList collapsedDockNames() const;
    void restoreDockCollapseStates(const QStringList &dockNames,
                                   bool restoreExpandedSizes = true);
    
    void onSelectAllChannelsStateChanged(int state);
    void onChannelItemChanged(class QListWidgetItem *item);
    void onPlotMouseDoubleClick(class QMouseEvent *event);
    void onPlotMouseMove(class QMouseEvent *event);
    void onPlotContextMenu(const QPoint &pos);
    void onChannelContextMenu(const QPoint &pos);
    void onClearPlots();
    void onRequestClearPlots();
    
private:
    struct StatusUiContext statusMonitorContext() const;
    void setupStatusMonitor();
    class StatusMonitorWindow *m_statusWindow = nullptr;
    QWidget *createBlePage();
    void setupBleBackend();
    void selectBleDevice();
    void openCloseBleConnection();
    void updateBleControls();
    void showBleProfiles();
    void showBleDiagnostics();
    QString bleDiagnosticText(bool includeIdentity) const;
    BleHostClient *bleClient = nullptr;
    BleStreamWorker *bleWorker = nullptr;
    QThread *bleThread = nullptr;
    quint64 m_bleDataSession = 0, m_bleDataSessionCounter = 0;
    quint64 m_bleRecordRequest = 0;
    bool m_bleRecordStopPending = false;
    bool m_bleCloseAfterRecordStop = false;
    QString m_bleRecordStopMessage;
    QLineEdit *editBleDevice = nullptr;
    QLabel *lblBleStatus = nullptr;
    QPushButton *btnBleScan = nullptr;
    QPushButton *btnBleProfiles = nullptr, *btnBleDiagnostics = nullptr;
    QJsonObject m_bleDevice;
    QJsonObject m_bleAppliedProfile;
    QJsonArray m_bleCharacteristics;
    bool m_bleConfigOpen = false;
    std::unique_ptr<DataProtocols::ProtocolProfileStore> m_protocolStore;
    DataProtocols::ProtocolSelection m_appliedProtocol, m_pendingProtocol;
    DataProtocols::ProtocolProfile m_appliedProfile, m_pendingProfile;
    bool m_protocolInitialized = false, m_protocolReady = false, m_protocolApplyPending = false;
    bool m_protocolAutoApplyDeferred = false, m_protocolAutoApplyScheduled = false;
    bool m_protocolReplacesData = true;
    bool m_configRecoveryPending = false;
    quint64 m_protocolRequest = 0;
    int m_protocolApplyInterface = -1, m_protocolAppliedInterface = -1;
    QComboBox *comboCustomProfile = nullptr;
    QLabel *lblProtocolSummary = nullptr;
    
    // Data Logging
    void onToggleRecord();
    void onExportData();
    void onImportData();
    void onClearCache();
    void onRecordTimer();

private:
    void setupDataInterfacePanel();
    void setupGlobalSettingsPanel();
    void setupDemoDataPanel();
    void updateInterfaceStackHeight();
    
    void onDemoTimerTick();
    
    QString getBluetoothNameForComPort(const QString &portName);

    void setupUi();
    void centerWindowOnScreen();
    void setupMenuAndStatusBar();
    class QColorDialog* createStyledColorDialog(const QString &title, const QColor &initialColor, QWidget *parent);
    void loadCustomColors();
    void saveCustomColors();
    void setupDockWidgets();
    void checkAndAddChannel(int count);
    void syncChannelCount(int count);
    void updateChannelListHeight();
    void leaveImportedDataMode();
    void requestChannelDockWidthUpdate();
    void updateChannelDockMinimumWidth();
    int defaultBottomDockHeight() const;
    void updateSpectrumFooterLayout();
    bool prepareCacheForRecording();
    bool hasEnoughDiskSpaceForRecording(QString *details = nullptr) const;
    void recoverInterruptedRecordings();
    void stopRecordingSafely(const QString &errorMessage = QString());
    qint64 getDirSize(const QString &dirPath);
    void updateSerialPortsList();
    bool sendDataToCurrentInterface(const QByteArray &data);
    bool consoleSendPayload(QByteArray &data, QString &error) const;
    QString consoleSendUnavailableReason() const;
    void stopConsolePeriodicSend(const QString &reason = QString());
    void updateConsoleSendControls();
    void setDataTransceiveMode(bool enabled, bool restoringSavedLayout = false);
    void finishConnectionOpen(int interfaceType);
    void finishConnectionClosed(int interfaceType, bool clearPendingData);
    void showCopyableSerialError(const QString &title, const QString &errorMsg, int errorCode);
    void requestSerialBreak(bool enabled, int durationMs = 0);
    void updateSerialBreakControls();
    void updateSerialFlowControls();
    void finishSerialFileSend();
    void startFileSendEta();
    void updateFileSendEta(qint64 completed, qint64 total);
    void resetFileSendEta();
    void setFileSendProgressHint(const QString &detail = {});
    void showSerialControlWarning(const QString &title, const QString &message);

    // Serial Config Dock
    QComboBox *comboInterface;
    QStackedWidget *stackInterface;
    QLineEdit *editIP;
    QSpinBox *spinPort;
    QSpinBox *spinServerPort;
    QComboBox *comboLocalIP = nullptr, *comboServerIP = nullptr;
    QSpinBox *spinLocalPort = nullptr;
    QLabel *lblClientEndpoint = nullptr;
    QComboBox *comboUdpLocalIP = nullptr;
    QLineEdit *editUdpRemoteIP = nullptr;
    QSpinBox *spinUdpLocalPort = nullptr, *spinUdpRemotePort = nullptr;
    QCheckBox *chkUdpFilterRemote = nullptr;
    QLabel *lblUdpEndpoint = nullptr;
    QLabel *lblServerConnCount;
    QLabel *lblServerCurrentConn;

    QComboBox *comboProtocol;
    QPushButton *btnCustomProtocols = nullptr;
    QComboBox *comboEndianness;
    QComboBox *comboPorts;
    QLabel *lblBluetoothName;
    QComboBox *comboBaudRate;
    QComboBox *comboDataBits;
    QComboBox *comboParity;
    QComboBox *comboStopBits;
    QComboBox *comboFlowControl = nullptr;
    int m_serialFlowControl = 0;
    bool m_serialSendCancelPending = false;
    QWidget *serialSendStatusWidget = nullptr;
    QLabel *lblSerialSendStatus = nullptr;
    QPushButton *btnCancelSerialSend = nullptr;
    QCheckBox *chkDtr = nullptr, *chkRts = nullptr;
    bool m_serialDtrPreference = false, m_serialRtsPreference = false;
    QCheckBox *chkSerialBreak = nullptr;
    QSpinBox *spinSerialBreakMs = nullptr;
    QPushButton *btnSerialBreak = nullptr;
    bool m_serialBreakActive = false, m_serialBreakPulse = false;
    bool m_serialBreakPending = false, m_serialBreakSupported = false;
    QDoubleSpinBox *spinFs;
    QLabel *lblDt;
    QLabel *lblDataRate;
    QPushButton *btnOpenClose;
    
    // Console Dock
    QPlainTextEdit *textReceive;
    QTextEdit *textSend;
    QComboBox *comboReceiveLines = nullptr;
    void appendReceiveText(const QString &text);
    void trimReceiveText();
    void applySystemPreferences(const SystemPreferences &preferences, bool persist);
    SystemPreferences m_systemPreferences;
    QPushButton *btnSend;
    QPushButton *btnClearReceive;
    QPushButton *btnSaveReceive = nullptr;
    QPushButton *btnCopyReceive = nullptr;
    QCheckBox *chkShowReceiveData;
    QCheckBox *chkHexReceive;
    QCheckBox *chkTimestamp;
    QComboBox *comboAppendNewline;
    QCheckBox *chkHexSend;
    QPushButton *btnClearSend;
    QCheckBox *chkConsolePeriodicSend = nullptr;
    QSpinBox *spinConsoleSendInterval = nullptr;
    QTimer *consoleSendTimer = nullptr;
    QSplitter *consoleSplitter = nullptr;
    
    class QLineEdit *txtSendFilePath;
    QPushButton *btnOpenSendFile;
    QPushButton *btnSendFile;
    QPushButton *btnStopSendFile;
    class QProgressBar *progressBarFile;
    QLabel *lblFileSendEta = nullptr;
    QTimer *fileSendEtaTimer = nullptr;
    QElapsedTimer m_fileSendElapsed;
    FileTransferEta m_fileSendEta;
    qint64 m_fileSendEtaPaintMs = -1000;
    
    QTimer *fileSendTimer = nullptr;
    class QFile *fileToSend;
    qint64 fileSendTotal;
    qint64 fileSendSent;
    
    class QComboBox *comboEncoding;
    class QLabel *lblRxCount;
    class QLabel *lblTxCount;
    class QPushButton *btnClearCounts;
    
    // Channel Dock (Right)
    QCheckBox *chkSelectAllChannels;
    QCheckBox *chkPausePlot;
    QCheckBox *chkAutoScaleY;
    QCheckBox *chkShowGrid;
    QSpinBox *spinDecimals;
    QVector<class QLabel*> channelValueLabels;
    QVector<class QPushButton*> channelColorButtons;
    class QListWidget *listChannels;
    class QSpinBox *spinPlotHistoryLimit;
    class QLabel *lblPlotHistoryUsage;
    class QPushButton *btnScrollToLatest;
    class QPushButton *btnMergeAllViews;
    class QPushButton *btnExportCache;
    QPushButton *btnClearPlots;

    // Data Logging Dock UI
    class QLineEdit *editCacheDir;
    class QSpinBox *spinCacheLimit;
    class QComboBox *comboRecordingMode;
    QPushButton *btnClearCache;
    QPushButton *btnToggleRecord;
    QCheckBox *chkClearBeforeRecord;
    QPushButton *btnExportData;
    QPushButton *btnImportData;
    QLabel *lblRecordStatus;
    
    QTimer *recordTimer;
    class QDateTime *recordStartTime; // Will just use a QDateTime value, but QDateTime needs include, let's use value
    qint64 currentRecordStartTimeMs; // instead of QDateTime to avoid include
    QString currentRecordFile;
    bool isRecording;
    bool m_devicePresetViewsPending = false;

    // Plotting Variables
    // Global configuration
    void saveConfig();
    void loadConfig();
    class UpdateManager *m_updates = nullptr;

    QTimer *plotTimer = nullptr;
    QTimer *rateTimer;
    QTimer *portRefreshTimer;
    QTimer *yScaleTimer;
    QTimer *spectrumTimer; // Timer for FFT updates
    
    uint64_t sampleCount;
    int frameCountForRate;
    double lastLatestTime;
    QSharedPointer<RightAlignedTicker> customTicker;
    double currentWindowSize;
    bool m_autoScroll;
    bool m_isImportedData = false;
    QStringList m_importedChannelNames;
    std::unique_ptr<class QTextDecoder> m_receiveDecoder;
    QString m_receiveDecoderEncoding;
    QVector<double> m_importedTimes;
    QVector<QVector<double>> m_importedData;
    QSharedPointer<OfflineWaveStore> m_pagedRaw, m_pagedData;
    OfflineFilterSpec m_pagedFilterSpec;
    QTimer m_offlineViewTimer;
    bool m_pagedViewExact = false;
    void refreshPagedView();
    qint64 offlineMemoryBudget() const;
    OfflineFilterSpec offlineFilterSpec(double fs = 0) const;
    void restoreOfflineFilters(const OfflineFilterSpec &spec);
    QSharedPointer<OfflineWaveStore> filterOfflineStore(const QSharedPointer<OfflineWaveStore> &raw, const OfflineFilterSpec &spec, bool &canceled, QString &error);
    void showImportAdvice(const QString &error);
    void exportPagedData(const QString &path);
    QVector<QQueue<double>> channelBuffers;
    QQueue<double> timeBuffer;
    double m_nextPlotTime = 0.0;
    QElapsedTimer m_plotLabelClock;
    QElapsedTimer m_plotHoverClock;
    QElapsedTimer m_plotOverloadClock;
    
    QVector<QColor> defaultColors;
    
    // Multi-Plot Architecture
    class QMainWindow *innerMainWindow;
    class PlotScrollArea *plotScrollArea = nullptr;
    RangeSlider *m_rangeSlider;
    
    struct PlotContext {
        class PlotMeasurementController *scope = nullptr;
        class QCustomPlot *plot;
        class QDockWidget *dock;
        class QCPItemTracer *tracer1;
        class QCPItemTracer *tracer2;
        class QCPItemTracer *hoverTracer;
        class QCPItemText *hoverText;
        class QCPItemLine *measureLine;
        class QCPItemText *measureText;
        class QCPItemText *measureText2;
        class QCPItemText *measureDeltaText;
        class QCPItemText *heightText;
        class QCPItemText *widthText;
        int measureState; // 0: no points, 1: 1 point, 2: 2 points
        QSharedPointer<RightAlignedTicker> ticker;
    };
    QList<PlotContext> allPlots;
    QColor m_mainPlotBackground, m_detachedPlotBackground; // Invalid means follow the system theme.
    void showPlotBackgroundSettings();
    void applyWaveformPlotTheme(QCustomPlot *plot, bool isMain);
    void updateWaveformPlotThemes();
    // --- Filter Dock ---
    QDockWidget *filterDock;
    QWidget *filterWidget;
    QCheckBox *chkNotch;
    
    // --- Other Docks ---
    QDockWidget *configDock;
    QDockWidget *consoleDock;
    QDockWidget *channelDock;
    class QScrollArea *rightSharedScrollArea = nullptr;
    QWidget *rightSharedContent = nullptr;
    bool m_channelDockWidthUpdatePending = false;
    QWidget *filterSection = nullptr;
    QAction *filterPanelAction = nullptr;
    bool m_channelDockUserResized = false;
    
    QComboBox *comboTheme = nullptr;
    void applyCustomTitleBar(QDockWidget *dock, const QString &title, const QString &iconText, const QString &expandText);
    
    QDoubleSpinBox *spinNotchFreq;
    QCheckBox *chkHPF;
    QDoubleSpinBox *spinHPFFreq;
    QCheckBox *chkLPF;
    QDoubleSpinBox *spinLPFFreq;
    QComboBox *comboFilterOrder;
    
    // --- Measure Dock ---
    QDockWidget *measureDock;
    QComboBox *comboMeasureChannel;
    QComboBox *comboSignalType;
    QLabel *signalTypeHint;
    QVBoxLayout *measureMetricsLayout;
    QMap<QString, QLabel*> measureLabels;
    
    QTimer *measureTimer;
    QFutureWatcher<PhysioResult> *measureWatcher;
    
    static PhysioResult calculatePhysioFeatures(SignalType type, const QVector<double>& data, double fs, int decimals = 4);
    void onMeasureFinished();
    void updateMeasureUI();
    
    struct ChannelFilterState {
        BiquadCascade notch;
        BiquadCascade hpf;
        BiquadCascade lpf;
    };
    QVector<ChannelFilterState> filterStates;
    void clearFilterSelections();
    void resetFilters();
    void applyFilters(QVector<double> &data);
    void reapplyFiltersToImportedData();
    
    QVector<class QCPGraph*> channelGraphs;
    QString receiveTextBuffer;
    QByteArray m_defaultState;
    QByteArray m_defaultInnerState;
    QByteArray m_normalWindowStateBeforeDataMode;
    QByteArray m_normalInnerStateBeforeDataMode;
    QByteArray m_normalConsoleSplitterState;
    QStringList m_normalCollapsedDockNames;
    QByteArray m_dataModeWindowState;
    QByteArray m_dataModeConsoleSplitterState;
    QToolButton *btnDataTransceiveMode = nullptr;
    bool m_dataTransceiveMode = false;
    void restoreDefaultLayout();
    QTimer *textUpdateTimer = nullptr;

    QMap<int, int> channelToPlotMap; // Maps channel index to plot index
    
    DataProcessor m_dataProcessor;
    
    // Spectrum Analyzer UI
    QDockWidget *spectrumDock;
    QCustomPlot *spectrumPlot;
    QLabel *spectrumXAxisLabel = nullptr;
    QWidget *spectrumFooter = nullptr;
    class QHBoxLayout *spectrumFooterLayout = nullptr;
    QToolButton *btnSpectrumChannels;
    QMenu *menuSpectrumChannels;
    QComboBox *comboSampleRate;
    QComboBox *comboFftPoints;
    QCheckBox *chkRemoveDC;
    QCheckBox *chkSpectrumAutoScale;
    QCheckBox *chkSpectrumFixX;
    class QDoubleSpinBox *spinSpectrumXMin;
    class QDoubleSpinBox *spinSpectrumXMax;
    QTimer *m_portCheckTimer;
    
    // Demo Data
    QWidget *wDemo;
    class QSpinBox *spinDemoInterval;
    class QComboBox *comboDemoChannels;
    class QComboBox *comboDemoType;
    class QComboBox *comboDemoProtocol;
    class QDoubleSpinBox *spinDemoFreq;
    class QDoubleSpinBox *spinDemoVpp;
    class QDoubleSpinBox *spinDemoOffset;
    class QDoubleSpinBox *spinDemoDuty;
    class QDoubleSpinBox *spinDemoPulseWidth;
    class QDoubleSpinBox *spinDemoNoise;
    
    QTimer *demoTimer;
    double demoTimeCounter;
    CsvRecordingWriter m_demoRecorder;
    WlrRecordingWriter m_demoWlrRecorder;
    RecordingChannelGuard m_demoRecordingChannels;
    StatusMonitoring::Session *m_demoStatusMonitor = new StatusMonitoring::Session(this);
    qint64 demoLoggedBytes;
    
    void writeDemoLogData(const QVector<double> &data);
    QVector<QCPGraph*> spectrumGraphs;
    
    QCPItemTracer *spectrumTracer;
    QCPItemText *spectrumTracerText;
    QCPItemText *spectrumWarningText;
    QCPItemLine *hpfCutoffLine;
    QCPItemLine *lpfCutoffLine;
    QCPItemLine *notchCutoffLine;
    QCPItemText *spectrumLegendText;
    bool spectrumTracerFixed;
    
    void updateFilterNyquistLimits();
    void reapplyFiltersToLiveBuffer();
    
protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    PlotContext createNewPlotContext(const QString &title, bool isMain);
    void moveChannelToNewPlot(int chIdx);
    void moveChannelToMainPlot(int chIdx);
    void updatePlotTitle(int plotIdx);
    void onMergeAllViews();
    void onExportCache();
    void autoScaleY();
    void syncTimeRangeSlider(const QCPRange &viewRange);
    void enforcePlotHistoryLimit(bool updateUsageLabel = true);
    void onPlotDockClosedEvent(class QDockWidget *dock);
    
    void updateSystemTime();

    // Backend
    QThread *serialThread;
    QThread *networkThread;
    QMenu *channelMenu;

    SerialHandler *serialHandler;
    NetworkHandler *networkHandler;
    ProtocolParser *parser = nullptr;
    class CommandPanelWindow *cmdWindow;

    // View tracking
    DataProcessor *processor;
    bool isPortOpen;
    enum class ConnectionState { Disconnected, Connecting, Connected, Disconnecting, Error };
    ConnectionState m_connectionState = ConnectionState::Disconnected;
    int m_activeInterfaceType = -1;
    void setConnectionState(ConnectionState state, int interfaceType);
    bool connectionIsReady() const { return m_connectionState == ConnectionState::Connected; }
    static bool isNetworkInterface(int type) { return type == 1 || type == 2 || type == 5; }
    bool m_connectionOperationPending = false;
    quint64 m_serialOpenRequestId = 0;
    QString m_pendingSerialPortName;
    bool m_hasWarnedAutoSendClosed = false;
    
    // Status Bar & Menu
    class QLabel *lblSystemTime;
    class QTimer *systemTimeTimer;
    class QLabel *lblStatusMessage;
    class QLabel *lblRecordingWarning = nullptr;
    QMenu *m_fileMenu = nullptr;
    QMenu *m_settingsMenu = nullptr;
    QMenu *m_viewMenu = nullptr;
    QMenu *m_toolsMenu = nullptr;
    QMenu *m_helpMenu = nullptr;
};

#endif // MAINWINDOW_H
