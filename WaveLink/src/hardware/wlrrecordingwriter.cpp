#include "wlrrecordingwriter.h"
#include "recordingchannelnames.h"
#include <QJsonDocument>
#include <QJsonArray>

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>

namespace {
const QByteArray kMagic("WAVELR1\0", 8);
const quint32 kVersion = 1;
const quint32 kHeaderSize = 36;
const quint32 kFrameMarker = 0x5746524d; // WFRM
const int kMaxChannels = 256;
const int kFlushThreshold = 64 * 1024;

QByteArray makeHeader(double sampleRate, const QStringList &names)
{
    // Schema-1 already permits an extended header. Older readers skip it;
    // new readers use flag 2 to restore optional UTF-8 channel names.
    const QByteArray metadata = names.isEmpty() ? QByteArray() : QJsonDocument(QJsonArray::fromStringList(names)).toJson(QJsonDocument::Compact);
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData(kMagic.constData(), kMagic.size());
    out << kVersion << quint32(kHeaderSize + metadata.size())
        << qint64(QDateTime::currentMSecsSinceEpoch())
        << sampleRate
        << quint32(1 | (metadata.isEmpty() ? 0 : 2));
    out.writeRawData(metadata.constData(), metadata.size());
    return bytes;
}

bool readHeader(QFile &file, QString &error, double *sampleRateOut = nullptr,
                bool *usesHostReceiveTime = nullptr, QStringList *channelNames = nullptr)
{
    if (file.size() < kHeaderSize) {
        error = QStringLiteral("WLR 文件头不完整：%1").arg(file.fileName());
        return false;
    }
    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);
    char magic[8] = {};
    if (in.readRawData(magic, 8) != 8 || QByteArray(magic, 8) != kMagic) {
        error = QStringLiteral("不是有效的 WaveLink WLR 文件：%1").arg(file.fileName());
        return false;
    }
    quint32 version = 0, headerSize = 0, reserved = 0;
    qint64 created = 0;
    double sampleRate = 0;
    in >> version >> headerSize >> created >> sampleRate >> reserved;
    Q_UNUSED(created)
    if (sampleRateOut) *sampleRateOut = sampleRate;
    if (usesHostReceiveTime) *usesHostReceiveTime = (reserved & 1u) != 0;
    if (version != kVersion || headerSize < kHeaderSize || headerSize > 65536 || qint64(headerSize) > file.size()) {
        error = QStringLiteral("不支持的 WLR 文件版本或文件头：%1").arg(file.fileName());
        return false;
    }
    QStringList names;
    if (reserved & 2u) {
        QJsonParseError jsonError;
        const auto doc = QJsonDocument::fromJson(file.read(headerSize - kHeaderSize), &jsonError);
        if (jsonError.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > kMaxChannels) {
            error = QStringLiteral("WLR 通道名称元数据无效。"); return false;
        }
        for (const auto &entry : doc.array()) {
            const auto name = entry.toString();
            if (!entry.isString() || !RecordingChannels::valid({name})) {
                error = QStringLiteral("WLR 通道名称无效。"); return false;
            }
            names.append(name);
        }
    }
    if (channelNames) *channelNames = names;
    return file.seek(headerSize);
}

bool readFrame(QFile &file, qint64 &timestamp, QVector<double> &values,
               qint64 *frameEnd, QString &error)
{
    if (file.atEnd()) return false;
    const qint64 frameStart = file.pos();
    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);
    quint32 marker = 0, payloadSize = 0, count = 0;
    in >> marker >> payloadSize;
    if (in.status() != QDataStream::Ok || marker != kFrameMarker ||
        payloadSize < 12 || payloadSize > quint32(12 + kMaxChannels * int(sizeof(double)))) {
        error = QStringLiteral("WLR 数据帧损坏，偏移：%1").arg(frameStart);
        return false;
    }
    in >> timestamp >> count;
    if (count == 0 || count > kMaxChannels || payloadSize != 12u + count * 8u ||
        file.size() - file.pos() < qint64(count) * 8) {
        error = QStringLiteral("WLR 数据帧长度无效，偏移：%1").arg(frameStart);
        return false;
    }
    values.resize(int(count));
    for (quint32 i = 0; i < count; ++i) in >> values[int(i)];
    if (in.status() != QDataStream::Ok) {
        error = QStringLiteral("WLR 数据帧未完整写入，偏移：%1").arg(frameStart);
        return false;
    }
    if (frameEnd) *frameEnd = file.pos();
    return true;
}
}

WlrRecordingWriter::~WlrRecordingWriter()
{
    stop();
}

bool WlrRecordingWriter::start(const QString &baseFilePath, QString &error, double sampleRate, const QStringList &channelNames, bool allowDynamicChannels)
{
    stop();
    m_backgroundError.clear();
    if (!RecordingChannels::valid(channelNames) || QJsonDocument(QJsonArray::fromStringList(channelNames)).toJson(QJsonDocument::Compact).size() > 65536 - int(kHeaderSize)) {
        error = QStringLiteral("录制通道名称无效或元数据超过上限。"); return false;
    }
    m_baseFilePath = baseFilePath;
    m_channelNames = channelNames;
    m_allowDynamicChannels = allowDynamicChannels;
    m_completedFiles.clear();
    m_totalBytes = 0;
    m_partBytes = 0;
    m_partNumber = 0;
    m_channelCount = 0;
    m_buffer.clear();
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 0.0;
    m_frameIndex = 0;
    m_startEpochNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    m_recordClock.start();
    m_active = true;
    if (!openPart(error)) return false;
    // Construct in the recording thread, including when its owner was moved
    // from the UI thread. Flush even if the device stops sending data.
    m_flushTimer.reset(new QTimer);
    m_flushTimer->setInterval(500);
    m_flushTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(m_flushTimer.get(), &QTimer::timeout, m_flushTimer.get(), [this] {
        QString error;
        if (!flushBuffer(error)) {
            m_backgroundError = error;
            if (m_errorHandler) m_errorHandler(error);
        }
    });
    m_flushTimer->start();
    return true;
}

QString WlrRecordingWriter::partFilePath(int partNumber) const
{
    QFileInfo baseInfo(m_baseFilePath);
    const QString stem = baseInfo.completeBaseName();
    const QString name = partNumber <= 1
        ? stem + QStringLiteral(".wlr")
        : QStringLiteral("%1_part%2.wlr").arg(stem).arg(partNumber, 3, 10, QChar('0'));
    return baseInfo.dir().absoluteFilePath(name);
}

bool WlrRecordingWriter::openPart(QString &error)
{
    ++m_partNumber;
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
        error = QStringLiteral("无法创建高速录制文件：%1\n%2")
                    .arg(temporaryPath, m_file.errorString());
        failAndClose();
        return false;
    }
    const QByteArray header = makeHeader(m_sampleRate, m_channelNames);
    if (m_file.write(header) != header.size() || !m_file.flush()) {
        error = QStringLiteral("无法写入 WLR 文件头：%1").arg(m_file.errorString());
        failAndClose();
        return false;
    }
    m_partBytes = header.size();
    m_totalBytes += header.size();
    return true;
}

bool WlrRecordingWriter::flushBuffer(QString &error)
{
    if (m_buffer.isEmpty()) return true;
    if (m_file.write(m_buffer) != m_buffer.size() || !m_file.flush()) {
        error = QStringLiteral("写入高速录制文件失败：%1\n%2")
                    .arg(m_file.fileName(), m_file.errorString());
        failAndClose();
        return false;
    }
    m_buffer.clear();
    return true;
}

bool WlrRecordingWriter::append(const QVector<double> &data, QString &error, qint64 receiveTimestampNs)
{
    if (!m_backgroundError.isEmpty()) { error = m_backgroundError; return false; }
    if (!m_active || data.isEmpty()) return true;
    if (data.size() > kMaxChannels) {
        error = QStringLiteral("WLR 单帧通道数量超过 256，录制已停止。");
        failAndClose();
        return false;
    }
    if (m_channelCount == 0) m_channelCount = data.size();
    if (!m_allowDynamicChannels && m_channelCount != data.size()) {
        error = QStringLiteral("录制期间通道数由 %1 变为 %2，为避免文件结构损坏已停止录制。")
                    .arg(m_channelCount).arg(data.size());
        failAndClose();
        return false;
    }

    QByteArray frame;
    QDataStream out(&frame, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 payloadSize = 12u + quint32(data.size()) * 8u;
    if (receiveTimestampNs <= 0) {
        receiveTimestampNs = m_startEpochNs + m_recordClock.nsecsElapsed();
    }
    out << kFrameMarker << payloadSize << receiveTimestampNs
        << quint32(data.size());
    for (double value : data) out << value;

    if (m_partBytes + frame.size() > m_maxPartBytes) {
        if (!flushBuffer(error) || !finalizeCurrent(error) || !openPart(error)) {
            failAndClose();
            return false;
        }
    }
    m_buffer.append(frame);
    m_partBytes += frame.size();
    m_totalBytes += frame.size();
    ++m_frameIndex;
    return m_buffer.size() < kFlushThreshold || flushBuffer(error);
}

bool WlrRecordingWriter::finalizeCurrent(QString &error)
{
    if (!m_file.isOpen()) return true;
    if (!flushBuffer(error)) return false;
    const QString temporaryPath = m_file.fileName();
    m_file.close();
    if (QFile::exists(m_finalPath) && !QFile::remove(m_finalPath)) {
        error = QStringLiteral("无法替换高速录制文件：%1").arg(m_finalPath);
        return false;
    }
    if (!QFile::rename(temporaryPath, m_finalPath)) {
        error = QStringLiteral("无法完成高速录制文件：%1").arg(m_finalPath);
        return false;
    }
    m_completedFiles.append(m_finalPath);
    if (m_lock) {
        m_lock->unlock();
        m_lock.reset();
    }
    return true;
}

void WlrRecordingWriter::failAndClose()
{
    if (m_flushTimer) m_flushTimer->stop();
    if (m_file.isOpen()) {
        if (!m_buffer.isEmpty()) m_file.write(m_buffer);
        m_file.flush();
        m_file.close();
    }
    m_buffer.clear();
    if (m_lock) {
        m_lock->unlock();
        m_lock.reset();
    }
    m_active = false;
}

void WlrRecordingWriter::stop()
{
    QString ignored;
    finish(ignored);
}
bool WlrRecordingWriter::finish(QString &error)
{
    if (m_flushTimer) m_flushTimer->stop();
    if (!m_backgroundError.isEmpty()) { error = m_backgroundError; return false; }
    if (!m_active && !m_file.isOpen()) return true;
    const bool ok = finalizeCurrent(error);
    if (!ok) failAndClose();
    m_active = false;
    return ok;
}

bool WlrRecordingWriter::visitFiles(const QStringList &files, const FrameVisitor &visitor,
                                    QString &error, double *sampleRateHint,
                                    bool *usesHostReceiveTime, QStringList *channelNames)
{
    qint64 completedBytes = 0;
    bool firstFile = true;
    QStringList firstNames;
    for (const QString &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("无法打开 WLR 文件：%1\n%2").arg(path, file.errorString());
            return false;
        }
        double partSampleRate = 0.0;
        bool partUsesHostTime = false;
        QStringList partNames;
        if (!readHeader(file, error, &partSampleRate, &partUsesHostTime, &partNames)) return false;
        if (firstFile) {
            if (sampleRateHint) *sampleRateHint = partSampleRate;
            if (usesHostReceiveTime) *usesHostReceiveTime = partUsesHostTime;
            firstNames = partNames;
            if (channelNames) *channelNames = firstNames;
            firstFile = false;
        } else if (partNames != firstNames) {
            error = QStringLiteral("WLR 分卷中的通道名称不一致。"); return false;
        }
        while (!file.atEnd()) {
            qint64 timestamp = 0;
            QVector<double> values;
            qint64 frameEnd = 0;
            if (!readFrame(file, timestamp, values, &frameEnd, error)) return false;
            if (!visitor(timestamp, values, completedBytes + frameEnd)) return true;
        }
        completedBytes += file.size();
    }
    return true;
}

bool WlrRecordingWriter::exportToCsv(const QStringList &files, QIODevice *destination,
                                     QString &error, const std::function<void(qint64, qint64)> &progress)
{
    bool headerWritten = false;
    int channelCount = 0;
    quint64 frameIndex = 0;
    double sampleRateHint = 0.0;
    bool hostTime = false;
    qint64 firstLegacyTimestamp = -1;
    QStringList names;
    error.clear();
    if (!destination || !destination->isWritable()) { error = "CSV 导出目标不可写。"; return false; }
    // The binary format retains each frame's actual width. Discover the union
    // once, then stream a rectangular CSV without losing any shorter frames.
    qint64 sourceBytes = 0;
    if (progress) for (const auto &path : files) sourceBytes += QFileInfo(path).size();
    if (!visitFiles(files, [&](qint64, const QVector<double> &values, qint64 bytes) {
        channelCount = qMax(channelCount, values.size());
        if (progress) progress(bytes, 2 * sourceBytes);
        return true;
    }, error)) return false;
    const bool ok = visitFiles(files, [&](qint64 timestamp, const QVector<double> &values, qint64 bytes) {
        if (!headerWritten) {
            QByteArray header = hostTime
                ? RecordingChannels::csvReceiveTimeColumn(sampleRateHint) + ",SampleIndex"
                : QByteArray("Time");
            header += RecordingChannels::csvColumns(names, channelCount);
            header += '\n';
            header.prepend(RecordingChannels::csvUtf8Bom());
            if (destination->write(header) != header.size()) {
                error = QStringLiteral("写入 CSV 表头失败。");
                return false;
            }
            headerWritten = true;
        }
        QByteArray line;
        if (hostTime) {
            const qint64 seconds = timestamp / 1000000000LL;
            const qint64 nanoseconds = qAbs(timestamp % 1000000000LL);
            line = QByteArray::number(seconds) + '.' +
                QByteArray::number(nanoseconds).rightJustified(9, '0') + ',' +
                QByteArray::number(frameIndex++);
        } else {
            if (firstLegacyTimestamp < 0) firstLegacyTimestamp = timestamp;
            line = QByteArray::number(
                double(timestamp - firstLegacyTimestamp) / 1000000000.0, 'f', 9);
        }
        for (double value : values) line += ',' + RecordingChannels::csvNumber(value);
        for (int i = values.size(); i < channelCount; ++i) line += ",nan";
        line += '\n';
        if (destination->write(line) != line.size()) {
            error = QStringLiteral("写入 CSV 数据失败。");
            return false;
        }
        if (progress) progress(sourceBytes + bytes, 2 * sourceBytes);
        return true;
    }, error, &sampleRateHint, &hostTime, &names) && error.isEmpty();
    if (ok && progress) progress(2 * sourceBytes, 2 * sourceBytes);
    return ok;
}

QStringList WlrRecordingWriter::recoverInterruptedFiles(const QString &directory,
                                                        QStringList &errors)
{
    QStringList recovered;
    QDir dir(directory);
    const QFileInfoList parts = dir.entryInfoList(
        QStringList() << QStringLiteral("*.wlr.part"), QDir::Files, QDir::Name);
    for (const QFileInfo &partInfo : parts) {
        QLockFile lock(partInfo.absoluteFilePath() + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) continue;
        QFile file(partInfo.absoluteFilePath());
        QString error;
        if (!file.open(QIODevice::ReadWrite) || !readHeader(file, error)) {
            errors.append(error.isEmpty() ? partInfo.fileName() + QStringLiteral("：无法打开") : error);
            lock.unlock();
            continue;
        }
        qint64 lastGood = file.pos();
        while (!file.atEnd()) {
            qint64 timestamp = 0, frameEnd = 0;
            QVector<double> values;
            QString frameError;
            if (!readFrame(file, timestamp, values, &frameEnd, frameError)) break;
            lastGood = frameEnd;
        }
        if (!file.resize(lastGood)) {
            errors.append(partInfo.fileName() + QStringLiteral("：无法截断损坏的数据帧"));
            file.close();
            lock.unlock();
            continue;
        }
        file.close();
        QString finalPath = partInfo.absoluteFilePath();
        finalPath.chop(5);
        if (QFile::exists(finalPath)) {
            QFileInfo info(finalPath);
            finalPath = info.dir().absoluteFilePath(
                info.completeBaseName() + QStringLiteral("_Recovered_") +
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + QStringLiteral(".wlr"));
        }
        if (QFile::rename(partInfo.absoluteFilePath(), finalPath)) recovered.append(finalPath);
        else errors.append(partInfo.fileName() + QStringLiteral("：无法重命名恢复文件"));
        lock.unlock();
    }
    return recovered;
}
