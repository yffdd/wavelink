#ifndef DEBUGLOGGER_H
#define DEBUGLOGGER_H

#include <QObject>
#include <QStringList>
#include <QMutex>
#include <QDateTime>

class DebugLogger : public QObject
{
    Q_OBJECT
public:
    static DebugLogger& instance();

    void log(const QString &message, const QString &level = "INFO");
    QStringList getLogs() const;
    void clearLogs();

signals:
    void logAdded(const QString &formattedMessage);
    void logsCleared();

private:
    explicit DebugLogger(QObject *parent = nullptr);
    ~DebugLogger();
    DebugLogger(const DebugLogger&) = delete;
    DebugLogger& operator=(const DebugLogger&) = delete;

    void saveLogsToFile();
    void loadLogsFromFile();

    mutable QMutex m_mutex;
    QStringList m_logs;
    const int m_maxLines = 1000;
    QString m_appDirPath;
};

#endif // DEBUGLOGGER_H
