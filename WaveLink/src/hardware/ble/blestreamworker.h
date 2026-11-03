#pragma once
#include <QObject>
#include <QMutex>
#include <QQueue>
#include <atomic>
#include "ProtocolParser.h"
#include "ProtocolProfile.h"
#include "plotframebuffer.h"
#include "csvrecordingwriter.h"
#include "wlrrecordingwriter.h"
#include "recordingchannelguard.h"
#include "StatusMonitor.h"

// GUI producers enqueue ordered control/data tasks into one bounded mailbox.
// Parsing and all recording IO run on the worker thread, never on the GUI.
class BleStreamWorker : public QObject
{
    Q_OBJECT
public:
    explicit BleStreamWorker(QObject *parent = nullptr);
    ~BleStreamWorker() override;
    StatusMonitoring::Session *statusMonitor() const { return m_statusMonitor; }
    void beginSession(quint64 session, int protocol, bool bigEndian);
    void beginSession(quint64 session, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile);
    void endSession();
    bool enqueueData(quint64 session, const QByteArray &data, qint64 timestampNs);
    void setProtocol(int protocol, bool bigEndian);
    void applyProtocol(quint64 request, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile);
    void startRecording(const QString &path, double sampleRate, quint64 request);
    void stopRecording(quint64 request);
    qint64 getLoggedBytes() const { return m_loggedBytes.load(); }
    quint64 parsedFrames() const { return m_parsedFrames.load(); }
    quint64 recordedFrames() const { return m_recordedFrames.load(); }
    quint64 skippedRecordingFrames() const { return m_recordingChannels.skippedFrames(); }
    quint64 displayDropped() const { return m_plotFrames.totalDropped(); }
    qint64 queuedBytes();
    QVector<QVector<double>> takePlotFrames(int limit, quint64 &dropped) {
        if (m_requestedPlotEpoch.load() != m_processedPlotEpoch.load()) { dropped = 0; return {}; }
        return m_plotFrames.take(limit, dropped);
    }
signals:
    void protocolApplyFinished(quint64 request, bool success, const QString &error);
    void recordingStartFinished(quint64 session, quint64 request, bool success, const QString &error);
    void recordingError(quint64 session, quint64 request, const QString &error);
    void pipelineError(quint64 session, const QString &error);
    void recordingStopped(quint64 request, const QString &error, qint64 bytes);
public slots:
    // Called with BlockingQueuedConnection only for explicit stop/shutdown.
    // Drain already accepted notifications before finalizing the .part file.
    QString stopRecordingNow();
private slots:
    void drain();
private:
    StatusMonitoring::Session *m_statusMonitor = new StatusMonitoring::Session(this);
    enum Kind { Begin, End, Protocol, StartRecording, StopRecording, Data };
    struct Task {
        Kind kind = Data; quint64 session = 0, request = 0, plotEpoch = 0; int protocol = 0; bool bigEndian = false;
        DataProtocols::ProtocolSelection selection;
        DataProtocols::ProtocolProfile profile;
        QByteArray data; QString path; qint64 timestampNs = 0, statusMonotonicMs = -1; double sampleRate = 0;
    };
    void post(Task task);
    void process(const Task &task);
    bool resetParser(const DataProtocols::ProtocolSelection &selection, const DataProtocols::ProtocolProfile &profile, QString &error);
    void frameReady(const QVector<double> &frame);
    void recordFrame(const QVector<double> &frame);
    void finishRecording();
    QMutex m_mutex;
    QQueue<Task> m_tasks;
    qint64 m_queuedBytes = 0;
    bool m_scheduled = false;
    quint64 m_acceptSession = 0, m_session = 0;
    ProtocolParser *m_parser = nullptr;
    QByteArray m_buffer;
    qint64 m_timestampNs = 0;
    PlotFrameBuffer m_plotFrames;
    CsvRecordingWriter m_csv;
    WlrRecordingWriter m_wlr;
    RecordingChannelGuard m_recordingChannels;
    bool m_useWlr = false;
    quint64 m_recordRequest = 0;
    QString m_recordError;
    std::atomic<qint64> m_loggedBytes{0};
    std::atomic<quint64> m_parsedFrames{0}, m_recordedFrames{0};
    std::atomic<quint64> m_requestedPlotEpoch{0}, m_processedPlotEpoch{0};
};
