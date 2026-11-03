#include "mainwindow.h"
#include "plotmeasurementcontroller.h"
#include "systemsettingsdialog.h"
#include "waveformgraph.h"
#include "recordingchannelnames.h"
#include "waveimportbudget.h"
#include <QApplication>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTextStream>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

qint64 MainWindow::offlineMemoryBudget() const {
    const qint64 configured = qint64(m_systemPreferences.offlineMemoryMB) * 1024 * 1024;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX memory = {}; memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) return WaveImportBudget::bytesForAvailableMemory(memory.ullAvailPhys, configured);
#endif
    return qMin<qint64>(configured, 128LL * 1024 * 1024);
}

OfflineFilterSpec MainWindow::offlineFilterSpec(double fs) const {
    OfflineFilterSpec spec;
    spec.fs = fs > 0 && std::isfinite(fs) ? fs : (spinFs ? spinFs->value() : 250);
    if (spinFs) {
        spec.fs = qBound(spinFs->minimum(), spec.fs, spinFs->maximum());
        const double scale = std::pow(10.0, spinFs->decimals()); spec.fs = std::round(spec.fs * scale) / scale;
    }
    spec.automaticRate = !comboSampleRate || comboSampleRate->currentText().toDouble() <= 0;
    if (!spec.automaticRate) spec.fs = comboSampleRate->currentText().toDouble();
    spec.notch = chkNotch && chkNotch->isChecked(); spec.highpass = chkHPF && chkHPF->isChecked(); spec.lowpass = chkLPF && chkLPF->isChecked();
    spec.notchHz = spinNotchFreq->value(); spec.highHz = spinHPFFreq->value(); spec.lowHz = spinLPFFreq->value();
    const double maximum = qMax(.1, spec.fs * .5 - .1);
    spec.notchHz = qMin(spec.notchHz, maximum); spec.highHz = qMin(spec.highHz, maximum); spec.lowHz = qMin(spec.lowHz, maximum);
    spec.order = comboFilterOrder->currentIndex() == 0 ? 2 : 4; return spec;
}
void MainWindow::restoreOfflineFilters(const OfflineFilterSpec &spec) {
    const QSignalBlocker a(chkNotch), b(chkHPF), c(chkLPF), d(spinNotchFreq), e(spinHPFFreq), f(spinLPFFreq), g(comboFilterOrder), h(comboSampleRate), rateBlock(spinFs);
    const int at = comboSampleRate->findText(QString::number(spec.fs));
    comboSampleRate->setCurrentIndex(!spec.automaticRate && at >= 0 ? at : 0);
    if (spec.automaticRate) spinFs->setValue(spec.fs);
    updateFilterNyquistLimits();
    chkNotch->setChecked(spec.notch); chkHPF->setChecked(spec.highpass); chkLPF->setChecked(spec.lowpass);
    spinNotchFreq->setValue(spec.notchHz); spinHPFFreq->setValue(spec.highHz); spinLPFFreq->setValue(spec.lowHz);
    comboFilterOrder->setCurrentIndex(spec.order == 4 ? 1 : 0);
}
QSharedPointer<OfflineWaveStore> MainWindow::filterOfflineStore(const QSharedPointer<OfflineWaveStore> &raw, const OfflineFilterSpec &spec, bool &canceled, QString &error) {
    canceled = false; if (!spec.active()) return raw;
    QProgressDialog progress(QStringLiteral("正在连续计算大文件滤波并建立索引……"), QStringLiteral("取消"), 0, 1000, this);
    progress.setObjectName("offlineFilterProgress"); progress.setWindowTitle(QStringLiteral("大文件滤波")); progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(0);
    const auto filtered = raw->filtered(spec, [&](qint64 row, qint64 total) {
        progress.setValue(int(1000.0 * row / qMax<qint64>(1, total))); QApplication::processEvents();
        canceled = progress.wasCanceled(); return !canceled;
    }, error);
    if (filtered) progress.setValue(1000);
    return filtered;
}
void MainWindow::showImportAdvice(const QString &error) {
    QMessageBox message(this); message.setObjectName("offlineImportAdvice"); message.setWindowTitle(QStringLiteral("导入失败"));
    message.setIcon(QMessageBox::Warning);
    message.setText(error + QStringLiteral("\n\n建议：\n• 内存或索引额度不足：在“系统设置 → 绘图显示”调整“离线完整加载上限”；较低额度会更早转为按需浏览。\n• 浏览缓存写入失败：在“数据存储”选择有足够空间且可写的本机缓存路径。\n• 文件格式或分卷错误：检查提示中的文件/行号，重新导出数据，或整理成独立文件。\n\n本次未替换当前波形，也不会修改原始文件。"));
    auto settings = message.addButton(QStringLiteral("绘图显示设置…"), QMessageBox::ActionRole);
    settings->setObjectName("offlineAdviceSettings");
    message.addButton(QMessageBox::Close); message.exec();
    if (message.clickedButton() == settings) {
        SystemSettingsDialog dialog(m_systemPreferences, [this](const SystemPreferences &p) { applySystemPreferences(p, true); }, this, SystemSettingsDialog::Category::Plot);
        dialog.exec();
    }
}
void MainWindow::refreshPagedView() {
    m_offlineViewTimer.stop(); if (!m_pagedData || allPlots.isEmpty()) return;
    const auto store = m_pagedData; const auto range = allPlots[0].plot->xAxis->range();
    const qint64 cacheBytes = offlineMemoryBudget();
    const int maximum = int(qBound<qint64>(128, cacheBytes / (qMax(1, store->channels()) * qint64(sizeof(QCPGraphData)) * 4), 2000000));
    QVector<QVector<OfflineWaveStore::Point>> curves; QString error; bool exact = false;
    if (!store->display(range.lower, range.upper, maximum, curves, exact, error)) {
        statusBar()->showMessage(error + QStringLiteral(" 请重新导入文件，或检查本机缓存路径。"), 10000); return;
    }
    m_pagedViewExact = exact;
    for (auto &context : allPlots) {
        for (int i = 0; i < context.plot->graphCount(); ++i) {
            auto graph = dynamic_cast<WaveformGraph *>(context.plot->graph(i)); if (!graph) continue;
            const int channel = graph->channelIndex(); if (channel < 0 || channel >= curves.size()) continue;
            QVector<QCPGraphData> display; display.reserve(curves[channel].size());
            for (const auto &p : curves[channel]) display.append({p.key, p.value});
            graph->data()->set(display, true);
            graph->setOfflineSource(store->rows(), [store, channel](double lower, double upper, int cap, QVector<QCPGraphData> &output, QString &problem, bool newest) {
                QVector<OfflineWaveStore::Point> data; output.clear();
                if (!store->exactChannel(channel, lower, upper, cap, data, problem, newest)) return false;
                output.reserve(data.size()); for (const auto &p : data) output.append({p.key, p.value}); return true;
            }, [store, channel](double time, QVector<QCPGraphData> &output, QString &problem) {
                QVector<OfflineWaveStore::Point> data; output.clear(); if (!store->nearby(channel, time, data, problem)) return false;
                for (const auto &p : data) output.append({p.key, p.value}); return true;
            });
        }
        if (context.scope) context.scope->refresh();
        context.plot->replot(QCustomPlot::rpQueuedReplot);
    }
    enforcePlotHistoryLimit(true); autoScaleY();
}
void MainWindow::exportPagedData(const QString &path) {
    const auto store = m_pagedData; const auto names = m_importedChannelNames;
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { QMessageBox::warning(this, "导出失败", "无法创建导出文件，请检查目标路径、权限和磁盘空间。"); return; }
    QTextStream out(&file); out.setCodec("UTF-8"); out.setGenerateByteOrderMark(true); out << "Time(s)";
    for (int ch = 0; ch < store->channels(); ++ch) out << ',' << RecordingChannels::csvCell(RecordingChannels::name(names, ch)); out << '\n';
    QProgressDialog progress(QStringLiteral("正在读取全部原始采样点并导出……"), QStringLiteral("取消"), 0, 1000, this);
    progress.setWindowTitle(QStringLiteral("保存数据")); progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(500);
    QString error; bool canceled = false;
    const bool ok = store->visit([&](qint64 row, double time, const QVector<double> &values) {
        out << QString::fromLatin1(RecordingChannels::csvNumber(time));
        for (double value : values) out << ',' << QString::fromLatin1(RecordingChannels::csvNumber(value)); out << '\n';
        if (out.status() != QTextStream::Ok) { error = "写入导出文件失败，请检查磁盘空间与权限。"; return false; }
        if (row % 10000 == 0) { progress.setValue(int(1000.0 * row / store->rows())); QApplication::processEvents(); canceled = progress.wasCanceled(); }
        return !canceled;
    }, error);
    out.flush();
    if (!ok || canceled || out.status() != QTextStream::Ok) {
        file.cancelWriting(); if (!canceled) QMessageBox::warning(this, "导出失败", error + "\n原文件未被覆盖，请检查缓存或重新导入。"); return;
    }
    if (!file.commit()) { QMessageBox::warning(this, "导出失败", "保存失败，原文件未被覆盖。请检查磁盘空间和权限。"); return; }
    progress.setValue(1000); QMessageBox::information(this, "成功", "全部离线波形已完整导出；使用原始采样点及当前滤波结果，未导出概览抽稀点。");
}
