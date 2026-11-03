#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>

// All source-file IO is owned by an isolated instance of the native helper.
// This GUI-side controller never queries or opens the source path itself.
class BleFileSource : public QObject
{
    Q_OBJECT
public:
    explicit BleFileSource(QObject *parent = nullptr, int ioTimeoutMs = 15000);
    ~BleFileSource() override;
    bool start(const QString &host, const QString &path, const QString &session,
               const QString &transfer, QString &error);
    bool requestChunk(qint64 offset, int length);
    void stop();
    bool cleaningUp() const { return m_process != nullptr && !m_active; }
    bool readPending() const { return !m_request.isEmpty(); }
signals:
    void opened(qint64 size);
    void chunkReady(const QByteArray &data, bool eof);
    void failed(const QString &error);
    void cleanupFinished();
private:
    void consume(const QJsonObject &object);
    void readOutput();
    bool send(QJsonObject object);
    void fail(const QString &message);
    QJsonObject request(const QString &operation);
    QProcess *m_process = nullptr;
    QTimer m_deadline;
    int m_ioTimeout;
    bool m_active = false, m_hello = false, m_opened = false;
    QString m_path, m_session, m_transfer, m_request, m_operation;
    quint64 m_requestCounter = 0;
    qint64 m_total = -1, m_offset = 0;
    int m_length = 0;
    QByteArray m_input;
};
