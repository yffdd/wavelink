#include "csvrecordingwriter.h"
#include "recordingchannelnames.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>

namespace {
QByteArray csvHeader(int channelCount, double sampleRateHint, const QStringList &names)
{
    QByteArray header = RecordingChannels::csvUtf8Bom() + RecordingChannels::csvReceiveTimeColumn(sampleRateHint) + ",SampleIndex";
    header += RecordingChannels::csvColumns(names, channelCount);
    header += '\n';
    return header;
}

QByteArray timestampText(qint64 timestampNs)
{
    const qint64 seconds = timestampNs / 1000000000LL;
    const qint64 nanoseconds = qAbs(timestampNs % 1000000000LL);
    return QByteArray::number(seconds) + '.' +
        QByteArray::number(nanoseconds).rightJustified(9, '0');
}

QByteArray csvLine(const QVector<double> &data, qint64 receiveTimestampNs, quint64 frameIndex)
{
    QByteArray line = timestampText(receiveTimestampNs) + ',' + QByteArray::number(frameIndex);
    for (double value : data) {
        line += ',' + RecordingChannels::csvNumber(value);
    }
    line += '\n';
    return line;
}
}

CsvRecordingWriter::~CsvRecordingWriter()
{
    stop();
}

bool CsvRecordingWriter::start(const QString &baseFilePath, QString &error, double sampleRate, const QStringList &channelNames, bool splitOnChannelChange)
{
    stop();
    if (!RecordingChannels::valid(channelNames)) { error = QStringLiteral("录制通道名称无效。"); return false; }
    m_baseFilePath = baseFilePath;
    m_channelNames = channelNames;
    m_splitOnChannelChange = splitOnChannelChange;
    m_completedFiles.clear();
    m_totalBytes = 0;
    m_partBytes = 0;
    m_partNumber = 0;
    m_channelCount = 0;
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 0.0;
    m_frameIndex = 0;
    m_startEpochNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    m_recordClock.start();
    m_active = true;

    // Create the first temporary file now so start failures are reported before
    // the UI claims recording has begun. The header is written with first data.
    return openPart(0, error);
}

QString CsvRecordingWriter::partFilePath(int partNumber) const
{
    QFileInfo baseInfo(m_baseFilePath);
    const QString stem = baseInfo.completeBaseName();
    const QString suffix = baseInfo.suffix().isEmpty() ? QStringLiteral("csv") : baseInfo.suffix();
    const QString name = partNumber <= 1
        ? QStringLiteral("%1.%2").arg(stem, suffix)
        : QStringLiteral("%1_part%2.%3").arg(stem).arg(partNumber, 3, 10, QChar('0')).arg(suffix);
    return baseInfo.dir().absoluteFilePath(name);
}

bool CsvRecordingWriter::openPart(int channelCount, QString &error)
{
    ++m_partNumber;
    m_channelCount = channelCount;
    m_finalPath = partFilePath(m_partNumber);
    const QString temporaryPath = m_finalPath + QStringLiteral(".part");
    m_lock.reset(new QLockFile(temporaryPath + QStringLiteral(".lock")));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) {
        error = QStringLiteral("录制临时文件正被另一个进程使用：%1").arg(temporaryPath);
        failAndClose();
        return false;
    }
    if (QFile::exists(temporaryPath) && !QFile::remove(temporaryPath)) {
        error = QStringLiteral("无法覆盖临时录制文件：%1").arg(temporaryPath);
        failAndClose();
        return false;
    }
    m_file.setFileName(temporaryPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("无法创建录制文件：%1\n%2")
                    .arg(temporaryPath, m_file.errorString());
        failAndClose();
        return false;
    }
    m_partBytes = 0;
    if (channelCount > 0 && !writeBytes(csvHeader(channelCount, m_sampleRate, m_channelNames), error)) return false;
    return true;
}

bool CsvRecordingWriter::writeBytes(const QByteArray &bytes, QString &error)
{
    if (m_file.write(bytes) != bytes.size() || !m_file.flush()) {
        error = QStringLiteral("写入录制文件失败：%1\n%2")
                    .arg(m_file.fileName(), m_file.errorString());
        failAndClose();
        return false;
    }
    m_partBytes += bytes.size();
    m_totalBytes += bytes.size();
    return true;
}

bool CsvRecordingWriter::append(const QVector<double> &data, QString &error, qint64 receiveTimestampNs)
{
    if (!m_active || data.isEmpty()) return true;

    if (receiveTimestampNs <= 0) {
        receiveTimestampNs = m_startEpochNs + m_recordClock.nsecsElapsed();
    }
    const QByteArray line = csvLine(data, receiveTimestampNs, m_frameIndex);
    if (m_channelCount == 0) {
        m_channelCount = data.size();
        if (!writeBytes(csvHeader(m_channelCount, m_sampleRate, m_channelNames), error)) return false;
    }
    if (m_channelCount != data.size()) {
        if (m_splitOnChannelChange) {
            // Each part retains its own rectangular header. Do not drop the
            // transition frame or reset host time/sample index at rollover.
            if (!finalizeCurrent(error)) { failAndClose(); return false; }
            if (!openPart(data.size(), error)) return false;
        } else {
        error = QStringLiteral("录制期间通道数由 %1 变为 %2，为避免 CSV 结构损坏已停止录制。")
                    .arg(m_channelCount).arg(data.size());
        failAndClose();
        return false;
        }
    }

    if (m_partBytes > 0 && m_partBytes + line.size() > m_maxPartBytes) {
        if (!finalizeCurrent(error)) {
            failAndClose();
            return false;
        }
        if (!openPart(data.size(), error)) return false;
    }
    if (!writeBytes(line, error)) return false;
    ++m_frameIndex;
    return true;
}

bool CsvRecordingWriter::finalizeCurrent(QString &error)
{
    if (!m_file.isOpen()) return true;
    if (!m_file.flush()) {
        error = QStringLiteral("刷新录制文件失败：%1\n%2")
                    .arg(m_file.fileName(), m_file.errorString());
        return false;
    }
    const QString temporaryPath = m_file.fileName();
    m_file.close();
    if (QFile::exists(m_finalPath) && !QFile::remove(m_finalPath)) {
        error = QStringLiteral("无法替换录制文件：%1").arg(m_finalPath);
        return false;
    }
    if (!QFile::rename(temporaryPath, m_finalPath)) {
        error = QStringLiteral("无法完成录制文件：%1").arg(m_finalPath);
        return false;
    }
    m_completedFiles.append(m_finalPath);
    if (m_lock) {
        m_lock->unlock();
        m_lock.reset();
        QFile::remove(temporaryPath + QStringLiteral(".lock"));
    }
    return true;
}

void CsvRecordingWriter::failAndClose()
{
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    if (m_lock) {
        m_lock->unlock();
        m_lock.reset();
    }
    m_active = false;
}

void CsvRecordingWriter::stop()
{
    QString ignored;
    finish(ignored);
}
bool CsvRecordingWriter::finish(QString &error)
{
    if (!m_active && !m_file.isOpen()) return true;
    const bool ok = finalizeCurrent(error);
    if (!ok) failAndClose();
    m_active = false;
    return ok;
}

bool CsvRecordingWriter::exportToCsv(const QStringList &files, QIODevice *destination, QString &error,
                                    const std::function<void(qint64, qint64)> &progress)
{
    error.clear();
    if (files.isEmpty() || !destination || !destination->isWritable()) {
        error = QStringLiteral("没有可导出的 CSV 文件或目标文件不可写。");
        return false;
    }
    const auto bom = RecordingChannels::csvUtf8Bom();
    // Inspect only bounded headers first, then stream the data. Dynamic-array
    // parts are a prefix-compatible column family; missing channels become
    // NaN, never zero or a renamed/shifted measurement.
    QVector<int> columns; QStringList widest; QByteArray outputHeader;
    qint64 totalBytes = 0;
    for (const auto &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { error = QStringLiteral("无法读取 CSV 录制文件：%1\n%2").arg(path, file.errorString()); return false; }
        if (progress) totalBytes += file.size();
        QByteArray header = file.readLine(256 * 1024);
        if (!header.endsWith('\n')) { error = "CSV 表头缺失或过长，不能合并。"; return false; }
        if (header.startsWith(bom)) header.remove(0, bom.size());
        auto content = header; content.chop(1); if (content.endsWith('\r')) content.chop(1);
        QStringList cells;
        if (!RecordingChannels::parseCsvRow(QString::fromUtf8(content), cells) || cells.size() < 2 || cells.size() > 258) { error = "CSV 表头格式或通道数量无效。"; return false; }
        for (int i = 0; i < qMin(cells.size(), widest.size()); ++i) if (cells[i] != widest[i]) { error = "CSV 分卷的时间格式或通道名称不一致，不能直接合并。"; return false; }
        columns.append(cells.size());
        if (cells.size() > widest.size()) { widest = cells; outputHeader = header; }
    }
    if (destination->write(bom) != bom.size()) {
        error = QStringLiteral("写入 CSV 编码标识失败。");
        return false;
    }
    if (destination->write(outputHeader) != outputHeader.size()) { error = "写入 CSV 表头失败。"; return false; }
    qint64 completedBytes = 0;
    for (int i = 0; i < files.size(); ++i) {
        QFile source(files[i]);
        if (!source.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("无法读取 CSV 录制文件：%1\n%2").arg(files[i], source.errorString());
            return false;
        }
        // Every recording part is independently valid CSV. Keep the first
        // header, but never copy a second BOM or a later part's header.
        source.readLine();
        if (columns[i] < widest.size()) {
            while (!source.atEnd()) {
                auto line = source.readLine(1024 * 1024);
                if (line.isEmpty() || !line.endsWith('\n')) { error = "CSV 数据行不完整或过长，不能合并。"; return false; }
                line.chop(1); if (line.endsWith('\r')) line.chop(1);
                for (int missing = columns[i]; missing < widest.size(); ++missing) line += ",nan";
                line += '\n';
                if (destination->write(line) != line.size()) { error = "写入 CSV 导出文件失败。"; return false; }
                if (progress) progress(completedBytes + source.pos(), totalBytes);
            }
        } else {
            while (!source.atEnd()) {
                const auto chunk = source.read(1024 * 1024);
                if (chunk.isEmpty() && source.error() != QFile::NoError) {
                    error = QStringLiteral("读取 CSV 录制文件失败：%1\n%2").arg(files[i], source.errorString());
                    return false;
                }
                if (destination->write(chunk) != chunk.size()) {
                    error = QStringLiteral("写入 CSV 导出文件失败。");
                    return false;
                }
                if (progress) progress(completedBytes + source.pos(), totalBytes);
            }
        }
        if (source.error() != QFile::NoError) {
            error = QStringLiteral("读取 CSV 录制文件失败：%1\n%2").arg(files[i], source.errorString());
            return false;
        }
        if (progress) {
            completedBytes += source.size();
            progress(completedBytes, totalBytes);
        }
    }
    return true;
}

QStringList CsvRecordingWriter::recoverInterruptedFiles(const QString &directory,
                                                        QStringList &errors)
{
    QStringList recovered;
    QDir dir(directory);
    if (!dir.exists()) return recovered;
    const QFileInfoList parts = dir.entryInfoList(
        QStringList() << QStringLiteral("*.csv.part"), QDir::Files, QDir::Name);
    for (const QFileInfo &partInfo : parts) {
        QLockFile lock(partInfo.absoluteFilePath() + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        // Another WaveLink process is still recording this file.
        if (!lock.tryLock(0)) continue;
        QFile file(partInfo.absoluteFilePath());
        if (!file.open(QIODevice::ReadWrite)) {
            errors.append(QStringLiteral("%1：%2").arg(partInfo.fileName(), file.errorString()));
            continue;
        }
        // Remove a partially written final row, while preserving every complete
        // CSV line flushed before the abnormal termination.
        if (file.size() > 0) {
            const qint64 scanSize = qMin<qint64>(file.size(), 1024 * 1024);
            file.seek(file.size() - scanSize);
            const QByteArray tail = file.read(scanSize);
            const int newline = tail.lastIndexOf('\n');
            const qint64 validSize = newline >= 0
                ? file.size() - scanSize + newline + 1
                : 0;
            if (!file.resize(validSize)) {
                errors.append(QStringLiteral("%1：无法移除不完整的数据行").arg(partInfo.fileName()));
                file.close();
                continue;
            }
        }
        file.close();

        QString finalPath = partInfo.absoluteFilePath();
        finalPath.chop(5); // remove .part
        if (QFile::exists(finalPath)) {
            QFileInfo finalInfo(finalPath);
            finalPath = finalInfo.dir().absoluteFilePath(
                finalInfo.completeBaseName() + QStringLiteral("_Recovered_") +
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") +
                QStringLiteral(".csv"));
        }
        if (QFile::rename(partInfo.absoluteFilePath(), finalPath)) {
            recovered.append(finalPath);
            QFile::remove(partInfo.absoluteFilePath() + QStringLiteral(".lock"));
        } else {
            errors.append(QStringLiteral("%1：无法重命名恢复文件").arg(partInfo.fileName()));
        }
        lock.unlock();
    }
    return recovered;
}
