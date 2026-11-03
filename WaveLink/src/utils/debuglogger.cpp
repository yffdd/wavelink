#include "debuglogger.h"
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

DebugLogger& DebugLogger::instance()
{
    static DebugLogger instance;
    return instance;
}

DebugLogger::DebugLogger(QObject *parent) : QObject(parent)
{
    m_appDirPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/WaveLink";
    QDir dir(m_appDirPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    // Remove the old log file from the app dir to clean up
    QString oldFilePath = QCoreApplication::applicationDirPath() + "/app_log.txt";
    if (QFile::exists(oldFilePath)) {
        QFile::remove(oldFilePath);
    }
    
    loadLogsFromFile();
}

DebugLogger::~DebugLogger()
{
    saveLogsToFile();
}

void DebugLogger::loadLogsFromFile()
{
    if (m_appDirPath.isEmpty()) return;
    QString filePath = m_appDirPath + "/WaveLink_app_log.txt";
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        in.setCodec("UTF-8");
        while (!in.atEnd()) {
            m_logs.append(in.readLine());
        }
        while (m_logs.size() > m_maxLines) {
            m_logs.removeFirst();
        }
        file.close();
    }
}

void DebugLogger::saveLogsToFile()
{
    if (m_appDirPath.isEmpty()) return;
    QString filePath = m_appDirPath + "/WaveLink_app_log.txt";
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out.setCodec("UTF-8");
        for (const QString &logMsg : qAsConst(m_logs)) {
            out << logMsg << "\n";
        }
        file.close();
    }
}

void DebugLogger::log(const QString &message, const QString &level)
{
    QMutexLocker locker(&m_mutex);
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString formatted = QString("[%1] [%2] %3").arg(timestamp, level, message);
    
    m_logs.append(formatted);
    while (m_logs.size() > m_maxLines) {
        m_logs.removeFirst();
    }
    
    // Save continuously in case of crash
    saveLogsToFile();
    
    emit logAdded(formatted);
}

QStringList DebugLogger::getLogs() const
{
    QMutexLocker locker(&m_mutex);
    return m_logs;
}

void DebugLogger::clearLogs()
{
    QMutexLocker locker(&m_mutex);
    m_logs.clear();
    saveLogsToFile();
    emit logsCleared();
}
