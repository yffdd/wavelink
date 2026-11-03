#ifndef NETWORKHANDLER_H
#define NETWORKHANDLER_H

#include <QObject>
#include <QTcpSocket>
#include <QTcpServer>
#include <QUdpSocket>
#include "../protocol/ProtocolParser.h"
#include "ProtocolProfile.h"
#include "plotframebuffer.h"
#include "csvrecordingwriter.h"
#include "wlrrecordingwriter.h"
#include "recordingchannelguard.h"
#include "StatusMonitor.h"
#include <atomic>
#include <QElapsedTimer>
#include <QMutex>

class NetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit NetworkHandler(QObject *parent = nullptr);
    ~NetworkHandler();
    StatusMonitoring::Session *statusMonitor() const { return m_statusMonitor; }
    QVector<QVector<double>> takePlotFrames(int limit, quint64 &dropped) { return m_plotFrames.take(limit, dropped); }
    quint64 skippedRecordingFrames() const { return m_recordingChannels.skippedFrames(); }

    bool isConnected() const;
    qint64 getLoggedBytes() const;
    bool enqueueData(const QByteArray &data);
    qint64 pendingSendBytes() const { QMutexLocker lock(&m_sendMutex); return m_pendingSendBytes; }
    bool sendReady() const { QMutexLocker lock(&m_sendMutex); return m_sendReady; }
    qint64 getRxCount() const { return m_rxCount.load(); }
    qint64 getTxCount() const { return m_txCount.load(); }
    void resetCounters() { m_rxCount = 0; m_txCount = 0; }

public slots:
    bool startConnection(int mode, const QString &ip, int port,
                         const QString &localIp = QStringLiteral("0.0.0.0"), int localPort = 0);
    bool startUdp(const QString &localIp, int localPort, const QString &remoteIp, int remotePort, bool filterRemote);
    void stopConnection();
    void setProtocol(int index);
    void applyProtocol(quint64 request, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile);
    Q_INVOKABLE void setEndianness(bool bigEndian);
    
    void startRecording(const QString &filePath, double sampleRate);
    void stopRecording();
    Q_INVOKABLE QString finishRecording();
    void sendData(const QByteArray &data);

signals:
    void protocolApplyFinished(quint64 request, bool success, const QString &error);
    void dataReceived(const QByteArray &data);
    void textReady(const QByteArray &text);
    void channelsUpdated(const QVector<double> &data);
    void connectionStateChanged(bool isConnected);
    void clientConnected(const QString &clientIp);
    void clientDisconnected();
    void errorOccurred(const QString &errorMsg);
    void connectionAttemptFinished(bool success, const QString &errorMsg);
    void localEndpointChanged(const QString &address, int port);
    void recordingStartFinished(bool success, const QString &errorMsg);
    void recordingError(const QString &errorMsg);

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError socketError);
    void onNewConnection();
    void onUdpReadyRead();
    void forwardTextReady(const QByteArray &text) { emit textReady(text); }
    void forwardChannelsUpdated(const QVector<double> &data);
    void writeLogData(const QVector<double> &data);

private:
    StatusMonitoring::Session *m_statusMonitor = new StatusMonitoring::Session(this);
    RecordingChannelGuard m_recordingChannels;
    void resetSendSession(bool ready, bool datagram = false);
    void connectSendProgress(QTcpSocket *socket);
    mutable QMutex m_sendMutex;
    qint64 m_pendingSendBytes = 0;
    int m_pendingSendRequests = 0;
    quint64 m_sendGeneration = 0;
    bool m_sendReady = false;
    bool m_datagramSend = false;
    std::atomic<qint64> m_rxCount{0}, m_txCount{0};
    PlotFrameBuffer m_plotFrames;
    QTcpSocket *m_tcpSocket;
    QTcpServer *m_tcpServer;
    QUdpSocket *m_udpSocket = nullptr;
    QHostAddress m_udpRemoteAddress, m_udpLastSender;
    quint16 m_udpRemotePort = 0, m_udpLastSenderPort = 0;
    bool m_udpFilterRemote = false;
    quint64 m_udpSession = 0;
    int m_mode; // Stable interface IDs: 1 TCP client, 2 TCP server, 5 UDP.
    ProtocolParser *m_parser;
    
    CsvRecordingWriter m_recorder;
    WlrRecordingWriter m_wlrRecorder;
    bool m_useWlrRecording = false;
    std::atomic<qint64> loggedBytes{0};
    QString m_serverIp;
    int m_serverPort;
    
    QByteArray m_buffer;
    QElapsedTimer m_receiveClock;
    qint64 m_receiveEpochNs = 0;
    qint64 m_currentReceiveTimestampNs = 0;
};

#endif // NETWORKHANDLER_H
