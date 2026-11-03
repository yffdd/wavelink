#include "mainwindow.h"
#include "connectionhints.h"
#include "bledialogs.h"
#include "commandpanelwindow.h"
#include "debuglogger.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>
#include <memory>
#include <QTextCodec>
#include "ble/bleprofilestore.h"
#include "ble/blechannelpolicy.h"
#include <QSysInfo>

QWidget *MainWindow::createBlePage()
{
    auto page = new QWidget;
    page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    editBleDevice = new QLineEdit; editBleDevice->setReadOnly(true);
    editBleDevice->setPlaceholderText(QStringLiteral("扫描并选择设备"));
    btnBleScan = new QPushButton(QStringLiteral("扫描…"));
    auto target = new QHBoxLayout; target->addWidget(editBleDevice, 1); target->addWidget(btnBleScan);
    form->addRow(QStringLiteral("目标设备"), target);
    lblBleStatus = new QLabel(QStringLiteral("连接后发现服务并选择收发特征"));
    lblBleStatus->setWordWrap(true); lblBleStatus->setMinimumWidth(0);
    lblBleStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    form->addRow(QStringLiteral("BLE 状态"), lblBleStatus);
    ConnectionHints::row(form, target, "扫描并选择 BLE 外设；经典蓝牙虚拟 COM 口请使用串口接口。");
    ConnectionHints::row(form, lblBleStatus, "显示蓝牙连接、服务发现及收发通道配置状态。");
    btnBleScan->setToolTip("搜索附近可连接的 BLE 外设并选择目标，不自动发送数据。");
    auto actions = new QHBoxLayout;
    btnBleProfiles = new QPushButton(QStringLiteral("收发配置…")); btnBleProfiles->setObjectName("bleProfilesButton");
    btnBleDiagnostics = new QPushButton(QStringLiteral("连接诊断…")); btnBleDiagnostics->setObjectName("bleDiagnosticsButton");
    actions->addWidget(btnBleProfiles); actions->addWidget(btnBleDiagnostics); form->addRow(actions);
    btnBleProfiles->setToolTip("选择接收/发送特征、模板和分包参数；须与外设协议一致。");
    btnBleDiagnostics->setToolTip("查看连接阶段、收发统计及错误信息，便于排查蓝牙连接问题。");
    connect(btnBleProfiles, &QPushButton::clicked, this, &MainWindow::showBleProfiles);
    connect(btnBleDiagnostics, &QPushButton::clicked, this, &MainWindow::showBleDiagnostics);
    auto help = new QPushButton(QStringLiteral("BLE 连接与收发说明"));
    help->setToolTip("查看 BLE 连接、特征选择、收发和录制的使用说明。");
    form->addRow(help);
    connect(btnBleScan, &QPushButton::clicked, this, &MainWindow::selectBleDevice);
    connect(help, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, QStringLiteral("BLE 连接与数据调试"), QStringLiteral(
            "1. 扫描并选择可连接的 BLE 外设，点击连接。\n"
            "2. 首次连接在快速配置中选择接收/发送通道，也可先仅接收候选通道；候选仅按属性推荐，不会发送探测命令。发送须确认透传用途。\n"
            "3. 写入方式按特征自动选择，有响应写优先；完整服务树、模板与发送参数位于高级配置。已知固件升级服务不参与推荐。\n"
            "4. 默认 Raw 文本/HEX 收发；需要波形时选择 FireWater、JustFloat 或匹配的自定义协议。CBF/IR/RED 可在自定义协议管理中从键值设备模板创建。\n"
            "5. 数值协议支持 CSV/WLR 录制，记录原始通知接收时间；绘图限流不影响已解析数据的录制。\n"
            "6. 默认 20 字节 / 10 ms 分包；在‘收发配置 → 高级发送控制’调整，并查看实际生效值。发送期间不允许改参数或交错发送其他命令。\n\n"
            "Windows 协商 MTU 不是模块缓冲容量；能力未知时最多 20 字节。提速需要确认，0 ms 仍逐包等待。能力降低会停止待发数据，接收/录制继续，不自动重发。\n"
            "发送源文件由独立后台读取；读取失败/超时只停止文件任务，不主动断开健康连接。\n"
            "成功配置会按设备保存。下次连接核对服务结构、特征与属性；一致且无需高级参数风险确认时直接应用，不自动发送业务命令。配置改变、旧配置或服务不匹配时重新核对。\n"
            "需要改通道时从‘收发配置’保存下次配置并重新连接。分包要求外设支持透传字节流。\n"
            "发送完成指 GATT 完成/提交，不等于外设业务层确认；取消时已提交的一包不能撤回。\n"
            "采集积压超限会明确报错并断开，不会静默丢弃录制数据。\n"
            "非透传设备的控制特征不要随意作为字节流发送。需要配对时先在 Windows 设置中配对。\n"
            "经典蓝牙虚拟 COM 口继续使用串口接口。"));
    });
    return page;
}
void MainWindow::setupBleBackend()
{
    bleClient = new BleHostClient(this);
    bleThread = new QThread(this);
    bleWorker = new BleStreamWorker;
    bleWorker->moveToThread(bleThread);
    connect(bleThread, &QThread::finished, bleWorker, &QObject::deleteLater);
    connect(bleClient, &BleHostClient::notificationReceived, this, [this](const QByteArray &bytes, qint64 timestampNs) {
        if (!connectionIsReady() || m_activeInterfaceType != 4 || !m_bleDataSession) return;
        if (!bleWorker->enqueueData(m_bleDataSession, bytes, timestampNs))
            bleClient->abortSession(QStringLiteral("BLE 后台采集积压超过安全上限，已停止连接和录制。请降低外设发送速率。"));
    });
    connect(bleWorker, &BleStreamWorker::recordingStartFinished, this, [this](quint64 session, quint64 request, bool ok, const QString &error) {
        if (session == m_bleDataSession && request == m_bleRecordRequest && m_activeInterfaceType == 4) handleRecordingStartFinished(ok, error);
    });
    connect(bleWorker, &BleStreamWorker::recordingError, this, [this](quint64 session, quint64 request, const QString &error) {
        if (session == m_bleDataSession && request == m_bleRecordRequest && m_activeInterfaceType == 4) handleRecordingError(error);
    });
    connect(bleWorker, &BleStreamWorker::pipelineError, this, [this](quint64 session, const QString &error) {
        if (session == m_bleDataSession && m_activeInterfaceType == 4) bleClient->abortSession(error);
    });
    connect(bleWorker, &BleStreamWorker::recordingStopped, this, [this](quint64 request, const QString &error, qint64 bytes) {
        if (!m_bleRecordStopPending || request != m_bleRecordRequest) return;
        m_bleRecordStopPending = false;
        QString message = m_bleRecordStopMessage; m_bleRecordStopMessage.clear();
        if (!error.isEmpty() && message.isEmpty()) message = QStringLiteral(
            "BLE 录制文件未能完整写入或完成收尾；已落盘的临时文件保留，可在下次启动时恢复。\n\n%1").arg(error);
        btnToggleRecord->setText(QStringLiteral("开始录制")); btnToggleRecord->setStyleSheet(QString());
        comboRecordingMode->setEnabled(true);
        lblRecordStatus->setText(QStringLiteral("已停止 | %1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2));
        updateBleControls();
        if (!message.isEmpty()) {
            lblRecordStatus->setText(QStringLiteral("录制已停止（写入失败）"));
            DebugLogger::instance().log(message, "ERROR");
            QMessageBox::critical(this, QStringLiteral("录制已安全停止"), message);
        }
        if (m_bleCloseAfterRecordStop) {
            m_bleCloseAfterRecordStop = false;
            QTimer::singleShot(0, this, &QWidget::close);
        }
    });
    connect(bleClient, &BleHostClient::fileSendProgress, this, [this](qint64 completed, qint64 total) {
        updateFileSendEta(completed, total);
        if (total < 0) { progressBarFile->setRange(0, 0); setFileSendProgressHint(QStringLiteral("后台正在打开发送文件…")); return; }
        progressBarFile->setRange(0, 100);
        progressBarFile->setValue(total > 0 ? int((double(completed) / double(total)) * 100.0) : 0);
        setFileSendProgressHint(QStringLiteral("GATT 已完成/提交：%1 / %2 字节").arg(completed).arg(total));
    });
    connect(bleClient, &BleHostClient::fileSendFinished, this, [this](bool ok, const QString &message) {
        resetFileSendEta();
        progressBarFile->setRange(0, 100);
        if (!ok && progressBarFile->value() >= 100) progressBarFile->setValue(0);
        setFileSendProgressHint(message);
        updateBleControls();
        statusBar()->showMessage(message, 8000);
        if (ok) QMessageBox::information(this, QStringLiteral("BLE 文件发送完成"), message);
        else DebugLogger::instance().log(message, "WARN");
    });
    connect(bleClient, &BleHostClient::fileSendStatus, this, [this](const QString &message) {
        statusBar()->showMessage(message); setFileSendProgressHint(message);
    });
    connect(bleClient, &BleHostClient::fileSendError, this, [this](const QString &message) {
        QTimer::singleShot(0, this, [this, message] { BleDialogs::showFileError(message, this); });
    });
    connect(bleClient, &BleHostClient::operationFinished, this, &MainWindow::updateBleControls);
    connect(bleClient, &BleHostClient::sendSettingsChanged, this, &MainWindow::updateBleControls);
    connect(bleClient, &BleHostClient::sendSettingsApplied, this, [this] {
        if (!connectionIsReady() || m_activeInterfaceType != 4 || m_bleAppliedProfile.isEmpty()) return;
        const auto s = bleClient->sendSettings();
        m_bleAppliedProfile["schemaVersion"] = 3;
        m_bleAppliedProfile["packetSize"] = s.value("packetSize"); m_bleAppliedProfile["intervalMs"] = s.value("intervalMs");
        QSettings settings("WaveLink", "WaveLink"); BleProfileStore store(settings); QString error;
        auto next = store.deviceProfile(m_bleDevice, error);
        // Do not overwrite a corrupt saved profile or change next-connection
        // UUIDs when the user only applied live sending parameters.
        if (error.isEmpty()) {
            if (next.isEmpty()) next = m_bleAppliedProfile;
            next["schemaVersion"] = 3; next["packetSize"] = s.value("packetSize"); next["intervalMs"] = s.value("intervalMs");
            store.saveDeviceProfile(m_bleDevice, next, error);
        }
        if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("参数已生效，但保存失败"), error);
        updateBleControls();
    });
    connect(bleClient, &BleHostClient::sendingStopped, this, [this](const QString &message) {
        if (cmdWindow) cmdWindow->forceStopAllTimers();
        stopConsolePeriodicSend(message);
        DebugLogger::instance().log(message, "WARN"); updateBleControls();
        QTimer::singleShot(0, this, [this, message] { BleDialogs::showSendStopped(message, this); });
    });
    bleThread->start();
    QSettings settings("WaveLink", "WaveLink");
    m_bleDevice = QJsonDocument::fromJson(settings.value("Ble/LastDevice").toByteArray()).object();
    editBleDevice->setText(m_bleDevice.value("name").toString());
    editBleDevice->setToolTip(m_bleDevice.value("address").toString());
    connect(bleClient, &BleHostClient::stateChanged, this, [this](BleHostClient::State state) {
        QString text;
        switch (state) {
        case BleHostClient::State::Idle:
            m_bleAppliedProfile = {};
            if (m_bleDataSession) {
                bleWorker->endSession();
                // Finalize accepted data before removing the active session.
                if (isRecording) stopRecordingSafely();
                m_bleDataSession = 0;
            }
            text = QStringLiteral("未连接"); break;
        case BleHostClient::State::Scanning: text = QStringLiteral("扫描中"); break;
        case BleHostClient::State::Connecting: text = QStringLiteral("正在连接设备"); break;
        case BleHostClient::State::Discovering: text = QStringLiteral("正在发现服务和特征"); break;
        case BleHostClient::State::AwaitingConfiguration: text = QStringLiteral("已连接，等待选择收发通道"); break;
        case BleHostClient::State::Configuring: text = QStringLiteral("正在配置订阅和写入通道"); break;
        case BleHostClient::State::Ready: text = QStringLiteral("收发就绪"); break;
        }
        lblBleStatus->setText(text); updateBleControls();
        QTimer::singleShot(0, this, [this] { if (comboInterface->currentData().toInt() == 4) updateInterfaceStackHeight(); });
    });
    // Stateful decoders preserve UTF-8/GBK characters spanning notifications.
    auto decoder = std::make_shared<std::unique_ptr<QTextDecoder>>();
    auto decoderEncoding = std::make_shared<QString>();
    connect(bleClient, &BleHostClient::stateChanged, this, [decoder, decoderEncoding](BleHostClient::State state) {
        if (state == BleHostClient::State::Idle) { decoder->reset(); decoderEncoding->clear(); }
    });
    connect(bleClient, &BleHostClient::dataReceived, this, [this, decoder, decoderEncoding](const QByteArray &bytes) {
        if (!connectionIsReady() || m_activeInterfaceType != 4 || !chkShowReceiveData->isChecked()) { decoder->reset(); return; }
        QString text;
        if (chkHexReceive->isChecked()) { decoder->reset(); text = QString::fromLatin1(bytes.toHex(' ').toUpper()) + " "; }
        else {
            const QString encoding = comboEncoding->currentText() == "GBK" ? "GBK" : "UTF-8";
            if (!*decoder || *decoderEncoding != encoding) {
                decoder->reset(QTextCodec::codecForName(encoding.toLatin1())->makeDecoder()); *decoderEncoding = encoding;
            }
            text = (*decoder)->toUnicode(bytes);
        }
        if (chkTimestamp->isChecked() && !text.isEmpty()) text = "[" + QTime::currentTime().toString("HH:mm:ss.zzz") + "] " + text + "\n";
        appendReceiveText(text);
    });
    connect(bleClient, &BleHostClient::ready, this, [this] {
        if (m_activeInterfaceType == 4 && m_connectionState == ConnectionState::Connecting) {
            if (m_isImportedData) onClearPlots(); // Leave offline analysis before live BLE data.
            m_bleDataSession = ++m_bleDataSessionCounter;
            bleWorker->beginSession(m_bleDataSession, m_appliedProtocol, m_appliedProfile);
            finishConnectionOpen(4); updateBleControls();
            lblTxCount->setToolTip(QStringLiteral("BLE Tx 为 GATT 写入完成/提交的字节数；不代表设备业务程序已经执行。"));
        }
    });
    connect(bleClient, &BleHostClient::characteristicsDiscovered, this, [this](const QJsonArray &entries) {
        if (m_activeInterfaceType != 4 || m_connectionState != ConnectionState::Connecting || m_bleConfigOpen) return;
        QSettings settings("WaveLink", "WaveLink");
        m_bleCharacteristics = entries;
        QJsonObject profile;
        QString error; BleProfileStore store(settings);
        const auto saved = store.deviceProfile(m_bleDevice, error);
        if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("请重新核对 BLE 配置"), error);
        int rx = 0, tx = 0;
        if (BleChannelPolicy::quickConnect(entries, saved, rx, tx)) {
            BleProfileStore::normalize(saved, m_bleAppliedProfile, error);
            if (bleClient->configure(rx, tx, saved.value("withoutResponse").toBool(),
                                     saved.value("packetSize").toInt(20), saved.value("intervalMs").toInt(10), false)) return;
            m_bleAppliedProfile = {};
            if (bleClient->state() != BleHostClient::State::AwaitingConfiguration) return;
        }
        m_bleConfigOpen = true;
        const QString identity = m_bleDevice.value("name").toString() + " · " +
            QString::number(m_bleDevice.value("address").toString().toULongLong(), 16).toUpper().rightJustified(12, '0');
        const bool configured = BleDialogs::configure(bleClient, entries, saved, profile, this, identity);
        m_bleConfigOpen = false;
        if (configured) {
            m_bleAppliedProfile = profile;
            if (!store.saveDeviceProfile(m_bleDevice, profile, error, entries)) QMessageBox::warning(this, QStringLiteral("BLE 配置未保存"), error);
            updateBleControls();
        } else if (m_connectionState != ConnectionState::Error) {
            bleClient->disconnectDevice(); finishConnectionClosed(4, false); updateBleControls();
        }
    });
    connect(bleClient, &BleHostClient::errorOccurred, this, [this](const QString &message, bool fatal) {
        DebugLogger::instance().log(QString("BLE: %1").arg(message), fatal ? "ERROR" : "WARN");
        if (!fatal || m_activeInterfaceType != 4 ||
            (m_connectionState != ConnectionState::Connecting && m_connectionState != ConnectionState::Connected)) return;
        if (cmdWindow) cmdWindow->forceStopAllTimers();
        finishConnectionClosed(4, false); setConnectionState(ConnectionState::Error, 4); updateBleControls();
        // Let an open configuration dialog close before showing diagnostics.
        QTimer::singleShot(0, this, [this, message] { BleDialogs::showError(message, editBleDevice->text(), this); });
    });
}
void MainWindow::selectBleDevice()
{
    if (!bleClient || m_connectionOperationPending || connectionIsReady()) return;
    const auto device = BleDialogs::selectDevice(bleClient, this);
    if (device.isEmpty()) return;
    m_bleDevice = device;
    m_bleCharacteristics = {};
    QString name = device.value("name").toString();
    if (name.isEmpty()) name = QStringLiteral("未命名设备");
    m_bleDevice.insert("name", name);
    editBleDevice->setText(name); editBleDevice->setToolTip(device.value("address").toString());
    QSettings settings("WaveLink", "WaveLink"); settings.setValue("Ble/LastDevice", QJsonDocument(m_bleDevice).toJson(QJsonDocument::Compact));
}
void MainWindow::openCloseBleConnection()
{
    if (!bleClient) return;
    if (connectionIsReady() || m_connectionOperationPending) {
        if (cmdWindow) cmdWindow->forceStopAllTimers();
        bleClient->disconnectDevice(); finishConnectionClosed(4, false); updateBleControls(); return;
    }
    if (m_bleDevice.isEmpty()) selectBleDevice();
    if (m_bleDevice.isEmpty()) return;
    setConnectionState(ConnectionState::Connecting, 4);
    comboInterface->setEnabled(false); btnBleScan->setEnabled(false);
    btnOpenClose->setEnabled(true); btnOpenClose->setText(QStringLiteral("取消 BLE 连接"));
    bleClient->connectDevice(m_bleDevice.value("address").toString(), m_bleDevice.value("addressType").toInt());
}
void MainWindow::updateBleControls()
{
    if (!comboInterface || !btnToggleRecord || !btnSendFile) return;
    const bool ble = comboInterface->currentData().toInt() == 4;
    const bool udp = comboInterface->currentData().toInt() == 5;
    const bool ready = bleClient && bleClient->state() == BleHostClient::State::Ready && connectionIsReady();
    const bool sendingFile = bleClient && bleClient->fileSendBusy();
    const bool serialBlocked = comboInterface->currentData().toInt() == 0 && (m_serialBreakActive || m_serialBreakPending || m_serialSendCancelPending || (fileSendTimer && fileSendTimer->isActive()));
    btnToggleRecord->setEnabled(!m_bleRecordStopPending && (!ble || isRecording || (ready && bleClient->canReceive() && comboProtocol->currentIndex() != 0)));
    btnSendFile->setEnabled(!udp && !serialBlocked && (!ble ? (!fileSendTimer || !fileSendTimer->isActive()) : (ready && bleClient->canSend() && !sendingFile)));
    btnSend->setEnabled(!serialBlocked && (udp ? connectionIsReady() && networkHandler && networkHandler->sendReady() : (!ble || (ready && bleClient->canSend() && !sendingFile))));
    if (ble) {
        btnOpenSendFile->setEnabled(!sendingFile); txtSendFilePath->setEnabled(!sendingFile);
        btnStopSendFile->setEnabled(bleClient && bleClient->fileSendActive());
        btnToggleRecord->setToolTip(QStringLiteral("BLE 录制需要接收特征和数值协议；Raw 模式不生成波形录制文件。"));
    } else {
        btnToggleRecord->setToolTip(QString());
        btnOpenSendFile->setEnabled(!fileSendTimer->isActive());
        txtSendFilePath->setEnabled(!fileSendTimer->isActive());
        btnStopSendFile->setEnabled(true);
    }
    btnBleScan->setEnabled(!m_connectionOperationPending && !connectionIsReady());
    updateProtocolControls();
    updateConsoleSendControls();
    if (ble && !bleClient->available()) lblBleStatus->setText(QStringLiteral("BLE 辅助程序尚未部署，请先构建"));
    else if (ble && ready && !m_bleAppliedProfile.isEmpty()) {
        const auto s = bleClient->sendSettings();
        lblBleStatus->setText(QStringLiteral("收发就绪 · 接收%1 · 发送%2 · 实际 %3 字节/%4 ms")
            .arg(bleClient->canReceive() ? QStringLiteral("启用") : QStringLiteral("未配置"),
                 s.value("blocked").toBool() ? QStringLiteral("暂停（请确认参数）") : s.value("pending").toBool() ? QStringLiteral("等待参数应用") : bleClient->canSend() ? QStringLiteral("启用") : QStringLiteral("未配置"))
            .arg(s.value("effectivePacket").toInt(20)).arg(s.value("intervalMs").toInt(10)));
        lblBleStatus->setToolTip(QStringLiteral("设置分包 %1 字节；MTU %2。实际分包受已知能力限制，提速并不保证外设业务层接收。")
            .arg(s.value("packetSize").toInt(20)).arg(s.value("mtu").toInt() ? QString::number(s.value("mtu").toInt()) : QStringLiteral("未知")));
    }
}

void MainWindow::showBleProfiles()
{
    if (!bleClient || m_bleConfigOpen) return;
    QSettings settings("WaveLink", "WaveLink"); BleProfileStore store(settings); QString error;
    const auto next = store.deviceProfile(m_bleDevice, error);
    if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("保存配置读取失败"), error);
    const bool ready = connectionIsReady() && m_activeInterfaceType == 4;
    const bool editable = !m_connectionOperationPending && !isRecording && !m_bleRecordStopPending && !bleClient->sendBusy() && !bleClient->readPending();
    const auto action = BleDialogs::manageProfiles(m_bleAppliedProfile, next, m_bleCharacteristics, ready, editable, this, bleClient);
    if (action.applyCurrent) { updateBleControls(); return; } // Persisted only after the helper's acknowledgement.
    if (!action.saveNext) return;
    if (!store.saveDeviceProfile(m_bleDevice, action.profile, error)) { QMessageBox::warning(this, QStringLiteral("BLE 配置未保存"), error); return; }
    // A disconnect/error may arrive while the modal dialog is open. Never turn
    // a reconnect action into an unsolicited new connection after such a loss.
    if (action.reconnect && connectionIsReady() && m_activeInterfaceType == 4 && !isRecording && !m_bleRecordStopPending && !bleClient->sendBusy()) {
        openCloseBleConnection();
        QTimer::singleShot(0, this, [this] {
            if (comboInterface->currentData().toInt() == 4 && !connectionIsReady() && !m_connectionOperationPending &&
                !isRecording && !m_bleRecordStopPending && bleClient->state() == BleHostClient::State::Idle) openCloseBleConnection();
        });
    }
}
void MainWindow::showBleDiagnostics()
{
    if (bleClient) BleDialogs::showDiagnostics([this](bool identity) { return bleDiagnosticText(identity); }, this);
}
QString MainWindow::bleDiagnosticText(bool identity) const
{
    const auto d = bleClient->diagnostics(), host = d.value("host").toObject();
    const QStringList states{QStringLiteral("未连接"), QStringLiteral("扫描中"), QStringLiteral("正在连接"), QStringLiteral("服务发现"),
        QStringLiteral("等待核对配置"), QStringLiteral("正在配置"), QStringLiteral("收发就绪")};
    auto duration = [&](const char *key) { const auto value = d.value(key).toDouble(-1); return value < 0 ? QStringLiteral("未完成/未知") : QString::number(qint64(value)) + " ms"; };
    QString text = QStringLiteral("WaveLink BLE 连接诊断\n时间：%1\n系统：%2 / %3\n状态：%4\n设备：%5\n辅助程序：%6\nIPC 版本：%7；文件读取能力版本：%8\n适配器详情：未知（当前后端未上报）\n\n最近结束阶段耗时（含辅助进程启动/系统等待，非无线 RTT）\n连接：%9；发现：%10；配置：%11\n\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"), QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
             states.value(d.value("state").toInt()), identity ? m_bleDevice.value("name").toString() + " / " + m_bleDevice.value("address").toString() + " / type=" + QString::number(m_bleDevice.value("addressType").toInt()) : QStringLiteral("[已脱敏]"),
             identity ? bleClient->hostPath() : QStringLiteral("WaveLinkBleHost.exe（路径已隐藏）"),
             host.contains("version") ? QString::number(host.value("version").toInt()) : QStringLiteral("未知"),
             host.contains("fileReaderVersion") ? QString::number(host.value("fileReaderVersion").toInt()) : QStringLiteral("未知"))
        .arg(duration("connectMs"), duration("discoverMs"), duration("configureMs"));
    const auto s = d.value("sendSettings").toObject();
    text += QStringLiteral("高级发送控制：%1\n协商 MTU：%2；普通写入上限：%3 字节\n设置分包：%4 字节；实际分包：%5 字节；最小间隔：%6 ms\n发送暂停：%7；参数应用等待：%8\n未知 MTU 时最多 20 字节；MTU−3 / 后端 512 上限不代表模块缓冲容量。\n0 ms 仍保持一个在途写入；能力增加不会自动提速。\n\n")
        .arg(s.value("supported").toBool() ? QStringLiteral("可用") : QStringLiteral("不可用/旧辅助程序"))
        .arg(s.value("mtu").toInt() ? QString::number(s.value("mtu").toInt()) : QStringLiteral("未知"))
        .arg(s.value("packetLimit").toInt(20)).arg(s.value("packetSize").toInt(20)).arg(s.value("effectivePacket").toInt(20)).arg(s.value("intervalMs").toInt(10))
        .arg(s.value("blocked").toBool() ? QStringLiteral("是") : QStringLiteral("否"), s.value("pending").toBool() ? QStringLiteral("是") : QStringLiteral("否"));
    text += QStringLiteral("传输计数（连接开始/清零计数后）\n通知：%1；Rx：%2 字节；Tx：%3 字节\nGATT 写入等待：%4；待发数据：%5 字节；文件任务：%6\nTx 指完成/提交，不是设备业务确认。\n\n后台采集（最近会话，分项快照并非原子事务）\n待解析：%7 字节；已解析：%8 帧\n最近录制写入已接纳：%9 帧；文件写入计数：%10 字节\n仅显示队列丢弃：%11 帧（不代表录制丢帧）\n写入已接纳可能尚在缓冲，不能据此保证异常断电后全部恢复。\n\n")
        .arg(d.value("notifications").toString(), d.value("rxBytes").toString(), d.value("txBytes").toString(),
             d.value("writePending").toBool() ? QStringLiteral("是") : QStringLiteral("否"), QString::number(d.value("sendQueuedBytes").toInt()),
             d.value("fileBusy").toBool() ? QStringLiteral("进行中") : QStringLiteral("无"), QString::number(bleWorker->queuedBytes()),
             QString::number(bleWorker->parsedFrames()), QString::number(bleWorker->recordedFrames()))
        .arg(QString::number(bleWorker->getLoggedBytes()), QString::number(bleWorker->displayDropped()));
    text += QStringLiteral("当前已确认 UUID 配置：\n") + (m_bleAppliedProfile.isEmpty() ? QStringLiteral("未确认\n") : QString::fromUtf8(QJsonDocument(m_bleAppliedProfile).toJson(QJsonDocument::Indented)));
    const auto error = d.value("lastError").toString();
    // Error strings may contain arbitrary paths and device identity. In the
    // default report omit their body instead of relying on incomplete regexes.
    text += QStringLiteral("\n最近错误：") + (error.isEmpty() ? QStringLiteral("无") : identity ? error : QStringLiteral("有（详细内容已隐藏；勾选完整信息后查看）"));
    return text;
}
