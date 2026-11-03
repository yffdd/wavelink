#ifndef CSVRECORDINGWRITER_H
#define CSVRECORDINGWRITER_H

#include <QFile>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QLockFile>
#include <memory>

// Crash-safe, size-limited CSV recorder shared by serial, network and Demo.
// Active files use a .part suffix and are atomically made visible as CSV files
// when a part is closed. A later application run can recover leftover parts.
class CsvRecordingWriter
{
public:
    static const qint64 MaxPartBytes = 1024LL * 1024LL * 1024LL;
    explicit CsvRecordingWriter(qint64 maxPartBytes = MaxPartBytes)
        : m_maxPartBytes(maxPartBytes > 0 ? maxPartBytes : MaxPartBytes) {}
    ~CsvRecordingWriter();

    bool start(const QString &baseFilePath, QString &error, double sampleRate = 0.0,
               const QStringList &channelNames = {}, bool splitOnChannelChange = false);
    void stop();
    bool finish(QString &error);
    bool append(const QVector<double> &data, QString &error, qint64 receiveTimestampNs = 0);
    bool isActive() const { return m_active; }
    qint64 totalBytes() const { return m_totalBytes; }
    QStringList completedFiles() const { return m_completedFiles; }

    // Export legacy or new UTF-8 parts to an empty destination, with one BOM
    // and one header. Source recordings are read-only and remain unchanged.
    static bool exportToCsv(const QStringList &files, QIODevice *destination, QString &error);

    static QStringList recoverInterruptedFiles(const QString &directory,
                                               QStringList &errors);

private:
    QString partFilePath(int partNumber) const;
    bool openPart(int channelCount, QString &error);
    bool writeBytes(const QByteArray &bytes, QString &error);
    bool finalizeCurrent(QString &error);
    void failAndClose();

    QFile m_file;
    QString m_baseFilePath;
    QString m_finalPath;
    QStringList m_completedFiles;
    QStringList m_channelNames;
    qint64 m_totalBytes = 0;
    qint64 m_partBytes = 0;
    int m_partNumber = 0;
    int m_channelCount = 0;
    bool m_active = false;
    bool m_splitOnChannelChange = false;
    qint64 m_maxPartBytes;
    double m_sampleRate = 0.0;
    quint64 m_frameIndex = 0;
    QElapsedTimer m_recordClock;
    qint64 m_startEpochNs = 0;
    std::unique_ptr<QLockFile> m_lock;
};

#endif // CSVRECORDINGWRITER_H
