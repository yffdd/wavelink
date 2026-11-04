#ifndef WLRRECORDINGWRITER_H
#define WLRRECORDINGWRITER_H

#include <QFile>
#include <QElapsedTimer>
#include <QLockFile>
#include <QStringList>
#include <QVector>
#include <QTimer>
#include <functional>
#include <memory>

class QIODevice;

// WaveLink Recording (WLR) is an append-only, little-endian binary format.
// Each frame carries a nanosecond recording-timeline timestamp and
// double-precision channels.
// Active parts use .part and can be repaired after an abnormal termination.
class WlrRecordingWriter
{
public:
    using FrameVisitor = std::function<bool(qint64, const QVector<double> &, qint64)>;
    static const qint64 MaxPartBytes = 1024LL * 1024LL * 1024LL;

    explicit WlrRecordingWriter(qint64 maxPartBytes = MaxPartBytes)
        : m_maxPartBytes(maxPartBytes > 0 ? maxPartBytes : MaxPartBytes) {}
    ~WlrRecordingWriter();

    bool start(const QString &baseFilePath, QString &error, double sampleRate = 0.0,
               const QStringList &channelNames = {}, bool allowDynamicChannels = false);
    void stop();
    bool finish(QString &error);
    bool append(const QVector<double> &data, QString &error, qint64 receiveTimestampNs = 0);
    bool isActive() const { return m_active; }
    void setErrorHandler(std::function<void(const QString &)> handler) { m_errorHandler = std::move(handler); }
    qint64 totalBytes() const { return m_totalBytes; }
    QStringList completedFiles() const { return m_completedFiles; }

    static bool visitFiles(const QStringList &files, const FrameVisitor &visitor,
                           QString &error, double *sampleRateHint = nullptr,
                           bool *usesHostReceiveTime = nullptr,
                           QStringList *channelNames = nullptr);
    static bool exportToCsv(const QStringList &files, QIODevice *destination,
                            QString &error, const std::function<void(qint64, qint64)> &progress = {});
    static QStringList recoverInterruptedFiles(const QString &directory,
                                               QStringList &errors);

private:
    QString partFilePath(int partNumber) const;
    bool openPart(QString &error);
    bool flushBuffer(QString &error);
    bool finalizeCurrent(QString &error);
    void failAndClose();

    QFile m_file;
    QByteArray m_buffer;
    QString m_baseFilePath;
    QString m_finalPath;
    QStringList m_completedFiles;
    QStringList m_channelNames;
    qint64 m_totalBytes = 0;
    qint64 m_partBytes = 0;
    qint64 m_maxPartBytes;
    int m_partNumber = 0;
    int m_channelCount = 0;
    bool m_active = false;
    bool m_allowDynamicChannels = false;
    QElapsedTimer m_recordClock;
    double m_sampleRate = 0.0;
    quint64 m_frameIndex = 0;
    qint64 m_startEpochNs = 0;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QTimer> m_flushTimer;
    QString m_backgroundError;
    std::function<void(const QString &)> m_errorHandler;
};

#endif // WLRRECORDINGWRITER_H
