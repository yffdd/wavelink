#include "mainwindow.h"
#include "waveformgraph.h"
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
#include <algorithm>
#include <cmath>
#include <limits>

void MainWindow::updateSpectrumChannelMenu()
{
    int checkedCount = 0;
    QList<QAction*> actions = menuSpectrumChannels->actions();
    for (QAction *a : actions) {
        if (a->isChecked()) checkedCount++;
    }
    
    for (QAction *a : actions) {
        if (!a->isChecked()) {
            a->setEnabled(checkedCount < 8);
        }
    }
    
    // Update button text
    if (checkedCount == 0) btnSpectrumChannels->setText("选择通道...");
    else btnSpectrumChannels->setText(QString("已选 %1 个").arg(checkedCount));
}

void MainWindow::onMeasureTimer()
{
    if (!measureDock || !measureDock->isVisible() || measureDock->widget()->maximumHeight() == 0) {
        return; 
    }
    
    // Prevent starting a new calculation if one is already running
    if (measureWatcher->isRunning()) {
        return;
    }
    
    int chIdx = comboMeasureChannel->currentIndex();
    if (chIdx < 0 || chIdx >= channelGraphs.size()) return;
    
    // Get visible time range
    QCPRange mainXRange = allPlots[0].plot->xAxis->range();
    
    QCPGraph *srcGraph = channelGraphs[chIdx];
    if (!srcGraph || srcGraph->dataCount() == 0) return;
    
    auto it = srcGraph->data()->findBegin(mainXRange.lower);
    auto end = srcGraph->data()->findEnd(mainXRange.upper);
    
    QVector<double> windowData;
    auto indexed = dynamic_cast<WaveformGraph *>(srcGraph);
    if (indexed && indexed->hasOfflineSource()) {
        QVector<QCPGraphData> original; QString error;
        if (!indexed->readExact(mainXRange.lower, mainXRange.upper, 2000000, original, error)) {
            for (auto label : measureLabels) { label->setText("--（请缩小分析范围）"); label->setToolTip(error); } return;
        }
        windowData.reserve(original.size()); for (const auto &point : original) windowData.append(point.value);
    } else {
        for (auto i = it; i != end; ++i) windowData.append(i->value);
    }
    
    if (windowData.isEmpty()) {
        return;
    }
    
    double fs = spinFs->value();
    if (fs <= 0) fs = 250.0;
    
    SignalType sigType = SignalType::General;
    QString typeStr = comboSignalType->currentText();
    if (typeStr == "50Hz") sigType = SignalType::Noise50Hz;
    else if (typeStr == "ECG") sigType = SignalType::ECG;
    else if (typeStr == "EEG") sigType = SignalType::EEG;
    else if (typeStr == "EMG") sigType = SignalType::EMG;
    else if (typeStr == "EOG") sigType = SignalType::EOG;
    else if (typeStr == "PPG") sigType = SignalType::PPG;
    else if (typeStr == "CBF") sigType = SignalType::CBF;
    
    QFuture<PhysioResult> future = QtConcurrent::run(calculatePhysioFeatures, sigType, windowData, fs, spinDecimals ? spinDecimals->value() : 4);
    measureWatcher->setFuture(future);
}

void MainWindow::onSpectrumTimer()
{
    if (!spectrumDock->isVisible() || spectrumDock->widget()->maximumHeight() == 0) {
        return; 
    }

    // Keep the footer title up to date even when no spectrum channel is selected
    // or there is not enough data to calculate an FFT.
    double displayFs = spinFs ? spinFs->value() : 250.0;
    QString displayFsText = comboSampleRate ? comboSampleRate->currentText().trimmed() : QStringLiteral("Auto");
    if (displayFsText.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) != 0) {
        bool ok = false;
        const double configuredFs = displayFsText.toDouble(&ok);
        if (ok && configuredFs > 0) {
            displayFs = configuredFs;
        }
    }
    if (displayFs <= 0) {
        displayFs = 250.0;
    }
    const double fs = displayFs;
    if (spectrumXAxisLabel) {
        spectrumXAxisLabel->setText(
            QString("频率 (Hz) | 采样频率: %1 Hz").arg(displayFs, 0, 'f', 1));
        updateSpectrumFooterLayout();
    }
    
    QList<QAction*> actions = menuSpectrumChannels->actions();
    QList<int> activeChannels;
    for (QAction *a : actions) {
        if (a->isChecked()) activeChannels.append(a->data().toInt());
    }
    
    if (activeChannels.isEmpty()) return;
    
    // Ensure we have enough spectrum graphs
    while (spectrumGraphs.size() < channelGraphs.size()) {
        QCPGraph *g = spectrumPlot->addGraph();
        g->setVisible(false);
        spectrumGraphs.append(g);
    }
    
    for (QCPGraph *g : spectrumGraphs) {
        if (g) g->setVisible(false);
    }
    
    int points = comboFftPoints->currentText().toInt();
    if (points <= 0) points = 1024;
    
    QCPRange mainXRange = allPlots[0].plot->xAxis->range();
    
    bool hasData = false;
    bool hasInvalidInput = false;
    double maxMagnitude = 0.0;
    double minFrequency = std::numeric_limits<double>::max();
    double maxFrequency = 0.0;
    int minPoints = qMin(points, 128); // Dynamic minimum points based on FFT setting
    if (points > 128) minPoints = points / 4;
    
    for (int chIdx : activeChannels) {
        if (chIdx < 0 || chIdx >= channelGraphs.size()) continue;
        
        QCPGraph *srcGraph = channelGraphs[chIdx];
        if (!srcGraph || srcGraph->dataCount() == 0) continue;
        
        auto it = srcGraph->data()->findBegin(mainXRange.lower);
        auto end = srcGraph->data()->findEnd(mainXRange.upper);
        
        // Use the newest contiguous run of finite samples. A dropped plot
        // frame or a text-only packet must not poison the entire FFT window.
        QVector<double> data;
        const int maxPoints = points;
        const int maxScan = qMax(maxPoints * 4, maxPoints + 128);
        QSharedPointer<QCPGraphDataContainer> rawWindow;
        if (auto indexed = dynamic_cast<WaveformGraph *>(srcGraph)) if (indexed->hasOfflineSource()) {
            QVector<QCPGraphData> original; QString error;
            if (!indexed->readExact(mainXRange.lower, mainXRange.upper, maxScan, original, error, true)) continue;
            rawWindow.reset(new QCPGraphDataContainer()); rawWindow->set(original, true); it = rawWindow->constBegin(); end = rawWindow->constEnd();
        }
        int scanned = 0;
        for (auto i = end; i != it && scanned < maxScan && data.size() < maxPoints;) {
            --i;
            ++scanned;
            if (!std::isfinite(i->value)) {
                hasInvalidInput = true;
                if (data.size() >= minPoints) break;
                data.clear();
                continue;
            }
            data.append(i->value);
        }
        std::reverse(data.begin(), data.end());
        
        if (data.size() < minPoints) continue; // Not enough points for a meaningful FFT
        
        // Available history may be 150/250 points even with a 256-point UI
        // setting. This radix-2 FFT requires a power-of-two window.
        int actualPoints = FftHelper::powerOfTwoFloor(data.size());
        if (actualPoints < minPoints) continue;
        if (actualPoints < data.size()) data.remove(0, data.size() - actualPoints);
        std::vector<std::complex<double>> complexData(actualPoints);
        for (int i = 0; i < actualPoints; ++i) {
            complexData[i] = std::complex<double>(data[i], 0.0);
        }
        
        FftHelper::computeFFT(complexData);
        
        int halfPoints = actualPoints / 2;
        
        double fs = 0;
        QString fsStr = comboSampleRate->currentText();
        if (fsStr.toLower() == "auto") {
            double dt = (spinFs->value() > 0) ? (1.0 / spinFs->value()) : 0.001;
            if (dt > 0) fs = 1.0 / dt;
            else fs = 1000;
        } else {
            fs = fsStr.toDouble();
            if (fs <= 0) fs = 1000;
        }
        
        bool removeDC = (chkRemoveDC && chkRemoveDC->isChecked());
        int startIdx = (removeDC && halfPoints > 1) ? 1 : 0;
        int plotCount = halfPoints - startIdx;
        QVector<double> freqs(plotCount);
        QVector<double> mags(plotCount);
        double channelMaxMagnitude = 0.0;
        bool validSpectrum = true;
        
        for (int i = startIdx; i < halfPoints; ++i) {
            int outIdx = i - startIdx;
            freqs[outIdx] = i * fs / actualPoints;
            double magVal = std::abs(complexData[i]) / halfPoints;
            if (i == 0) magVal /= 2.0;
            if (!std::isfinite(magVal)) {
                validSpectrum = false;
                break;
            }
            if (magVal < 1e-8) magVal = 0.0;
            mags[outIdx] = magVal;
            channelMaxMagnitude = qMax(channelMaxMagnitude, magVal);
        }
        if (!validSpectrum || freqs.isEmpty()) {
            hasInvalidInput = true;
            continue;
        }

        hasData = true;
        maxMagnitude = qMax(maxMagnitude, channelMaxMagnitude);
        minFrequency = qMin(minFrequency, freqs.first());
        maxFrequency = qMax(maxFrequency, freqs.last());
        
        QCPGraph *targetGraph = spectrumGraphs[chIdx];
        targetGraph->setData(freqs, mags);
        
        // Sync color
        if (chIdx < defaultColors.size()) {
            targetGraph->setPen(QPen(defaultColors[chIdx], 1));
        }
        targetGraph->setVisible(true);
    }
    
    if (!hasData) {
        spectrumWarningText->setText(hasInvalidInput
            ? QStringLiteral("当前可视数据包含非数值或无效值，暂无有效频谱。")
            : QString("主视图中可视的数据点不足 (当前要求至少 %1 个点)\n请尝试放大主视图的时间窗口，或缩减 FFT点数设置。").arg(minPoints));
        spectrumWarningText->setVisible(true);
    } else {
        spectrumWarningText->setVisible(false);
    }
    
    QStringList legendEntries;
    if (chkNotch && chkNotch->isChecked() && spinNotchFreq && notchCutoffLine) {
        double fNotch = spinNotchFreq->value();
        notchCutoffLine->start->setCoords(fNotch, spectrumPlot->yAxis->range().lower);
        notchCutoffLine->end->setCoords(fNotch, spectrumPlot->yAxis->range().upper);
        notchCutoffLine->setVisible(true);
        legendEntries.append(QString("■ 陷波: %1Hz").arg(fNotch, 0, 'f', 1));
    } else if (notchCutoffLine) {
        notchCutoffLine->setVisible(false);
    }

    if (chkHPF && chkHPF->isChecked() && spinHPFFreq && hpfCutoffLine) {
        double fHpf = spinHPFFreq->value();
        hpfCutoffLine->start->setCoords(fHpf, spectrumPlot->yAxis->range().lower);
        hpfCutoffLine->end->setCoords(fHpf, spectrumPlot->yAxis->range().upper);
        hpfCutoffLine->setVisible(true);
        legendEntries.append(QString("■ 高通: %1Hz").arg(fHpf, 0, 'f', 1));
    } else if (hpfCutoffLine) {
        hpfCutoffLine->setVisible(false);
    }

    if (chkLPF && chkLPF->isChecked() && spinLPFFreq && lpfCutoffLine) {
        double fLpf = spinLPFFreq->value();
        lpfCutoffLine->start->setCoords(fLpf, spectrumPlot->yAxis->range().lower);
        lpfCutoffLine->end->setCoords(fLpf, spectrumPlot->yAxis->range().upper);
        lpfCutoffLine->setVisible(true);
        legendEntries.append(QString("■ 低通: %1Hz").arg(fLpf, 0, 'f', 1));
    } else if (lpfCutoffLine) {
        lpfCutoffLine->setVisible(false);
    }

    if (!legendEntries.isEmpty() && spectrumLegendText) {
        spectrumLegendText->setText(legendEntries.join("  "));
        spectrumLegendText->setVisible(true);
    } else if (spectrumLegendText) {
        spectrumLegendText->setVisible(false);
    }

    if (chkSpectrumAutoScale && chkSpectrumAutoScale->isChecked()) {
        if (hasData) {
            // Derive the Y range only from this update's finite FFT magnitudes.
            // Reusing the previous axis upper bound would multiply it on every
            // timer tick when the selected channel contains only invalid values.
            spectrumPlot->yAxis->setRange(0, qMax(1e-6, maxMagnitude * 1.15));
            if (maxFrequency > minFrequency) {
                spectrumPlot->xAxis->setRange(minFrequency, maxFrequency);
            }
        } else if (hasInvalidInput) {
            spectrumPlot->yAxis->setRange(0, 1);
        }
    }

    if (chkSpectrumFixX && chkSpectrumFixX->isChecked()) {
        double minX = spinSpectrumXMin ? spinSpectrumXMin->value() : 0.0;
        double maxX = spinSpectrumXMax ? spinSpectrumXMax->value() : 100.0;
        
        // Validation rules
        double nyquist = fs / 2.0;
        bool clamped = false;
        
        if (maxX > nyquist) { maxX = nyquist; clamped = true; }
        if (minX < 0.0) { minX = 0.0; clamped = true; }
        if (minX >= maxX) {
            // Adjust based on which one is more likely to be wrong or just enforce min < max
            minX = maxX - 0.1;
            clamped = true;
            if (minX < 0.0) {
                minX = 0.0;
                maxX = 0.1;
            }
        }
        
        // Update spinboxes only if values were invalid to avoid interrupting user typing
        if (clamped && spinSpectrumXMin && spinSpectrumXMax) {
            spinSpectrumXMin->blockSignals(true);
            spinSpectrumXMax->blockSignals(true);
            spinSpectrumXMin->setValue(minX);
            spinSpectrumXMax->setValue(maxX);
            spinSpectrumXMin->blockSignals(false);
            spinSpectrumXMax->blockSignals(false);
        }

        if (minX < maxX) {
            spectrumPlot->xAxis->setRange(minX, maxX);
        }
    }

    spectrumPlot->replot(QCustomPlot::rpQueuedReplot);
}


void MainWindow::updateFilterNyquistLimits() {
    double fs = 0;
    QString fsStr = comboSampleRate ? comboSampleRate->currentText() : "Auto";
    if (fsStr.toLower() != "auto" && fsStr.toDouble() > 0) {
        fs = fsStr.toDouble();
    } else {
        fs = spinFs ? spinFs->value() : 250.0;
    }
    if (fs <= 0) fs = 250.0;

    double nyquistMax = (fs / 2.0) - 0.1;
    if (nyquistMax < 0.1) nyquistMax = 0.1;

    for (auto spin : {spinNotchFreq, spinHPFFreq, spinLPFFreq}) {
        if (!spin) continue;
        const QSignalBlocker blocker(spin);
        spin->setMaximum(nyquistMax);
    }
}

void MainWindow::clearFilterSelections()
{
    if (!chkNotch || !chkHPF || !chkLPF) return;

    // Avoid three separate reset passes while changing the checkbox states.
    const QSignalBlocker notchBlocker(chkNotch);
    const QSignalBlocker hpfBlocker(chkHPF);
    const QSignalBlocker lpfBlocker(chkLPF);

    chkNotch->setChecked(false);
    chkHPF->setChecked(false);
    chkLPF->setChecked(false);

    resetFilters();
}

void MainWindow::resetFilters() {
    updateFilterNyquistLimits();
    int numChannels = channelGraphs.size();
    if (filterStates.size() != numChannels) {
        filterStates.resize(numChannels);
    }
    
    double fs = 0;
    QString fsStr = comboSampleRate ? comboSampleRate->currentText() : "Auto";
    if (fsStr.toLower() != "auto" && fsStr.toDouble() > 0) {
        fs = fsStr.toDouble();
    } else {
        fs = spinFs->value();
    }
    if (fs <= 0) fs = 250.0;
    
    int order = (comboFilterOrder->currentIndex() == 0) ? 2 : 4;
    
    for (int i = 0; i < numChannels; ++i) {
        if (chkNotch && chkNotch->isChecked()) {
            filterStates[i].notch.configure(BiquadFilter::NOTCH, fs, spinNotchFreq->value(), order);
        } else {
            filterStates[i].notch.reset();
        }
        
        if (chkHPF && chkHPF->isChecked()) {
            filterStates[i].hpf.configure(BiquadFilter::HIGHPASS, fs, spinHPFFreq->value(), order);
        } else {
            filterStates[i].hpf.reset();
        }
        
        if (chkLPF && chkLPF->isChecked()) {
            filterStates[i].lpf.configure(BiquadFilter::LOWPASS, fs, spinLPFFreq->value(), order);
        } else {
            filterStates[i].lpf.reset();
        }
    }
    if (m_isImportedData) {
        reapplyFiltersToImportedData();
    }
}

void MainWindow::reapplyFiltersToLiveBuffer() {
    int numChannels = channelGraphs.size();
    if (numChannels == 0) return;

    double fs = spinFs ? spinFs->value() : 250.0;
    if (fs <= 0) fs = 250.0;
    int order = (comboFilterOrder && comboFilterOrder->currentIndex() == 0) ? 2 : 4;

    for (int ch = 0; ch < numChannels; ++ch) {
        if (ch >= channelGraphs.size() || !channelGraphs[ch]) continue;
        QCPGraph *g = channelGraphs[ch];
        QCPGraphDataContainer *dataContainer = g->data().data();
        int count = dataContainer->size();
        if (count == 0) continue;

        if (ch < channelBuffers.size() && !channelBuffers[ch].isEmpty()) {
            BiquadFilter tempNotch, tempHPF, tempLPF;
            if (chkNotch && chkNotch->isChecked()) tempNotch.configure(BiquadFilter::NOTCH, fs, spinNotchFreq->value(), order);
            if (chkHPF && chkHPF->isChecked()) tempHPF.configure(BiquadFilter::HIGHPASS, fs, spinHPFFreq->value(), order);
            if (chkLPF && chkLPF->isChecked()) tempLPF.configure(BiquadFilter::LOWPASS, fs, spinLPFFreq->value(), order);

            const QQueue<double> &rawVals = channelBuffers[ch];
            int pCount = rawVals.size();
            QVector<double> times(pCount);
            QVector<double> filtered(pCount);
            auto it = dataContainer->constBegin();
            for (int i = 0; i < pCount && it != dataContainer->constEnd(); ++i, ++it) {
                times[i] = it->key;
                double val = rawVals[i];
                if (chkNotch && chkNotch->isChecked()) val = tempNotch.process(val);
                if (chkHPF && chkHPF->isChecked()) val = tempHPF.process(val);
                if (chkLPF && chkLPF->isChecked()) val = tempLPF.process(val);
                filtered[i] = val;
            }
            g->setData(times, filtered, true);
        }
    }

    if (!allPlots.isEmpty() && allPlots[0].plot) {
        allPlots[0].plot->replot(QCustomPlot::rpQueuedReplot);
    }
    onSpectrumTimer();
}

void MainWindow::applyFilters(QVector<double> &data) {
    int count = qMin(data.size(), filterStates.size());
    for (int i = 0; i < count; ++i) {
        double val = data[i];
        if (chkNotch->isChecked()) val = filterStates[i].notch.process(val);
        if (chkHPF->isChecked()) val = filterStates[i].hpf.process(val);
        if (chkLPF->isChecked()) val = filterStates[i].lpf.process(val);
        data[i] = val;
    }
}

void MainWindow::reapplyFiltersToImportedData() {
    if (!m_pagedRaw && (m_importedTimes.isEmpty() || m_importedData.isEmpty())) return;

    const auto spec = offlineFilterSpec();
    if (m_appliedOfflineFilter && spec.sameResponseAs(*m_appliedOfflineFilter)) {
        m_appliedOfflineFilter = spec;
        return;
    }

    if (m_pagedRaw) {
        const auto raw = m_pagedRaw;
        bool canceled = false;
        QString error;
        const auto filtered = filterOfflineStore(raw, spec, canceled, error);
        if (m_pagedRaw != raw) return;
        if (!filtered) {
            restoreOfflineFilters(*m_appliedOfflineFilter);
            if (!canceled) showImportAdvice(error);
            return;
        }
        m_pagedData = filtered;
        m_appliedOfflineFilter = spec;
        for (auto &context : allPlots) {
            context.measureState = 0;
            for (auto tracer : {context.tracer1, context.tracer2, context.hoverTracer}) tracer->setVisible(false);
            context.measureLine->setVisible(false);
            for (auto text : {context.measureText, context.measureText2, context.measureDeltaText, context.hoverText}) text->setVisible(false);
        }
        refreshPagedView();
        return;
    }

    // Each channel has independent filter state. Build its plotted samples
    // directly, in chronological order, and share them across its views.
    const auto times = m_importedTimes.constData();
    for (int channel = 0; channel < qMin(m_importedData.size(), channelGraphs.size()); ++channel) {
        const auto &raw = m_importedData.at(channel);
        const auto values = raw.constData();
        auto &filters = filterStates[channel];
        QVector<QCPGraphData> points(raw.size());
        auto output = points.data();
        for (int i = 0; i < raw.size(); ++i) {
            double value = values[i];
            if (spec.notch) value = filters.notch.process(value);
            if (spec.highpass) value = filters.hpf.process(value);
            if (spec.lowpass) value = filters.lpf.process(value);
            output[i] = QCPGraphData(times[i], value);
        }
        channelGraphs[channel]->data()->set(points, true);
        for (int view = 1; view < allPlots.size(); ++view) {
            if (auto graph = WaveformGraph::forChannel(allPlots[view].plot, channel))
                graph->data()->set(points, true);
        }
    }
    m_appliedOfflineFilter = spec;
    enforcePlotHistoryLimit(true);
    
    // Trigger redraw
    for (PlotContext &ctx : allPlots) {
        ctx.plot->replot(QCustomPlot::rpQueuedReplot);
    }
    if (chkAutoScaleY && chkAutoScaleY->isChecked()) {
        autoScaleY();
    }
}

void MainWindow::updateMeasureUI() {
    // Clear existing layout
    QLayoutItem *child;
    while ((child = measureMetricsLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->deleteLater();
        }
        delete child;
    }
    measureLabels.clear();
    
    QString type = comboSignalType->currentText();    const QMap<QString, QString> signalDescriptions = {
        {QStringLiteral("General"), QStringLiteral("General：通用信号，显示基础统计特征。")},
        {QStringLiteral("50Hz"), QStringLiteral("50Hz：50Hz 工频干扰信号，分析工频成分强度。")},
        {QStringLiteral("ECG"), QStringLiteral("ECG：心电信号，分析心率、R-R 间期和心率变异性。")},
        {QStringLiteral("EEG"), QStringLiteral("EEG：脑电信号，分析 Delta、Theta、Alpha、Beta 频带能量。")},
        {QStringLiteral("EMG"), QStringLiteral("EMG：肌电信号，分析肌肉活动强度、过零率和中位频率。")},
        {QStringLiteral("EOG"), QStringLiteral("EOG：眼电信号，分析眨眼次数和眼动幅度。")},
        {QStringLiteral("PPG"), QStringLiteral("PPG：光电容积脉搏波信号，分析心率、脉搏波幅度和波峰时间。")},
        {QStringLiteral("CBF"), QStringLiteral("CBF：脑血流信号，分析血流波动、心率和血流灌注相关特征。")}
    };
    if (signalTypeHint) {
        signalTypeHint->setText(signalDescriptions.value(type));
    }
    QStringList metrics;
    QMap<QString, QString> tooltips;
    
    if (type == "General") {
        metrics << "Mean" << "RMS" << "Vpp";
        tooltips["Mean"] = "信号的平均值 (直流偏移)\n公式: Mean = (1/N) * Σ(x_i)\n单位: 同信号输入单位";
        tooltips["RMS"] = "信号的有效值 (能量大小)\n公式: RMS = √( (1/N) * Σ(x_i^2) )\n单位: 同信号输入单位";
        tooltips["Vpp"] = "信号的峰峰值 (最大值 - 最小值)\n公式: Vpp = X_max - X_min\n单位: 同信号输入单位";
    } else if (type == "50Hz") {
        metrics << "Mean" << "Total Vpp" << "50Hz Vpp";
        tooltips["Mean"] = "信号的平均值 (直流偏移)\n公式: Mean = (1/N) * Σ(x_i)";
        tooltips["Total Vpp"] = "时域总的峰峰值\n公式: Total Vpp = X_max - X_min";
        tooltips["50Hz Vpp"] = "50Hz 工频成分的峰峰值 (由FFT推算)\n公式: 50Hz Vpp ≈ 2 * FFT_amp(50Hz)\n单位: 同信号单位";
    } else if (type == "CBF") {
        metrics << "HR (BPM)" << "AC Amp" << "DC" << "AC/DC";
        tooltips["HR (BPM)"] = "实时心率 (BPM)\n公式: HR = 60 / 波峰间距(秒)\n单位: 次/分钟 (BPM)";
        tooltips["AC Amp"] = "交流峰值 (血管容积变化)\n公式: AC Amp = X_max - X_min\n单位: 同信号单位";
        tooltips["DC"] = "直流偏移量 (本底基线)\n公式: DC = (1/N) * Σ(x_i)";
        tooltips["AC/DC"] = "血流灌注指数 (PI参考)\n公式: PI = (AC Amp / DC) * 100%\n单位: %";
    } else if (type == "ECG") {
        metrics << "HR (BPM)" << "RR (ms)" << "HRV (SDNN)";
        tooltips["HR (BPM)"] = "实时心率\n公式: HR = 60 / R-R间距(秒)\n单位: 次/分钟 (BPM)";
        tooltips["RR (ms)"] = "心动周期 (R波到R波的时间)\n公式: RR = T_R(i) - T_R(i-1)\n单位: 毫秒 (ms)";
        tooltips["HRV (SDNN)"] = "心率变异性 (SDNN, 反映疲劳状态)\n公式: SDNN = √( 1/(N-1) * Σ(RR_i - Mean_RR)^2 )\n单位: 毫秒 (ms)";
    } else if (type == "EEG") {
        metrics << "Delta" << "Theta" << "Alpha" << "Beta";
        tooltips["Delta"] = "Delta波能量 (0.5-4Hz, 深度睡眠)\n公式: Delta = ∫(0.5~4Hz) PSD(f) df\n单位: uV² (绝对功率)";
        tooltips["Theta"] = "Theta波能量 (4-8Hz, 浅睡/冥想)\n公式: Theta = ∫(4~8Hz) PSD(f) df\n单位: uV² (绝对功率)";
        tooltips["Alpha"] = "Alpha波能量 (8-13Hz, 放松/清醒)\n公式: Alpha = ∫(8~13Hz) PSD(f) df\n单位: uV² (绝对功率)";
        tooltips["Beta"] = "Beta波能量 (13-30Hz, 专注/紧张)\n公式: Beta = ∫(13~30Hz) PSD(f) df\n单位: uV² (绝对功率)";
    } else if (type == "EMG") {
        metrics << "RMS" << "iEMG" << "ZCR" << "MDF (Hz)";
        tooltips["RMS"] = "均方根值 (反映肌肉实时发力大小)\n公式: RMS = √( (1/N) * Σ(x_i^2) )\n单位: 毫伏(mV) 或 微伏(uV)";
        tooltips["iEMG"] = "积分肌电值 (一段时间内的总发力)\n公式: iEMG = Σ|x_i|\n单位: mV·s";
        tooltips["ZCR"] = "过零率 (反映肌肉疲劳度)\n公式: ZCR = 穿过零电平的次数 / 时间\n单位: 次/秒 (Hz)";
        tooltips["MDF (Hz)"] = "中位频率 (反映肌肉疲劳下降指标)\n公式: MDF = 频带能量一分为二的频率点\n单位: 赫兹 (Hz)";
    } else if (type == "EOG") {
        metrics << "Blink Count" << "Max Amp";
        tooltips["Blink Count"] = "当前数据段内的眨眼次数\n算法: Blink Count = 识别突发的特定幅度的大波峰次数\n单位: 次";
        tooltips["Max Amp"] = "眼动幅度 (反映眼球偏转角度)\n公式: Max Amp = max(|x_i|)\n单位: 微伏 (uV)";
    } else if (type == "PPG") {
        metrics << "HR (BPM)" << "AC Amp" << "Crest Time (ms)";
        tooltips["HR (BPM)"] = "实时心率\n公式: HR = 60 / 心动周期(秒)\n单位: 次/分钟 (BPM)";
        tooltips["AC Amp"] = "交流波动峰值 (血管射血参考)\n公式: AC Amp = X_max - X_min";
        tooltips["Crest Time (ms)"] = "波峰时间 (从谷底到波峰的时间，反映心血管阻力)\n算法: Crest Time = 极大值时间点 - 极小值时间点\n单位: 毫秒 (ms)";
    }
    
    for (const QString& m : metrics) {
        QLabel *lbl = new QLabel(QString("%1: --").arg(m));
        lbl->setObjectName("measureLabel");
        lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (tooltips.contains(m)) {
            lbl->setToolTip(tooltips[m]);
        }
        measureMetricsLayout->addWidget(lbl);
        measureLabels[m] = lbl;
    }
}

void MainWindow::onMeasureFinished() {
    PhysioResult res = measureWatcher->result();
    for (auto it = measureLabels.begin(); it != measureLabels.end(); ++it) {
        it.value()->setText(QString("%1: %2").arg(it.key(), res.metrics.value(it.key(), "--")));
        it.value()->setToolTip("通用统计忽略 NaN/无效值；频率、时序及生理特征只分析最新连续有效段，不跨缺口拼接。无有效数据时显示 --。");
    }
}








PhysioResult MainWindow::calculatePhysioFeatures(SignalType type, const QVector<double>& input, double fs, int decimals) {
    PhysioResult res;
    QVector<double> data;
    if (type == SignalType::General) {
        for (double value : input) if (std::isfinite(value)) data.append(value);
    } else {
        int start = input.size();
        while (start > 0 && std::isfinite(input[start - 1])) --start;
        data = input.mid(start); // Never compress a gap and falsify intervals.
        if (data.size() < 2) return res;
    }
    if (data.isEmpty()) return res;
    
    int n = data.size();
    double sum = 0, sumSq = 0, minVal = data[0], maxVal = data[0];
    for (double v : data) {
        sum += v;
        sumSq += v * v;
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
    }
    double mean = sum / n;
    double rms = std::sqrt(sumSq / n);
    double vpp = maxVal - minVal;
    
    if (type == SignalType::General) {
        res.metrics["Mean"] = QString::number(mean, 'f', decimals);
        res.metrics["RMS"] = QString::number(rms, 'f', decimals);
        res.metrics["Vpp"] = QString::number(vpp, 'f', decimals);
        return res;
    }
    
    // --- FFT based features ---
    int fft_n = 1;
    while (fft_n * 2 <= n) fft_n *= 2;
    if (fft_n < 256) fft_n = 256;
    if (fft_n > 8192) fft_n = 8192;
    
    std::vector<std::complex<double>> fftData;
    bool fftReady = false;
    double freqRes = fs / fft_n;
    
    auto runFFT = [&]() {
        if (fftReady || n == 0) return;
        fftData.assign(fft_n, std::complex<double>(0, 0));
        int actualN = qMin(n, fft_n);
        int startIndex = n - actualN;
        for (int i = 0; i < actualN; ++i) {
            double v = data[startIndex + i];
            double w = 0.5 * (1 - cos(2 * M_PI * i / (actualN - 1))); // Hanning
            fftData[i] = std::complex<double>(v * w, 0);
        }
        FftHelper::computeFFT(fftData);
        fftReady = true;
    };
    
    if (type == SignalType::Noise50Hz) {
        res.metrics["Mean"] = QString::number(mean, 'f', decimals);
        res.metrics["Total Vpp"] = QString::number(vpp, 'f', decimals);
        
        runFFT();
        if (fftReady) {
            int targetBin = qRound(50.0 / freqRes);
            if (targetBin > 0 && targetBin < fft_n / 2) {
                double maxMag = 0;
                for (int b = qMax(1, targetBin - 2); b <= qMin(fft_n/2 - 1, targetBin + 2); ++b) {
                    int actualN = qMin(n, fft_n);
                    double mag = std::abs(fftData[b]) * 2.0 / (actualN > 0 ? actualN : 1);
                    if (mag > maxMag) maxMag = mag;
                }
                res.metrics["50Hz Vpp"] = QString::number(maxMag * 4.0, 'f', decimals);
            } else {
                res.metrics["50Hz Vpp"] = "N/A";
            }
        } else {
            res.metrics["50Hz Vpp"] = "N/A";
        }
        return res;
    }
    
    if (type == SignalType::EEG) {
        runFFT();
        if (fftReady) {
            double delta = 0, theta = 0, alpha = 0, beta = 0;
            for (int b = 1; b < fft_n / 2; ++b) {
                double f = b * freqRes;
                double p = std::norm(fftData[b]) * (4.0 / (fft_n * fft_n)); // Normalized Power in uV^2
                if (f >= 0.5 && f < 4.0) delta += p;
                else if (f >= 4.0 && f < 8.0) theta += p;
                else if (f >= 8.0 && f < 13.0) alpha += p;
                else if (f >= 13.0 && f < 30.0) beta += p;
            }
            res.metrics["Delta"] = QString::number(delta, 'f', 2);
            res.metrics["Theta"] = QString::number(theta, 'f', 2);
            res.metrics["Alpha"] = QString::number(alpha, 'f', 2);
            res.metrics["Beta"] = QString::number(beta, 'f', 2);
        } else {
            res.metrics["Delta"] = "N/A"; res.metrics["Theta"] = "N/A";
            res.metrics["Alpha"] = "N/A"; res.metrics["Beta"] = "N/A";
        }
        return res;
    }
    
    if (type == SignalType::EMG) {
        double iemg = 0;
        int zcr = 0;
        for (int i = 0; i < n; ++i) {
            iemg += std::abs(data[i]);
            if (i > 0 && ((data[i] >= 0 && data[i-1] < 0) || (data[i] < 0 && data[i-1] >= 0))) {
                zcr++;
            }
        }
        res.metrics["RMS"] = QString::number(rms, 'f', decimals);
        res.metrics["iEMG"] = QString::number(iemg, 'f', decimals);
        res.metrics["ZCR"] = QString::number(zcr);
        
        runFFT();
        if (fftReady) {
            double totalPower = 0;
            QVector<double> powers(fft_n / 2);
            for (int b = 1; b < fft_n / 2; ++b) {
                double p = std::norm(fftData[b]);
                powers[b] = p;
                totalPower += p;
            }
            double cumPower = 0;
            double mdf = 0;
            for (int b = 1; b < fft_n / 2; ++b) {
                cumPower += powers[b];
                if (cumPower >= totalPower / 2.0) {
                    mdf = b * freqRes;
                    break;
                }
            }
            res.metrics["MDF (Hz)"] = QString::number(mdf, 'f', 2);
        } else {
            res.metrics["MDF (Hz)"] = "N/A";
        }
        return res;
    }
    
    // Simple peak detection for HR/PPG/CBF
    auto findPeaks = [&](double threshold, int minDistance) {
        QVector<int> peaks;
        for (int i = 1; i < n - 1; ++i) {
            if (data[i] > data[i-1] && data[i] > data[i+1] && data[i] > threshold) {
                if (peaks.isEmpty() || i - peaks.last() >= minDistance) {
                    peaks.append(i);
                }
            }
        }
        return peaks;
    };
    
    if (type == SignalType::ECG || type == SignalType::PPG || type == SignalType::CBF) {
        double threshold = mean + 0.5 * (maxVal - mean);
        int minDistance = fs * 0.4; // max ~150 BPM
        QVector<int> peaks = findPeaks(threshold, minDistance);
        
        double hr = 0, rr = 0, sdnn = 0;
        if (peaks.size() > 1) {
            double totalRR = 0;
            QVector<double> rrIntervals;
            for (int i = 1; i < peaks.size(); ++i) {
                double interval = (peaks[i] - peaks[i-1]) / fs;
                rrIntervals.append(interval);
                totalRR += interval;
            }
            double meanRR = totalRR / rrIntervals.size();
            hr = 60.0 / meanRR;
            rr = meanRR * 1000.0;
            
            if (type == SignalType::ECG && rrIntervals.size() > 1) {
                double sumSqDiff = 0;
                for (double val : rrIntervals) sumSqDiff += (val - meanRR) * (val - meanRR);
                sdnn = std::sqrt(sumSqDiff / (rrIntervals.size() - 1)) * 1000.0;
            }
        }
        
        if (type == SignalType::ECG) {
            res.metrics["HR (BPM)"] = peaks.size() > 1 ? QString::number(hr, 'f', 1) : "N/A";
            res.metrics["RR (ms)"] = peaks.size() > 1 ? QString::number(rr, 'f', 1) : "N/A";
            res.metrics["HRV (SDNN)"] = peaks.size() > 2 ? QString::number(sdnn, 'f', 2) : "N/A";
        } else if (type == SignalType::PPG) {
            res.metrics["HR (BPM)"] = peaks.size() > 1 ? QString::number(hr, 'f', 1) : "N/A";
            res.metrics["AC Amp"] = QString::number(vpp, 'f', decimals);
            // Crest time estimation
            double crestTime = 0;
            if (peaks.size() > 0) {
                int p = peaks.last();
                int trough = p;
                while (trough > 0 && data[trough-1] <= data[trough]) trough--;
                crestTime = (p - trough) / fs * 1000.0;
            }
            res.metrics["Crest Time (ms)"] = peaks.size() > 0 ? QString::number(crestTime, 'f', 1) : "N/A";
        } else if (type == SignalType::CBF) {
            res.metrics["HR (BPM)"] = peaks.size() > 1 ? QString::number(hr, 'f', 1) : "N/A";
            res.metrics["AC Amp"] = QString::number(vpp, 'f', decimals);
            res.metrics["DC"] = QString::number(mean, 'f', decimals);
            double acdc = std::abs(mean) > 1e-6 ? vpp / std::abs(mean) : 0;
            res.metrics["AC/DC"] = QString::number(acdc, 'f', decimals);
        }
        return res;
    }
    
    if (type == SignalType::EOG) {
        double threshold = mean + 0.6 * (maxVal - mean);
        int minDistance = fs * 0.2;
        QVector<int> peaks = findPeaks(threshold, minDistance);
        res.metrics["Blink Count"] = QString::number(peaks.size());
        res.metrics["Max Amp"] = QString::number(maxVal - mean, 'f', decimals);
        return res;
    }
    
    return res;
}
