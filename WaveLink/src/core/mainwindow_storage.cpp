#include "mainwindow.h"
#include "updatemanager.h"
#include "recordingchannelnames.h"
#include "waveimportbudget.h"
#include "systemsettingsdialog.h"
#include <QTime>
#include <QProcess>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include "ffthelper.h"
#include "demodatagenerator.h"
#include <QOpenGLContext>
#include "debuglogger.h"
#include "commandpanelwindow.h"
#include "helpdialog.h"
#include <QSerialPortInfo>
#include <QMessageBox>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollBar>
#include <QScrollArea>
#include <QListWidget>
#include <QLineEdit>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFileDialog>
#include <QProgressBar>
#include <QTimer>
#include <QFile>
#include <QSaveFile>
#include <QCoreApplication>
#include <QMenu>
#include <QSettings>
#include <QTextCodec>
#include <QColorDialog>
#include <QProgressDialog>
#include <QScreen>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QSignalBlocker>
#include <QDesktopServices>
#include <QUrl>
#include <QPushButton>
#include <QDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QStorageInfo>
#include <atomic>
#include <climits>
#include <cmath>
#include <limits>

namespace {
class RecordingSaveDialog final : public QDialog {
public:
    using QDialog::QDialog;
    void reject() override {} // Keep the progress visible until the file is finalized.
};

bool rebuildUniformTimeline(QVector<double> &times, double sampleRateHint)
{
    if (times.size() < 2) return false;

    // A receive timestamp belongs to the complete batch decoded from one OS
    // read. Use each batch end as a timing observation and fit elapsed host
    // time against the cumulative frame index. This preserves the measured
    // average duration while removing transport batching from the plot axis.
    QVector<double> indices;
    QVector<double> observations;
    int groupStart = 0;
    for (int i = 1; i <= times.size(); ++i) {
        const bool groupEnded = i == times.size() || qAbs(times[i] - times[groupStart]) > 1e-6;
        if (!groupEnded) continue;
        indices.append(i - 1);
        observations.append(times[groupStart]);
        groupStart = i;
    }

    double interval = 0.0;
    if (indices.size() >= 2) {
        double meanX = 0.0, meanY = 0.0;
        for (int i = 0; i < indices.size(); ++i) {
            meanX += indices[i];
            meanY += observations[i];
        }
        meanX /= indices.size();
        meanY /= observations.size();
        double covariance = 0.0, variance = 0.0;
        for (int i = 0; i < indices.size(); ++i) {
            const double dx = indices[i] - meanX;
            covariance += dx * (observations[i] - meanY);
            variance += dx * dx;
        }
        if (variance > 0.0) interval = covariance / variance;
    }
    if (!(interval > 0.0) && sampleRateHint > 0.0) interval = 1.0 / sampleRateHint;
    if (!(interval > 0.0) || !qIsFinite(interval)) return false;

    for (int i = 0; i < times.size(); ++i) times[i] = i * interval;
    return true;
}
}

void MainWindow::writeDemoLogData(const QVector<double> &data)
{
    if ((m_demoRecorder.isActive() || m_demoWlrRecorder.isActive()) && !data.isEmpty()) {
        if (!m_demoRecordingChannels.accept(data.size())) return;
        QString error;
        const qint64 receiveTimestampNs = demoTimer
            ? demoTimer->property("currentReceiveTimestampNs").toLongLong() : 0;
        const bool useWlr = currentRecordFile.endsWith(QStringLiteral(".wlr"), Qt::CaseInsensitive);
        const bool written = useWlr
            ? m_demoWlrRecorder.append(data, error, receiveTimestampNs)
            : m_demoRecorder.append(data, error, receiveTimestampNs);
        if (!written) {
            handleRecordingError(error);
            return;
        }
        demoLoggedBytes = useWlr
            ? m_demoWlrRecorder.totalBytes()
            : m_demoRecorder.totalBytes();
    }
}

bool MainWindow::hasEnoughDiskSpaceForRecording(QString *details) const
{
    const QString cachePath = QDir(editCacheDir->text()).absoluteFilePath(QStringLiteral("WaveLink_Cache"));
    QStorageInfo storage(cachePath);
    if (!storage.isValid() || !storage.isReady()) {
        // File creation will still provide a concrete error. Do not show a
        // speculative low-space warning when the OS cannot report capacity.
        return true;
    }
    const qint64 required = qint64(spinCacheLimit->value()) * 1024LL * 1024LL;
    const qint64 available = storage.bytesAvailable();
    if (available >= required) return true;
    if (details) {
        *details = QStringLiteral(
            "录制所在磁盘的剩余空间不足。\n\n"
            "缓存文件夹：\n%1\n\n"
            "磁盘可用空间：%2 MB\n"
            "安全空间要求：%3 MB（采用当前磁盘缓存上限）\n\n"
            "请释放磁盘空间或选择其他缓存路径后再录制。")
            .arg(QDir::toNativeSeparators(cachePath))
            .arg(available / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(spinCacheLimit->value());
    }
    return false;
}

void MainWindow::recoverInterruptedRecordings()
{
    const QString cachePath = QDir(editCacheDir->text()).absoluteFilePath(QStringLiteral("WaveLink_Cache"));
    QStringList errors;
    QStringList recovered = CsvRecordingWriter::recoverInterruptedFiles(cachePath, errors);
    recovered.append(WlrRecordingWriter::recoverInterruptedFiles(cachePath, errors));
    if (recovered.isEmpty() && errors.isEmpty()) return;

    QString message;
    if (!recovered.isEmpty()) {
        QFileInfo recoveredInfo(recovered.last());
        QString baseStem = recoveredInfo.completeBaseName();
        baseStem.remove(QRegularExpression(QStringLiteral("_part\\d{3,}$")));
        const QString suffix = recoveredInfo.suffix().isEmpty()
            ? QStringLiteral("csv") : recoveredInfo.suffix();
        const QString baseCandidate = recoveredInfo.dir().absoluteFilePath(
            baseStem + QStringLiteral(".") + suffix);
        currentRecordFile = QFile::exists(baseCandidate) ? baseCandidate : recovered.last();
        message += QStringLiteral("检测到上次程序异常退出时未完成的录制，已恢复以下文件：\n\n%1")
                       .arg(recovered.join(QStringLiteral("\n")));
    }
    if (!errors.isEmpty()) {
        if (!message.isEmpty()) message += QStringLiteral("\n\n");
        message += QStringLiteral("以下文件未能自动恢复：\n%1").arg(errors.join(QStringLiteral("\n")));
    }
    QMessageBox::information(this, QStringLiteral("录制文件恢复"), message);
}

void MainWindow::stopRecordingSafely(const QString &errorMessage)
{
    if (!isRecording && errorMessage.isEmpty()) return;
    if (m_bleRecordStopPending) return;
    if (lblRecordingWarning) { lblRecordingWarning->clear(); lblRecordingWarning->hide(); }
    if (m_activeInterfaceType == 4 && bleWorker && bleThread && bleThread->isRunning()) {
        // A slow CSV flush must not block clicks or the connection state machine.
        // The ordered stop task finishes all previously accepted notifications.
        m_bleRecordStopPending = true; m_bleRecordStopMessage = errorMessage;
        isRecording = false; recordTimer->stop();
        btnToggleRecord->setText(QStringLiteral("正在结束录制…"));
        bleWorker->stopRecording(++m_bleRecordRequest);
        updateBleControls(); return;
    }
    QString finalError = errorMessage;

    // Wait until worker-owned files have been flushed, closed and renamed from
    // .part to .csv before allowing export or application shutdown.
    if (serialHandler && serialThread && serialThread->isRunning()) {
        QString error;
        QMetaObject::invokeMethod(serialHandler, "finishRecording", Qt::BlockingQueuedConnection, Q_RETURN_ARG(QString, error));
        if (finalError.isEmpty()) finalError = error;
    }
    if (networkHandler && networkThread && networkThread->isRunning()) {
        QString error;
        QMetaObject::invokeMethod(networkHandler, "finishRecording", Qt::BlockingQueuedConnection, Q_RETURN_ARG(QString, error));
        if (finalError.isEmpty()) finalError = error;
    }
    if (bleWorker && bleThread && bleThread->isRunning()) {
        QString bleError;
        QMetaObject::invokeMethod(bleWorker, "stopRecordingNow", Qt::BlockingQueuedConnection, Q_RETURN_ARG(QString, bleError));
        if (m_activeInterfaceType == 4 && finalError.isEmpty() && !bleError.isEmpty())
            finalError = QStringLiteral("BLE 录制文件未能完整写入或完成收尾，已停止录制；已落盘的临时文件保留，可在下次启动时恢复。\n\n%1").arg(bleError);
        ++m_bleRecordRequest; // Ignore acknowledgements from the stopped recording.
    }
    QString demoCsvError, demoWlrError;
    const bool demoCsvOk = m_demoRecorder.finish(demoCsvError), demoWlrOk = m_demoWlrRecorder.finish(demoWlrError);
    m_demoRecordingChannels.reset();
    if (finalError.isEmpty()) finalError = !demoCsvOk ? demoCsvError : !demoWlrOk ? demoWlrError : QString();
    demoLoggedBytes = currentRecordFile.endsWith(QStringLiteral(".wlr"), Qt::CaseInsensitive)
        ? m_demoWlrRecorder.totalBytes() : m_demoRecorder.totalBytes();

    isRecording = false;
    recordTimer->stop();
    btnToggleRecord->setText(QStringLiteral("开始录制"));
    btnToggleRecord->setStyleSheet(QString());
    if (comboRecordingMode) comboRecordingMode->setEnabled(true);
    if (bleClient) updateBleControls();
    if (!finalError.isEmpty()) {
        lblRecordStatus->setText(QStringLiteral("录制已停止（写入失败）"));
        QMessageBox::critical(this, QStringLiteral("录制已安全停止"), finalError);
    }
}

bool MainWindow::prepareCacheForRecording()
{
    const QString cachePath = QDir::cleanPath(
        QDir(editCacheDir->text()).absoluteFilePath("WaveLink_Cache"));
    QDir dir(cachePath);
    if (!dir.exists()) return true;

    qint64 totalSize = 0;
    const QFileInfoList list = dir.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo &info : list) {
        totalSize += info.size();
    }

    const qint64 limit = spinCacheLimit->value() * 1024LL * 1024LL;
    if (totalSize <= limit) return true;

    QDialog prompt(this);
    prompt.setWindowTitle(QStringLiteral("缓存已超过上限"));
    prompt.setMinimumWidth(520);
    QVBoxLayout *promptLayout = new QVBoxLayout(&prompt);
    promptLayout->setContentsMargins(20, 20, 20, 20);
    promptLayout->setSpacing(12);
    QLabel *message = new QLabel(&prompt);
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    message->setText(QStringLiteral(
        "缓存已超过设置上限，请选择如何开始录制。\n\n"
        "当前缓存：%1 MB\n设置上限：%2 MB\n\n"
        "缓存文件夹：\n%3\n\n"
        "选择清空将删除该文件夹内的全部缓存文件，删除后无法通过软件恢复。\n"
        "打开文件夹或关闭此窗口将取消本次录制。")
        .arg(totalSize / (1024.0 * 1024.0), 0, 'f', 2)
        .arg(spinCacheLimit->value())
        .arg(QDir::toNativeSeparators(cachePath)));
    message->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    promptLayout->addWidget(message);
    int selectedAction = 0;
    const QStringList choices = {
        QStringLiteral("保留缓存并开始录制"),
        QStringLiteral("清空缓存并开始录制"),
        QStringLiteral("打开缓存文件夹自行处理")
    };
    const bool darkTheme = !comboTheme || comboTheme->currentIndex() == 0;
    const QStringList borders = { "#49b878", "#e06464", "#4a9fe0" };
    const QStringList backgrounds = darkTheme
        ? QStringList{ "#234b36", "#512c30", "#263f59" }
        : QStringList{ "#eaf8ef", "#fff0f0", "#eaf3ff" };
    const QStringList foregrounds = darkTheme
        ? QStringList{ "#ddffe9", "#ffe2e2", "#e0f0ff" }
        : QStringList{ "#17663a", "#8f2525", "#195d93" };
    const QStringList hoverColors = darkTheme
        ? QStringList{ "#306449", "#704047", "#355775" }
        : QStringList{ "#d3f1df", "#ffdddd", "#d3e7ff" };
    for (int i = 0; i < choices.size(); ++i) {
        QPushButton *button = new QPushButton(choices[i], &prompt);
        button->setMinimumHeight(42);
        button->setAutoDefault(false);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { border: 2px solid %1; border-radius: 5px; "
            "background-color: %2; color: %3; font-weight: bold; padding: 8px 14px; }"
            "QPushButton:hover { background-color: %4; }"
            "QPushButton:pressed { background-color: %1; }"
            "QPushButton:focus { border-style: solid; background-color: %4; }")
            .arg(borders[i], backgrounds[i], foregrounds[i], hoverColors[i]));
        promptLayout->addWidget(button);
        connect(button, &QPushButton::clicked, &prompt, [&prompt, &selectedAction, i]() {
            selectedAction = i + 1;
            prompt.accept();
        });
    }
    // Closing or pressing Escape leaves selectedAction at zero (cancel).
    prompt.exec();

    if (selectedAction == 1) return true;
    if (selectedAction == 3) {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(cachePath))) {
            QMessageBox::warning(this, QStringLiteral("打开文件夹失败"),
                                 QStringLiteral("无法打开缓存文件夹：\n%1")
                                     .arg(QDir::toNativeSeparators(cachePath)));
        }
        return false;
    }
    if (selectedAction != 2) return false;
    if (!RecordingChannels::cacheFilesIdle(list)) {
        QMessageBox::warning(this, "缓存正在使用", "检测到其他实例正在录制此缓存文件夹，已取消清空，本次录制未开始。请先停止其他实例的录制。"); return false;
    }

    // Only remove the files shown in this check, never recurse into directories.
    QStringList failedFiles;
    for (const QFileInfo &info : list) {
        if (!RecordingChannels::removeCacheFile(info)) {
            failedFiles.append(info.fileName());
        }
    }
    if (!failedFiles.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缓存未完全清空"),
                             QStringLiteral("部分文件无法删除，本次录制已取消。\n"
                                            "请进入缓存文件夹处理后重试：\n%1\n\n%2")
                                 .arg(QDir::toNativeSeparators(cachePath))
                                 .arg(failedFiles.mid(0, 10).join("\n")));
        return false;
    }
    return true;
}

qint64 MainWindow::getDirSize(const QString &dirPath)
{
    qint64 size = 0;
    QDir dir(dirPath);
    QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : list) {
        if (info.isDir()) {
            size += getDirSize(info.absoluteFilePath());
        } else {
            size += info.size();
        }
    }
    return size;
}

void MainWindow::onClearCache()
{
    if (isRecording || m_bleRecordStopPending) {
        QMessageBox::warning(this, QString::fromUtf8("警告"), QString::fromUtf8("当前正在录制数据！\n请先停止录制（建议同时保存重要数据）后再清空缓存。"));
        return;
    }
    QDir dir(QDir(editCacheDir->text()).absoluteFilePath("WaveLink_Cache"));
    const QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System);
    if (!dir.exists() || list.isEmpty()) {
        QMessageBox::information(this, "清空缓存", "缓存文件夹中没有需要清理的文件，未做任何修改。"); return;
    }
    const QString confirmedPath = dir.canonicalPath();
    qint64 totalBytes = 0; QMap<QString, QString> confirmedFiles;
    for (const auto &file : list) { totalBytes += file.size(); confirmedFiles.insert(file.absoluteFilePath(), file.canonicalFilePath()); }
    QDialog dialog(this); dialog.setObjectName("cacheClearConfirm"); dialog.setWindowTitle("确认清空磁盘录制缓存");
    dialog.setSizeGripEnabled(true); dialog.resize(600, 360); dialog.setMinimumSize(480, 300);
    auto layout = new QVBoxLayout(&dialog); layout->setContentsMargins(20, 20, 20, 16); layout->setSpacing(14);
    auto message = new QLabel(QString("将删除以下缓存文件夹中的 %1 个文件，约 %2 MB：\n%3\n\n"
        "作用：释放磁盘空间，删除其中的 CSV / WLR 录制文件、分卷及未恢复的临时录制文件。尚未另存的重要录制也会被删除，软件无法撤销。\n\n"
        "不会清空当前显示的波形或绘图内存，不会删除该文件夹之外已另存的数据，也不会递归清理子文件夹。\n\n"
        "请先通过“保存数据”另存重要录制。是否确认清空？")
        .arg(list.size()).arg(totalBytes / (1024.0 * 1024.0), 0, 'f', 2).arg(QDir::toNativeSeparators(confirmedPath)), &dialog);
    message->setObjectName("cacheClearMessage"); message->setTextFormat(Qt::PlainText); message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(message);
    auto buttons = new QDialogButtonBox; auto confirm = buttons->addButton("确认清空", QDialogButtonBox::DestructiveRole);
    confirm->setObjectName("confirmClearCache"); confirm->setAutoDefault(false);
    confirm->setStyleSheet("QPushButton { background-color: #ad3434; color: white; font-weight: bold; padding: 6px 16px; } QPushButton:hover { background-color: #c34343; }");
    auto cancel = buttons->addButton("取消", QDialogButtonBox::RejectRole); cancel->setDefault(true);
    connect(confirm, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    // Modal dialogs keep processing events. Recheck the current folder and all
    // active recordings, but delete only the immutable, confirmed snapshot.
    if (isRecording || m_bleRecordStopPending) {
        QMessageBox::warning(this, "清空已取消", "录制状态已变化，请先停止录制；文件未删除。"); return;
    }
    const QDir currentDir(QDir(editCacheDir->text()).absoluteFilePath("WaveLink_Cache"));
    if (confirmedPath.isEmpty() || currentDir.canonicalPath() != confirmedPath || dir.canonicalPath() != confirmedPath) {
        QMessageBox::warning(this, "清空已取消", "缓存路径已变化；文件未删除，请重新检查后操作。"); return;
    }
    {
        if (!RecordingChannels::cacheFilesIdle(dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System))) {
            QMessageBox::warning(this, "缓存正在使用", "其他实例正在录制此缓存文件夹，已取消清空，文件未改变。请先停止其他实例的录制。"); return;
        }
        QStringList failedFiles;
        for (const QFileInfo &info : list) {
            const QFileInfo current(info.absoluteFilePath());
            if (!current.exists() && !current.isSymLink()) continue;
            if (current.size() != info.size() || current.lastModified() != info.lastModified() || current.birthTime() != info.birthTime()
                || current.isSymLink() != info.isSymLink() || current.canonicalFilePath() != confirmedFiles.value(info.absoluteFilePath())
                || !RecordingChannels::removeCacheFile(current)) {
                failedFiles.append(info.fileName());
            }
        }
        if (failedFiles.isEmpty()) {
            QMessageBox::information(this, "清理缓存", dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System).isEmpty()
                ? "缓存清理完成！" : "已清理本次确认的缓存文件；确认期间新增的文件已保留。");
        } else {
            QMessageBox::warning(this, "清理缓存",
                                 QString("以下 %1 个文件已变化或无法删除，本次保留：\n%2")
                                     .arg(failedFiles.size())
                                     .arg(failedFiles.mid(0, 10).join("\n")));
        }
    }
    if (!currentRecordFile.isEmpty() && !QFile::exists(currentRecordFile)) currentRecordFile.clear();
    if (dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System).isEmpty()) lblRecordStatus->setText("00:00:00 | 0.00 MB");
}

void MainWindow::onToggleRecord()
{
    if (!isRecording && (m_protocolApplyPending || !protocolUiMatchesApplied() || !protocolHasNumericData())) {
        QMessageBox::information(this, QStringLiteral("无法开始录制"), QStringLiteral("请先选择并应用数值数据协议。Raw 文本/HEX 模式不生成波形录制文件。")); return;
    }
    if (m_bleRecordStopPending) return;
    if (!isRecording && comboInterface->currentData().toInt() == 4 &&
        (!bleClient->canReceive() || comboProtocol->currentIndex() == 0)) {
        QMessageBox::information(this, QStringLiteral("无法开始 BLE 录制"), QStringLiteral("请先配置接收特征，并选择匹配外设数据的数值协议。Raw 文本/HEX 模式不生成波形录制文件。")); return;
    }
    if (!isRecording) {
        const auto hasActiveConnection = [this]() {
            return connectionIsReady();
        };
        if (!hasActiveConnection()) {
            QMessageBox::warning(this, QStringLiteral("无法开始录制"),
                                 QStringLiteral("请先开启连接，再开始录制数据。"));
            return;
        }
        if (!prepareCacheForRecording()) return;
        // Modal dialogs process events; the device may disconnect meanwhile.
        if (!hasActiveConnection()) {
            QMessageBox::warning(this, QStringLiteral("无法开始录制"),
                                 QStringLiteral("连接已关闭，请重新开启连接后再开始录制。"));
            return;
        }
        if (chkClearBeforeRecord->isChecked()) {
            onClearPlots();
        }
        isRecording = true;
        if (lblRecordingWarning) { lblRecordingWarning->clear(); lblRecordingWarning->hide(); }
        if (comboRecordingMode) comboRecordingMode->setEnabled(false);
        btnToggleRecord->setText("停止录制");
        btnToggleRecord->setStyleSheet("color: #ff5555; font-weight: bold;");

        const bool isDemoActive = (demoTimer && demoTimer->isActive());
        if (isPortOpen || isDemoActive) {
            const QString baseDir = editCacheDir->text();
            QDir dir(QDir(baseDir).absoluteFilePath("WaveLink_Cache"));
            if ((!dir.exists() && !dir.mkpath(".")) || !dir.exists()) {
                isRecording = false;
                if (comboRecordingMode) comboRecordingMode->setEnabled(true);
                btnToggleRecord->setText(QStringLiteral("开始录制"));
                btnToggleRecord->setStyleSheet(QString());
                QMessageBox::critical(this, QStringLiteral("录制失败"),
                                      QStringLiteral("无法创建缓存目录：\n%1").arg(dir.absolutePath()));
                return;
            }
            QString diskWarning;
            if (!hasEnoughDiskSpaceForRecording(&diskWarning)) {
                isRecording = false;
                if (comboRecordingMode) comboRecordingMode->setEnabled(true);
                btnToggleRecord->setText(QStringLiteral("开始录制"));
                btnToggleRecord->setStyleSheet(QString());
                QMessageBox::critical(this, QStringLiteral("磁盘空间不足"), diskWarning);
                return;
            }
            const QString extension = comboRecordingMode && comboRecordingMode->currentIndex() == 1
                ? QStringLiteral("wlr") : QStringLiteral("csv");
            currentRecordFile = dir.absoluteFilePath(
                QString("WaveLog_%1.%2")
                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"), extension));

            if (isPortOpen) {
                const int interfaceType = comboInterface->currentData().toInt();
                if (interfaceType == 0) {
                    QMetaObject::invokeMethod(serialHandler, "startRecording", Qt::QueuedConnection,
                                              Q_ARG(QString, currentRecordFile),
                                              Q_ARG(double, spinFs->value()));
                } else if (isNetworkInterface(interfaceType)) {
                    QMetaObject::invokeMethod(networkHandler, "startRecording", Qt::QueuedConnection,
                                              Q_ARG(QString, currentRecordFile),
                                              Q_ARG(double, spinFs->value()));
                } else if (interfaceType == 4) {
                    bleWorker->startRecording(currentRecordFile, spinFs->value(), ++m_bleRecordRequest);
                }
            }

            if (isDemoActive) {
                m_demoRecordingChannels.reset();
                QString error;
                const bool useWlr = currentRecordFile.endsWith(QStringLiteral(".wlr"), Qt::CaseInsensitive);
                const double demoSampleRate = spinDemoInterval && spinDemoInterval->value() > 0
                    ? 1000.0 / spinDemoInterval->value()
                    : spinFs->value();
                m_demoWlrRecorder.setErrorHandler([this](const QString &error) {
                    QTimer::singleShot(0, this, [this, error] { if (isRecording) stopRecordingSafely(error); });
                });
                const bool started = useWlr
                    ? m_demoWlrRecorder.start(currentRecordFile, error, demoSampleRate)
                    : m_demoRecorder.start(currentRecordFile, error, demoSampleRate);
                if (started) {
                    demoLoggedBytes = 0;
                } else {
                    isRecording = false;
                    if (comboRecordingMode) comboRecordingMode->setEnabled(true);
                    btnToggleRecord->setText(QStringLiteral("开始录制"));
                    btnToggleRecord->setStyleSheet(QString());
                    currentRecordFile.clear();
                    QMessageBox::critical(this, QStringLiteral("录制失败"),
                                          QStringLiteral("无法创建录制文件：\n%1").arg(error));
                    return;
                }
            }

            currentRecordStartTimeMs = QDateTime::currentMSecsSinceEpoch();
            recordTimer->start(1000);
            if (bleClient) updateBleControls();
        } else {
            lblRecordStatus->setText("等待串口打开...");
        }
    } else {
        stopRecordingSafely();
    }
}

void MainWindow::onExportData()
{
    if (isRecording || m_bleRecordStopPending) {
        QMessageBox::warning(this, QString::fromUtf8("警告"), QString::fromUtf8("当前正在录制或完成文件收尾，请等待录制完全停止后再保存。"));
        return;
    }
    if (currentRecordFile.isEmpty() || !QFile::exists(currentRecordFile)) {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("当前没有可保存的录制数据，请先进行录制。"));
        return;
    }
    QSettings settings("WaveLink", "AdvancedApp");
    QString lastDir = settings.value("LastDataDir", QDir::homePath()).toString();
    QString defaultName = QDir(lastDir).filePath(QString("WaveData_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    QString destPath = QFileDialog::getSaveFileName(this, "导出数据", defaultName, "CSV Files (*.csv)");
    if (destPath.isEmpty()) return;
    settings.setValue("LastDataDir", QFileInfo(destPath).absolutePath());

    QFileInfo baseInfo(currentRecordFile);
    QString baseStem = baseInfo.completeBaseName();
    baseStem.remove(QRegularExpression(QStringLiteral("_part\\d{3,}$")));
    const QString sourceSuffix = baseInfo.suffix().toLower();
    const QString normalizedBase = baseInfo.dir().absoluteFilePath(baseStem + '.' + baseInfo.suffix());
    if (QFile::exists(normalizedBase)) baseInfo.setFile(normalizedBase);
    QStringList sourceFiles;
    if (baseInfo.exists()) sourceFiles.append(baseInfo.absoluteFilePath());
    const auto parts = RecordingChannels::recordingParts(baseInfo);
    for (const auto &part : parts) sourceFiles.append(part.absoluteFilePath());

    const QString selectedPath = QFileInfo(destPath).canonicalFilePath();
    for (const auto &source : sourceFiles) {
        if (!selectedPath.isEmpty() && QFileInfo(source).canonicalFilePath() == selectedPath) {
            QMessageBox::warning(this, "导出失败", "目标文件不能覆盖录制缓存的任意分卷，请选择其他文件名。");
            return;
        }
    }
    if (sourceFiles.isEmpty()) {
        QMessageBox::warning(this, "导出失败", "找不到录制缓存文件。");
        return;
    }

    RecordingSaveDialog dialog(this);
    dialog.setObjectName("recordingSaveProgress");
    dialog.setWindowTitle(QStringLiteral("保存数据"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setWindowFlag(Qt::WindowCloseButtonHint, false);
    dialog.setMinimumWidth(360);
    auto layout = new QVBoxLayout(&dialog);
    auto label = new QLabel(QStringLiteral("正在将录制数据保存为 CSV…"), &dialog);
    auto bar = new QProgressBar(&dialog);
    bar->setValue(0);
    layout->addWidget(label);
    layout->addWidget(bar);

    std::atomic<int> percent{0};
    QTimer refresh;
    connect(&refresh, &QTimer::timeout, &dialog, [&] {
        const int value = percent.load(std::memory_order_relaxed);
        bar->setValue(value);
        if (value == 99) label->setText(QStringLiteral("正在完成文件写入…"));
    });
    QFutureWatcher<QString> watcher;
    connect(&watcher, &QFutureWatcher<QString>::finished, &dialog, &QDialog::accept);
    watcher.setFuture(QtConcurrent::run([sourceFiles, sourceSuffix, destPath, &percent]() -> QString {
        QSaveFile destination(destPath);
        if (!destination.open(QIODevice::WriteOnly))
            return QStringLiteral("无法创建目标文件。\n%1").arg(destination.errorString());

        const auto report = [&percent](qint64 bytes, qint64 total) {
            percent.store(qBound(0, int(100.0 * bytes / qMax<qint64>(1, total)), 99),
                          std::memory_order_relaxed);
        };
        QString error;
        const bool converted = sourceSuffix == QStringLiteral("wlr")
            ? WlrRecordingWriter::exportToCsv(sourceFiles, &destination, error, report)
            : CsvRecordingWriter::exportToCsv(sourceFiles, &destination, error, report);
        if (!converted) return error.isEmpty() ? QStringLiteral("文件转换或复制失败。") : error;

        percent.store(99, std::memory_order_relaxed);
        if (!destination.commit())
            return QStringLiteral("无法完成文件写入。\n%1").arg(destination.errorString());
        return {};
    }));
    refresh.start(80);
    dialog.exec();
    refresh.stop();
    watcher.waitForFinished();

    const QString error = watcher.result();
    if (error.isEmpty()) {
        QMessageBox::information(this, "导出成功", QString("文件已成功保存至\n%1").arg(destPath));
    } else {
        QMessageBox::warning(this, "导出失败", error + QStringLiteral("\n原目标文件未被覆盖。"));
    }
}
void MainWindow::onImportData()
{
    if (isPortOpen) {
        QMessageBox::warning(this, QString::fromUtf8("警告"), QString::fromUtf8("当前设备已连接并在接收数据。\n为了避免波形显示冲突，请先断开连接后再导入历史数据。"));
        return;
    }
    stopOfflinePlayback();
    
    QSettings settings("WaveLink", "AdvancedApp");
    QString lastDir = settings.value("LastDataDir", QDir::homePath()).toString();
    QString filePath = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("导入数据"), lastDir,
        QString::fromUtf8("支持的数据文件 (*.csv *.wlr);;WaveLink 高速录制 (*.wlr);;CSV 文件 (*.csv)"));
    if (filePath.isEmpty()) return;
    settings.setValue("LastDataDir", QFileInfo(filePath).absolutePath());
    int numCh = 0;
    QVector<QVector<double>> chData;
    QVector<double> times;
    qint64 validRows = 0;
    double dt_step = spinFs ? (1.0 / spinFs->value()) : 0.004;
    QFileInfo selectedInfo(filePath);
    QString baseStem = selectedInfo.completeBaseName();
    baseStem.remove(QRegularExpression(QStringLiteral("_part\\d{3,}$")));
    const QString suffix = selectedInfo.suffix().toLower();
    const QString normalizedBase = selectedInfo.dir().absoluteFilePath(baseStem + "." + suffix);
    if (QFile::exists(normalizedBase)) filePath = normalizedBase;
    QStringList sourceFiles{filePath};
    const QFileInfo baseInfo(filePath);
    const QFileInfoList parts = RecordingChannels::recordingParts(baseInfo);
    for (const QFileInfo &part : parts) sourceFiles.append(part.absoluteFilePath());
    qint64 totalSourceBytes = 0;
    for (const QString &source : sourceFiles) totalSourceBytes += QFileInfo(source).size();
    const qint64 importBudgetBytes = offlineMemoryBudget();

    QProgressDialog progress(QString::fromUtf8("正在导入数据..."), QString::fromUtf8("取消"),
                             0, 1000, this);
    progress.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    progress.setWindowTitle(QString::fromUtf8("导入数据"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    progress.setMinimumWidth(300);
    progress.setStyleSheet("QProgressDialog { background-color: #1e1e1e; } "
                           "QLabel { color: #dcdcdc; font-weight: bold; } "
                           "QProgressBar { border: 1px solid #555555; background-color: #2d2d30; color: #dcdcdc; text-align: center; } "
                           "QProgressBar::chunk { background-color: #007acc; width: 1px; } "
                           "QPushButton { background-color: #333337; color: #dcdcdc; border: 1px solid #555555; padding: 4px 12px; border-radius: 2px; } "
                           "QPushButton:hover { background-color: #3e3e42; }");

    bool importOk = true;
    QString importError;
    QSharedPointer<OfflineWaveStore> pendingStore;
    double previousImportedTime = 0, importedRateHint = 0;
    bool importedHostTime = false, rebuildImportedTime = false;
    auto withinImportBudget = [&](qint64 rows, int channels) {
        if (pendingStore || WaveImportBudget::fits(rows, channels, importBudgetBytes)) return true;
        if (channels <= 0 || channels > 256) { importError = "文件通道数量无效，最多支持 256 路。"; return false; }
        progress.setLabelText(QStringLiteral("数据超过完整加载额度，正在建立按需浏览缓存……"));
        pendingStore = OfflineWaveStore::create(editCacheDir->text(), qMax<qint64>(4LL * 1024 * 1024, importBudgetBytes / 4), importError);
        if (!pendingStore) return false;
        QVector<double> frame(chData.size());
        for (int row = 0; row < times.size(); ++row) {
            for (int ch = 0; ch < chData.size(); ++ch) frame[ch] = chData[ch][row];
            if (!pendingStore->append(times[row], frame, importError)) return false;
            if (row % 4096 == 0) { QApplication::processEvents(); if (progress.wasCanceled()) return false; }
        }
        times.clear(); times.squeeze(); chData.clear(); chData.squeeze();
        return true;
    };
    const auto appendImportRow = [&](double time, const QVector<double> &frame) {
        previousImportedTime = time;
        if (pendingStore) return pendingStore->append(time, frame, importError);
        times.append(time);
        for (int ch = 0; ch < frame.size(); ++ch) chData[ch].append(frame[ch]);
        return true;
    };
    QStringList importedNames;
    if (suffix == QStringLiteral("wlr")) {
        qint64 firstTimestampNs = -1;
        double recordedSampleRateHint = 0.0;
        bool usesHostReceiveTime = false;
        importOk = WlrRecordingWriter::visitFiles(
            sourceFiles,
            [&](qint64 timestampNs, const QVector<double> &values, qint64 processedBytes) {
                if (!withinImportBudget(validRows + 1LL, qMax(numCh, values.size()))) return false;
                if (numCh == 0) {
                    numCh = values.size();
                    if (!pendingStore) chData.resize(numCh);
                }
                if (values.size() > numCh) {
                    const int oldCount = numCh; numCh = values.size();
                    if (!pendingStore) { chData.resize(numCh); for (int i = oldCount; i < numCh; ++i) chData[i].fill(std::numeric_limits<double>::quiet_NaN(), times.size()); }
                }
                if (firstTimestampNs < 0) firstTimestampNs = timestampNs;
                QVector<double> frame = values; frame.resize(numCh);
                for (int i = values.size(); i < numCh; ++i) frame[i] = qQNaN();
                if (!appendImportRow((timestampNs - firstTimestampNs) / 1000000000.0, frame)) return false;
                ++validRows;
                if (validRows % 10000 == 0) {
                    progress.setValue(int(1000.0 * processedBytes / qMax<qint64>(1, totalSourceBytes)));
                    QApplication::processEvents();
                    if (progress.wasCanceled()) return false;
                }
                return true;
            }, importError, &recordedSampleRateHint, &usesHostReceiveTime, &importedNames);
        importedHostTime = usesHostReceiveTime; importedRateHint = recordedSampleRateHint;
        if (importOk && usesHostReceiveTime && !pendingStore) {
            rebuildUniformTimeline(times, recordedSampleRateHint);
        }
    } else {
        double baseTime = -1.0;
        bool timelineHasDuplicateOrReversePoints = false;
        bool waveLinkHostTime = false;
        double recordedSampleRateHint = 0.0;
        qint64 completedBytes = 0;
        for (int sourceIndex = 0; sourceIndex < sourceFiles.size(); ++sourceIndex) {
            QFile file(sourceFiles[sourceIndex]);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                importError = QStringLiteral("无法打开 CSV 分卷：\n%1\n%2")
                                  .arg(sourceFiles[sourceIndex], file.errorString());
                importOk = false;
                break;
            }
            QTextStream in(&file);
            in.setCodec("UTF-8");
            QStringList channelNames;
            constexpr int MaximumCsvLine = 1024 * 1024;
            const auto header = in.readLine(MaximumCsvLine);
            if (header.size() >= MaximumCsvLine) { importError = QStringLiteral("CSV 表头超过 1 MB，请检查分隔符和换行格式。"); importOk = false; break; }
            if (!RecordingChannels::parseCsvRow(header, channelNames)) {
                importError = QStringLiteral("CSV 表头格式无效。"); importOk = false; break;
            }
            double partSampleRate = 0.0;
            const bool partUsesHostTime = !channelNames.isEmpty() &&
                RecordingChannels::parseReceiveTimeColumn(channelNames.first(), partSampleRate);
            if (partUsesHostTime) {
                // UnixTime and legacy HostReceiveTime/Hint have identical
                // timing semantics. Only the label changes, not stored times.
                recordedSampleRateHint = partSampleRate;
                waveLinkHostTime = true;
                if (channelNames.size() >= 2) {
                    channelNames.removeFirst();
                    channelNames.removeFirst();
                }
            } else if (!channelNames.isEmpty()) {
                channelNames.removeFirst();
            }
            if (sourceIndex == 0) {
                numCh = channelNames.size();
                if (numCh > 256) { importError = QStringLiteral("CSV 通道数量超过 256。"); importOk = false; break; }
                importedNames = RecordingChannels::normalized(channelNames, numCh);
                chData.resize(numCh);
            } else {
                const auto partNames = RecordingChannels::normalized(channelNames, channelNames.size());
                bool compatible = channelNames.size() <= 256 && (partUsesHostTime || channelNames.size() == numCh);
                for (int i = 0; i < qMin(partNames.size(), importedNames.size()); ++i) compatible = compatible && partNames[i] == importedNames[i];
                if (!compatible) { importError = QStringLiteral("CSV 分卷中的通道名称或时间格式不一致：%1").arg(sourceFiles[sourceIndex]); importOk = false; break; }
                if (partNames.size() > numCh) {
                    if (!withinImportBudget(validRows, partNames.size())) { importOk = false; break; }
                    const int oldCount = numCh; numCh = partNames.size(); importedNames = partNames;
                    if (!pendingStore) { chData.resize(numCh); for (int i = oldCount; i < numCh; ++i) chData[i].fill(std::numeric_limits<double>::quiet_NaN(), times.size()); }
                }
            }
            const int partChannels = channelNames.size();
            qint64 sourceLine = 1;
            while (!in.atEnd()) {
                if (validRows % 10000 == 0) {
                    progress.setValue(int(1000.0 * (completedBytes + file.pos()) / qMax<qint64>(1, totalSourceBytes)));
                    QApplication::processEvents();
                    if (progress.wasCanceled()) break;
                }
                const QString line = in.readLine(MaximumCsvLine).trimmed();
                ++sourceLine;
                if (line.size() >= MaximumCsvLine) { importError = QString("CSV 第 %1 行超过 1 MB，请检查分隔符和换行格式。").arg(sourceLine); importOk = false; break; }
                if (line.isEmpty()) continue;
                QStringList row;
                const int requiredColumns = partChannels + (partUsesHostTime ? 2 : 1);
                if (!RecordingChannels::parseCsvRow(line, row) || row.size() != requiredColumns) {
                    importError = QString("CSV 文件 %1 第 %2 行列数不完整或格式损坏：应为 %3 列，实际 %4 列。已取消导入；当前波形和原文件未改变。")
                        .arg(sourceFiles[sourceIndex]).arg(sourceLine).arg(requiredColumns).arg(row.size());
                    importOk = false; break;
                }
                if (!withinImportBudget(validRows + 1LL, numCh)) { importOk = false; break; }
                double t = validRows * dt_step;
                int dataStartIndex = 0;
                if (partUsesHostTime && row.size() >= partChannels + 2) {
                    bool ok = false;
                    const double hostTime = row[0].toDouble(&ok);
                    if (ok) {
                        if (baseTime < 0.0) baseTime = hostTime;
                        t = hostTime - baseTime;
                    }
                    dataStartIndex = 2; // Skip raw host timestamp and frame index.
                } else if (row.size() >= partChannels + 1) {
                    bool ok = false;
                    t = row[0].toDouble(&ok);
                    if (!ok) {
                        const QDateTime dt = QDateTime::fromString(row[0], "yyyy-MM-dd HH:mm:ss.zzz");
                        if (dt.isValid()) {
                            if (baseTime < 0) baseTime = dt.toMSecsSinceEpoch() / 1000.0;
                            t = dt.toMSecsSinceEpoch() / 1000.0 - baseTime;
                        } else t = validRows * dt_step;
                    }
                    dataStartIndex = 1;
                }
                if (validRows && t <= previousImportedTime) {
                    timelineHasDuplicateOrReversePoints = true;
                }
                QVector<double> frame(numCh);
                for (int i = 0; i < numCh; ++i) {
                    const auto token = i < partChannels ? row[dataStartIndex + i].trimmed() : QString("nan");
                    bool ok = false;
                    const double value = token.isEmpty() || token.compare("nan", Qt::CaseInsensitive) == 0
                        ? std::numeric_limits<double>::quiet_NaN() : token.toDouble(&ok);
                    if (!std::isnan(value) && (!ok || !std::isfinite(value))) {
                        importError = QStringLiteral("CSV 第 %1 个数据点、通道 %2 的数值格式无效。").arg(validRows + 1).arg(i + 1);
                        importOk = false; break;
                    }
                    frame[i] = value; // Never silently alter a valid measurement.
                }
                if (!importOk) break;
                if (!appendImportRow(t, frame)) { importOk = false; break; }
                ++validRows;
            }
            completedBytes += file.size();
            file.close();
            if (progress.wasCanceled() || !importOk) break;
        }
        importedHostTime = waveLinkHostTime; importedRateHint = recordedSampleRateHint; rebuildImportedTime = timelineHasDuplicateOrReversePoints;
        if (waveLinkHostTime && !pendingStore) {
            rebuildUniformTimeline(times, recordedSampleRateHint);
        // CSV files produced by earlier versions used millisecond wall-clock
        // timestamps. Multiple samples decoded from one receive batch therefore
        // had identical timestamps and were drawn as vertical stair steps. Such
        // a timeline is invalid for uniformly sampled waveform data, so rebuild
        // it with the configured plotting interval while preserving every value.
        } else if (timelineHasDuplicateOrReversePoints && !pendingStore) {
            for (int i = 0; i < times.size(); ++i) times[i] = i * dt_step;
        }
    }
    if (progress.wasCanceled()) return; // Keep the existing view/data intact.
    if (!importOk || !importError.isEmpty()) {
        showImportAdvice(importError);
        return;
    }
    if (numCh <= 0) {
        QMessageBox::warning(this, QString::fromUtf8("格式错误"), QString::fromUtf8("文件中无通道数据！"));
        return;
    }
    if (!validRows) {
        QMessageBox::warning(this, QString::fromUtf8("数据为空"), QString::fromUtf8("文件中没有有效数据行！"));
        return;
    }
    if (pendingStore && !pendingStore->finish(importedRateHint, dt_step, importedHostTime, rebuildImportedTime, importError)) {
        showImportAdvice(importError); return;
    }
    const double importedFirst = pendingStore ? pendingStore->firstTime() : times.first();
    const double importedLast = pendingStore ? pendingStore->lastTime() : times.last();
    const double importedFs = validRows > 1 && importedLast > importedFirst ? (validRows - 1) / (importedLast - importedFirst) : 1.0 / dt_step;
    QSharedPointer<OfflineWaveStore> filteredStore;
    const auto importedFilter = offlineFilterSpec(importedFs);
    if (pendingStore) {
        progress.setValue(progress.maximum()); bool canceled = false;
        filteredStore = filterOfflineStore(pendingStore, importedFilter, canceled, importError);
        if (!filteredStore) { if (!canceled) showImportAdvice(importError); return; }
    }
    if (isPortOpen || isRecording || m_connectionOperationPending) {
        showImportAdvice(QStringLiteral("导入期间连接或录制状态已改变。请先断开连接并停止录制，再重新导入。")); return;
    }
    // Clear old channel widgets before constructing the imported ones. Their
    // labels, graph names and analysis menus must come from the file, not the
    // currently selected live protocol. Reset to live labels on the next clear.
    clearProtocolPlots();
    m_isImportedData = true;
    m_importedChannelNames = RecordingChannels::normalized(importedNames, numCh);
    
    if (validRows >= 2) {
        double avgInterval = (importedLast - importedFirst) / (validRows - 1);
        if (avgInterval > 0) {
            double sampleRate = 1.0 / avgInterval;
            // Auto FFT and filters follow spinFs, not a stale live setting.
            if (spinFs && std::isfinite(sampleRate) && sampleRate > 0) {
                const QSignalBlocker blocker(spinFs);
                spinFs->setValue(sampleRate);
            }
            if (lblDataRate) {
                lblDataRate->setText(QString("%1 帧/秒 (Δt: %2 ms)").arg(qRound(sampleRate)).arg(avgInterval * 1000.0, 0, 'f', 2));
            }
        }
    }
    // Create channels if needed
    checkAndAddChannel(numCh);
    m_pagedRaw = pendingStore; m_pagedData = filteredStore;
    if (pendingStore) m_appliedOfflineFilter = importedFilter;
    m_importedTimes = times;
    m_importedData = chData;
    
    // Update time axis and auto-scale
    if (validRows) {
        lastLatestTime = importedLast;
        sampleCount = (uint64_t)validRows;
        m_nextPlotTime = importedLast + (spinFs->value() > 0 ? 1.0 / spinFs->value() : 0.001);
        m_autoScroll = false;
        const QCPRange fullRange = importedLast > importedFirst
            ? QCPRange(importedFirst, importedLast) : QCPRange(lastLatestTime - 10, lastLatestTime);
        for (PlotContext &ctx : allPlots) {
            ctx.plot->xAxis->blockSignals(true);
            ctx.plot->xAxis->setRange(fullRange);
            ctx.plot->xAxis->blockSignals(false);
            if (ctx.widthText) ctx.widthText->setText(QString("X跨度: %1 s").arg(fullRange.size(), 0, 'f', 2));
        }
        resetFilters();
        if (m_pagedData) refreshPagedView();
        // Refresh Y-span text after import
        int decimals = spinDecimals ? spinDecimals->value() : 2;
        for (PlotContext &ctx : allPlots) {
            if (ctx.heightText) {
                ctx.heightText->setText(QString::fromUtf8("Y跨度: %1").arg(ctx.plot->yAxis->range().size(), 0, 'f', decimals));
            }
        }
    }
    if (!allPlots.isEmpty()) syncTimeRangeSlider(allPlots[0].plot->xAxis->range());
    updateOfflinePlaybackButton();
    const QString details = pendingStore
        ? QStringLiteral("当前完整加载额度为 %1 MB，已切换到按需浏览。全部采样数据均已保留，缩小时显示峰谷概览，放大后可查看原始采样点。\n\n"
                         "如需将全部采样点一次加载到内存，请前往「设置 → 系统设置 → 绘图显示 → 离线完整加载上限」，手动调大该值后重新导入。"
                         "实际额度还受可用内存保护限制。")
              .arg(importBudgetBytes / (1024.0 * 1024.0), 0, 'f', 1)
        : QStringLiteral("离线波形完整保留，不受实时历史上限裁剪。");
    QMessageBox::information(this, QStringLiteral("导入成功"),
        QStringLiteral("成功导入 %1 个通道，共 %2 行采样数据。\n\n%3").arg(numCh).arg(validRows).arg(details));
}
void MainWindow::onRecordTimer()
{
    if (!isRecording) return;
    QString diskWarning;
    if (!hasEnoughDiskSpaceForRecording(&diskWarning)) {
        stopRecordingSafely(diskWarning);
        return;
    }
    qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - currentRecordStartTimeMs;
    int secs = (elapsedMs / 1000) % 60;
    int mins = (elapsedMs / 60000) % 60;
    int hours = (elapsedMs / 3600000);
    qint64 bytes = 0;
    const int interfaceType = comboInterface ? comboInterface->currentData().toInt() : -1;
    if (interfaceType == 0) bytes = serialHandler->getLoggedBytes();
    else if (isNetworkInterface(interfaceType)) bytes = networkHandler->getLoggedBytes();
    else if (interfaceType == 3) bytes = demoLoggedBytes;
    else if (interfaceType == 4 && bleWorker) bytes = bleWorker->getLoggedBytes();
    quint64 skipped = 0;
    if (interfaceType == 0 && serialHandler) skipped = serialHandler->skippedRecordingFrames();
    else if (isNetworkInterface(interfaceType) && networkHandler) skipped = networkHandler->skippedRecordingFrames();
    else if (interfaceType == 3) skipped = m_demoRecordingChannels.skippedFrames();
    else if (interfaceType == 4 && bleWorker) skipped = bleWorker->skippedRecordingFrames();
    if (lblRecordingWarning) {
        lblRecordingWarning->setText(skipped ? QString("录制中 · 已跳过 %1 帧异常数据").arg(skipped) : QString());
        lblRecordingWarning->setVisible(skipped > 0);
    }
    double mb = bytes / (1024.0 * 1024.0);
    lblRecordStatus->setText(QString("%1:%2:%3 | %4 MB")
        .arg(hours, 2, 10, QChar('0'))
        .arg(mins, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'))
        .arg(mb, 0, 'f', 2));
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    stopOfflinePlayback();
    m_offlineViewTimer.stop();
    stopConsolePeriodicSend();
    if (bleClient) bleClient->disconnectDevice();
    if (isRecording) stopRecordingSafely();
    if (m_bleRecordStopPending) {
        m_bleCloseAfterRecordStop = true; event->ignore(); return;
    }
    clearFilterSelections();
    saveConfig();
    if (serialHandler && m_activeInterfaceType == 0) {
        // Release/close in the owning worker before shutdown; the worker's
        // destructor also performs safe cleanup if the queued call is late.
        QMetaObject::invokeMethod(serialHandler, "closePort", Qt::QueuedConnection);
    }
    QMainWindow::closeEvent(event);
    if (event->isAccepted() && m_updates) m_updates->commitInstallerHandoff();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    static bool firstShow = true;
    if (firstShow) {
        firstShow = false;
        centerWindowOnScreen();
        if (centralWidget()) {
            centralWidget()->setFocus(Qt::OtherFocusReason);
        }
    }
}

void MainWindow::onExportCache()
{
    QString defaultPath = QDir(editCacheDir->text()).absoluteFilePath(QString("WaveExported_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    QString filePath = QFileDialog::getSaveFileName(this, QString::fromUtf8("保存当前波形数据"), defaultPath, QString::fromUtf8("CSV 文件 (*.csv)"));
    if (filePath.isEmpty()) return;

    if (m_pagedData) { exportPagedData(filePath); return; }
    int numCh = channelGraphs.size();
    if (numCh == 0) {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("当前没有可保存的波形数据！"));
        return;
    }

    int maxPoints = 0;
    QVector<QSharedPointer<QCPGraphDataContainer>> snapshots;
    QString header = "Time(s)";
    for (int i = 0; i < numCh; ++i) {
        if (channelGraphs[i] && channelGraphs[i]->data()->size() > 0) {
            QSharedPointer<QCPGraphDataContainer> snapshot(new QCPGraphDataContainer());
            snapshot->set(*channelGraphs[i]->data());
            snapshots.append(snapshot);
            QString name = QStringLiteral("Ch%1").arg(i + 1);
            if (m_isImportedData) name = RecordingChannels::name(m_importedChannelNames, i);
            else if (comboInterface->currentData().toInt() != 3 && m_appliedProtocol.kind == "custom")
                name = RecordingChannels::name(m_appliedProfile.outputChannelNames(), i);
            header += ',' + RecordingChannels::csvCell(name);
            if (channelGraphs[i]->data()->size() > maxPoints) {
                maxPoints = channelGraphs[i]->data()->size();
            }
        }
    }
    
    if (maxPoints == 0) {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("当前没有可保存的波形数据！"));
        return;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QString::fromUtf8("错误"), QString::fromUtf8("无法创建文件！"));
        return;
    }
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out.setGenerateByteOrderMark(true);

    out << header << "\n";

    QProgressDialog progress(QString::fromUtf8("正在保存数据..."), QString::fromUtf8("取消"), 0, maxPoints, this);
    progress.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    progress.setWindowTitle(QString::fromUtf8("保存数据"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    progress.setMinimumWidth(300);
    progress.setStyleSheet("QProgressDialog { background-color: #1e1e1e; } "
                           "QLabel { color: #dcdcdc; font-weight: bold; } "
                           "QProgressBar { border: 1px solid #555555; background-color: #2d2d30; color: #dcdcdc; text-align: center; } "
                           "QProgressBar::chunk { background-color: #007acc; width: 1px; } "
                           "QPushButton { background-color: #333337; color: #dcdcdc; border: 1px solid #555555; padding: 4px 12px; border-radius: 2px; } "
                           "QPushButton:hover { background-color: #3e3e42; }");

    QVector<QCPDataContainer<QCPGraphData>::const_iterator> iters;
    for (const auto &snapshot : snapshots) {
        iters.append(snapshot->constBegin());
    }

    int rowsWritten = 0;
    while (true) {
        bool anyValid = false;
        double minT = std::numeric_limits<double>::max();
        for (int i = 0; i < snapshots.size(); ++i) {
            if (iters[i] != snapshots[i]->constEnd()) {
                if (iters[i]->key < minT) minT = iters[i]->key;
                anyValid = true;
            }
        }
        
        if (!anyValid) break;
        
        out << QString::fromLatin1(RecordingChannels::csvNumber(minT));
        
        for (int i = 0; i < snapshots.size(); ++i) {
            if (iters[i] != snapshots[i]->constEnd() && qAbs(iters[i]->key - minT) < 1e-6) {
                out << "," << QString::fromLatin1(RecordingChannels::csvNumber(iters[i]->value));
                ++iters[i];
            } else {
                out << ",";
            }
        }
        out << "\n";
        
        rowsWritten++;
        if (rowsWritten % 10000 == 0) {
            progress.setValue(rowsWritten);
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                file.cancelWriting();
                return;
            }
        }
    }
    
    progress.setValue(maxPoints);
    if (!file.commit()) {
        QMessageBox::warning(this, QString::fromUtf8("错误"), QString::fromUtf8("保存文件失败，原文件未被覆盖。"));
        return;
    }
    QMessageBox::information(this, QString::fromUtf8("成功"), QString::fromUtf8("缓存数据已成功保存！"));
}
