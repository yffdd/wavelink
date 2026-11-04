#include "mainwindow.h"
#include "plotmeasurementcontroller.h"
#include "receiveconsolebuffer.h"
#include "systemsettingsdialog.h"
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
#include "statusmonitorwindow.h"
#include "ProtocolConfigTransfer.h"
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QSignalBlocker>
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

void MainWindow::applySystemPreferences(const SystemPreferences &preferences, bool persist)
{
    m_systemPreferences = preferences;
    m_systemPreferences.receiveLines = ReceiveConsoleBuffer::validLines(preferences.receiveLines);
    m_systemPreferences.receiveCharacters = ReceiveConsoleBuffer::validCharacters(preferences.receiveCharacters);
    m_systemPreferences.receiveRefreshMs = SystemPreferences::validRefresh(preferences.receiveRefreshMs);
    m_systemPreferences.plotFps = SystemPreferences::validFps(preferences.plotFps);
    m_systemPreferences.plotLineWidth = SystemPreferences::validLineWidth(preferences.plotLineWidth);
    m_systemPreferences.offlineMemoryMB = SystemPreferences::validOfflineMemoryMB(preferences.offlineMemoryMB);
    m_systemPreferences.defaultBottomPercent = SystemPreferences::validBottomPercent(preferences.defaultBottomPercent);
    {
        const QSignalBlocker block(comboReceiveLines);
        comboReceiveLines->setCurrentIndex(comboReceiveLines->findData(m_systemPreferences.receiveLines));
    }
    textReceive->document()->setMaximumBlockCount(m_systemPreferences.receiveLines);
    trimReceiveText();
    const auto hint = QStringLiteral("当前保留最近 %1 行，最多 %2 字符，每 %3 ms 刷新一次。达到任一上限只移除最旧内容，不整框清空；自动折行不计入行数。仅影响显示及保存/复制接收，不影响协议解析、波形或录制。更多选项在“设置 → 系统设置 → 数据收发”。")
        .arg(m_systemPreferences.receiveLines).arg(m_systemPreferences.receiveCharacters).arg(m_systemPreferences.receiveRefreshMs);
    comboReceiveLines->setToolTip(hint);
    if (auto label = findChild<QLabel *>("receiveCacheLabel")) label->setToolTip(hint);
    if (textUpdateTimer) textUpdateTimer->setInterval(m_systemPreferences.receiveRefreshMs);
    if (plotTimer) plotTimer->setInterval(qRound(1000.0 / m_systemPreferences.plotFps));
    for (const auto &context : allPlots) {
        bool changed = false;
        for (int ch = 0; ch < context.plot->graphCount(); ++ch) {
            auto graph = dynamic_cast<WaveformGraph *>(context.plot->graph(ch));
            if (!graph) continue;
            auto pen = graph->pen();
            if (pen.isCosmetic() && pen.widthF() == m_systemPreferences.plotLineWidth) continue;
            pen.setWidthF(m_systemPreferences.plotLineWidth); pen.setCosmetic(true);
            graph->setPen(pen); changed = true;
        }
        if (changed) context.plot->replot(QCustomPlot::rpQueuedReplot);
    }
    if (persist) { QSettings settings("WaveLink", "WaveLink"); m_systemPreferences.write(settings); settings.sync(); }
    if (m_pagedData) m_offlineViewTimer.start(0);
}

void MainWindow::showSystemSettings()
{
    // The menu opens the first category; the console's shortcut still opens
    // its own receive settings after reordering the categories.
    const auto initialCategory = sender() && sender()->objectName() == "btnReceiveSettings"
        ? SystemSettingsDialog::Category::Receive : SystemSettingsDialog::Category::Appearance;
    SystemSettingsDialog dialog(m_systemPreferences, [this](const SystemPreferences &preferences) {
        applySystemPreferences(preferences, true);
    }, this, initialCategory);
    dialog.exec();
}

void MainWindow::loadCustomColors()
{
    QSettings settings("MyCompany", "WaveLink");
    settings.beginGroup("CustomColors");
    for (int i = 0; i < QColorDialog::customCount(); ++i) {
        if (settings.contains(QString("Color%1").arg(i))) {
            QColorDialog::setCustomColor(i, settings.value(QString("Color%1").arg(i)).value<QColor>());
        }
    }
    settings.endGroup();
}

void MainWindow::saveCustomColors()
{
    QSettings settings("MyCompany", "WaveLink");
    settings.beginGroup("CustomColors");
    for (int i = 0; i < QColorDialog::customCount(); ++i) {
        settings.setValue(QString("Color%1").arg(i), QColorDialog::customColor(i));
    }
    settings.endGroup();
}

QColorDialog* MainWindow::createStyledColorDialog(const QString &title, const QColor &initialColor, QWidget *parent)
{
    QColorDialog *dlg = new QColorDialog(initialColor, parent);
    dlg->setWindowTitle(title);
    dlg->setOption(QColorDialog::DontUseNativeDialog, true);
    
    bool isDark = (comboTheme && comboTheme->currentIndex() == 0);
    if (isDark) {
        dlg->setStyleSheet("QColorDialog { background-color: #1e1e1e; color: #dcdcdc; } "
                           "QLabel { color: #dcdcdc; } "
                           "QColorLuminancePicker { color: #000000; } "
                           "QPushButton { background-color: #333337; color: #dcdcdc; border: 1px solid #555555; padding: 4px 12px; border-radius: 2px; } "
                           "QPushButton:hover { background-color: #3e3e42; } "
                           "QSpinBox { background-color: #2d2d30; color: #dcdcdc; border: 1px solid #555555; padding: 2px; } "
                           "QLineEdit { background-color: #2d2d30; color: #dcdcdc; border: 1px solid #555555; padding: 2px; }");
    } else {
        dlg->setStyleSheet("QColorDialog { background-color: #f5f5f5; color: #111111; } "
                           "QLabel { color: #111111; } "
                           "QColorLuminancePicker { color: #000000; } "
                           "QPushButton { background-color: #e0e0e0; color: #111111; border: 1px solid #adadad; padding: 4px 12px; border-radius: 2px; } "
                           "QPushButton:hover { background-color: #d0d0d0; } "
                           "QSpinBox { background-color: #ffffff; color: #111111; border: 1px solid #adadad; padding: 2px; } "
                           "QLineEdit { background-color: #ffffff; color: #111111; border: 1px solid #adadad; padding: 2px; }");
    }
    
    QLineEdit *htmlEdit = dlg->findChild<QLineEdit *>();
    if (htmlEdit) {
        connect(htmlEdit, &QLineEdit::textEdited, [htmlEdit](const QString &text) {
            if (!text.startsWith("#")) {
                QString newText = text;
                htmlEdit->setText("#" + newText.remove("#"));
            }
            htmlEdit->setText(htmlEdit->text().toUpper());
        });
    }
    
    QPushButton *btnAddCustom = nullptr;
    for (QLabel *lbl : dlg->findChildren<QLabel*>()) {
        if (lbl->text().contains("Basic colors")) lbl->setText("提示");
        if (lbl->text().contains("Custom colors")) lbl->setText(QString::fromUtf8("自定义颜色"));
    }
    for (QPushButton *btn : dlg->findChildren<QPushButton*>()) {
        if (btn->text().contains("Pick Screen Color")) btn->setText(QString::fromUtf8("拾取屏幕颜色"));
        if (btn->text().contains("Add to Custom Colors")) {
            btn->setText(QString::fromUtf8("添加到自定义颜色"));
            btnAddCustom = btn;
        }
    }
    
    if (btnAddCustom) {
        QBoxLayout *layout = nullptr;
        for (QBoxLayout *l : dlg->findChildren<QBoxLayout*>()) {
            if (l->indexOf(btnAddCustom) != -1) {
                layout = l;
                break;
            }
        }
        if (layout) {
            QPushButton *btnClear = new QPushButton(QString::fromUtf8("清除自定义颜色"));
            if (isDark) {
                btnClear->setStyleSheet("QPushButton { background-color: #333337; color: #dcdcdc; border: 1px solid #555555; padding: 4px 12px; border-radius: 2px; } QPushButton:hover { background-color: #3e3e42; }");
            } else {
                btnClear->setStyleSheet("QPushButton { background-color: #e0e0e0; color: #111111; border: 1px solid #adadad; padding: 4px 12px; border-radius: 2px; } QPushButton:hover { background-color: #d0d0d0; }");
            }
            layout->insertWidget(layout->indexOf(btnAddCustom) + 1, btnClear);
            connect(btnClear, &QPushButton::clicked, [dlg, btnAddCustom]() {
                QColor oldColor = dlg->currentColor();
                dlg->setCurrentColor(Qt::white);
                btnAddCustom->click();
                dlg->setCurrentColor(oldColor);
            });
            // Hook up saving custom colors on add
            connect(btnAddCustom, &QPushButton::clicked, this, &MainWindow::saveCustomColors);
            // Only add the "Cancel TopMost" button for the global Color Picker, not the channel waveform color picker!
            if (title == QString::fromUtf8("颜色拾取器")) {
                QPushButton *btnPin = new QPushButton(QString::fromUtf8("取消置顶"));
                btnPin->setStyleSheet("QPushButton { background-color: #0078d7; color: #ffffff; border: 1px solid #005a9e; padding: 4px 12px; border-radius: 2px; font-weight: bold; } "
                                      "QPushButton:hover { background-color: #1084d9; } "
                                      "QPushButton:checked { background-color: #005a9e; border-color: #004578; }");
                btnPin->setCheckable(true);
                btnPin->setChecked(true);
                dlg->setWindowFlag(Qt::WindowStaysOnTopHint, true);
                connect(btnPin, &QPushButton::toggled, [dlg, btnPin](bool checked) {
                    dlg->setWindowFlag(Qt::WindowStaysOnTopHint, checked);
                    btnPin->setText(checked ? QString::fromUtf8("取消置顶") : QString::fromUtf8("窗口置顶"));
                    dlg->show(); // Re-show to apply flags on Windows
                });
                layout->insertWidget(layout->indexOf(btnClear) + 1, btnPin);
            }
        }
    }
    
    return dlg;
}

void MainWindow::saveConfig()
{
    QSettings settings("WaveLink", "WaveLink");
    QSettings networkSettings("WaveLink", "AdvancedApp");
    networkSettings.setValue("NetworkIP", editIP->text().trimmed()); networkSettings.setValue("NetworkPort", spinPort->value());
    networkSettings.setValue("NetworkLocalIP", comboLocalIP->currentText().trimmed()); networkSettings.setValue("NetworkLocalPort", spinLocalPort->value());
    networkSettings.setValue("ServerIP", comboServerIP->currentText().trimmed()); networkSettings.setValue("ServerPort", spinServerPort->value());
    networkSettings.setValue("Udp/LocalIP", comboUdpLocalIP->currentText().trimmed()); networkSettings.setValue("Udp/LocalPort", spinUdpLocalPort->value());
    networkSettings.setValue("Udp/RemoteIP", editUdpRemoteIP->text().trimmed()); networkSettings.setValue("Udp/RemotePort", spinUdpRemotePort->value());
    networkSettings.setValue("Udp/FilterRemote", chkUdpFilterRemote->isChecked());
    settings.setValue("comboTheme", comboTheme->currentIndex());
    
    // Save UI Layout State
    settings.setValue("windowGeometry", saveGeometry());
    settings.setValue("dataTransceiveModeActive", m_dataTransceiveMode);

    if (m_dataTransceiveMode) {
        m_dataModeWindowState = saveState();
        if (consoleSplitter) {
            m_dataModeConsoleSplitterState = consoleSplitter->saveState();
        }
    } else {
        m_normalWindowStateBeforeDataMode = saveState();
        if (innerMainWindow) {
            m_normalInnerStateBeforeDataMode = innerMainWindow->saveState();
        }
        if (consoleSplitter) {
            m_normalConsoleSplitterState = consoleSplitter->saveState();
        }
        m_normalCollapsedDockNames = collapsedDockNames();
    }

    settings.setValue("windowState", m_normalWindowStateBeforeDataMode);
    settings.setValue("innerWindowState", m_normalInnerStateBeforeDataMode);
    settings.setValue("normalConsoleSplitterState", m_normalConsoleSplitterState);
    settings.setValue("normalCollapsedDocks", m_normalCollapsedDockNames);
    settings.setValue("dataModeWindowState", m_dataModeWindowState);
    settings.setValue("dataModeConsoleSplitterState", m_dataModeConsoleSplitterState);
    
    // Config Dock
    if (m_protocolStore && m_protocolStore->ready()) {
        const auto selection = m_protocolStore->snapshot().shared;
        settings.setValue("comboProtocol", selection.kind == "custom" ? 4 : selection.legacyIndex());
    }
    settings.setValue("comboBaudRate", comboBaudRate->currentIndex());
    settings.setValue("comboDataBits", comboDataBits->currentIndex());
    settings.setValue("comboParity", comboParity->currentIndex());
    settings.setValue("comboStopBits", comboStopBits->currentIndex());
    settings.setValue("Serial/DTR", m_serialDtrPreference);
    settings.setValue("Serial/RTS", m_serialRtsPreference);
    settings.setValue("Serial/FlowControl", comboFlowControl->currentData());
    settings.setValue("Serial/BreakDurationMs", spinSerialBreakMs->value());
    // Break's asserted state is deliberately never saved or restored.
    settings.setValue("spinFs", spinFs->value());
    settings.setValue("editCacheDir", editCacheDir->text());
    settings.setValue("comboRecordingMode", comboRecordingMode->currentIndex());
    settings.setValue("spinCacheLimit", spinCacheLimit->value());
    settings.setValue("chkClearBeforeRecord", chkClearBeforeRecord->isChecked());
    
    // Console Dock
    settings.setValue("chkShowReceiveData", chkShowReceiveData->isChecked());
    settings.setValue("chkHexReceive", chkHexReceive->isChecked());
    settings.setValue("comboEncoding", comboEncoding->currentIndex());
    settings.setValue("comboAppendNewline", comboAppendNewline->currentIndex());
    settings.setValue("chkHexSend", chkHexSend->isChecked());
    settings.setValue("Console/SendIntervalMs", spinConsoleSendInterval->value());
    m_systemPreferences.write(settings);
    // Never persist a running send operation or resume it after import/startup.
    
    // Channel Dock
    settings.setValue("chkAutoScaleY", chkAutoScaleY->isChecked());
    settings.setValue("spinDecimals", spinDecimals->value());
    settings.setValue("chkPausePlot", btnPausePlot->isChecked());
    settings.setValue("spinPlotHistoryLimit", spinPlotHistoryLimit->value());
    settings.setValue("filterPanelVisible",
                      filterPanelAction ? filterPanelAction->isChecked() : true);
    
    // Filter Dock
    // Filter enable states are intentionally not persisted. A new session starts raw.
    settings.setValue("chkNotch", false);
    settings.setValue("spinNotchFreq", spinNotchFreq->value());
    settings.setValue("chkHPF", false);
    settings.setValue("spinHPFFreq", spinHPFFreq->value());
    settings.setValue("chkLPF", false);
    settings.setValue("spinLPFFreq", spinLPFFreq->value());
    settings.setValue("comboFilterOrder", comboFilterOrder->currentIndex());
    
    // Measure Dock
    settings.setValue("comboMeasureChannel", comboMeasureChannel->currentIndex());
    settings.setValue("comboSignalType", comboSignalType->currentIndex());
    settings.setValue("comboSignalTypeName", comboSignalType->currentText());
    
    // Spectrum Dock
    settings.setValue("comboSampleRate", comboSampleRate->currentIndex());
    settings.setValue("comboFftPoints", comboFftPoints->currentIndex());
    if (chkRemoveDC) settings.setValue("chkRemoveDC", chkRemoveDC->isChecked());
}

void MainWindow::loadConfig()
{
    QSettings settings("WaveLink", "WaveLink");
    
    // Theme
    m_mainPlotBackground = QColor(settings.value("PlotBackground/Main").toString());
    m_detachedPlotBackground = QColor(settings.value("PlotBackground/Detached").toString());
    int themeIdx = settings.value("comboTheme", 0).toInt();
    if (themeIdx >= 0 && themeIdx < comboTheme->count()) {
        comboTheme->setCurrentIndex(themeIdx);
        // Explicitly apply theme in case currentIndex hasn't changed (e.g. both are 0)
        ThemeManager::Theme theme = static_cast<ThemeManager::Theme>(comboTheme->itemData(themeIdx).toInt());
        ThemeManager::applyTheme(qApp, theme);
        if(cmdWindow)cmdWindow->applyTheme(theme==ThemeManager::DarkTheme);
        if(m_statusWindow)m_statusWindow->applyTheme(theme==ThemeManager::DarkTheme);
        updateWaveformPlotThemes();
    }
    
    // Restore UI Layout State
    // Clear potentially corrupted layout state from older versions to fix docking rules (ONLY ONCE)
    if (!settings.contains("layoutCacheCleared_v5")) {
        settings.remove("windowState");
        settings.remove("innerWindowState");
        settings.setValue("layoutCacheCleared_v5", true);
    }
    
    if (settings.contains("windowGeometry")) {
        restoreGeometry(settings.value("windowGeometry").toByteArray());
    }
    const QByteArray winState = settings.value("windowState").toByteArray();
    const QByteArray innerWinState = settings.value("innerWindowState").toByteArray();
    m_normalWindowStateBeforeDataMode = winState;
    m_normalInnerStateBeforeDataMode = innerWinState;
    m_normalConsoleSplitterState =
        settings.value("normalConsoleSplitterState").toByteArray();
    m_normalCollapsedDockNames =
        settings.value("normalCollapsedDocks").toStringList();
    m_dataModeWindowState = settings.value("dataModeWindowState").toByteArray();
    m_dataModeConsoleSplitterState =
        settings.value("dataModeConsoleSplitterState").toByteArray();
    const bool restoreDataMode =
        settings.value("dataTransceiveModeActive", false).toBool();

    QTimer::singleShot(100, this, [this, winState, innerWinState, restoreDataMode]() {
        if (restoreDataMode) {
            if (m_normalWindowStateBeforeDataMode.isEmpty()) {
                m_normalWindowStateBeforeDataMode = m_defaultState;
            }
            if (m_normalInnerStateBeforeDataMode.isEmpty()) {
                m_normalInnerStateBeforeDataMode = m_defaultInnerState;
            }
            setDataTransceiveMode(true, true);
            return;
        }

        if (!winState.isEmpty()) {
            restoreState(winState);
            if (!innerWinState.isEmpty() && innerMainWindow) {
                innerMainWindow->restoreState(innerWinState);
            }
            if (consoleSplitter && !m_normalConsoleSplitterState.isEmpty()) {
                consoleSplitter->restoreState(m_normalConsoleSplitterState);
            }
            restoreDockCollapseStates(m_normalCollapsedDockNames);
        } else {
            restoreDefaultLayout();
        }
    });
    
    // Config Dock
    // Stable selections in the protocol repository are authoritative. Legacy
    // indices are read once, during initial migration, never on reload.
    if (!connectionIsReady() && !m_connectionOperationPending) {
        QSettings networkSettings("WaveLink", "AdvancedApp");
        {
            const QSignalBlocker a(editIP), b(spinPort), c(comboLocalIP), d(spinLocalPort), e(comboServerIP), f(spinServerPort);
            editIP->setText(networkSettings.value("NetworkIP", "192.168.4.1").toString());
            spinPort->setValue(networkSettings.value("NetworkPort", 8080).toInt());
            comboLocalIP->setCurrentText(networkSettings.value("NetworkLocalIP", "0.0.0.0").toString());
            spinLocalPort->setValue(networkSettings.value("NetworkLocalPort", 0).toInt());
            comboServerIP->setCurrentText(networkSettings.value("ServerIP", "0.0.0.0").toString());
            spinServerPort->setValue(networkSettings.value("ServerPort", 4970).toInt());
            const QSignalBlocker g(comboUdpLocalIP), h(spinUdpLocalPort), i(editUdpRemoteIP), j(spinUdpRemotePort), k(chkUdpFilterRemote);
            comboUdpLocalIP->setCurrentText(networkSettings.value("Udp/LocalIP", "0.0.0.0").toString());
            spinUdpLocalPort->setValue(networkSettings.value("Udp/LocalPort", 4970).toInt());
            editUdpRemoteIP->setText(networkSettings.value("Udp/RemoteIP", "").toString());
            spinUdpRemotePort->setValue(networkSettings.value("Udp/RemotePort", 0).toInt());
            chkUdpFilterRemote->setChecked(networkSettings.value("Udp/FilterRemote", true).toBool());
        }
        QMetaObject::invokeMethod(comboServerIP, "currentTextChanged", Q_ARG(QString, comboServerIP->currentText()));
    }
    if (!m_protocolInitialized) { const QSignalBlocker blocker(comboProtocol); comboProtocol->setCurrentIndex(qBound(0, settings.value("comboProtocol", 1).toInt(), 3)); }
    comboBaudRate->setCurrentIndex(settings.value("comboBaudRate", 4).toInt()); // 115200 default
    comboDataBits->setCurrentIndex(settings.value("comboDataBits", 3).toInt());
    comboParity->setCurrentIndex(settings.value("comboParity", 0).toInt());
    comboStopBits->setCurrentIndex(settings.value("comboStopBits", 0).toInt());
    {
        const QSignalBlocker dtrBlocker(chkDtr), rtsBlocker(chkRts);
        m_serialDtrPreference = settings.value("Serial/DTR", false).toBool();
        m_serialRtsPreference = settings.value("Serial/RTS", false).toBool();
        chkDtr->setTristate(false);
        chkRts->setTristate(false);
        chkDtr->setChecked(m_serialDtrPreference);
        chkRts->setChecked(m_serialRtsPreference);
    }
    if (connectionIsReady() && m_activeInterfaceType == 0) onSerialControlLinesChanged();
    if (!connectionIsReady() && !m_connectionOperationPending) {
        comboFlowControl->setCurrentIndex(settings.value("Serial/FlowControl", 0).toInt() == 1 ? 1 : 0);
    }
    updateSerialFlowControls();
    spinSerialBreakMs->setValue(qBound(1, settings.value("Serial/BreakDurationMs", 250).toInt(), 5000));
    spinFs->setValue(settings.value("spinFs", 250.0).toDouble());
    editCacheDir->setText(settings.value("editCacheDir", QDir::homePath() + "/Downloads").toString());
    comboRecordingMode->setCurrentIndex(
        settings.value("comboRecordingMode", settings.value("RecordingMode", 0)).toInt());
    spinCacheLimit->setValue(settings.value("spinCacheLimit", settings.value("CacheLimit", 500)).toInt());
    chkClearBeforeRecord->setChecked(settings.value("chkClearBeforeRecord", true).toBool());
    
    // Console Dock
    chkShowReceiveData->setChecked(settings.value("chkShowReceiveData", true).toBool());
    chkHexReceive->setChecked(settings.value("chkHexReceive", false).toBool());
    comboEncoding->setCurrentIndex(settings.value("comboEncoding", 0).toInt());
    comboAppendNewline->setCurrentIndex(settings.value("comboAppendNewline", 1).toInt());
    chkHexSend->setChecked(settings.value("chkHexSend", false).toBool());
    stopConsolePeriodicSend();
    spinConsoleSendInterval->setValue(qBound(1, settings.value("Console/SendIntervalMs", 1000).toInt(), 3600000));
    applySystemPreferences(SystemPreferences::read(settings), false);
    
    // Channel Dock
    chkAutoScaleY->setChecked(settings.value("chkAutoScaleY", true).toBool());
    spinDecimals->setValue(settings.value("spinDecimals", 2).toInt());
    btnPausePlot->setChecked(settings.value("chkPausePlot", false).toBool());
    spinPlotHistoryLimit->setValue(
        settings.value("spinPlotHistoryLimit",
                       settings.value("PlotHistoryLimitMB", 64)).toInt());
    if (filterPanelAction) {
        filterPanelAction->setChecked(
            settings.value("filterPanelVisible", true).toBool());
    }
    
    // Filter Dock
    spinNotchFreq->setValue(settings.value("spinNotchFreq", 50.0).toDouble());
    spinHPFFreq->setValue(settings.value("spinHPFFreq", 0.1).toDouble());
    spinLPFFreq->setValue(settings.value("spinLPFFreq", 10.0).toDouble());
    comboFilterOrder->setCurrentIndex(settings.value("comboFilterOrder", 0).toInt()); // 2nd order default (0)
    // Do not restore old enable states. The first waveform after startup/reload is raw.
    clearFilterSelections();
    
    // Measure Dock
    comboMeasureChannel->setCurrentIndex(settings.value("comboMeasureChannel", 0).toInt());
    const QString savedSignalType = settings.value("comboSignalTypeName").toString();
    if (!savedSignalType.isEmpty()) {
        const QString normalizedSignalType =
            savedSignalType == QStringLiteral("NFC")
                ? QStringLiteral("CBF")
                : savedSignalType;
        const int savedIndex = comboSignalType->findText(normalizedSignalType);
        comboSignalType->setCurrentIndex(savedIndex >= 0 ? savedIndex : 0);
    } else {
        const int oldIndex = settings.value("comboSignalType", 0).toInt();
        const int compatibleIndex =
            oldIndex == 2
                ? comboSignalType->findText(QStringLiteral("CBF"))
                : (oldIndex >= 3 ? oldIndex - 1 : oldIndex);
        comboSignalType->setCurrentIndex(
            qBound(0, compatibleIndex, comboSignalType->count() - 1));
    }
    
    // Spectrum Dock
    comboSampleRate->setCurrentIndex(settings.value("comboSampleRate", 0).toInt());
    comboFftPoints->setCurrentIndex(settings.value("comboFftPoints", 0).toInt()); // 256 default
    if (chkRemoveDC) chkRemoveDC->setChecked(settings.value("chkRemoveDC", true).toBool());
    for (const auto &context : allPlots) if (context.scope) context.scope->reloadPreferences();
}


void MainWindow::onExportConfig()
{
    QString fileName = QFileDialog::getSaveFileName(this, "导出全局配置", "WaveLinkConfig.ini", "Config Files (*.ini)");
    if (fileName.isEmpty()) return;
    
    if (!m_protocolStore || !m_protocolStore->ready() || m_protocolApplyPending || m_configRecoveryPending) {
        QMessageBox::warning(this, "暂不能导出", "请等待协议应用完成；若协议库损坏，请先在管理窗口恢复。"); return;
    }
    saveConfig();
    if(m_statusWindow)m_statusWindow->savePreferences();
    QString error;
    const auto cmdPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("cmd_panels.json");
    if (DataProtocols::ProtocolConfigTransfer::exportFile(fileName, m_protocolStore->snapshot(), cmdPath, error))
        QMessageBox::information(this, "导出成功", QString("全局配置已导出。\n包含全部 %1 个自定义协议及当前数据协议选择。%2")
            .arg(m_protocolStore->snapshot().profiles.size()).arg((QFile::exists(cmdPath) ? QString("\n命令交互面板配置也已包含。") : QString())+"\n状态监视面板及窗口偏好也已包含。\n缓存路径仅保存在本机，不包含在导出配置中。"));
    else QMessageBox::critical(this, "导出失败", error);
}

void MainWindow::onImportConfig()
{
    if (isPortOpen || protocolChangeBlocked()) {
        QMessageBox::warning(this, "暂不能导入", "请先断开连接、停止录制和发送，再导入全局配置。"); return;
    }
    QString fileName = QFileDialog::getOpenFileName(this, QString::fromUtf8("导入全局配置"), "", "Config Files (*.ini)");
    if (fileName.isEmpty()) return;
    
    QVariantMap values; QString error;
    if (!DataProtocols::ProtocolConfigTransfer::read(fileName, values, error)) { QMessageBox::critical(this, "导入失败", error); return; }
    int protocolPolicy = static_cast<int>(DataProtocols::ProtocolImportMode::MergeById);
    bool restoreSelections = true;
    if (values.contains("DataProtocols/json")) {
        QDialog dialog(this); dialog.setObjectName("configProtocolImportDialog"); dialog.setWindowTitle("全局配置 · 自定义协议导入"); dialog.resize(660, 460); dialog.setSizeGripEnabled(true);
        auto layout = new QVBoxLayout(&dialog); auto mode = new QComboBox;
        auto intro = new QLabel("检测到全局配置包含自定义协议库。请选择导入方式；命令面板将在下一步单独选择。");
        intro->setWordWrap(true); layout->addWidget(intro);
        mode->setObjectName("configProtocolImportMode");
        mode->addItems({"追加：保留现有协议，导入为新协议", "合并：按 ID 更新同一协议，添加新协议", "覆盖：替换整个自定义协议库", "跳过：保留当前协议库及协议选择"}); mode->setCurrentIndex(1); layout->addWidget(mode);
        auto restore = new QCheckBox("同时恢复配置文件中的当前数据协议选择（所有接口共用）"); restore->setObjectName("configProtocolRestoreSelection"); restore->setChecked(true); layout->addWidget(restore);
        auto text = new QPlainTextEdit; text->setObjectName("configProtocolImportPreview"); text->setReadOnly(true); layout->addWidget(text);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
        auto preview = [&] {
            const bool skip = mode->currentIndex() == 3; restore->setEnabled(!skip);
            DataProtocols::ProtocolImportPreview result;
            const auto bytes = values.value("DataProtocols/json").toString().toUtf8();
            error.clear();
            const bool ok = m_protocolStore && m_protocolStore->ready() && (skip ||
                DataProtocols::ProtocolProfileStore::prepareImport(m_protocolStore->snapshot(), bytes, true, static_cast<DataProtocols::ProtocolImportMode>(mode->currentIndex()), restore->isChecked(), result, error));
            buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
            QString message = !ok ? error : skip ? "协议库及协议选择保持不变。旧索引、大小端和 BLE 协议设置也不会覆盖当前选择。" :
                QString("新增 %1 个，更新 %2 个，移除 %3 个。\n%4\n\n导入后的协议：\n").arg(result.added).arg(result.updated).arg(result.removed)
                    .arg(restore->isChecked() ? "恢复协议选择（追加时自动映射到新 ID）。" : "保持当前协议选择；替换不能删除其引用的方案。");
            if (ok && !skip) for (const auto &p : result.snapshot.profiles) message += p.name + "\n";
            text->setPlainText(message);
        };
        connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, preview); connect(restore, &QCheckBox::toggled, &dialog, preview);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); preview();
        if (dialog.exec() != QDialog::Accepted) return;
        protocolPolicy = mode->currentIndex() == 3 ? -1 : mode->currentIndex(); restoreSelections = restore->isChecked();
    }
    int importCommandPanelMode = 0; // 0=skip, 1=overwrite, 2=append
    if (values.contains("CommandPanel/json")) {
        QMessageBox msgBox(this);
        msgBox.setObjectName("configCommandImportDialog");
        msgBox.setWindowTitle(QString::fromUtf8("导入命令面板"));
        msgBox.setText(QString::fromUtf8("检测到全局配置文件中包含“命令交互面板”的数据。\n\n"
                       "覆盖：将当前的命令面板配置完全替换。\n"
                       "追加：将新配置追加到现有的命令面板列表中。\n"
                       "跳过：不导入命令面板配置（直接关闭也默认跳过）。"));
        QPushButton *btnOverwrite = msgBox.addButton(QString::fromUtf8("覆盖"), QMessageBox::AcceptRole);
        QPushButton *btnAppend = msgBox.addButton(QString::fromUtf8("追加"), QMessageBox::AcceptRole);
        QPushButton *btnSkip = msgBox.addButton(QString::fromUtf8("跳过"), QMessageBox::RejectRole);
        btnOverwrite->setObjectName("configCommandOverwrite"); btnAppend->setObjectName("configCommandAppend"); btnSkip->setObjectName("configCommandSkip");
        msgBox.setDefaultButton(btnSkip);
        msgBox.exec();

        if (msgBox.clickedButton() == btnOverwrite) {
            importCommandPanelMode = 1;
        } else if (msgBox.clickedButton() == btnAppend) {
            importCommandPanelMode = 2;
        } else {
            importCommandPanelMode = 0;
        }
    }
    
    int importStatusMode=0;
    if(values.contains("StatusPanel/json")){
        QMessageBox box(this);box.setObjectName("configStatusImportDialog");box.setWindowTitle("导入状态面板");
        box.setText("配置文件包含状态监视面板。\n\n覆盖：替换全部状态面板及窗口偏好。\n追加：保留现有面板，导入为新面板并避免重名。\n跳过：保留当前状态面板。\n\n协议追加导入时，同步映射导入监视项的协议 ID；缺失或结构不符的绑定不会自动跟随通道。");
        auto replace=box.addButton("覆盖",QMessageBox::AcceptRole);auto append=box.addButton("追加",QMessageBox::AcceptRole);auto skip=box.addButton("跳过",QMessageBox::RejectRole);
        replace->setObjectName("configStatusOverwrite");append->setObjectName("configStatusAppend");skip->setObjectName("configStatusSkip");box.setDefaultButton(skip);box.exec();
        if(box.clickedButton()==replace)importStatusMode=1;else if(box.clickedButton()==append)importStatusMode=2;
    }
    if (isPortOpen || protocolChangeBlocked()) { QMessageBox::warning(this, "导入已取消", "当前连接或录制状态已改变，请先停止操作。"); return; }
    const auto cmdPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("cmd_panels.json");
    // Stopping the OLD UI emits auto-save signals. Do it before the transaction
    // writes imported command data, or it would overwrite that new file.
    if (cmdWindow && importCommandPanelMode != 0) cmdWindow->forceStopAllTimers();
    if (!DataProtocols::ProtocolConfigTransfer::apply(values, *m_protocolStore, protocolPolicy, restoreSelections, importCommandPanelMode, cmdPath, error,importStatusMode)) {
        m_configRecoveryPending = QFile::exists(QDir(QFileInfo(m_protocolStore->path()).absolutePath()).filePath("global-config-transaction.json"));
        updateProtocolControls();
        QMessageBox::critical(this, "导入失败", error); return;
    }
    if (cmdWindow && importCommandPanelMode != 0) {
        cmdWindow->reloadConfig();
        // Imported commands must not silently start periodic sending. This
        // auto-save now belongs to the NEW UI and cannot restore stale panels.
        cmdWindow->forceStopAllTimers();
    }
    if(m_statusWindow&&importStatusMode!=0&&!m_statusWindow->reloadConfig(error))QMessageBox::warning(this,"状态面板未应用",error);
    loadConfig(); refreshProtocolChoices(); restoreProtocolUi();
    const int interfaceRow = comboInterface->findData(QSettings("WaveLink", "AdvancedApp").value("InterfaceType", 0).toInt());
    if (interfaceRow >= 0 && interfaceRow != comboInterface->currentIndex()) comboInterface->setCurrentIndex(interfaceRow);
    else requestProtocolApply();
    QMessageBox::information(this, "导入成功", "配置已导入。协议按所选策略恢复，部分其他设置可能需要重启生效。\n缓存路径保持本机设置，旧配置中的缓存路径也不会覆盖。");
}
