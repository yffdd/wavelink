#include "blehostclient.h"
#include "bleipc.h"
#include "blefilesource.h"
#include "blesendpolicy.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <limits>

BleHostClient::BleHostClient(QObject *parent, const QString &path)
    : QObject(parent), m_hostPath(path)
{
    m_fileSource = new BleFileSource(this);
    connect(m_fileSource, &BleFileSource::opened, this, [this](qint64 total) {
        if (!m_fileActive) return;
        m_fileTotal = total;
        if (!total) { finishFileSend(false, QStringLiteral("文件为空，没有数据需要发送。")); return; }
        emit fileSendProgress(0, total);
        emit fileSendStatus(QStringLiteral("正在发送文件…"));
        if (m_fileActive) fillFileQueue();
    });
    connect(m_fileSource, &BleFileSource::chunkReady, this, [this](const QByteArray &bytes, bool eof) {
        if (!m_fileActive) return;
        m_fileEof = eof; m_sendQueue = bytes; pumpWrites();
    });
    connect(m_fileSource, &BleFileSource::failed, this, [this](const QString &error) {
        if (!m_fileActive) return;
        m_lastError = error; finishFileSend(false, error); emit fileSendError(error);
    });
    connect(m_fileSource, &BleFileSource::cleanupFinished, this, &BleHostClient::operationFinished);
    m_deadline.setSingleShot(true);
    m_writeDelay.setSingleShot(true);
    m_writeDelay.setTimerType(Qt::PreciseTimer); // Never fire before the requested minimum interval.
    connect(&m_writeDelay, &QTimer::timeout, this, [this] { pumpWrites(); emit operationFinished(); });
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_state == State::Scanning && m_hello) { stopScan(); return; }
        fail(QStringLiteral("BLE 操作超时，已取消当前会话。请检查设备距离、供电、配对和系统蓝牙状态。"));
    });
}
BleHostClient::~BleHostClient()
{
    // QProcess destruction may wait for a child. Kill only this owned helper,
    // and bound the wait; normal disconnect is fully asynchronous.
    if (m_process) { m_process->disconnect(this); m_process->kill(); m_process->waitForFinished(1000); }
}
QString BleHostClient::hostPath() const
{
    return m_hostPath.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("WaveLinkBleHost.exe")) : m_hostPath;
}
bool BleHostClient::available() const { return QFileInfo(hostPath()).isFile(); }
void BleHostClient::changeState(State state)
{
    if (m_state != state) {
        if (m_phaseTimer.isValid()) {
            if (m_state == State::Connecting) m_connectMs = m_phaseTimer.elapsed();
            if (m_state == State::Discovering) m_discoverMs = m_phaseTimer.elapsed();
            if (m_state == State::Configuring) m_configureMs = m_phaseTimer.elapsed();
        }
        m_phaseTimer.start();
    }
    m_state = state; emit stateChanged(state);
}
QJsonObject BleHostClient::diagnostics() const
{
    return {{"state", int(m_state)}, {"host", m_hostInfo}, {"lastError", m_lastError},
            {"notifications", QString::number(m_notificationCount)}, {"rxBytes", QString::number(m_rxCount)},
            {"txBytes", QString::number(m_txCount)}, {"writePending", !m_writeRequest.isEmpty()},
            {"sendQueuedBytes", m_sendQueue.size() + m_packet.size()}, {"fileBusy", fileSendBusy()},
            {"connectMs", double(m_connectMs)}, {"discoverMs", double(m_discoverMs)}, {"configureMs", double(m_configureMs)}, {"sendSettings", sendSettings()}};
}
QJsonObject BleHostClient::sendSettings() const
{
    return {{"supported", m_sendControlSupported}, {"packetSize", m_requestedPacket}, {"effectivePacket", m_effectivePacket},
            {"intervalMs", m_intervalMs}, {"mtu", m_mtu}, {"packetLimit", BleSendPolicy::limit(m_mtu)},
            {"blocked", m_sendBlocked}, {"pending", !m_settingsRequest.isEmpty()}};
}
QJsonObject BleHostClient::makeCommand(const QString &operation)
{
    return {{QStringLiteral("op"), operation}, {QStringLiteral("session"), m_session},
            {QStringLiteral("request"), QString::number(++m_requestCounter)}};
}
bool BleHostClient::command(QJsonObject object)
{
    if (!m_process || !m_hello || m_process->state() != QProcess::Running) return false;
    const QByteArray bytes = BleIpc::encode(object);
    if (m_process->bytesToWrite() + bytes.size() > 128 * 1024 || m_process->write(bytes) != bytes.size()) {
        fail(QStringLiteral("BLE 辅助程序的发送通道已阻塞或关闭。")); return false;
    }
    return true;
}
void BleHostClient::startHost(const QJsonObject &firstCommand)
{
    if (!available()) { fail(QStringLiteral("找不到 Windows BLE 辅助程序：\n%1\n请先构建并部署 WaveLinkBleHost.exe。").arg(hostPath())); return; }
    m_firstCommand = firstCommand; m_hello = false; m_input.clear();
    auto process = new QProcess(this); m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (m_process == process) readOutput(); else process->readAllStandardOutput();
    });
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
        if (m_process == process && e != QProcess::Crashed) fail(QStringLiteral("无法运行 BLE 辅助程序：%1").arg(process->errorString()));
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus) {
        if (m_process == process) fail(QStringLiteral("BLE 辅助程序意外结束（代码 %1），当前连接已停止。").arg(code));
        process->deleteLater();
    });
    process->start(hostPath(), QStringList());
    m_deadline.start(5000); // Version handshake, separate from device timeouts.
}
void BleHostClient::disconnectDevice()
{
    const bool fileWasActive = m_fileActive;
    m_fileActive = false; m_fileSource->stop(); m_fileWriteRequest.clear();
    m_fileReaderSupported = false;
    m_sendControlSupported = m_sendBlocked = false; m_settingsRequest.clear();
    m_requestedPacket = m_effectivePacket = 20; m_intervalMs = 10; m_mtu = 0; m_capabilityRevision = 0;
    m_deadline.stop(); m_writeDelay.stop();
    m_session = QString::number(++m_sessionCounter);
    m_controlRequest.clear(); m_scanRequest.clear(); m_writeRequest.clear(); m_readRequest.clear();
    m_sendQueue.clear(); m_packet.clear(); m_preReadyData.clear(); m_input.clear(); m_firstCommand = {};
    m_preReadyBytes = 0;
    m_hello = m_canSend = m_canReceive = false;
    auto process = m_process; m_process = nullptr;
    if (process) {
        // Ending the process invalidates outstanding Windows operations whose
        // physical connection attempt cannot be cancelled by the GATT API.
        process->kill();
        if (process->state() == QProcess::NotRunning) process->deleteLater();
    }
    changeState(State::Idle);
    if (fileWasActive) emit fileSendFinished(false, QStringLiteral("BLE 连接结束，文件发送已停止；未完成的数据不会自动重发。"));
}
void BleHostClient::fail(const QString &message)
{ m_lastError = message; disconnectDevice(); emit errorOccurred(message, true); }
void BleHostClient::scan()
{
    if (m_state != State::Idle && m_state != State::Scanning) return;
    disconnectDevice();
    changeState(State::Scanning);
    auto obj = makeCommand(QStringLiteral("scan")); m_scanRequest = obj.value("request").toString();
    startHost(obj);
}
void BleHostClient::stopScan()
{
    if (m_state != State::Scanning) return;
    m_scanRequest.clear(); m_deadline.stop();
    if (m_hello) command(makeCommand(QStringLiteral("stopScan")));
    else { disconnectDevice(); return; }
    changeState(State::Idle);
}
void BleHostClient::connectDevice(const QString &address, int addressType)
{
    disconnectDevice(); resetCounters();
    m_lastError.clear(); m_hostInfo = {}; m_connectMs = m_discoverMs = m_configureMs = -1;
    bool ok = false; const quint64 numericAddress = address.toULongLong(&ok);
    if (!ok || !numericAddress || numericAddress > 0xffffffffffffULL || (addressType != 0 && addressType != 1)) {
        fail(QStringLiteral("BLE 设备地址或地址类型无效，请重新扫描选择设备。")); return;
    }
    changeState(State::Connecting);
    auto obj = makeCommand(QStringLiteral("connect"));
    obj.insert("address", address); obj.insert("addressType", addressType);
    m_controlRequest = obj.value("request").toString(); startHost(obj);
}
bool BleHostClient::configure(int rx, int tx, bool withoutResponse, int packetSize, int intervalMs, bool confirmed)
{
    if (m_state != State::AwaitingConfiguration || readPending() || (!rx && !tx)) return false;
    if (!BleSendPolicy::valid(packetSize, intervalMs) || (BleSendPolicy::needsConfirmation(packetSize, intervalMs) && !confirmed) ||
        (!m_sendControlSupported && (packetSize != 20 || intervalMs != 10))) return false;
    auto obj = makeCommand(QStringLiteral("configure"));
    obj.insert("rx", rx); obj.insert("tx", tx); obj.insert("withoutResponse", withoutResponse);
    obj.insert("packetSize", packetSize); obj.insert("intervalMs", intervalMs); obj.insert("advancedConfirmed", confirmed);
    m_controlRequest = obj.value("request").toString(); changeState(State::Configuring);
    m_deadline.start(15000); return command(obj);
}
bool BleHostClient::applySendSettings(int packetSize, int intervalMs, bool confirmed)
{
    if (m_state != State::Ready || !m_canSend || !m_sendControlSupported || sendBusy() || readPending() ||
        !BleSendPolicy::valid(packetSize, intervalMs) || (BleSendPolicy::needsConfirmation(packetSize, intervalMs) && !confirmed)) return false;
    auto obj = makeCommand(QStringLiteral("sendSettings"));
    obj.insert("packetSize", packetSize); obj.insert("intervalMs", intervalMs); obj.insert("advancedConfirmed", confirmed);
    m_settingsRequest = obj.value("request").toString(); m_deadline.start(5000);
    emit sendSettingsChanged(); return command(obj);
}
void BleHostClient::stopSending(const QString &message)
{
    if (m_sendBlocked) return;
    m_sendBlocked = true; m_sendQueue.clear(); m_writeDelay.stop(); m_lastError = message;
    // Preserve the submitted packet/request for its eventual completion and
    // exact Tx accounting. It cannot be recalled, and is never retried.
    finishFileSend(false, message);
    emit sendingStopped(message); emit operationFinished();
}
bool BleHostClient::consumeSendSettings(const QJsonObject &obj, bool applied)
{
    const auto settings = obj.value("sendSettings").toObject();
    bool ok = false; const auto revision = settings.value("revision").toString().toULongLong(&ok);
    const auto integer = [&](const char *key, int low, int high) {
        const auto v = settings.value(key); const double n = v.toDouble(-1);
        return v.isDouble() && n >= low && n <= high && n == int(n);
    };
    if (!ok || !revision || !integer("mtu", 0, 65535) || !integer("packetSize", 1, 512) ||
        !integer("effectivePacket", 1, 512) || !integer("intervalMs", 0, 1000) || !settings.value("blocked").isBool() ||
        (settings.value("mtu").toInt() > 0 && settings.value("mtu").toInt() < 23)) {
        fail(QStringLiteral("BLE 辅助程序上报了无效的发送能力/参数，当前会话已停止。")); return false;
    }
    if (revision < m_capabilityRevision) {
        if (!applied) return true; // Late capability event, never roll back newer limits.
        const auto message = QStringLiteral("BLE 能力在参数确认期间已变化，请重新核对并应用发送参数。待发数据已停止，接收和录制保持运行。");
        stopSending(message); emit errorOccurred(message, false);
        emit operationFinished(); return false;
    }
    const int packet = settings.value("packetSize").toInt(), effective = settings.value("effectivePacket").toInt();
    const int mtu = settings.value("mtu").toInt(), interval = settings.value("intervalMs").toInt();
    if (effective > packet || (!settings.value("blocked").toBool() && effective > BleSendPolicy::limit(mtu))) {
        fail(QStringLiteral("BLE 实际发送分包超出已知安全上限，当前会话已停止。")); return false;
    }
    m_capabilityRevision = revision; m_mtu = mtu;
    if (applied) { m_requestedPacket = packet; m_effectivePacket = effective; m_intervalMs = interval; m_sendBlocked = settings.value("blocked").toBool(); }
    else if (settings.value("blocked").toBool() || m_effectivePacket > BleSendPolicy::limit(m_mtu))
        stopSending(QStringLiteral("BLE 发送能力降低，当前分包超过新的上限（%1 字节）。已停止待发数据；已提交的一包仍可能到达设备。\n接收和录制保持运行。请在收发配置中重新确认发送参数后再手动发送；软件不会自动重发。").arg(BleSendPolicy::limit(m_mtu)));
    // A capability increase never enlarges packets or speeds up a live queue.
    emit sendSettingsChanged(); return true;
}
bool BleHostClient::readCharacteristic(int handle)
{
    if (m_state != State::AwaitingConfiguration || readPending()) return false;
    auto obj = makeCommand(QStringLiteral("read")); obj.insert("handle", handle);
    m_readRequest = obj.value("request").toString(); m_deadline.start(10000);
    return command(obj);
}
bool BleHostClient::sendData(const QByteArray &data)
{
    if (m_state != State::Ready || !canSend() || m_fileActive || !m_fileWriteRequest.isEmpty() || data.isEmpty()) return false;
    if (m_sendQueue.size() + m_packet.size() + data.size() > 256 * 1024) return false;
    m_sendQueue += data; pumpWrites(); return true;
}
bool BleHostClient::startFileSend(const QString &path, QString &error)
{
    if (m_state != State::Ready || !canSend() || fileSendBusy() || !m_sendQueue.isEmpty() || !m_packet.isEmpty()) {
        error = QStringLiteral("连接尚未就绪、没有发送特征或仍有发送数据未完成，请稍后再试。"); return false;
    }
    if (!m_fileReaderSupported) { error = QStringLiteral("BLE 辅助程序不支持安全后台文件读取，请更新完整软件包。"); return false; }
    m_fileTotal = -1; m_fileCompleted = 0; m_fileEof = false; m_fileActive = true;
    if (!m_fileSource->start(hostPath(), path, m_session, QString::number(++m_transferCounter), error)) {
        m_fileActive = false; return false;
    }
    emit fileSendProgress(0, -1);
    emit fileSendStatus(QStringLiteral("正在准备文件…")); return true;
}
bool BleHostClient::fileSendBusy() const
{ return m_fileActive || !m_fileWriteRequest.isEmpty() || m_fileSource->cleaningUp(); }
void BleHostClient::finishFileSend(bool success, const QString &message)
{
    if (!m_fileActive) return;
    m_fileActive = false; m_fileSource->stop(); m_sendQueue.clear();
    emit fileSendFinished(success, message);
}
void BleHostClient::cancelFileSend()
{
    // The one submitted GATT packet cannot be recalled. Keep its request alive
    // for accounting, but discard all unsent file data and never replay it.
    finishFileSend(false, QStringLiteral("文件发送已取消；已提交的最后一包可能仍会到达设备。"));
}
void BleHostClient::fillFileQueue()
{
    if (!m_fileActive || m_fileTotal < 0 || m_fileSource->readPending() || !m_packet.isEmpty() || !m_sendQueue.isEmpty()) return;
    if (m_fileCompleted == m_fileTotal && m_fileEof) {
        finishFileSend(true, QStringLiteral("文件的全部 GATT 写入已完成/提交；不代表设备业务程序已经确认接收。")); return;
    }
    if (!m_fileSource->requestChunk(m_fileCompleted, int(qMin<qint64>(4096, m_fileTotal - m_fileCompleted))) && m_fileActive) {
        const auto message = QStringLiteral("后台文件读取请求无效，文件发送已停止。");
        finishFileSend(false, message); emit fileSendError(message);
    }
}
bool BleHostClient::decodeNotification(const QJsonObject &obj, QByteArray &data, qint64 &timestampNs)
{
    bool ok = false;
    const qint64 ticks = obj.value("timestamp100ns").toString().toLongLong(&ok);
    // WinRT DateTime counts 100 ns since 1601-01-01; WLR/CSV use Unix ns.
    const qint64 unixTicks = ok && ticks >= 116444736000000000LL ? ticks - 116444736000000000LL : -1;
    if (unixTicks < 0 || unixTicks > std::numeric_limits<qint64>::max() / 100) {
        fail(QStringLiteral("BLE 通知缺少有效的原始接收时间，已停止当前会话。")); return false;
    }
    timestampNs = unixTicks * 100;
    const auto encoded = obj.value("data").toString().toLatin1();
    data = QByteArray::fromBase64(encoded);
    if (!obj.value("data").isString() || data.toBase64() != encoded) {
        fail(QStringLiteral("BLE 通知内容编码无效，已停止当前会话。")); return false;
    }
    return true;
}
void BleHostClient::deliverNotification(const QByteArray &data, qint64 timestampNs)
{
    ++m_notificationCount;
    m_rxCount += quint64(data.size());
    emit notificationReceived(data, timestampNs);
    if (m_state == State::Ready) emit dataReceived(data);
}
void BleHostClient::pumpWrites()
{
    if (m_state != State::Ready || !canSend() || !m_packet.isEmpty() || m_sendQueue.isEmpty() || m_writeDelay.isActive()) return;
    m_packet = m_sendQueue.left(m_effectivePacket); m_sendQueue.remove(0, m_packet.size());
    auto obj = makeCommand(QStringLiteral("write")); obj.insert("data", QString::fromLatin1(m_packet.toBase64()));
    m_writeRequest = obj.value("request").toString(); m_deadline.start(10000); command(obj);
    if (m_fileActive && !m_packet.isEmpty()) {
        m_fileWriteRequest = m_writeRequest;
        if (m_fileEof && m_sendQueue.isEmpty()) emit fileSendStatus(QStringLiteral("等待最后一包确认…"));
    }
}
void BleHostClient::readOutput()
{
    // Consume in bounded pieces even if the OS pipe contains a burst.
    auto process = m_process;
    if (process->bytesAvailable() > 4 * 1024 * 1024) {
        fail(QStringLiteral("BLE 接收积压超过安全上限，当前会话已停止。")); return;
    }
    QElapsedTimer budget; budget.start();
    while (m_process == process && process->bytesAvailable() > 0) {
        m_input += process->read(qMin<qint64>(process->bytesAvailable(), 64 * 1024));
        QJsonObject obj; QString error;
        while (BleIpc::take(m_input, obj, error)) {
            consume(obj);
            if (m_process != process) return;
        }
        if (!error.isEmpty() || m_input.size() > int(BleIpc::MaxPayload) + 4) {
            fail(error.isEmpty() ? QStringLiteral("BLE 接收消息超过大小限制。") : error); return;
        }
        if (budget.elapsed() >= 8 && process->bytesAvailable() > 0) {
            QTimer::singleShot(0, this, [this, process] { if (m_process == process) readOutput(); }); return;
        }
    }
}
void BleHostClient::consume(const QJsonObject &obj)
{
    const QString type = obj.value("event").toString();
    if (type == "hello") {
        if (m_hello || obj.value("version").toInt() != 1) { fail(QStringLiteral("BLE 辅助程序版本不匹配。")); return; }
        if (obj.contains("role") && obj.value("role").toString() != "ble") { fail(QStringLiteral("BLE 辅助程序角色不匹配。")); return; }
        m_fileReaderSupported = obj.value("fileReaderVersion").toInt() == 1;
        m_sendControlSupported = obj.value("sendControlVersion").isDouble() && obj.value("sendControlVersion").toDouble() == 1;
        m_hostInfo = {{"version", obj.value("version")}, {"role", obj.value("role")}, {"fileReaderVersion", obj.value("fileReaderVersion")}, {"sendControlVersion", obj.value("sendControlVersion")}};
        m_hello = true; m_deadline.start(m_state == State::Scanning ? 10000 : 45000);
        const auto first = m_firstCommand; m_firstCommand = {}; command(first); return;
    }
    if (obj.value("session").toString() != m_session) return;
    const QString request = obj.value("request").toString();
    if (type == "error") {
        if (request != "0" && request != m_controlRequest && request != m_scanRequest && request != m_writeRequest && request != m_readRequest && request != m_settingsRequest) return;
        const QString text = QStringLiteral("阶段：%1\n系统错误代码：%2\n%3")
            .arg(obj.value("phase").toString()).arg(obj.value("code").toInt()).arg(obj.value("message").toString());
        if (obj.value("fatal").toBool(true)) fail(text);
        else if (request == m_settingsRequest && !m_settingsRequest.isEmpty()) {
            m_lastError = text; m_settingsRequest.clear(); m_deadline.stop(); emit errorOccurred(text, false); emit sendSettingsChanged(); emit operationFinished();
        } else if (request == m_readRequest && readPending()) {
            m_lastError = text; m_readRequest.clear(); m_deadline.stop(); emit errorOccurred(text, false); emit operationFinished();
        } // Unrelated informational errors must not cancel an outstanding write deadline.
    } else if (type == "device" && m_state == State::Scanning && request == m_scanRequest) {
        emit deviceFound(obj);
    } else if (type == "discovering" && request == m_controlRequest && m_state == State::Connecting) {
        changeState(State::Discovering); m_deadline.start(45000);
    } else if (type == "services" && request == m_controlRequest && m_state == State::Discovering) {
        m_deadline.stop(); changeState(State::AwaitingConfiguration);
        emit characteristicsDiscovered(obj.value("characteristics").toArray());
    } else if (type == "ready" && request == m_controlRequest && m_state == State::Configuring) {
        if (m_sendControlSupported && !consumeSendSettings(obj, true)) return;
        m_deadline.stop(); m_canSend = obj.value("canSend").toBool(); m_canReceive = obj.value("canReceive").toBool();
        changeState(State::Ready); emit ready();
        if (m_state == State::Ready && m_canReceive && !m_preReadyData.isEmpty()) {
            const auto notifications = m_preReadyData; m_preReadyData.clear(); m_preReadyBytes = 0;
            for (const auto &batch : notifications) {
                if (m_state != State::Ready) break;
                deliverNotification(batch.data, batch.timestampNs);
            }
        }
    } else if (type == "data" && m_state == State::Configuring) {
        QByteArray bytes; qint64 timestampNs;
        if (!decodeNotification(obj, bytes, timestampNs)) return;
        if (m_preReadyBytes + bytes.size() > 256 * 1024 || m_preReadyData.size() >= 8192) {
            fail(QStringLiteral("BLE 订阅确认前的数据积压超过安全上限。")); return;
        }
        m_preReadyData.enqueue({bytes, timestampNs}); m_preReadyBytes += bytes.size();
    } else if (type == "data" && m_state == State::Ready && m_canReceive) {
        QByteArray bytes; qint64 timestampNs;
        if (decodeNotification(obj, bytes, timestampNs)) deliverNotification(bytes, timestampNs);
    } else if (type == "written" && request == m_writeRequest && !m_packet.isEmpty() && m_state == State::Ready) {
        if (obj.value("bytes").toInt() != m_packet.size()) { fail(QStringLiteral("BLE 写入完成长度与发送长度不一致。")); return; }
        const int bytes = m_packet.size();
        const bool filePacket = !m_fileWriteRequest.isEmpty() && request == m_fileWriteRequest;
        m_fileWriteRequest.clear();
        m_deadline.stop(); m_txCount += quint64(bytes); m_packet.clear(); m_writeRequest.clear();
        m_writeDelay.start(m_intervalMs); // Even 0 ms yields; at most one GATT write is in flight.
        if (m_fileActive && filePacket) {
            m_fileCompleted += bytes; emit fileSendProgress(m_fileCompleted, m_fileTotal);
            if (m_state == State::Ready) fillFileQueue();
        }
        if (filePacket && !m_fileActive) emit operationFinished();
    } else if (type == "capabilities" && m_sendControlSupported && (m_state == State::Ready || m_state == State::Configuring)) {
        consumeSendSettings(obj, false);
    } else if (type == "sendSettingsApplied" && request == m_settingsRequest && !m_settingsRequest.isEmpty() && m_state == State::Ready) {
        m_settingsRequest.clear(); m_deadline.stop();
        if (consumeSendSettings(obj, true)) { emit sendSettingsApplied(); emit operationFinished(); }
    } else if (type == "writeRejected" && request == m_writeRequest && !m_packet.isEmpty() && m_state == State::Ready) {
        // The helper rejected this packet before submitting it to GATT.
        m_deadline.stop(); m_packet.clear(); m_writeRequest.clear(); m_fileWriteRequest.clear();
        if (m_sendControlSupported && !consumeSendSettings(obj, false)) return;
        stopSending(QStringLiteral("BLE 分包未提交：发送能力已变化。待发数据已停止，请重新确认发送参数。接收和录制不受影响；不会自动重发。"));
        emit operationFinished();
    } else if (type == "read" && request == m_readRequest && readPending()) {
        m_deadline.stop(); m_readRequest.clear();
        emit characteristicRead(obj.value("handle").toInt(), QByteArray::fromBase64(obj.value("data").toString().toLatin1()));
        emit operationFinished();
    }
}
