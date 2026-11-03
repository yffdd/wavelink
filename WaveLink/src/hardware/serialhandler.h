#ifndef SERIALHANDLER_H
#define SERIALHANDLER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QVector>
#include <QElapsedTimer>
#include <QTimer>
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

class SerialHandler : public QObject
{
    Q_OBJECT
public:
    explicit SerialHandler(QObject *parent = nullptr, QSerialPort *serialPort = nullptr);
    ~SerialHandler();
    StatusMonitoring::Session *statusMonitor() const { return m_statusMonitor; }
    QVector<QVector<double>> takePlotFrames(int limit, quint64 &dropped) { return m_plotFrames.take(limit, dropped); }
    quint64 skippedRecordingFrames() const { return m_recordingChannels.skippedFrames(); }
    // Thread-safe admission bounds BOTH queued GUI calls and native writes.
    bool enqueueData(quint64 requestId, const QByteArray &data);
    qint64 pendingSendBytes() const;
    bool sendSessionReady(quint64 requestId) const;
    static constexpr qint64 SendQueueLimit = 256 * 1024;
    static constexpr int SendRequestLimit = 1024;

public slots:
    void openPort(const QString &portName, int baudRate, int dataBits, const QString &parityStr,
                  const QString &stopBitsStr, quint64 requestId, bool dtr, bool rts, int flowControl = 0);
    void setControlLines(quint64 requestId, bool dtr, bool rts);
    // durationMs == 0 holds Break until released; positive durations auto-release.
    void setBreakState(quint64 requestId, bool enabled, int durationMs);
    void closePort();
    void sendData(const QByteArray &data);
    void cancelPendingSend(quint64 requestId);
    Q_INVOKABLE void setProtocol(int index);
    void applyProtocol(quint64 request, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile);
    Q_INVOKABLE void setEndianness(bool bigEndian);
    
    // Data Logging
    Q_INVOKABLE void startRecording(const QString &filePath, double sampleRate);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE QString finishRecording();
    Q_INVOKABLE qint64 getLoggedBytes() const;

    Q_INVOKABLE qint64 getRxCount() const { return m_rxCount.load(); }
    Q_INVOKABLE qint64 getTxCount() const { return m_txCount.load(); }
    Q_INVOKABLE void resetCounters() { m_rxCount.store(0); m_txCount.store(0); }

signals:
    void protocolApplyFinished(quint64 request, bool success, const QString &error);
    void dataReceived(const QByteArray &data); // Still emit text
    void channelsUpdated(const QVector<double> &data);
    void portOpenFinished(quint64 requestId, bool success, const QString &errorMsg, int errorCode);
    void errorOccurred(const QString &errorMsg, int errorCode);
    // -1 means unconfirmed; RTS == -2 means automatic hardware-flow control.
    void controlLinesFinished(quint64 requestId, int dtrState, int rtsState, const QString &warning);
    void breakStateChanged(quint64 requestId, bool active, bool pulse, bool supported, const QString &warning);
    void sendRejected(quint64 requestId, const QString &reason);
    // ctsState: -1 unavailable/not applicable, 0 waiting, 1 ready.
    void sendStatusChanged(quint64 requestId, qint64 pendingBytes, int ctsState);
    void sendCancellationFinished(quint64 requestId, bool disconnected);
    void recordingStartFinished(bool success, const QString &errorMsg);
    void recordingError(const QString &errorMsg);

protected:
    // A narrow test seam allows software tests without opening real COM ports.
    virtual bool applyControlLine(bool isDtr, bool enabled);
    virtual bool supportsCombinedControlLines() const;
    virtual bool applyControlLinesTogether(bool dtr, bool rts, bool hardwareFlow,
                                          QSerialPort::SerialPortError &error, QString &message);
    virtual bool applyBreakLine(bool enabled);
    virtual bool configureFlowControl(QSerialPort::FlowControl flow);
    virtual int readCtsState();

private slots:
    void handleReadyRead();
    void handleError(QSerialPort::SerialPortError error);
    void forwardTextReady(const QByteArray &text);
    void forwardChannelsUpdated(const QVector<double> &data);
    void writeLogData(const QVector<double> &data);

private:
    StatusMonitoring::Session *m_statusMonitor = new StatusMonitoring::Session(this);
    RecordingChannelGuard m_recordingChannels;
    PlotFrameBuffer m_plotFrames;
    void closePortSafely();
    QString buildErrorMessage(QSerialPort::SerialPortError error, const QString &systemMessage) const;
    bool tryApplyBreak(bool enabled, QSerialPort::SerialPortError &error, QString &message);
    bool initializeBreak(QString &warning, QSerialPort::SerialPortError &error);
    bool configureControlLines(bool dtr, bool rts, QString &warning, QSerialPort::SerialPortError &error);
    bool reserveSend(quint64 requestId, qint64 size, quint64 &generation);
    void acceptQueuedSend(quint64 requestId, quint64 generation, const QByteArray &data);
    void pumpSendQueue();
    void reportSendStatus(bool force = false);
    void releaseSendBytes(quint64 generation, qint64 count, bool completesRequest = false);
    void invalidateSendQueue(bool closing);

    QSerialPort *m_serialPort;
    ProtocolParser *m_parser;
    QByteArray m_buffer;
    
    CsvRecordingWriter m_recorder;
    WlrRecordingWriter m_wlrRecorder;
    bool m_useWlrRecording = false;
    std::atomic<qint64> loggedBytes{0};
    bool m_isOpening = false;
    bool m_isClosing = false;
    QSerialPort::SerialPortError m_openError = QSerialPort::NoError;
    QString m_openErrorString;
    quint64 m_activeRequestId = 0;
    bool m_isSettingControlLines = false;
    int m_controlLinesAttempted = 0;
    bool m_requestedDtr = false, m_requestedRts = false;
    int m_dtrState = -1, m_rtsState = -1;
    QSerialPort::SerialPortError m_controlLineError = QSerialPort::NoError;
    QString m_controlLineErrorString;
    QTimer *m_breakTimer = nullptr;
    bool m_breakActive = false, m_breakPulse = false, m_breakSupported = false;
    bool m_breakMayBeActive = false;
    bool m_hardwareFlow = false, m_pumpingSend = false;
    int m_ctsState = -1;
    QTimer *m_sendTimer = nullptr;
    QElapsedTimer m_sendStatusClock;
    int m_lastReportedCts = -2;
    struct SendChunk { QByteArray data; int offset = 0; };
    QQueue<SendChunk> m_sendQueue;
    qint64 m_inFlightSendBytes = 0;
    bool m_currentSendCompletesRequest = false;
    mutable QMutex m_sendMutex;
    quint64 m_sendSession = 0, m_sendGeneration = 0;
    qint64 m_reservedSendBytes = 0;
    int m_reservedSendRequests = 0;
    QElapsedTimer m_receiveClock;
    qint64 m_receiveEpochNs = 0;
    qint64 m_currentReceiveTimestampNs = 0;

    std::atomic<qint64> m_rxCount{0};
    std::atomic<qint64> m_txCount{0};
};

#endif // SERIALHANDLER_H
