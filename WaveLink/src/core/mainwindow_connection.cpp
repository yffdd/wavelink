#include "mainwindow.h"
#include "receiveconsolebuffer.h"
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
#include <QDialogButtonBox>
#include <QClipboard>
#include <QApplication>
#include <QSaveFile>

void MainWindow::onRefreshPorts()
{
    if (isPortOpen) return;
    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    QStringList newPorts;
    for (const QSerialPortInfo &info : ports) {
        newPorts << info.portName() + " (" + info.description() + ")";
    }
    QStringList currentPorts;
    for (int i = 0; i < comboPorts->count(); ++i) {
        currentPorts << comboPorts->itemText(i);
    }
    if (currentPorts == newPorts) return;
    QString currentSelection = comboPorts->currentText();
    comboPorts->blockSignals(true);
    comboPorts->clear();
    for (const QSerialPortInfo &info : ports) {
        comboPorts->addItem(info.portName() + " (" + info.description() + ")", info.portName());
    }
    int idx = comboPorts->findText(currentSelection);
    if (idx != -1) {
        comboPorts->setCurrentIndex(idx);
    }
    comboPorts->blockSignals(false);
    emit comboPorts->currentTextChanged(comboPorts->currentText());
    if (ports.isEmpty()) {
        comboPorts->addItem(QString::fromUtf8("未检测到串口设备"));
    }
}
void MainWindow::onInterfaceChanged(int row)
{
    stopConsolePeriodicSend("数据接口已切换，定时发送已停止。");
    m_receiveDecoder.reset(); m_receiveDecoderEncoding.clear();
    if (row < 0 || !comboInterface->itemData(row).isValid()) return;
    const int index = comboInterface->itemData(row).toInt();
    // Persistent IDs and page indices remain stable, independent of display order.
    QSettings("WaveLink", "AdvancedApp").setValue("InterfaceType", comboInterface->currentData());
    // All real transports share the current protocol. Demo retains the shared
    // choice, but generates its own numeric samples.
    if (m_protocolInitialized && !m_protocolApplyPending) {
        restoreProtocolUi();
        if (index != m_protocolAppliedInterface) requestProtocolApply();
    }
    if (m_connectionState == ConnectionState::Error) {
        setConnectionState(ConnectionState::Disconnected, index);
    }
    stackInterface->setCurrentIndex(index);
    updateInterfaceStackHeight();
    if (!isPortOpen) {
        if (index == 0)
            btnOpenClose->setText("🔌 打开串口");
        else if (index == 1)
            btnOpenClose->setText("🔌 连接网络");
        else if (index == 2)
            btnOpenClose->setText("🔌 开启服务器");
        else if (index == 3)
            btnOpenClose->setText("▶️ 启动 Demo");
        else if (index == 4)
            btnOpenClose->setText(QStringLiteral("连接 BLE 设备"));
        else if (index == 5)
            btnOpenClose->setText(QStringLiteral("🔌 打开 UDP"));
    }
    if (bleClient) updateBleControls();
    updateSerialBreakControls();
    updateSerialFlowControls();
}

void MainWindow::setConnectionState(ConnectionState state, int interfaceType)
{
    if (state != ConnectionState::Connected) stopConsolePeriodicSend("连接尚未就绪或已断开，定时发送已停止。");
    m_connectionState = state;
    m_activeInterfaceType = (state == ConnectionState::Disconnected) ? -1 : interfaceType;
    isPortOpen = (state == ConnectionState::Connected);
    m_connectionOperationPending = (state == ConnectionState::Connecting ||
                                    state == ConnectionState::Disconnecting);

    QString stateText;
    switch (state) {
    case ConnectionState::Disconnected: stateText = QStringLiteral("未连接"); break;
    case ConnectionState::Connecting: stateText = QStringLiteral("正在连接"); break;
    case ConnectionState::Connected: stateText = interfaceType == 5 ? QStringLiteral("UDP 监听中（不代表远端在线）") : QStringLiteral("已连接"); break;
    case ConnectionState::Disconnecting: stateText = QStringLiteral("正在断开"); break;
    case ConnectionState::Error: stateText = QStringLiteral("连接异常"); break;
    }
    if (lblStatusMessage) lblStatusMessage->setText(stateText);
    updateProtocolControls();
    updateSerialBreakControls();
    updateSerialFlowControls();
    if (!btnOpenClose) return;
    const bool networkEditable = state != ConnectionState::Connected && !m_connectionOperationPending;
    if (editIP) editIP->setEnabled(networkEditable);
    if (spinPort) spinPort->setEnabled(networkEditable);
    if (comboLocalIP) comboLocalIP->setEnabled(networkEditable);
    if (spinLocalPort) spinLocalPort->setEnabled(networkEditable);
    if (comboServerIP) comboServerIP->setEnabled(networkEditable);
    if (spinServerPort) spinServerPort->setEnabled(networkEditable);
    if (comboUdpLocalIP) comboUdpLocalIP->setEnabled(networkEditable);
    if (spinUdpLocalPort) spinUdpLocalPort->setEnabled(networkEditable);
    if (editUdpRemoteIP) editUdpRemoteIP->setEnabled(networkEditable);
    if (spinUdpRemotePort) spinUdpRemotePort->setEnabled(networkEditable);
    if (chkUdpFilterRemote) chkUdpFilterRemote->setEnabled(networkEditable);
    updateConsoleSendControls();
    if (state == ConnectionState::Connecting) {
        btnOpenClose->setEnabled(false);
        btnOpenClose->setText(QStringLiteral("⏳ 正在连接..."));
    } else if (state == ConnectionState::Disconnecting) {
        btnOpenClose->setEnabled(false);
        btnOpenClose->setText(QStringLiteral("⏳ 正在断开..."));
    } else if (state == ConnectionState::Error) {
        btnOpenClose->setEnabled(true);
        if (interfaceType == 0) btnOpenClose->setText(QStringLiteral("↻ 重新打开串口"));
        else if (interfaceType == 1) btnOpenClose->setText(QStringLiteral("↻ 重新连接网络"));
        else if (interfaceType == 2) btnOpenClose->setText(QStringLiteral("↻ 重新开启服务器"));
        else if (interfaceType == 4) btnOpenClose->setText(QStringLiteral("↻ 重新连接 BLE"));
        else if (interfaceType == 5) btnOpenClose->setText(QStringLiteral("↻ 重新打开 UDP"));
        else btnOpenClose->setText(QStringLiteral("↻ 重新启动 Demo"));
    }
}

void MainWindow::updateInterfaceStackHeight()
{
    if (!stackInterface) return;

    QWidget *page = stackInterface->currentWidget();
    if (!page) return;

    if (QLayout *layout = page->layout()) {
        layout->activate();
    }
    // Only the page height changes between interfaces. adjustSize() also
    // resets its width to the size hint and causes a visible shrink/expand
    // while the parent layout is catching up.
    const int pageHeight = qMax(page->sizeHint().height(), page->minimumSizeHint().height());
    stackInterface->setFixedHeight(qMax(0, pageHeight));
    stackInterface->updateGeometry();
}

void MainWindow::onOpenCloseConnection()
{
    if (!isPortOpen && comboInterface->currentData().toInt() != 3 && comboProtocol->currentData().toString() == "custom" && comboCustomProfile->currentIndex() < 0) {
        updateProtocolControls();
        QDialog dialog(this); dialog.setObjectName("protocolMissingDialog"); dialog.setWindowTitle("没有可用的自定义协议"); dialog.resize(520, 210);
        auto layout = new QVBoxLayout(&dialog); auto label = new QLabel("当前未选择可用的自定义协议，不能开启连接。\n\n请在“管理自定义协议”中新建或导入协议，再选择；也可切换到 Raw 等内置协议。", &dialog); label->setWordWrap(true); layout->addWidget(label);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok); layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); dialog.exec(); return;
    }
    if (m_protocolApplyPending || (!isPortOpen && !protocolUiMatchesApplied())) {
        statusBar()->showMessage(QStringLiteral("数据协议尚未就绪，请等待自动应用完成或检查协议设置后再开启连接。"), 5000); return;
    }
    if (comboInterface->currentData().toInt() == 4) { openCloseBleConnection(); return; }
    if (m_connectionState == ConnectionState::Connecting ||
        m_connectionState == ConnectionState::Disconnecting) return;

    qDebug() << "onOpenCloseConnection START, isPortOpen:" << isPortOpen;
    if (!isPortOpen) {
        // Every new connection starts from raw data, never from a previous filter state.
        clearFilterSelections();

        // Reconnecting must not clear the existing waveform. The explicit
        // "清空波形" action remains the only operation that clears plots.
        // Channel structures are kept so new samples continue on the current
        // view; syncChannelCount() will reconcile a changed channel count when
        // the first new frame arrives.
        qDebug() << "onOpenClosePort: preserving existing plots for reconnect";
        int interfaceType = comboInterface->currentData().toInt();
        bool success = false;

        if (interfaceType == 0) {
            QString portName = comboPorts->currentData().toString();
            if (portName.isEmpty()) {
                portName = comboPorts->currentText().trimmed().split(' ').first();
            }
            if (portName.isEmpty() || portName == QStringLiteral("未检测到串口设备")) {
                showCopyableSerialError(
                    QStringLiteral("串口打开失败"),
                    QStringLiteral("没有检测到可用的串口设备。"),
                    static_cast<int>(QSerialPort::DeviceNotFoundError));
                return;
            }

            // Never block the GUI while Windows opens/configures a COM handle.
            // Faulty or reconnecting drivers can take an unpredictable amount
            // of time to return from QSerialPort::open().
            setConnectionState(ConnectionState::Connecting, interfaceType);
            m_serialFlowControl = comboFlowControl->currentData().toInt();
            updateSerialFlowControls();
            const quint64 requestId = ++m_serialOpenRequestId;
            m_pendingSerialPortName = portName;
            btnOpenClose->setText(QStringLiteral("⏳ 正在打开串口..."));
            btnOpenClose->setEnabled(false);
            comboInterface->setEnabled(false);
            comboPorts->setEnabled(false);
            comboBaudRate->setEnabled(false);
            comboDataBits->setEnabled(false);
            comboParity->setEnabled(false);
            comboStopBits->setEnabled(false);
            comboFlowControl->setEnabled(false);
            chkDtr->setEnabled(false);
            chkRts->setEnabled(false);
            comboProtocol->setEnabled(false);

            const bool invoked = QMetaObject::invokeMethod(
                serialHandler, "openPort", Qt::QueuedConnection,
                Q_ARG(QString, portName),
                Q_ARG(int, comboBaudRate->currentText().toInt()),
                Q_ARG(int, comboDataBits->currentText().toInt()),
                Q_ARG(QString, comboParity->currentText()),
                Q_ARG(QString, comboStopBits->currentText()),
                Q_ARG(quint64, requestId),
                Q_ARG(bool, m_serialDtrPreference),
                Q_ARG(bool, m_serialRtsPreference),
                Q_ARG(int, m_serialFlowControl));
            if (!invoked) {
                finishConnectionClosed(interfaceType, false);
                setConnectionState(ConnectionState::Error, interfaceType);
                showCopyableSerialError(
                    QStringLiteral("串口打开失败"),
                    QStringLiteral("无法把串口打开任务提交到工作线程。"),
                    static_cast<int>(QSerialPort::UnknownError));
            } else {
                QTimer::singleShot(8000, this, [this, requestId]() {
                    if (!m_connectionOperationPending || m_serialOpenRequestId != requestId) return;

                    const QString timedOutPort = m_pendingSerialPortName;
                    QMetaObject::invokeMethod(serialHandler, "closePort", Qt::QueuedConnection);
                    finishConnectionClosed(0, false);
                    setConnectionState(ConnectionState::Error, 0);
                    m_pendingSerialPortName = timedOutPort;
                    const QString message = QStringLiteral(
                        "串口打开操作超过 8 秒仍未完成，可能是设备驱动或系统串口服务没有响应。");
                    DebugLogger::instance().log(
                        QString("串口打开超时，已恢复界面: %1").arg(timedOutPort), "ERROR");
                    showCopyableSerialError(
                        QStringLiteral("串口打开超时"), message,
                        static_cast<int>(QSerialPort::TimeoutError));
                    m_pendingSerialPortName.clear();
                });
            }
            return;
        } else if (isNetworkInterface(interfaceType)) {
            // Network Mode
            int mode = interfaceType;
            qDebug() << "onOpenCloseConnection: Queuing startConnection mode=" << mode;
            setConnectionState(ConnectionState::Connecting, interfaceType);
            btnOpenClose->setText(QStringLiteral("⏳ 正在建立连接..."));
            btnOpenClose->setEnabled(false);
            comboInterface->setEnabled(false);
            const bool invoked = mode == 5
                ? QMetaObject::invokeMethod(networkHandler, "startUdp", Qt::QueuedConnection,
                    Q_ARG(QString, comboUdpLocalIP->currentText().trimmed()), Q_ARG(int, spinUdpLocalPort->value()),
                    Q_ARG(QString, editUdpRemoteIP->text().trimmed()), Q_ARG(int, spinUdpRemotePort->value()),
                    Q_ARG(bool, chkUdpFilterRemote->isChecked()))
                : QMetaObject::invokeMethod(networkHandler, "startConnection", Qt::QueuedConnection,
                    Q_ARG(int, mode), Q_ARG(QString, mode == 2 ? comboServerIP->currentText().trimmed() : editIP->text().trimmed()),
                    Q_ARG(int, mode == 2 ? spinServerPort->value() : spinPort->value()),
                    Q_ARG(QString, comboLocalIP->currentText().trimmed()), Q_ARG(int, spinLocalPort->value()));
            if (!invoked) {
                finishConnectionClosed(interfaceType, false);
                setConnectionState(ConnectionState::Error, interfaceType);
                QMessageBox::critical(this, "打开失败", "无法把网络连接任务提交到工作线程。");
            }
            return;
        } else if (interfaceType == 3) {
            // Demo Data Mode
            setConnectionState(ConnectionState::Connecting, interfaceType);
            success = true;
            demoTimer->setProperty("lastTickMs", 0);
            demoTimer->setProperty("residualMs", 0.0);
            demoTimer->start(20); // Run UI timer at a reliable 20ms (50fps)
            m_demoStatusMonitor->setConnected(true);
            demoTimeCounter = 0.0;
        }
        qDebug() << "onOpenCloseConnection: connection returned success =" << success;
        if (success) {
            finishConnectionOpen(interfaceType);
        } else {
            QMessageBox::critical(this, "打开失败", "无法建立连接！请检查端口是否被占用或配置是否正确。");
        }
    } else {
        const int interfaceType = comboInterface->currentData().toInt();
        setConnectionState(ConnectionState::Disconnecting, interfaceType);
        // Stop using filters before disconnecting so the visible data and checkboxes stay consistent.
        clearFilterSelections();
        if (isRecording) {
            onToggleRecord();
        }
        
        qDebug() << "onOpenCloseConnection: Invoking close";
        if (interfaceType == 0) {
            QMetaObject::invokeMethod(serialHandler, "closePort", Qt::QueuedConnection);
        } else if (isNetworkInterface(interfaceType)) {
            QMetaObject::invokeMethod(networkHandler, "stopConnection", Qt::QueuedConnection);
        } else if (interfaceType == 3) {
            demoTimer->stop();
            m_demoStatusMonitor->setConnected(false);
        }
        finishConnectionClosed(interfaceType, true);
    }
}

void MainWindow::handleNetworkConnectionAttemptFinished(bool success, const QString &errorMsg)
{
    if (!m_connectionOperationPending || isPortOpen) return;
    const int interfaceType = comboInterface->currentData().toInt();
    if (!isNetworkInterface(interfaceType)) return;
    if (success) {
        finishConnectionOpen(interfaceType);
    } else {
        finishConnectionClosed(interfaceType, false);
        setConnectionState(ConnectionState::Error, interfaceType);
        QMessageBox::critical(this, QStringLiteral("打开失败"),
                              errorMsg.isEmpty() ? QStringLiteral("无法建立网络连接。") : errorMsg);
    }
}

void MainWindow::handleRecordingStartFinished(bool success, const QString &errorMsg)
{
    if (success || !isRecording) return;
    stopRecordingSafely(QStringLiteral("无法创建录制文件：\n%1").arg(errorMsg));
    currentRecordFile.clear();
}

void MainWindow::handleRecordingError(const QString &errorMsg)
{
    if (!isRecording) return;
    DebugLogger::instance().log(QString("录制写入失败，已停止录制: %1").arg(errorMsg), "ERROR");
    stopRecordingSafely(QStringLiteral(
        "写入录制文件失败，录制已立即停止。\n"
        "已成功写入的数据仍保留在缓存文件夹中，可在下次启动时恢复。\n\n%1")
        .arg(errorMsg));
}

void MainWindow::finishConnectionOpen(int interfaceType)
{
    m_receiveDecoder.reset(); m_receiveDecoderEncoding.clear();
    // Keep displayed history, but never re-filter old imported vectors over
    // newly received live data or continue using imported channel names.
    leaveImportedDataMode();
    setConnectionState(ConnectionState::Connected, interfaceType);
    m_pendingSerialPortName.clear();
    m_hasWarnedAutoSendClosed = false;
    frameCountForRate = 0;

    QString closeText;
    if (interfaceType == 0) closeText = QStringLiteral("🔒 关闭串口");
    else if (interfaceType == 1) closeText = QStringLiteral("🔒 断开网络");
    else if (interfaceType == 2) closeText = QStringLiteral("🔒 关闭服务器");
    else if (interfaceType == 4) closeText = QStringLiteral("断开 BLE 连接");
    else if (interfaceType == 5) closeText = QStringLiteral("🔒 关闭 UDP");
    else closeText = QStringLiteral("⏹️ 停止 Demo");

    btnOpenClose->setEnabled(true);
    btnOpenClose->setText(closeText);
    btnOpenClose->setStyleSheet("QPushButton { font-weight: bold; padding: 5px; background-color: #6a2a2a; color: #ffcccc; border: 1px solid #9a4a4a; border-radius: 4px; }"
                                "QPushButton:hover { background-color: #8a3a3a; }");
    comboInterface->setEnabled(false);
    comboPorts->setEnabled(false);
    comboBaudRate->setEnabled(false);
    comboDataBits->setEnabled(false);
    comboParity->setEnabled(false);
    comboStopBits->setEnabled(false);
    comboFlowControl->setEnabled(false);
    comboProtocol->setEnabled(true);
    editIP->setEnabled(false);
    spinPort->setEnabled(false);
    spinServerPort->setEnabled(false);

    if (interfaceType == 2) {
        lblServerConnCount->setText(QStringLiteral("0"));
        lblServerCurrentConn->setText(QStringLiteral("无"));
    }

    if (isRecording) {
        const QString baseDir = editCacheDir->text();
        QDir dir(QDir(baseDir).absoluteFilePath(QStringLiteral("WaveLink_Cache")));
        if (!dir.exists()) dir.mkpath(QStringLiteral("."));
        currentRecordFile = dir.absoluteFilePath(
            QString("WaveLog_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
        if (interfaceType == 0) {
            QMetaObject::invokeMethod(serialHandler, "startRecording", Qt::QueuedConnection,
                                      Q_ARG(QString, currentRecordFile),
                                      Q_ARG(double, spinFs->value()));
        } else if (isNetworkInterface(interfaceType)) {
            QMetaObject::invokeMethod(networkHandler, "startRecording", Qt::QueuedConnection,
                                      Q_ARG(QString, currentRecordFile),
                                      Q_ARG(double, spinFs->value()));
        }
        currentRecordStartTimeMs = QDateTime::currentMSecsSinceEpoch();
        recordTimer->start(1000);
    }
}

void MainWindow::finishConnectionClosed(int interfaceType, bool clearPendingData)
{
    const bool hadBreak = m_serialBreakActive || m_serialBreakPending;
    m_serialSendCancelPending = false;
    m_serialFlowControl = 0;
    m_serialBreakActive = m_serialBreakPulse = m_serialBreakPending = m_serialBreakSupported = false;
    setConnectionState(ConnectionState::Disconnected, interfaceType);
    if (lblClientEndpoint) { lblClientEndpoint->clear(); lblClientEndpoint->hide(); updateInterfaceStackHeight(); }
    if (lblUdpEndpoint) { lblUdpEndpoint->clear(); lblUdpEndpoint->hide(); updateInterfaceStackHeight(); }
    if (hadBreak && interfaceType == 0) {
        // Do not leave a persistent "Break is held" message on a closed port.
        statusBar()->showMessage(QStringLiteral("串口已关闭，Break 控制已停止。"), 5000);
    }
    m_hasWarnedAutoSendClosed = false;

    if (fileSendTimer && fileSendTimer->isActive()) fileSendTimer->stop();
    if (fileToSend && fileToSend->isOpen()) fileToSend->close();
    resetFileSendEta();
    if (btnOpenSendFile) btnOpenSendFile->setEnabled(true);
    if (btnSendFile) btnSendFile->setEnabled(true);
    if (btnStopSendFile) btnStopSendFile->setEnabled(true);
    if (txtSendFilePath) txtSendFilePath->setEnabled(true);

    if (clearPendingData) {
        timeBuffer.clear();
        for (auto &queue : channelBuffers) queue.clear();
    }

    QString openText;
    if (interfaceType == 0) openText = QStringLiteral("🔌 打开串口");
    else if (interfaceType == 1) openText = QStringLiteral("🔌 连接网络");
    else if (interfaceType == 2) openText = QStringLiteral("🔌 开启服务器");
    else if (interfaceType == 4) openText = QStringLiteral("连接 BLE 设备");
    else if (interfaceType == 5) openText = QStringLiteral("🔌 打开 UDP");
    else openText = QStringLiteral("▶️ 启动 Demo");

    btnOpenClose->setEnabled(true);
    btnOpenClose->setText(openText);
    btnOpenClose->setStyleSheet("QPushButton { font-weight: bold; padding: 5px; background-color: #2a6a2a; color: #ccffcc; border: 1px solid #4a9a4a; border-radius: 4px; }"
                                "QPushButton:hover { background-color: #3a8a3a; }");
    comboInterface->setEnabled(true);
    comboPorts->setEnabled(true);
    comboBaudRate->setEnabled(true);
    comboDataBits->setEnabled(true);
    comboParity->setEnabled(true);
    comboStopBits->setEnabled(true);
    comboFlowControl->setEnabled(true);
    {
        const QSignalBlocker dtrBlocker(chkDtr), rtsBlocker(chkRts);
        chkDtr->setTristate(false);
        chkRts->setTristate(false);
        chkDtr->setChecked(m_serialDtrPreference);
        chkRts->setChecked(m_serialRtsPreference);
        chkDtr->setEnabled(true);
        chkRts->setEnabled(true);
        chkDtr->setAccessibleDescription(QStringLiteral("下次打开串口时应用此设置"));
        chkRts->setAccessibleDescription(QStringLiteral("下次打开串口时应用此设置"));
    }
    comboProtocol->setEnabled(true);
    editIP->setEnabled(true);
    spinPort->setEnabled(true);
    spinServerPort->setEnabled(true);
    updateSerialFlowControls();
    updateConsoleSendControls();
}
void MainWindow::onClearReceive()
{
    textReceive->clear();
    receiveTextBuffer.clear();
    m_receiveDecoder.reset(); m_receiveDecoderEncoding.clear();
}
void MainWindow::appendReceiveText(const QString &text)
{
    receiveTextBuffer.append(text);
    ReceiveConsoleBuffer::trimPending(receiveTextBuffer, chkHexReceive->isChecked(),
        m_systemPreferences.receiveLines, m_systemPreferences.receiveCharacters);
}
void MainWindow::trimReceiveText()
{
    ReceiveConsoleBuffer::trimPending(receiveTextBuffer, chkHexReceive->isChecked(),
        m_systemPreferences.receiveLines, m_systemPreferences.receiveCharacters);
    ReceiveConsoleBuffer::trimDocument(textReceive->document(), chkHexReceive->isChecked(), m_systemPreferences.receiveCharacters);
}
void MainWindow::flushReceiveText()
{
    if (receiveTextBuffer.isEmpty()) return;
    ReceiveConsoleBuffer::trimPending(receiveTextBuffer, chkHexReceive->isChecked(),
        m_systemPreferences.receiveLines, m_systemPreferences.receiveCharacters);
    QTextCursor cursor(textReceive->document());
    cursor.beginEditBlock();
    // Evict BEFORE insertion so an oversized paragraph is never laid out and
    // then laid out a second time just to remove its oldest prefix.
    ReceiveConsoleBuffer::trimDocument(textReceive->document(), chkHexReceive->isChecked(),
        qMax(0, m_systemPreferences.receiveCharacters - receiveTextBuffer.size()));
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(receiveTextBuffer);
    receiveTextBuffer.clear();
    cursor.endEditBlock();
    cursor.movePosition(QTextCursor::End);
    textReceive->setTextCursor(cursor);
    textReceive->verticalScrollBar()->setValue(textReceive->verticalScrollBar()->maximum());
}
void MainWindow::onCopyReceive()
{
    const QString content = textReceive->toPlainText();
    if (content.isEmpty()) {
        statusBar()->showMessage("接收文本框为空，没有可复制的内容。", 4000);
        return;
    }
    QApplication::clipboard()->setText(content);
    statusBar()->showMessage("接收内容已复制到剪贴板。", 3000);
}
void MainWindow::onSaveReceive()
{
    // Snapshot before opening the picker: incoming data cannot change this save.
    const QString content = textReceive->toPlainText();
    if (content.isEmpty()) {
        QMessageBox::information(this, "暂无接收数据", "接收文本框为空，没有可保存的数据。"); return;
    }
    QSettings settings("WaveLink", "AdvancedApp");
    const auto directory = settings.value("LastReceiveTextDir", settings.value("LastDataDir", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))).toString();
    const auto name = QString("WaveLink_Receive_%1.txt").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"));
    QFileDialog picker(this, "保存接收数据", QDir(directory).filePath(name), "文本文件 (*.txt)");
    picker.setObjectName("receiveSaveDialog"); picker.setAcceptMode(QFileDialog::AcceptSave);
    picker.setFileMode(QFileDialog::AnyFile); picker.setDefaultSuffix("txt");
    if (picker.exec() != QDialog::Accepted || picker.selectedFiles().isEmpty()) return;
    const auto path = picker.selectedFiles().first();
    QSaveFile file(path); file.setDirectWriteFallback(false);
    const QByteArray bytes = QByteArray::fromHex("EFBBBF") + content.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::critical(this, "保存失败", "无法完整保存接收数据，原文件未被覆盖。\n" + path + "\n" + file.errorString()); return;
    }
    settings.setValue("LastReceiveTextDir", QFileInfo(path).absolutePath());
    statusBar()->showMessage("接收数据已保存：" + path, 6000);
}
bool MainWindow::consoleSendPayload(QByteArray &data, QString &error) const
{
    const QString text = textSend->toPlainText(); data.clear(); error.clear();
    if (text.isEmpty()) { error = "发送内容为空，请先填写要发送的数据。"; return false; }
    if (chkHexSend->isChecked()) {
        if (!DataProtocols::ProtocolProfileValidator::parseHex(text, data) || data.isEmpty()) {
            error = "HEX 发送内容必须是完整的十六进制字节，每个字节两位，请补全后重试。"; return false;
        }
    } else data = text.toUtf8();
    const int appendIdx = comboAppendNewline->currentIndex();
    if (appendIdx == 1) data.append("\r\n"); else if (appendIdx == 2) data.append("\n\r");
    else if (appendIdx == 3) data.append("\r"); else if (appendIdx == 4) data.append("\n");
    return true;
}
QString MainWindow::consoleSendUnavailableReason() const
{
    const int interfaceType = comboInterface ? comboInterface->currentData().toInt() : -1;
    if (!connectionIsReady() || interfaceType != m_activeInterfaceType) return "请先建立可发送的设备连接。";
    if (m_protocolApplyPending || m_configRecoveryPending) return "协议或配置处理尚未完成。";
    if (fileSendTimer && fileSendTimer->isActive()) return "文件发送期间不能混入定时发送数据。";
    if (interfaceType == 0) {
        if (m_serialBreakActive || m_serialBreakPending || m_serialSendCancelPending) return "请先释放 Break 或完成取消发送操作。";
        if (!serialHandler || !serialHandler->sendSessionReady(m_serialOpenRequestId)) return "串口发送会话尚未就绪。";
    } else if (isNetworkInterface(interfaceType)) {
        if (!networkHandler || !networkHandler->sendReady()) return interfaceType == 5 ? "UDP 未设置远端 IP/端口，仅可接收。" : "网络客户端未连接，暂时不能发送。";
    } else if (interfaceType == 4) {
        if (!bleClient || !bleClient->canSend() || bleClient->state() != BleHostClient::State::Ready) return "BLE 发送通道尚未就绪。";
        if (bleClient->fileSendBusy()) return "BLE 文件发送期间不能混入定时发送数据。";
    } else return "Demo 接口不向设备发送数据。";
    return {};
}
void MainWindow::stopConsolePeriodicSend(const QString &reason)
{
    const bool wasActive = (consoleSendTimer && consoleSendTimer->isActive()) || (chkConsolePeriodicSend && chkConsolePeriodicSend->isChecked());
    if (consoleSendTimer) consoleSendTimer->stop();
    if (chkConsolePeriodicSend) { const QSignalBlocker blocker(chkConsolePeriodicSend); chkConsolePeriodicSend->setChecked(false); }
    if (wasActive) {
        if (!reason.isEmpty()) statusBar()->showMessage(reason, 6000);
        updateProtocolControls();
    }
}
void MainWindow::updateConsoleSendControls()
{
    if (!chkConsolePeriodicSend || !consoleSendTimer) return;
    const auto reason = consoleSendUnavailableReason();
    const bool udp = comboInterface && comboInterface->currentData().toInt() == 5;
    if (udp && btnSend) btnSend->setEnabled(reason.isEmpty());
    if (btnSendFile) {
        if (udp) btnSendFile->setEnabled(false);
        btnSendFile->setToolTip(udp ? "UDP 不保证到达和顺序，本版不支持文件发送；可使用文本/HEX、命令或定时发送。" : "发送文件");
    }
    if (!reason.isEmpty()) stopConsolePeriodicSend("定时发送已停止：" + reason);
    chkConsolePeriodicSend->setEnabled(reason.isEmpty());
    chkConsolePeriodicSend->setToolTip("勾选后每个周期发送当前发送框内容，沿用 HEX 和换行设置；第一个周期结束时首次发送。断开、切换接口、清空内容、Break、文件发送或队列拒绝时自动停止，重连不会自动恢复。" + (reason.isEmpty() ? QString() : "\n当前不可用：" + reason));
}
void MainWindow::onConsolePeriodicSendToggled(bool enabled)
{
    if (!enabled) { stopConsolePeriodicSend(); return; }
    QByteArray data; QString error = consoleSendUnavailableReason();
    if (!error.isEmpty() || !consoleSendPayload(data, error)) { stopConsolePeriodicSend("无法开启定时发送：" + error); return; }
    consoleSendTimer->start(spinConsoleSendInterval->value()); updateProtocolControls();
}
void MainWindow::onConsolePeriodicSendTimer()
{
    if (!consoleSendTimer->isActive() || !chkConsolePeriodicSend->isChecked()) return;
    QByteArray data; QString error = consoleSendUnavailableReason();
    if (!error.isEmpty() || !consoleSendPayload(data, error)) { stopConsolePeriodicSend("定时发送已停止：" + error); return; }
    sendDataToCurrentInterface(data);
}
void MainWindow::onSend()
{
    if (m_protocolApplyPending) return;
    if (!isPortOpen) {
        QMessageBox::warning(this, "提示", "请先打开连接！");
        return;
    }
    if (textSend->toPlainText().isEmpty()) return;
    QByteArray sendData; QString error;
    if (!consoleSendPayload(sendData, error)) { QMessageBox::warning(this, "无法发送", error); return; }

    sendDataToCurrentInterface(sendData);
}
bool MainWindow::sendDataToCurrentInterface(const QByteArray &data)
{
    if (data.isEmpty()) return false;
    if (!connectionIsReady()) { stopConsolePeriodicSend("连接已关闭，定时发送已停止。"); return false; }

    const int interfaceIdx = comboInterface ? comboInterface->currentData().toInt() : -1;
    if (interfaceIdx == 0 && serialHandler) {
        if (m_serialBreakActive || m_serialBreakPending || m_serialSendCancelPending || (fileSendTimer && fileSendTimer->isActive())) {
            if (cmdWindow) cmdWindow->forceStopAllTimers();
            const QString message = QStringLiteral("未发送：Break、取消操作或文件发送正在进行，请结束当前操作后手动重发。命令循环已停止。");
            statusBar()->showMessage(message, 6000);
            DebugLogger::instance().log(message, "WARN");
            stopConsolePeriodicSend(message);
            return false;
        }
        if (!serialHandler->enqueueData(m_serialOpenRequestId, data)) {
            if (cmdWindow) cmdWindow->forceStopAllTimers();
            statusBar()->showMessage(QStringLiteral("未发送：连接已关闭或发送队列已满，请检查连接和 CTS 状态。"), 6000);
            stopConsolePeriodicSend("连接已关闭或发送队列已满，定时发送已停止。请检查连接和 CTS 状态。");
            return false;
        }
    } else if (isNetworkInterface(interfaceIdx) && networkHandler) {
        if ((fileSendTimer && fileSendTimer->isActive()) || !networkHandler->enqueueData(data)) {
            if (cmdWindow) cmdWindow->forceStopAllTimers();
            const QString message = interfaceIdx == 5
                ? "未发送：UDP 未设置远端 IP/端口、单包超过 65507 字节，或待发送队列已满。命令循环已停止，本次不会自动重发。"
                : "未发送：网络客户端未连接、待发送队列已满，或文件发送正在进行。命令循环已停止，本次不会自动重发。";
            statusBar()->showMessage(message, 6000);
            DebugLogger::instance().log(message, "WARN");
            stopConsolePeriodicSend(message);
            return false;
        }
    } else if (interfaceIdx == 4 && bleClient) {
        if (!bleClient->sendData(data)) {
            if (cmdWindow) cmdWindow->forceStopAllTimers();
            const QString message = bleClient->fileSendBusy()
                ? QStringLiteral("未发送：文件发送期间禁止混入命令（命令循环已停止）")
                : QStringLiteral("未发送：发送通道未就绪或队列已满（命令循环已停止）");
            lblBleStatus->setText(message); statusBar()->showMessage(message, 5000);
            DebugLogger::instance().log(message, "WARN");
            stopConsolePeriodicSend(message);
            return false;
        }
    } else return false;
    return true; // Queue admission only; not delivery or device acknowledgement.
}
void MainWindow::handleProtocolChanged(int index)
{
    if (!m_protocolInitialized) return;
    if (protocolChangeBlocked()) { restoreProtocolUi(); statusBar()->showMessage(QStringLiteral("录制、发送或连接操作期间不能切换协议。"), 5000); return; }
    updateProtocolControls();
    if (comboProtocol->currentData().toString() == "custom") queueProtocolAutoApply();
    else { m_protocolAutoApplyDeferred = false; requestProtocolApply(); }
}
void MainWindow::handleSerialDataReceived(const QByteArray &data)
{
    if (comboInterface && comboInterface->currentData().toInt() == 4) return;
    // If not checked, drop the text completely to save CPU/UI overhead
    if (!chkShowReceiveData->isChecked()) { m_receiveDecoder.reset(); return; }
    
    QString timestamp;
    if (chkTimestamp && chkTimestamp->isChecked()) {
        timestamp = "[" + QTime::currentTime().toString("HH:mm:ss.zzz") + "] ";
    }
    
    if (chkHexReceive->isChecked()) {
        m_receiveDecoder.reset();
        QString out = timestamp + data.toHex(' ').toUpper() + " ";
        if (!timestamp.isEmpty()) out += "\n";
        appendReceiveText(out);
    } else {
        const QString encoding = comboEncoding && comboEncoding->currentText() == "GBK" ? "GBK" : "UTF-8";
        if (!m_receiveDecoder || m_receiveDecoderEncoding != encoding) {
            auto codec = QTextCodec::codecForName(encoding.toLatin1());
            if (!codec) codec = QTextCodec::codecForName("UTF-8");
            m_receiveDecoder.reset(codec->makeDecoder()); m_receiveDecoderEncoding = encoding;
        }
        // Serial/TCP may split a Chinese character across readyRead batches.
        // Keep its partial bytes until the next batch rather than emitting �.
        const QString txt = m_receiveDecoder->toUnicode(data);
        QString out = timestamp + txt;
        if (!timestamp.isEmpty() && !txt.endsWith("\n")) out += "\n";
        appendReceiveText(out);
    }
}
void MainWindow::handleSerialOpenFinished(quint64 requestId, bool success, const QString &errorMsg, int errorCode)
{
    if (!m_connectionOperationPending || requestId != m_serialOpenRequestId) {
        DebugLogger::instance().log(
            QString("已忽略过期的串口打开结果，请求编号: %1").arg(requestId), "WARN");
        return;
    }

    if (success) {
        DebugLogger::instance().log(
            QString("串口 %1 已成功打开").arg(m_pendingSerialPortName), "INFO");
        finishConnectionOpen(0);
        return;
    }

    const QString failedPort = m_pendingSerialPortName;
    finishConnectionClosed(0, false);
    setConnectionState(ConnectionState::Error, 0);
    m_pendingSerialPortName = failedPort;
    DebugLogger::instance().log(QString("串口打开失败: %1").arg(errorMsg), "ERROR");
    showCopyableSerialError(QStringLiteral("串口打开失败"), errorMsg, errorCode);
    m_pendingSerialPortName.clear();
}

void MainWindow::handleSerialError(const QString &errorMsg, int errorCode)
{
    // Ignore a queued serial error after switching to Demo or a network mode.
    // Such a stale notification must not interrupt the active non-serial run.
    if (!comboInterface || comboInterface->currentData().toInt() != 0) {
        DebugLogger::instance().log(
            QString("已忽略非串口模式下的串口错误: %1").arg(errorMsg), "WARN");
        return;
    }

    if (!isPortOpen && !m_connectionOperationPending) {
        DebugLogger::instance().log(QString("已忽略连接关闭后的串口错误: %1").arg(errorMsg), "WARN");
        return;
    }

    // Stop every producer/consumer before restoring the controls. Never call
    // SerialHandler directly here: it belongs to serialThread.
    if (isRecording) onToggleRecord();
    clearFilterSelections();
    if (serialHandler) {
        QMetaObject::invokeMethod(serialHandler, "closePort", Qt::QueuedConnection);
    }
    finishConnectionClosed(0, true);
    setConnectionState(ConnectionState::Error, 0);

    DebugLogger::instance().log(QString("串口异常并已安全关闭: %1").arg(errorMsg), "ERROR");
    showCopyableSerialError(QStringLiteral("串口通信异常"), errorMsg, errorCode);
    m_pendingSerialPortName.clear();
}

void MainWindow::onSerialControlLinesChanged()
{
    // A disabled, half-checked line is unknown, not an asserted signal.
    if (chkDtr->checkState() != Qt::PartiallyChecked) m_serialDtrPreference = chkDtr->isChecked();
    const bool automaticRts = (connectionIsReady() || m_connectionOperationPending)
        ? m_serialFlowControl == 1 : comboFlowControl->currentData().toInt() == 1;
    if (!automaticRts && chkRts->checkState() != Qt::PartiallyChecked) m_serialRtsPreference = chkRts->isChecked();
    QSettings settings("WaveLink", "WaveLink");
    settings.setValue("Serial/DTR", m_serialDtrPreference);
    settings.setValue("Serial/RTS", m_serialRtsPreference);
    if (!connectionIsReady() || m_activeInterfaceType != 0 || !serialHandler) return;

    chkDtr->setEnabled(false);
    chkRts->setEnabled(false);
    QMetaObject::invokeMethod(serialHandler, "setControlLines", Qt::QueuedConnection,
        Q_ARG(quint64, m_serialOpenRequestId), Q_ARG(bool, m_serialDtrPreference), Q_ARG(bool, m_serialRtsPreference));
}

void MainWindow::handleSerialControlLinesFinished(quint64 requestId, int dtrState, int rtsState, const QString &warning)
{
    if (requestId != m_serialOpenRequestId || !connectionIsReady() || m_activeInterfaceType != 0) return;
    for (int line = 0; line < 2; ++line) {
        QCheckBox *box = line == 0 ? chkDtr : chkRts;
        const int state = line == 0 ? dtrState : rtsState;
        const QSignalBlocker blocker(box);
        if (line == 1 && m_serialFlowControl == 1) {
            box->setTristate(false); box->setChecked(false); box->setEnabled(false);
            box->setAccessibleDescription(QStringLiteral("硬件流控启用，RTS 由驱动自动控制"));
            continue;
        }
        box->setTristate(state < 0);
        box->setCheckState(state < 0 ? Qt::PartiallyChecked : (state ? Qt::Checked : Qt::Unchecked));
        box->setEnabled(state >= 0);
        box->setAccessibleDescription(state < 0
            ? QStringLiteral("驱动未确认此控制信号，当前连接中不可用；重新连接后可重试")
            : QStringLiteral("驱动已确认控制信号设置"));
    }
    if (warning.isEmpty()) return;
    DebugLogger::instance().log(QString("串口控制信号设置失败: %1").arg(warning), "WARN");
    // This is a nonfatal configuration warning, not the communication-error
    // dialog whose diagnostic claims that recording/connection were stopped.
    showSerialControlWarning(QStringLiteral("串口控制信号未生效"), QStringLiteral(
        "%1\n\n端口：%2\n\n未确认的控制信号显示为半选状态，本次连接中已禁用。\n"
        "串口连接仍保持，接收和录制未因此停止。\n"
        "请核对驱动及设备是否支持 DTR/RTS；关闭连接后可重新设置并重试。\n"
        "设置已保存，但保存不代表该设备已成功应用。")
        .arg(warning, comboPorts->currentText()));
}

void MainWindow::showSerialControlWarning(const QString &title, const QString &message)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(620, 330);
    auto layout = new QVBoxLayout(&dialog);
    auto details = new QPlainTextEdit(&dialog);
    details->setReadOnly(true);
    details->setPlainText(message);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto copy = buttons->addButton(QStringLiteral("复制全部信息"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dialog, [details]() { QApplication::clipboard()->setText(details->toPlainText()); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(details);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::updateSerialFlowControls()
{
    if (!comboFlowControl || !chkRts || !serialSendStatusWidget) return;
    const bool serial = comboInterface && comboInterface->currentData().toInt() == 0;
    const bool active = serial && (connectionIsReady() || m_connectionOperationPending);
    const bool hardware = active ? m_serialFlowControl == 1 : comboFlowControl->currentData().toInt() == 1;
    comboFlowControl->setEnabled(!connectionIsReady() && !m_connectionOperationPending);
    const QSignalBlocker blocker(chkRts);
    chkRts->setText(hardware ? QStringLiteral("RTS（自动）") : QStringLiteral("RTS"));
    if (hardware) {
        chkRts->setTristate(false); chkRts->setChecked(false); chkRts->setEnabled(false);
        chkRts->setToolTip(QStringLiteral("硬件流控启用，RTS 由驱动自动控制。原来的手动 RTS 设置保留，切回无流控后恢复。"));
    } else {
        chkRts->setToolTip(chkDtr->toolTip());
        if (!active) {
            chkRts->setTristate(false); chkRts->setChecked(m_serialRtsPreference); chkRts->setEnabled(true);
        }
    }
    serialSendStatusWidget->setVisible(hardware);
    if (!connectionIsReady()) {
        lblSerialSendStatus->setText(m_connectionOperationPending ? QStringLiteral("连接处理中…") : QStringLiteral("未连接"));
        btnCancelSerialSend->setEnabled(false);
    } else if (serialHandler && serialHandler->pendingSendBytes() == 0 && !m_serialSendCancelPending) {
        lblSerialSendStatus->setText(QStringLiteral("发送空闲"));
        btnCancelSerialSend->setEnabled(false);
    }
}

void MainWindow::handleSerialSendStatus(quint64 requestId, qint64, int ctsState)
{
    if (!connectionIsReady() || m_activeInterfaceType != 0 || requestId != m_serialOpenRequestId || !serialHandler) return;
    // Use current admission count: an older idle notification must not mark
    // newly queued data as completed (especially between file chunks).
    const qint64 pending = serialHandler->pendingSendBytes();
    if (m_serialSendCancelPending) lblSerialSendStatus->setText(QStringLiteral("正在取消…"));
    else if (!pending) lblSerialSendStatus->setText(QStringLiteral("发送空闲"));
    else lblSerialSendStatus->setText(QStringLiteral("待发 %1\n%2")
        .arg(pending < 1024 ? QStringLiteral("%1 B").arg(pending)
                           : QStringLiteral("%1 KB").arg(pending / 1024.0, 0, 'f', 1))
        .arg(ctsState == 0 ? QStringLiteral("等待 CTS，请检查接线") : ctsState == 1
             ? QStringLiteral("正在发送") : QStringLiteral("等待写出，CTS 状态不可用")));
    btnCancelSerialSend->setEnabled(pending > 0 && !m_serialSendCancelPending);
    if (btnStopSendFile) btnStopSendFile->setEnabled(!m_serialSendCancelPending && (pending > 0 || fileSendTimer->isActive()));
    if (fileSendTimer->isActive() && fileToSend->isOpen()) {
        const qint64 completed = qMax<qint64>(0, fileToSend->pos() - pending);
        const qint64 total = fileToSend->size();
        progressBarFile->setValue(total > 0 ? qMin(1000, static_cast<int>(completed * 1000 / total)) : 0);
        updateFileSendEta(completed, total);
    }
    if (m_serialFlowControl == 1) updateInterfaceStackHeight();
}

void MainWindow::finishSerialFileSend()
{
    resetFileSendEta();
    if (fileSendTimer) fileSendTimer->stop();
    if (fileToSend && fileToSend->isOpen()) fileToSend->close();
    btnOpenSendFile->setEnabled(true); txtSendFilePath->setEnabled(true);
    updateSerialBreakControls();
}

void MainWindow::cancelSerialSend()
{
    stopConsolePeriodicSend("取消待发送数据，定时发送已停止。");
    if (!connectionIsReady() || m_activeInterfaceType != 0 || m_serialSendCancelPending) return;
    if (cmdWindow) cmdWindow->forceStopAllTimers();
    finishSerialFileSend();
    m_serialSendCancelPending = true;
    btnCancelSerialSend->setEnabled(false); btnStopSendFile->setEnabled(false);
    lblSerialSendStatus->setText(QStringLiteral("正在取消…"));
    updateSerialBreakControls();
    if (!QMetaObject::invokeMethod(serialHandler, "cancelPendingSend", Qt::QueuedConnection, Q_ARG(quint64, m_serialOpenRequestId))) {
        m_serialSendCancelPending = false; updateSerialBreakControls();
        showSerialControlWarning(QStringLiteral("无法取消发送"), QStringLiteral("无法提交取消任务，请关闭串口连接后再操作。"));
    }
}

void MainWindow::handleSerialSendCancelled(quint64 requestId, bool disconnected)
{
    if (!connectionIsReady() || m_activeInterfaceType != 0 || requestId != m_serialOpenRequestId) return;
    m_serialSendCancelPending = false;
    if (disconnected) {
        if (isRecording) onToggleRecord();
        clearFilterSelections(); finishConnectionClosed(0, true);
        statusBar()->showMessage(QStringLiteral("发送已取消，串口已安全关闭；需要继续时请手动重新连接。"), 10000);
        showSerialControlWarning(QStringLiteral("发送已取消，串口已关闭"), QStringLiteral(
            "待发送数据已取消。因为已有写入进入串口驱动，软件安全关闭了串口，接收和录制也已停止。\n"
            "软件队列中的旧数据不会在 CTS 恢复后自动重发；已提交到驱动或硬件的少量字节仍可能到达，无法撤回。\n"
            "需要继续调试时，请手动重新打开串口；软件不会自动重发取消的数据。"));
    } else {
        updateSerialBreakControls();
        btnStopSendFile->setEnabled(false);
        lblSerialSendStatus->setText(QStringLiteral("已取消，发送空闲"));
        statusBar()->showMessage(QStringLiteral("已取消尚未发送的数据，连接、接收和录制保持；已发送的字节无法撤回。"), 6000);
    }
}

void MainWindow::updateSerialBreakControls()
{
    if (!chkSerialBreak || !btnSerialBreak || !spinSerialBreakMs) return;
    const bool serial = comboInterface && comboInterface->currentData().toInt() == 0;
    const bool ready = serial && connectionIsReady() && m_activeInterfaceType == 0;
    const bool sendingFile = fileSendTimer && fileSendTimer->isActive();
    const QSignalBlocker blocker(chkSerialBreak);
    chkSerialBreak->setChecked(ready && m_serialBreakActive && !m_serialBreakPulse);
    chkSerialBreak->setEnabled(ready && m_serialBreakSupported && !m_serialBreakPending && !m_serialBreakPulse && !sendingFile && !m_serialSendCancelPending);
    spinSerialBreakMs->setEnabled(!m_serialBreakPending && !m_serialBreakActive);
    btnSerialBreak->setText(m_serialBreakPending ? QStringLiteral("处理中…") : m_serialBreakActive ? QStringLiteral("释放") : QStringLiteral("发送"));
    btnSerialBreak->setEnabled(ready && m_serialBreakSupported && !m_serialBreakPending && !sendingFile && !m_serialSendCancelPending);
    if (serial) {
        const bool blocked = m_serialBreakActive || m_serialBreakPending || m_serialSendCancelPending;
        if (btnSend) btnSend->setEnabled(!blocked && !sendingFile);
        if (btnSendFile) btnSendFile->setEnabled(!blocked && !sendingFile);
    }
    updateConsoleSendControls();
}

void MainWindow::requestSerialBreak(bool enabled, int durationMs)
{
    if (!connectionIsReady() || m_activeInterfaceType != 0 || m_serialBreakPending || !m_serialBreakSupported || m_serialSendCancelPending) {
        updateSerialBreakControls(); return;
    }
    if (enabled && fileSendTimer && fileSendTimer->isActive()) {
        updateSerialBreakControls();
        showSerialControlWarning(QStringLiteral("暂时无法开启 Break"), QStringLiteral("当前正在发送文件。请先完成或停止文件发送，再操作 Break；本次未开启 Break。"));
        return;
    }
    if (enabled && cmdWindow) cmdWindow->forceStopAllTimers();
    m_serialBreakPending = true;
    updateSerialBreakControls();
    const bool submitted = QMetaObject::invokeMethod(serialHandler, "setBreakState", Qt::QueuedConnection,
        Q_ARG(quint64, m_serialOpenRequestId), Q_ARG(bool, enabled), Q_ARG(int, durationMs));
    if (!submitted) {
        m_serialBreakPending = false;
        updateSerialBreakControls();
        showSerialControlWarning(QStringLiteral("串口 Break 控制提示"), QStringLiteral("无法将 Break 任务提交到串口工作线程，操作未执行。"));
    }
}

void MainWindow::handleSerialBreakChanged(quint64 requestId, bool active, bool pulse, bool supported, const QString &warning)
{
    if (requestId != m_serialOpenRequestId || !connectionIsReady() || m_activeInterfaceType != 0) return;
    m_serialBreakPending = false;
    m_serialBreakActive = active;
    m_serialBreakPulse = pulse;
    m_serialBreakSupported = supported;
    updateSerialBreakControls();
    statusBar()->showMessage(active
        ? (pulse ? QStringLiteral("Break 短时发送中，到时自动释放；普通发送暂时禁用。")
                 : QStringLiteral("Break 持续保持中；请取消“保持”或点击“释放”恢复普通发送。"))
        : QStringLiteral("Break 已结束或未开启，普通发送可用。"), active ? 0 : 5000);
    if (!warning.isEmpty()) {
        DebugLogger::instance().log(QString("串口 Break 提示: %1").arg(warning), "WARN");
        showSerialControlWarning(QStringLiteral("串口 Break 控制提示"), QStringLiteral("端口：%1\n\n%2").arg(comboPorts->currentText(), warning));
    }
}

void MainWindow::showCopyableSerialError(const QString &title, const QString &errorMsg, int errorCode)
{
    const auto serialError = static_cast<QSerialPort::SerialPortError>(errorCode);
    QString errorName;
    QString suggestion;
    switch (serialError) {
    case QSerialPort::DeviceNotFoundError:
        errorName = QStringLiteral("DeviceNotFoundError");
        suggestion = QStringLiteral("确认设备仍已连接；如果设备刚刚复位，请等待端口重新出现后再打开。");
        break;
    case QSerialPort::PermissionError:
        errorName = QStringLiteral("PermissionError");
        suggestion = QStringLiteral("关闭 ESP-IDF Monitor、串口助手、烧录工具等可能占用该端口的程序，然后重试。");
        break;
    case QSerialPort::OpenError:
        errorName = QStringLiteral("OpenError");
        suggestion = QStringLiteral("端口可能已经被占用或驱动尚未释放；关闭相关程序，等待数秒后重试。");
        break;
    case QSerialPort::ParityError:
        errorName = QStringLiteral("ParityError");
        suggestion = QStringLiteral("检查上下位机的校验位、数据位和波特率是否完全一致。");
        break;
    case QSerialPort::FramingError:
        errorName = QStringLiteral("FramingError");
        suggestion = QStringLiteral("检查波特率、数据位和停止位，并排查线路干扰或信号质量问题。");
        break;
    case QSerialPort::BreakConditionError:
        errorName = QStringLiteral("BreakConditionError");
        suggestion = QStringLiteral("设备发送了中断信号，请检查设备复位状态和串口线路。");
        break;
    case QSerialPort::ReadError:
        errorName = QStringLiteral("ReadError");
        suggestion = QStringLiteral("检查 USB 线缆、转换芯片驱动和设备供电，必要时重新插拔设备。");
        break;
    case QSerialPort::WriteError:
        errorName = QStringLiteral("WriteError");
        suggestion = QStringLiteral("设备可能在发送过程中断开或复位，请检查连接后重新打开串口。");
        break;
    case QSerialPort::ResourceError:
        errorName = QStringLiteral("ResourceError");
        suggestion = QStringLiteral("设备资源已经失效。等待系统重新枚举端口，必要时重新插拔设备。");
        break;
    case QSerialPort::UnsupportedOperationError:
        errorName = QStringLiteral("UnsupportedOperationError");
        suggestion = QStringLiteral("检查波特率、数据位、校验位和停止位是否受当前驱动支持。");
        break;
    case QSerialPort::TimeoutError:
        errorName = QStringLiteral("TimeoutError");
        suggestion = QStringLiteral("设备响应超时，请检查设备状态、线缆和串口参数。");
        break;
    case QSerialPort::NotOpenError:
        errorName = QStringLiteral("NotOpenError");
        suggestion = QStringLiteral("串口尚未成功打开，请刷新端口列表后重试。");
        break;
    default:
        errorName = QStringLiteral("UnknownError");
        suggestion = QStringLiteral("请保存此诊断信息，并检查设备驱动、端口占用和物理连接。");
        break;
    }

    QString portName = m_pendingSerialPortName;
    if (portName.isEmpty() && comboPorts) {
        portName = comboPorts->currentData().toString();
        if (portName.isEmpty()) portName = comboPorts->currentText().trimmed().split(' ').first();
    }
    if (portName.isEmpty()) portName = QStringLiteral("未知端口");

    const QString diagnostic = QStringLiteral(
        "WaveLink 串口异常诊断信息\n"
        "========================================\n"
        "发生时间：%1\n"
        "端口：%2\n"
        "波特率：%3\n"
        "数据位：%4\n"
        "校验位：%5\n"
        "停止位：%6\n"
        "错误类型：%7\n"
        "错误代码：%8\n\n"
        "错误信息：\n%9\n\n"
        "已执行的安全处理：\n"
        "- 停止文件发送和数据录制\n"
        "- 请求工作线程关闭并释放串口\n"
        "- 清理待处理数据并恢复界面操作\n"
        "- 保留已经显示的历史波形\n\n"
        "建议处理：\n%10")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(portName)
        .arg(comboBaudRate ? comboBaudRate->currentText() : QStringLiteral("未知"))
        .arg(comboDataBits ? comboDataBits->currentText() : QStringLiteral("未知"))
        .arg(comboParity ? comboParity->currentText() : QStringLiteral("未知"))
        .arg(comboStopBits ? comboStopBits->currentText() : QStringLiteral("未知"))
        .arg(errorName)
        .arg(errorCode)
        .arg(errorMsg)
        .arg(suggestion);

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.resize(720, 460);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QLabel *summary = new QLabel(errorMsg, &dialog);
    summary->setWordWrap(true);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QPlainTextEdit *details = new QPlainTextEdit(&dialog);
    details->setReadOnly(true);
    details->setPlainText(diagnostic);
    details->setLineWrapMode(QPlainTextEdit::NoWrap);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    if (QPushButton *closeButton = buttons->button(QDialogButtonBox::Close)) {
        closeButton->setText(QStringLiteral("关闭"));
    }
    QPushButton *copyButton = buttons->addButton(QStringLiteral("复制全部信息"), QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, &dialog, [diagnostic]() {
        QApplication::clipboard()->setText(diagnostic);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    layout->addWidget(summary);
    layout->addWidget(details, 1);
    layout->addWidget(buttons);
    dialog.exec();
}
void MainWindow::handleNetworkError(const QString &errorMsg)
{
    qDebug() << "Network error:" << errorMsg;
    if (m_connectionOperationPending && !isPortOpen) {
        // The asynchronous connection result will present one consolidated
        // error dialog. Avoid showing a second dialog for the same failure.
        return;
    }
    if (m_connectionState == ConnectionState::Disconnected ||
        m_connectionState == ConnectionState::Disconnecting ||
        m_connectionState == ConnectionState::Error ||
        !comboInterface ||
        !isNetworkInterface(comboInterface->currentData().toInt())) {
        DebugLogger::instance().log(
            QString("已忽略非活动网络连接的延迟错误: %1").arg(errorMsg), "WARN");
        return;
    }
    const int interfaceType = comboInterface ? comboInterface->currentData().toInt() : 1;
    if (connectionIsReady() && isNetworkInterface(interfaceType)) {
        if (isRecording) stopRecordingSafely();
        QMetaObject::invokeMethod(networkHandler, "stopConnection", Qt::QueuedConnection);
        finishConnectionClosed(interfaceType, true);
        setConnectionState(ConnectionState::Error, interfaceType);
    }
    QMessageBox::critical(this, "网络错误", errorMsg);
}

void MainWindow::handleNetworkStateChanged(bool isConnected)
{
    qDebug() << "Network state changed:" << isConnected;
    if (!isConnected && connectionIsReady() && comboInterface->currentData().toInt() == 1) {
        if (isRecording) stopRecordingSafely();
        finishConnectionClosed(1, true);
        setConnectionState(ConnectionState::Error, 1);
        QMessageBox::warning(this, "连接断开", "网络物理连接已断开！");
    }
}

void MainWindow::handleClientConnected(const QString &clientIp)
{
    if (lblServerConnCount && lblServerCurrentConn) {
        QString ip = clientIp;
        if (ip.startsWith("::ffff:")) {
            ip = ip.mid(7);
        }
        lblServerConnCount->setText("1");
        lblServerCurrentConn->setText(ip);
    }
    updateConsoleSendControls();
}

void MainWindow::handleClientDisconnected()
{
    if (lblServerConnCount && lblServerCurrentConn) {
        lblServerConnCount->setText("0");
        lblServerCurrentConn->setText("无");
    }
    updateConsoleSendControls();
}

QString MainWindow::getBluetoothNameForComPort(const QString &portName)
{
#ifdef Q_OS_WIN
    QSettings bthenum("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Enum\\BTHENUM", QSettings::NativeFormat);
    QStringList devices = bthenum.childGroups();
    
    QString targetMac;
    
    // Step 1: Find MAC address for the given COM port
    for (const QString &device : devices) {
        bthenum.beginGroup(device);
        QStringList instances = bthenum.childGroups();
        for (const QString &instance : instances) {
            bthenum.beginGroup(instance);
            if (bthenum.childGroups().contains("Device Parameters")) {
                bthenum.beginGroup("Device Parameters");
                QString portNameReg = bthenum.value("PortName").toString();
                bthenum.endGroup();
                
                if (portNameReg.compare(portName, Qt::CaseInsensitive) == 0) {
                    // Extract MAC from instance string (e.g. 7&2a8adc52&0&0055445D2C7E_C00000000)
                    QStringList parts = instance.split('&');
                    if (parts.size() >= 4) {
                        QString macPart = parts[3];
                        targetMac = macPart.split('_').first();
                    }
                }
            }
            bthenum.endGroup();
            if (!targetMac.isEmpty()) break;
        }
        bthenum.endGroup();
        if (!targetMac.isEmpty()) break;
    }
    
    if (targetMac.isEmpty() || targetMac == "000000000000") return QString();
    
    // Step 2: Find FriendlyName using MAC
    for (const QString &device : devices) {
        bthenum.beginGroup(device);
        QStringList instances = bthenum.childGroups();
        for (const QString &instance : instances) {
            if (instance.contains(targetMac, Qt::CaseInsensitive)) {
                bthenum.beginGroup(instance);
                QString friendlyName = bthenum.value("FriendlyName").toString();
                bthenum.endGroup();
                
                if (!friendlyName.isEmpty() && !friendlyName.contains("COM") && !friendlyName.contains(QString::fromUtf8("串行"))) {
                    bthenum.endGroup(); // device
                    return friendlyName;
                }
            }
        }
        bthenum.endGroup();
    }
#endif
    return QString();
}

void MainWindow::showCommandPanel()
{
    if (cmdWindow) {
        if (cmdWindow->windowState() & Qt::WindowMinimized) {
            cmdWindow->setWindowState(cmdWindow->windowState() & ~Qt::WindowMinimized);
        }
        cmdWindow->show();
        cmdWindow->raise();
        cmdWindow->activateWindow();
    }
}

bool MainWindow::sendPanelData(const QByteArray &data, bool isAuto)
{
    if (m_protocolApplyPending) {
        statusBar()->showMessage("协议正在切换，命令未发送。", 5000); return false;
    }
    if (isPortOpen) {
        return sendDataToCurrentInterface(data);
    } else {
        if (!isAuto) {
            QMessageBox::warning(this, "提示", "请先打开连接！");
        } else {
            if (cmdWindow) {
                cmdWindow->forceStopAllTimers();
            }
            QMessageBox::warning(this, "提示", "请先打开连接！\n(检测到定时发送，已自动将其停止)");
        }
    }
    return false;
}

void MainWindow::onOpenSendFile()
{
    QString filePath = QFileDialog::getOpenFileName(this, QString::fromUtf8("选择要发送的文件"), "", "所有文件 (*);;常用数据文件 (*.bin *.hex *.txt)");
    if (!filePath.isEmpty()) {
        txtSendFilePath->setText(filePath);
    }
}

void MainWindow::onStartSendFile()
{
    stopConsolePeriodicSend("开始文件发送，定时发送已停止。");
    if (m_protocolApplyPending) return;
    if (comboInterface->currentData().toInt() == 5) {
        QMessageBox::information(this, "UDP 文件发送不可用", "UDP 不保证数据到达或顺序，本版不提供 UDP 文件发送。请使用 TCP、串口或具备确认机制的设备协议。文本/HEX、命令和定时发送可正常使用。"); return;
    }
    if (comboInterface->currentData().toInt() == 0 && (m_serialSendCancelPending || (fileSendTimer && fileSendTimer->isActive()))) return;
    if (comboInterface->currentData().toInt() == 0 && (m_serialBreakActive || m_serialBreakPending)) {
        statusBar()->showMessage(QStringLiteral("无法发送文件：请先释放 Break 后重试。"), 6000);
        return;
    }
    if (comboInterface->currentData().toInt() == 4) {
        if (cmdWindow) cmdWindow->forceStopAllTimers();
        QString error;
        startFileSendEta();
        if (!bleClient->startFileSend(txtSendFilePath->text(), error)) {
            resetFileSendEta();
            QMessageBox::warning(this, QStringLiteral("无法发送文件"), error);
        }
        updateBleControls(); return;
    }
    if (!isPortOpen) {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("请先打开连接！"));
        return;
    }
    if (comboInterface->currentData().toInt() == 0 && serialHandler->pendingSendBytes() > 0) {
        showSerialControlWarning(QStringLiteral("暂时无法发送文件"), QStringLiteral("串口还有待发送数据。请等待写出或取消待发送数据，再开始文件发送，避免文件中混入其他命令。"));
        return;
    }
    
    QString filePath = txtSendFilePath->text();
    if (filePath.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("请先选择文件！"));
        return;
    }
    
    if (fileToSend->isOpen()) {
        fileToSend->close();
    }
    
    fileToSend->setFileName(filePath);
    if (!fileToSend->open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QString::fromUtf8("错误"), QString::fromUtf8("无法打开文件！"));
        return;
    }
    
    const int interfaceType = comboInterface->currentData().toInt();
    if ((interfaceType == 1 || interfaceType == 2) &&
        (!networkHandler->sendReady() || networkHandler->pendingSendBytes() > 0)) {
        fileToSend->close();
        QMessageBox::warning(this, "暂时无法发送文件", "请先等待网络客户端连接并完成已有待发送数据，再开始文件发送。"); return;
    }
    if (interfaceType == 3) { fileToSend->close(); QMessageBox::information(this, "无法发送文件", "Demo 接口只生成测试数据，不向设备发送文件。"); return; }
    progressBarFile->setRange(0, 1000); // Ratios also support files larger than 2 GB.
    progressBarFile->setValue(0);
    startFileSendEta();
    updateFileSendEta(0, fileToSend->size());
    
    btnOpenSendFile->setEnabled(false);
    btnSendFile->setEnabled(false);
    btnStopSendFile->setEnabled(true);
    txtSendFilePath->setEnabled(false);
    
    fileSendTimer->start();
    btnSend->setEnabled(false);
    if (cmdWindow) cmdWindow->forceStopAllTimers();
    updateSerialBreakControls();
}

void MainWindow::onStopSendFile()
{
    resetFileSendEta();
    stopConsolePeriodicSend("发送操作已停止。");
    if (comboInterface->currentData().toInt() == 4 && bleClient) {
        bleClient->cancelFileSend(); updateBleControls(); return;
    }
    if (comboInterface->currentData().toInt() == 0 && connectionIsReady() &&
        (fileSendTimer->isActive() || serialHandler->pendingSendBytes() > 0)) {
        cancelSerialSend(); return;
    }
    if (!fileSendTimer->isActive() && (!fileToSend || !fileToSend->isOpen())) {
        QMessageBox::information(this, QString::fromUtf8("提示"), QString::fromUtf8("当前没有正在发送的文件。"));
        return;
    }

    if (fileSendTimer->isActive()) {
        fileSendTimer->stop();
    }
    if (fileToSend->isOpen()) {
        fileToSend->close();
    }
    
    btnOpenSendFile->setEnabled(true);
    btnSendFile->setEnabled(true);
    btnStopSendFile->setEnabled(true);
    txtSendFilePath->setEnabled(true);
    btnSend->setEnabled(connectionIsReady());
    updateSerialBreakControls();
}

void MainWindow::onFileSendTimer()
{
    if (comboInterface->currentData().toInt() == 0) {
        if (!connectionIsReady() || m_serialSendCancelPending || !serialHandler->sendSessionReady(m_serialOpenRequestId)) { finishSerialFileSend(); return; }
        const qint64 pending = serialHandler->pendingSendBytes();
        if (fileToSend->isOpen() && fileToSend->size() > 0) {
            progressBarFile->setValue(static_cast<int>(qMax<qint64>(0, fileToSend->pos() - pending) * 1000 / fileToSend->size()));
            updateFileSendEta(qMax<qint64>(0, fileToSend->pos() - pending), fileToSend->size());
        }
        if (pending > 0) return; // CTS wait/backpressure: never read ahead unboundedly.
        if (!fileToSend->isOpen()) { finishSerialFileSend(); return; }
        if (fileToSend->atEnd()) {
            finishSerialFileSend(); progressBarFile->setValue(1000); btnStopSendFile->setEnabled(false);
            statusBar()->showMessage(QStringLiteral("文件已完成串口写入（不代表设备业务确认）。"), 6000);
            return;
        }
        const QByteArray chunk = fileToSend->read(4096);
        if (chunk.isEmpty() || fileToSend->error() != QFile::NoError) {
            const QString detail = fileToSend->errorString(); cancelSerialSend();
            showSerialControlWarning(QStringLiteral("文件发送失败"), QStringLiteral("读取源文件失败，文件发送已停止。\n%1").arg(detail)); return;
        }
        if (!serialHandler->enqueueData(m_serialOpenRequestId, chunk)) {
            cancelSerialSend();
            statusBar()->showMessage(QStringLiteral("文件发送已停止：连接关闭或发送队列已满，未自动重发。"), 6000);
        }
        return;
    }
    if (!isPortOpen) {
        onStopSendFile();
        return;
    }
    
    const int interfaceType = comboInterface->currentData().toInt();
    if (interfaceType != 1 && interfaceType != 2) {
        onStopSendFile(); statusBar()->showMessage("当前接口不支持文件发送。", 5000); return;
    }
    if (!networkHandler->sendReady()) {
        onStopSendFile(); QMessageBox::warning(this, "文件发送已停止", "网络客户端已断开，未完成数据不会自动重发。"); return;
    }
    const qint64 pending = networkHandler->pendingSendBytes();
    if (fileToSend->isOpen() && fileToSend->size() > 0) {
        progressBarFile->setValue(int(qMax<qint64>(0, fileToSend->pos() - pending) * 1000 / fileToSend->size()));
        updateFileSendEta(qMax<qint64>(0, fileToSend->pos() - pending), fileToSend->size());
    }
    if (pending > 0) return; // Backpressure: no unchecked socket/event backlog.
    if (!fileToSend->isOpen() || fileToSend->atEnd()) {
        onStopSendFile();
        progressBarFile->setValue(progressBarFile->maximum());
        QMessageBox::information(this, QString::fromUtf8("提示"), QString::fromUtf8("文件发送完成！"));
        return;
    }
    
    const QByteArray chunk = fileToSend->read(4096);
    if (chunk.isEmpty() || fileToSend->error() != QFile::NoError || !networkHandler->enqueueData(chunk)) {
        const QString detail = fileToSend->errorString(); onStopSendFile();
        QMessageBox::warning(this, "文件发送失败", "源文件读取失败或网络发送通道不可用。已停止发送，未自动重发。\n" + detail);
    }
}

void MainWindow::startFileSendEta()
{
    setFileSendProgressHint();
    m_fileSendEta.start(); m_fileSendElapsed.start(); m_fileSendEtaPaintMs = -1000;
    if (fileSendEtaTimer) fileSendEtaTimer->start();
    refreshFileSendEta();
}
void MainWindow::setFileSendProgressHint(const QString &detail)
{
    if (!progressBarFile) return;
    progressBarFile->setToolTip((detail.isEmpty() ? QString() : detail + "\n\n") +
        QStringLiteral("文件发送进度。发送结束或停止后，左键双击可清空上次发送的进度及时间显示，保留已选择的文件。\n发送中（包括准备文件、等待发送完成或正在取消）双击无效，不会中断发送或重置进度。进度不代表接收端已校验或保存文件。"));
}
void MainWindow::updateFileSendEta(qint64 completed, qint64 total)
{
    if (!m_fileSendEta.active() || !m_fileSendElapsed.isValid()) return;
    m_fileSendEta.observe(m_fileSendElapsed.elapsed(), completed, total);
    refreshFileSendEta();
}
void MainWindow::refreshFileSendEta()
{
    if (!lblFileSendEta) return;
    const qint64 elapsed = m_fileSendElapsed.isValid() ? m_fileSendElapsed.elapsed() : 0;
    if (m_fileSendEta.active() && elapsed - m_fileSendEtaPaintMs < 500) return;
    m_fileSendEtaPaintMs = elapsed;
    const auto text = m_fileSendEta.text(elapsed);
    if (lblFileSendEta->text() != text) lblFileSendEta->setText(text);
}
void MainWindow::resetFileSendEta()
{
    if (fileSendEtaTimer) fileSendEtaTimer->stop();
    m_fileSendEta.reset(); m_fileSendElapsed.invalidate(); refreshFileSendEta();
}
