#pragma once
#include <QStringList>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QLockFile>
#include <QFile>
#include <algorithm>
#include <cmath>

// Shared by CSV/WLR recording, conversion and import. CSV files are UTF-8
// with a BOM so spreadsheet applications can recognize non-ASCII names;
// commas and quotes are escaped instead of being mistaken for more channels.
namespace RecordingChannels {
inline QByteArray csvReceiveTimeColumn(double sampleRate) {
    return "UnixTime(s;FS=" + QByteArray::number(sampleRate, 'g', 12) + ')';
}
inline bool parseReceiveTimeColumn(const QString &column, double &sampleRate) {
    static const QRegularExpression expression(QStringLiteral(
        "^(?:UnixTime|HostReceiveTime)\\(s(?:;(?:FS|Hint)=([0-9eE+.-]+))?\\)$"));
    const auto match = expression.match(column);
    if (!match.hasMatch()) return false;
    bool ok = false;
    const double rate = match.captured(1).toDouble(&ok);
    sampleRate = ok && std::isfinite(rate) && rate > 0 ? rate : 0.0;
    return true;
}
inline bool cacheFilesIdle(const QFileInfoList &files) {
    for (const auto &file : files) if (file.fileName().endsWith(".part")) {
        QLockFile lock(file.absoluteFilePath() + ".lock"); lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) return false;
    }
    return true;
}
inline bool removeCacheFile(const QFileInfo &file) {
    // Never unlink the lock protecting another process's live recording.
    if (file.fileName().endsWith(".part.lock")) return true;
    if (!QFile::exists(file.absoluteFilePath())) return true;
    if (file.fileName().endsWith(".part")) {
        QLockFile lock(file.absoluteFilePath() + ".lock"); lock.setStaleLockTime(0);
        return lock.tryLock(0) && QFile::remove(file.absoluteFilePath());
    }
    return QFile::remove(file.absoluteFilePath());
}
// Keep the familiar six-decimal layout ONLY when it is lossless. Otherwise,
// seventeen significant digits round-trip the original IEEE-754 double.
inline QByteArray csvNumber(double value) {
    const auto friendly = QByteArray::number(value, 'f', 6);
    if (friendly.toDouble() == value) return friendly;
    return QByteArray::number(value, 'g', 17);
}
inline QByteArray csvUtf8Bom() { return QByteArray("\xEF\xBB\xBF", 3); }
inline QFileInfoList recordingParts(const QFileInfo &base) {
    const QRegularExpression expression("^" + QRegularExpression::escape(base.completeBaseName())
        + "_part(\\d{3,})\\." + QRegularExpression::escape(base.suffix()) + "$");
    QFileInfoList parts;
    for (const auto &file : base.dir().entryInfoList({base.completeBaseName() + "_part*." + base.suffix()}, QDir::Files, QDir::Name))
        if (expression.match(file.fileName()).hasMatch()) parts.append(file);
    std::sort(parts.begin(), parts.end(), [&](const QFileInfo &a, const QFileInfo &b) {
        return expression.match(a.fileName()).captured(1).toULongLong() < expression.match(b.fileName()).captured(1).toULongLong();
    });
    return parts;
}
inline bool valid(const QStringList &names) {
    if (names.size() > 256) return false;
    for (const auto &name : names) {
        if (name.size() > 128) return false;
        for (auto ch : name) if (ch.isNull() || ch.category() == QChar::Other_Control) return false;
    }
    return true;
}
inline QString name(const QStringList &names, int index) {
    return index < names.size() && !names[index].trimmed().isEmpty()
        ? names[index] : QStringLiteral("Ch%1").arg(index + 1);
}
inline QStringList normalized(const QStringList &names, int count) {
    QStringList result;
    for (int i = 0; i < count; ++i) result.append(name(names, i));
    return result;
}
inline QString csvCell(QString value) {
    if (value.contains(',') || value.contains('"')) {
        value.replace("\"", "\"\""); value = '"' + value + '"';
    }
    return value;
}
inline QByteArray csvColumns(const QStringList &names, int count) {
    QByteArray result;
    for (int i = 0; i < count; ++i) result += ',' + csvCell(name(names, i)).toUtf8();
    return result;
}
inline bool parseCsvRow(const QString &line, QStringList &cells) {
    cells.clear(); QString cell; bool quoted = false, closed = false;
    for (int i = 0; i < line.size(); ++i) {
        const auto ch = line[i];
        if (quoted) {
            if (ch != '"') cell += ch;
            else if (i + 1 < line.size() && line[i + 1] == '"') { cell += '"'; ++i; }
            else { quoted = false; closed = true; }
        } else if (ch == ',') { cells.append(cell); cell.clear(); closed = false; }
        else if (closed) return false;
        else if (ch == '"') { if (!cell.isEmpty()) return false; quoted = true; }
        else cell += ch;
    }
    if (quoted) return false;
    cells.append(cell); return true;
}
}
