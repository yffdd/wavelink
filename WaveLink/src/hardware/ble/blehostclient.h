#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QQueue>
#include <QElapsedTimer>
class BleFileSource;

// BLE transport and bounded streaming sender. Acquisition parsing/recording
// belong to BleStreamWorker, not this GUI-side process controller.
class BleHostClient : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Scanning, Connecting, Discovering, AwaitingConfiguration, Configuring, Ready };
    explicit BleHostClient(QObject *parent = nullptr, const QString &hostPath = QString());
    ~BleHostClient() override;
    QString hostPath() const;
    bool available() const;
    State state() const { return m_state; }
    bool canSend() const { return m_canSend && !m_sendBlocked && m_settingsRequest.isEmpty(); }
    bool supportsSendControl() const { return m_sendControlSupported; }
    QJsonObject sendSettings() const;
    bool canReceive() const { return m_canReceive; }
    bool readPending() const { return !m_readRequest.isEmpty(); }
    quint64 rxCount() const { return m_rxCount; }
    quint64 txCount() const { return m_txCount; }
    void resetCounters() { m_rxCount = m_txCount = m_notificationCount = 0; }
    QJsonObject diagnostics() const;
    void scan();
    void stopScan();
    void connectDevice(const QString &address, int addressType);
    bool configure(int receiveHandle, int transmitHandle, bool withoutResponse, int packetSize = 20, int intervalMs = 10, bool confirmed = false);
    bool applySendSettings(int packetSize, int intervalMs, bool confirmed = false);
    bool readCharacteristic(int handle);
    bool sendData(const QByteArray &data);
    void disconnectDevice();
    void abortSession(const QString &message) { fail(message); }
    bool startFileSend(const QString &path, QString &error);
    void cancelFileSend();
    bool fileSendActive() const { return m_fileActive; }
    bool fileSendBusy() const;
    bool sendBusy() const { return fileSendBusy() || !m_sendQueue.isEmpty() || !m_packet.isEmpty() || !m_writeRequest.isEmpty() || !m_settingsRequest.isEmpty() || m_writeDelay.isActive(); }
signals:
    void stateChanged(BleHostClient::State state);
    void deviceFound(const QJsonObject &device);
    void characteristicsDiscovered(const QJsonArray &characteristics);
    void dataReceived(const QByteArray &data);
    void notificationReceived(const QByteArray &data, qint64 receiveTimestampNs);
    void fileSendProgress(qint64 completedBytes, qint64 totalBytes);
    void fileSendFinished(bool success, const QString &message);
    void fileSendStatus(const QString &message);
    void fileSendError(const QString &message);
    void characteristicRead(int handle, const QByteArray &data);
    void ready();
    void errorOccurred(const QString &message, bool fatal);
    void operationFinished();
    void sendSettingsChanged();
    void sendSettingsApplied();
    void sendingStopped(const QString &message);
private:
    void changeState(State state);
    void startHost(const QJsonObject &firstCommand);
    bool command(QJsonObject object);
    QJsonObject makeCommand(const QString &operation);
    void consume(const QJsonObject &object);
    void readOutput();
    void fail(const QString &message);
    void pumpWrites();
    void fillFileQueue();
    void finishFileSend(bool success, const QString &message);
    bool consumeSendSettings(const QJsonObject &object, bool applied);
    void stopSending(const QString &message);
    bool decodeNotification(const QJsonObject &object, QByteArray &data, qint64 &timestampNs);
    void deliverNotification(const QByteArray &data, qint64 timestampNs);
    QString m_hostPath;
    QProcess *m_process = nullptr;
    State m_state = State::Idle;
    quint64 m_sessionCounter = 0, m_requestCounter = 0;
    QString m_session = QStringLiteral("0"), m_controlRequest, m_scanRequest, m_writeRequest, m_readRequest;
    QByteArray m_input, m_sendQueue, m_packet;
    struct Notification { QByteArray data; qint64 timestampNs; };
    QQueue<Notification> m_preReadyData;
    int m_preReadyBytes = 0;
    BleFileSource *m_fileSource = nullptr;
    quint64 m_transferCounter = 0;
    QString m_fileWriteRequest;
    bool m_fileReaderSupported = false, m_fileEof = false;
    bool m_fileActive = false;
    qint64 m_fileCompleted = 0, m_fileTotal = 0;
    QJsonObject m_firstCommand;
    bool m_hello = false, m_canSend = false, m_canReceive = false;
    quint64 m_rxCount = 0, m_txCount = 0;
    quint64 m_notificationCount = 0;
    QJsonObject m_hostInfo;
    QString m_lastError;
    QString m_settingsRequest;
    bool m_sendControlSupported = false, m_sendBlocked = false;
    int m_requestedPacket = 20, m_effectivePacket = 20, m_intervalMs = 10, m_mtu = 0;
    quint64 m_capabilityRevision = 0;
    QElapsedTimer m_phaseTimer;
    qint64 m_connectMs = -1, m_discoverMs = -1, m_configureMs = -1;
    QTimer m_deadline, m_writeDelay;
};
