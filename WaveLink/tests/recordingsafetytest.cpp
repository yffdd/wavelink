#include "csvrecordingwriter.h"
#include "wlrrecordingwriter.h"
#include "recordingchannelnames.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QBuffer>
#include <QEventLoop>
#include <QTimer>
#include <QFileInfo>

namespace {
bool require(bool condition, const QString &message)
{
    if (condition) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!require(dir.isValid(), QStringLiteral("temporary directory unavailable"))) return 1;

    {
        WlrRecordingWriter timed;
        QString error;
        const auto path = dir.filePath("timed.wlr");
        if (!require(timed.start(path, error, 100), error)) return 100;
        const auto headerSize = QFileInfo(path + ".part").size();
        for (int i = 0; i < 100; ++i) if (!require(timed.append(QVector<double>(12, 42), error), error)) return 101;
        QEventLoop wait; QTimer::singleShot(800, &wait, &QEventLoop::quit); wait.exec();
        if (!require(QFileInfo(path + ".part").size() == headerSize + 11600, "WLR idle timer did not flush complete frames")) return 102;
        if (!require(timed.finish(error), error)) return 103;
        int frames = 0;
        if (!require(WlrRecordingWriter::visitFiles({path}, [&](qint64, const QVector<double> &values, qint64) {
            ++frames; return values == QVector<double>(12, 42);
        }, error) && frames == 100, "timed WLR frames invalid")) return 104;
    }

    const QString base = dir.filePath(QStringLiteral("WaveLog_test.csv"));
    {
        CsvRecordingWriter writer;
        QString error;
        if (!require(writer.start(base, error, 100.0), error)) return 2;
        if (!require(writer.append(QVector<double>{1.25, 2.5}, error, 1700000000000000000LL), error)) return 3;
        if (!require(writer.append(QVector<double>{3.75, 5.0}, error, 1700000000010000000LL), error)) return 31;

        // Recovery must not touch an active recording owned by this process.
        QStringList recoveryErrors;
        if (!require(CsvRecordingWriter::recoverInterruptedFiles(dir.path(), recoveryErrors).isEmpty(),
                     QStringLiteral("active file was incorrectly recovered"))) return 4;
        writer.stop();
    }
    QFile completed(base);
    if (!require(completed.open(QIODevice::ReadOnly), QStringLiteral("completed CSV missing"))) return 5;
    const QByteArray contents = completed.readAll();
    const auto bom = RecordingChannels::csvUtf8Bom();
    if (!require(contents.startsWith(bom + "UnixTime(s;FS=100),SampleIndex,Ch1,Ch2\n") && contents.count(bom) == 1,
                 QStringLiteral("CSV header invalid"))) return 6;
    if (!require(contents.contains("1700000000.000000000,0,1.250000,2.500000\n") &&
                 contents.endsWith("1700000000.010000000,1,3.750000,5.000000\n"),
                 QStringLiteral("CSV host timeline invalid"))) return 7;

    const QString splitBase = dir.filePath(QStringLiteral("WaveLog_split.csv"));
    {
        CsvRecordingWriter writer(90); // Small deterministic limit for this test only.
        QString error;
        if (!require(writer.start(splitBase, error), error)) return 8;
        for (int i = 0; i < 12; ++i) {
            if (!require(writer.append(QVector<double>{double(i), double(i + 1)}, error), error)) return 9;
        }
        writer.stop();
        if (!require(writer.completedFiles().size() > 1,
                     QStringLiteral("recording did not roll over"))) return 10;
        for (const QString &part : writer.completedFiles()) {
            QFile csv(part);
            if (!require(csv.open(QIODevice::ReadOnly), QStringLiteral("split CSV missing"))) return 11;
            if (!require(csv.readLine().startsWith(bom + "UnixTime(s;FS="),
                         QStringLiteral("split CSV header invalid"))) return 12;
        }
    }

    const QString interrupted = dir.filePath(QStringLiteral("WaveLog_crash.csv.part"));
    QFile partial(interrupted);
    if (!require(partial.open(QIODevice::WriteOnly), QStringLiteral("cannot create partial file"))) return 13;
    partial.write("Time,Ch1\n2026-01-01 00:00:00.000,1.000000\nINCOMPLETE");
    partial.close();
    QStringList recoveryErrors;
    const QStringList recovered = CsvRecordingWriter::recoverInterruptedFiles(dir.path(), recoveryErrors);
    if (!require(recoveryErrors.isEmpty(), recoveryErrors.join('\n'))) return 14;
    if (!require(recovered.size() == 1, QStringLiteral("partial file was not recovered"))) return 15;
    QFile recoveredFile(recovered.first());
    if (!require(recoveredFile.open(QIODevice::ReadOnly), QStringLiteral("recovered CSV missing"))) return 16;
    if (!require(!recoveredFile.readAll().contains("INCOMPLETE"),
                 QStringLiteral("incomplete row survived recovery"))) return 17;

    const QString wlrBase = dir.filePath(QStringLiteral("WaveLog_fast.wlr"));
    QStringList wlrParts;
    {
        WlrRecordingWriter writer(150);
        QString error;
        if (!require(writer.start(wlrBase, error, 100.0), error)) return 18;
        for (int i = 0; i < 12; ++i) {
            const qint64 batchTimestamp = 1700000000000000000LL + (i / 3) * 30000000LL;
            if (!require(writer.append(QVector<double>{double(i), double(i + 1)}, error,
                                       batchTimestamp), error)) return 19;
        }
        writer.stop();
        wlrParts = writer.completedFiles();
        if (!require(wlrParts.size() > 1, QStringLiteral("WLR recording did not roll over"))) return 20;
    }
    int visitedFrames = 0;
    QString wlrError;
    double wlrSampleRateHint = 0.0;
    bool wlrUsesHostTime = false;
    if (!require(WlrRecordingWriter::visitFiles(
            wlrParts,
            [&](qint64, const QVector<double> &values, qint64) {
                ++visitedFrames;
                return values.size() == 2;
            }, wlrError, &wlrSampleRateHint, &wlrUsesHostTime), wlrError)) return 21;
    if (!require(visitedFrames == 12, QStringLiteral("WLR frame count mismatch"))) return 22;
    if (!require(wlrUsesHostTime && wlrSampleRateHint == 100.0,
                 QStringLiteral("WLR timing metadata mismatch"))) return 33;
    QByteArray exportedCsv;
    QBuffer csvBuffer(&exportedCsv);
    csvBuffer.open(QIODevice::WriteOnly);
    if (!require(WlrRecordingWriter::exportToCsv(wlrParts, &csvBuffer, wlrError), wlrError)) return 23;
    if (!require(exportedCsv.startsWith(bom + "UnixTime(s;FS=100),SampleIndex,Ch1,Ch2\n") && exportedCsv.count(bom) == 1,
                 QStringLiteral("WLR CSV conversion failed"))) return 24;
    if (!require(exportedCsv.contains("1700000000.000000000,1,1.000000,2.000000\n"),
                 QStringLiteral("WLR host timeline invalid"))) return 32;

    const QString crashedWlr = dir.filePath(QStringLiteral("WaveLog_binary_crash.wlr.part"));
    if (!require(QFile::copy(wlrParts.first(), crashedWlr), QStringLiteral("cannot create crashed WLR"))) return 25;
    QFile damaged(crashedWlr);
    if (!require(damaged.open(QIODevice::Append), QStringLiteral("cannot damage WLR"))) return 26;
    damaged.write("BROKEN_FRAME");
    damaged.close();
    QStringList wlrRecoveryErrors;
    const QStringList recoveredWlr = WlrRecordingWriter::recoverInterruptedFiles(dir.path(), wlrRecoveryErrors);
    if (!require(wlrRecoveryErrors.isEmpty(), wlrRecoveryErrors.join('\n'))) return 27;
    if (!require(recoveredWlr.size() == 1, QStringLiteral("WLR crash file was not recovered"))) return 28;
    int recoveredFrames = 0;
    wlrError.clear();
    if (!require(WlrRecordingWriter::visitFiles(
            recoveredWlr,
            [&](qint64, const QVector<double> &, qint64) {
                ++recoveredFrames;
                return true;
            }, wlrError), wlrError)) return 29;
    if (!require(recoveredFrames > 0, QStringLiteral("recovered WLR contains no valid frames"))) return 30;
    // Names survive UTF-8, quoted CSV headers, WLR extension, conversion,
    // size-based splitting and crash recovery. Legacy WLR has no extension.
    const QStringList names{QStringLiteral("温度,环境"), QStringLiteral("电压\"A\""), "Ch3"};
    CsvRecordingWriter namedCsv(190); WlrRecordingWriter namedWlr(180);
    QString namedError;
    if (!require(namedCsv.start(dir.filePath("named.csv"), namedError, 100, names), namedError)) return 40;
    if (!require(namedWlr.start(dir.filePath("named.wlr"), namedError, 100, names), namedError)) return 41;
    for (int i = 0; i < 8; ++i) {
        if (!require(namedCsv.append({1, 2, 3}, namedError) && namedWlr.append({1, 2, 3}, namedError), namedError)) return 42;
    }
    if (!require(namedCsv.finish(namedError) && namedWlr.finish(namedError), namedError)) return 43;
    if (!require(namedCsv.completedFiles().size() > 1 && namedWlr.completedFiles().size() > 1, "named recordings split")) return 52;
    for (const auto &path : namedCsv.completedFiles()) {
        QFile file(path); if (!require(file.open(QIODevice::ReadOnly), "open named CSV")) return 44;
        const auto data = file.readAll();
        if (!require(data.startsWith(bom) && data.count(bom) == 1 && data.contains(names[0].toUtf8()), "named CSV UTF-8 BOM")) return 53;
        QBuffer input; input.setData(data); input.open(QIODevice::ReadOnly); QTextStream stream(&input); stream.setCodec("UTF-8");
        QStringList cells; const auto line = stream.readLine();
        if (!require(RecordingChannels::parseCsvRow(line, cells) && cells.first() == "UnixTime(s;FS=100)" && cells.mid(2) == names, "named CSV header")) return 45;
    }
    QStringList restoredNames; int restoredCount = 0;
    if (!require(WlrRecordingWriter::visitFiles(namedWlr.completedFiles(), [&](qint64, const QVector<double> &v, qint64) {
        ++restoredCount; return v == QVector<double>({1, 2, 3});
    }, namedError, nullptr, nullptr, &restoredNames) && restoredNames == names && restoredCount == 8, "named WLR round-trip")) return 46;
    QByteArray namedExport; QBuffer namedBuffer(&namedExport); namedBuffer.open(QIODevice::WriteOnly);
    if (!require(WlrRecordingWriter::exportToCsv(namedWlr.completedFiles(), &namedBuffer, namedError), namedError)) return 47;
    if (!require(namedExport.startsWith(bom) && namedExport.count(bom) == 1, "WLR CSV UTF-8 BOM once")) return 54;
    QStringList exportedCells;
    if (!require(RecordingChannels::parseCsvRow(QString::fromUtf8(namedExport.split('\n').first()), exportedCells) && exportedCells.mid(2) == names, "named WLR CSV export")) return 48;
    const QString crashedNamed = dir.filePath("named_recovery.wlr.part");
    if (!require(QFile::copy(namedWlr.completedFiles().first(), crashedNamed), "copy named recovery")) return 49;
    QStringList namedRecoveryErrors; auto namedRecovery = WlrRecordingWriter::recoverInterruptedFiles(dir.path(), namedRecoveryErrors);
    if (!require(namedRecoveryErrors.isEmpty() && namedRecovery.size() == 1 && WlrRecordingWriter::visitFiles(namedRecovery,
        [](qint64, const QVector<double> &, qint64) { return true; }, namedError, nullptr, nullptr, &restoredNames) && restoredNames == names, "named WLR recovery")) return 50;
    if (!require(!namedCsv.start(dir.filePath("invalid.csv"), namedError, 100, {"bad\nname"}) && !namedWlr.start(dir.filePath("invalid.wlr"), namedError, 100, {"bad\nname"}), "reject unsafe names")) return 51;
    // Merge independently usable parts without interior BOMs or repeated
    // headers. Preserve every data line and all names exactly as recorded.
    QByteArray merged; QBuffer mergedBuffer(&merged); mergedBuffer.open(QIODevice::WriteOnly);
    if (!require(CsvRecordingWriter::exportToCsv(namedCsv.completedFiles(), &mergedBuffer, namedError), namedError)) return 55;
    if (!require(merged.startsWith(bom) && merged.count(bom) == 1 && merged.count("UnixTime") == 1 && merged.count('\n') == 9,
                 "merged CSV one BOM/header and all eight frames")) return 56;
    QBuffer mergedInput; mergedInput.setData(merged); mergedInput.open(QIODevice::ReadOnly); QTextStream mergedStream(&mergedInput); mergedStream.setCodec("UTF-8");
    QStringList mergedNames;
    if (!require(RecordingChannels::parseCsvRow(mergedStream.readLine(), mergedNames) && mergedNames.mid(2) == names, "merged CSV names")) return 57;

    QFile firstPart(namedCsv.completedFiles().first()); if (!require(firstPart.open(QIODevice::ReadOnly), "read first part")) return 58;
    const auto firstData = firstPart.readAll(); firstPart.close();
    const auto legacyPath = dir.filePath("legacy_named.csv"); QFile legacyNamed(legacyPath);
    const auto legacyData = firstData.mid(bom.size());
    if (!require(legacyNamed.open(QIODevice::WriteOnly) && legacyNamed.write(legacyData) == legacyData.size(), "legacy UTF-8 fixture")) return 59;
    legacyNamed.close();
    for (const QStringList &files : {QStringList{legacyPath}, QStringList{legacyPath, namedCsv.completedFiles().first()},
                                   QStringList{namedCsv.completedFiles().first(), legacyPath}}) {
        QByteArray converted; QBuffer output(&converted); output.open(QIODevice::WriteOnly);
        if (!require(CsvRecordingWriter::exportToCsv(files, &output, namedError), namedError)) return 60;
        const auto expected = firstData + (files.size() > 1 ? legacyData.mid(legacyData.indexOf('\n') + 1) : QByteArray());
        if (!require(converted == expected, "legacy/new CSV export preserves bytes with one BOM")) return 61;
    }
    if (!require(legacyNamed.open(QIODevice::ReadOnly) && legacyNamed.readAll() == legacyData, "export did not modify legacy source")) return 62;
    legacyNamed.close();

    const auto crashedCsv = dir.filePath("named_crash.csv.part");
    if (!require(QFile::copy(namedCsv.completedFiles().first(), crashedCsv), "copy interrupted UTF-8 CSV")) return 63;
    QFile brokenCsv(crashedCsv); if (!require(brokenCsv.open(QIODevice::Append), "append partial CSV row")) return 64;
    brokenCsv.write("INCOMPLETE"); brokenCsv.close();
    QStringList csvRecoveryErrors; const auto recoveredCsv = CsvRecordingWriter::recoverInterruptedFiles(dir.path(), csvRecoveryErrors);
    if (!require(csvRecoveryErrors.isEmpty() && recoveredCsv.size() == 1, "recover UTF-8 CSV")) return 65;
    QFile repaired(recoveredCsv.first());
    if (!require(repaired.open(QIODevice::ReadOnly) && repaired.readAll() == firstData, "recovery preserves BOM, names and complete data")) return 66;
    QBuffer readOnly; readOnly.open(QIODevice::ReadOnly);
    if (!require(!CsvRecordingWriter::exportToCsv({legacyPath}, &readOnly, namedError) && !namedError.isEmpty(), "reject unwritable CSV export")) return 67;
    return 0;
}
