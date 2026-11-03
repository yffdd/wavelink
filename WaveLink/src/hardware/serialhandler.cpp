#include "serialhandler.h"
#include "ProtocolParserFactory.h"
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QSignalBlocker>
#include <QMutexLocker>
#include <QScopedValueRollback>
#include "serialcontrolstate_win.h"

SerialHandler::SerialHandler(QObject *parent, QSerialPort *serialPort) : QObject(parent), m_parser(nullptr), loggedBytes(0)
{
    m_wlrRecorder.setErrorHandler([this](const QString &error) { emit recordingError(error); });
    m_receiveEpochNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    m_receiveClock.start();
    m_serialPort = serialPort ? serialPort : new QSerialPort(this);
    m_serialPort->setParent(this);
    // Qt 5.14 defaults to restoring the OLD DCB at close, including DTR/RTS.
    // That can undo the user's released state and pulse a board's reset circuit.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_serialPort->setSettingsRestoredOnClose(false);
    QT_WARNING_POP
    m_breakTimer = new QTimer(this);
    m_breakTimer->setSingleShot(true);
    m_breakTimer->setTimerType(Qt::PreciseTimer);
    connect(m_breakTimer, &QTimer::timeout, this, [this]() {
        if (m_breakActive && m_breakPulse) setBreakState(m_activeRequestId, false, 0);
    });
    m_sendTimer = new QTimer(this);
    m_sendTimer->setInterval(200);
    connect(m_sendTimer, &QTimer::timeout, this, [this]() { pumpSendQueue(); reportSendStatus(); });
    connect(m_serialPort, &QSerialPort::bytesWritten, this, [this](qint64 bytes) {
        if (!m_activeRequestId || m_inFlightSendBytes <= 0 || bytes <= 0) return;
        const qint64 completed = qMin(bytes, m_inFlightSendBytes);
        m_inFlightSendBytes -= completed;
        m_txCount.fetch_add(completed);
        const bool completesRequest = !m_inFlightSendBytes && m_currentSendCompletesRequest;
        if (completesRequest) m_currentSendCompletesRequest = false;
        quint64 generation;
        { QMutexLocker lock(&m_sendMutex); generation = m_sendGeneration; }
        releaseSendBytes(generation, completed, completesRequest);
        if (!m_inFlightSendBytes) pumpSendQueue();
        reportSendStatus();
    });
    
    connect(m_serialPort, &QSerialPort::readyRead, this, &SerialHandler::handleReadyRead);
    connect(m_serialPort, &QSerialPort::errorOccurred, this, &SerialHandler::handleError);
    
    setProtocol(1); // Default to FireWater
}

SerialHandler::~SerialHandler()
{
    closePortSafely();
    if (m_parser) {
        m_parser->deleteLater();
    }
}

void SerialHandler::setProtocol(int index)
{
    applyProtocol(0, DataProtocols::ProtocolSelection::fromLegacy(index), {});
}
void SerialHandler::applyProtocol(quint64 request, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile)
{
    if (m_recorder.isActive() || m_wlrRecorder.isActive() || pendingSendBytes() > 0 || m_isOpening || m_isClosing) {
        emit protocolApplyFinished(request, false, QStringLiteral("录制、发送或连接操作期间不能切换协议。")); return;
    }
    QString error;
    auto candidate = DataProtocols::ProtocolParserFactory::create(selection, profile, this, error);
    if (!candidate) { emit protocolApplyFinished(request, false, error); return; }
    // Build first, then replace at the worker's event boundary. A failed
    // validation never destroys the old parser, partial packet or display data.
    delete m_parser; m_parser = candidate; m_plotFrames.clear(); m_buffer.clear();
    m_statusMonitor->bindProtocol(m_parser, selection, profile);
    connect(m_parser, &ProtocolParser::textReady, this, &SerialHandler::forwardTextReady);
    connect(m_parser, &ProtocolParser::channelsUpdated, this, &SerialHandler::forwardChannelsUpdated);
    connect(m_parser, &ProtocolParser::recordingFrameReady, this, &SerialHandler::writeLogData);
    emit protocolApplyFinished(request, true, QString());
}

void SerialHandler::setEndianness(bool bigEndian)
{
    if (m_recorder.isActive() || m_wlrRecorder.isActive()) return;
    if (JustFloatParser *p = qobject_cast<JustFloatParser*>(m_parser)) {
        p->setBigEndian(bigEndian);
    }
}

void SerialHandler::openPort(const QString &portName, int baudRate, int dataBits, const QString &parityStr,
                             const QString &stopBitsStr, quint64 requestId, bool dtr, bool rts, int flowControl)
{
    qDebug() << "SerialHandler::openPort START";
    closePortSafely();

    m_requestedDtr = dtr;
    m_requestedRts = rts;
    m_isOpening = true;
    m_openError = QSerialPort::NoError;
    m_openErrorString.clear();
    
    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(baudRate);
    
    // Set Data Bits
    if (dataBits == 5) m_serialPort->setDataBits(QSerialPort::Data5);
    else if (dataBits == 6) m_serialPort->setDataBits(QSerialPort::Data6);
    else if (dataBits == 7) m_serialPort->setDataBits(QSerialPort::Data7);
    else m_serialPort->setDataBits(QSerialPort::Data8);
    
    // Set Parity
    if (parityStr == "Even") m_serialPort->setParity(QSerialPort::EvenParity);
    else if (parityStr == "Odd") m_serialPort->setParity(QSerialPort::OddParity);
    else if (parityStr == "Space") m_serialPort->setParity(QSerialPort::SpaceParity);
    else if (parityStr == "Mark") m_serialPort->setParity(QSerialPort::MarkParity);
    else m_serialPort->setParity(QSerialPort::NoParity);
    
    // Set Stop Bits
    if (stopBitsStr == "1.5") m_serialPort->setStopBits(QSerialPort::OneAndHalfStop);
    else if (stopBitsStr == "2") m_serialPort->setStopBits(QSerialPort::TwoStop);
    else m_serialPort->setStopBits(QSerialPort::OneStop);
    
    const bool flowValid = flowControl == 0 || flowControl == 1;
    m_hardwareFlow = flowControl == 1;
    const bool configured = flowValid && configureFlowControl(m_hardwareFlow
        ? QSerialPort::HardwareControl : QSerialPort::NoFlowControl);
    if (!configured || m_openError != QSerialPort::NoError) {
        const auto error = m_openError == QSerialPort::NoError ? QSerialPort::UnsupportedOperationError : m_openError;
        const QString detail = m_openErrorString.isEmpty() ? QStringLiteral("无法应用所选流控配置；请检查串口驱动和设备支持情况。") : m_openErrorString;
        const QString message = buildErrorMessage(error, detail);
        closePortSafely();
        emit portOpenFinished(requestId, false, message, static_cast<int>(error));
        return;
    }
    
    qDebug() << "SerialHandler::openPort: Calling m_serialPort->open(ReadWrite)";
    const bool success = m_serialPort->open(QIODevice::ReadWrite);
    qDebug() << "SerialHandler::openPort: m_serialPort->open returned" << success;
    const QSerialPort::SerialPortError openError =
        m_openError != QSerialPort::NoError ? m_openError : m_serialPort->error();
    const QString openErrorString = !m_openErrorString.isEmpty()
        ? m_openErrorString
        : m_serialPort->errorString();
    m_isOpening = false;

    if (success && openError == QSerialPort::NoError) {
        m_buffer.clear();
        if (m_parser) m_parser->reset();
        m_statusMonitor->setConnected(true);
        m_activeRequestId = requestId;
        { QMutexLocker lock(&m_sendMutex); m_sendSession = requestId; }
        // QSerialPort setters require an open handle. Apply control lines and
        // release Break BEFORE publishing ready, then report states AFTER ready
        // so queued GUI handlers can consume them in the correct session.
        QString controlWarning, breakWarning;
        QSerialPort::SerialPortError controlError = QSerialPort::NoError;
        if (!configureControlLines(dtr, rts, controlWarning, controlError) ||
            !initializeBreak(breakWarning, controlError)) {
            const auto message = !breakWarning.isEmpty() ? breakWarning : controlWarning;
            closePortSafely();
            emit portOpenFinished(requestId, false, message, static_cast<int>(controlError));
            return;
        }
        qDebug() << "SerialHandler::openPort END (success)";
        emit portOpenFinished(requestId, true, QString(), static_cast<int>(QSerialPort::NoError));
        emit controlLinesFinished(requestId, m_dtrState, m_rtsState, controlWarning);
        emit breakStateChanged(requestId, false, false, m_breakSupported, breakWarning);
        reportSendStatus(true);
    } else {
        const QString message = buildErrorMessage(openError, openErrorString);
        qDebug() << "SerialHandler::openPort END (error:" << message << ")";
        closePortSafely();
        emit portOpenFinished(requestId, false, message, static_cast<int>(openError));
    }
}

bool SerialHandler::applyControlLine(bool isDtr, bool enabled)
{
    return isDtr ? m_serialPort->setDataTerminalReady(enabled)
                 : m_serialPort->setRequestToSend(enabled);
}

bool SerialHandler::supportsCombinedControlLines() const
{
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(m_serialPort->handle());
    return handle != INVALID_HANDLE_VALUE && handle != nullptr;
#else
    return false;
#endif
}

bool SerialHandler::applyControlLinesTogether(bool dtr, bool rts, bool hardwareFlow,
                                            QSerialPort::SerialPortError &error, QString &message)
{
    error = QSerialPort::NoError; message.clear();
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(m_serialPort->handle());
    const auto failed = [&](DWORD code) {
        if (code == ERROR_ACCESS_DENIED) error = QSerialPort::PermissionError;
        else if (code == ERROR_INVALID_HANDLE || code == ERROR_DEVICE_NOT_CONNECTED || code == ERROR_GEN_FAILURE)
            error = QSerialPort::ResourceError;
        else if (code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION)
            error = QSerialPort::UnsupportedOperationError;
        else error = QSerialPort::UnknownError;
        wchar_t *text = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                           nullptr, code, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
        const auto detail = length && text ? QString::fromWCharArray(text, int(length)).trimmed() : QString();
        if (text) LocalFree(text);
        message = QStringLiteral("Windows 错误 %1：%2").arg(code).arg(detail);
        return false;
    };
    DCB state{}; state.DCBlength = sizeof(state);
    if (!GetCommState(handle, &state)) return failed(GetLastError());
    if (!SerialControlState::update(state, dtr, rts, hardwareFlow)) return true;
    // Do not fall back to sequential EscapeCommFunction calls on failure:
    // an unconfirmed pair must not introduce another transient reset sequence.
    if (!SetCommState(handle, &state)) return failed(GetLastError());
    DCB confirmed{}; confirmed.DCBlength = sizeof(confirmed);
    if (!GetCommState(handle, &confirmed)) return failed(GetLastError());
    if (SerialControlState::update(confirmed, dtr, rts, hardwareFlow)) {
        error = QSerialPort::UnknownError;
        message = QStringLiteral("驱动未确认 DTR/RTS 的请求状态。");
        return false;
    }
    return true;
#else
    Q_UNUSED(dtr) Q_UNUSED(rts) Q_UNUSED(hardwareFlow)
    error = QSerialPort::UnsupportedOperationError;
    message = QStringLiteral("当前平台不支持合并设置 DTR/RTS。");
    return false;
#endif
}

bool SerialHandler::configureFlowControl(QSerialPort::FlowControl flow)
{
    return m_serialPort->setFlowControl(flow);
}

int SerialHandler::readCtsState()
{
    return m_serialPort->pinoutSignals().testFlag(QSerialPort::ClearToSendSignal) ? 1 : 0;
}

void SerialHandler::setControlLines(quint64 requestId, bool dtr, bool rts)
{
    // Discard delayed requests after disconnect/reconnect. All native calls
    // run in the serial worker, never in the GUI thread.
    if (!requestId || requestId != m_activeRequestId || m_isClosing ||
        m_isOpening || !m_serialPort->isOpen()) return;

    QString warning; QSerialPort::SerialPortError error;
    if (!configureControlLines(dtr, rts, warning, error)) {
        closePortSafely(); emit errorOccurred(warning, static_cast<int>(error)); return;
    }
    emit controlLinesFinished(requestId, m_dtrState, m_rtsState, warning);
}

bool SerialHandler::configureControlLines(bool dtr, bool rts, QString &warning, QSerialPort::SerialPortError &error)
{
    warning.clear(); error = QSerialPort::NoError;
    if (supportsCombinedControlLines()) {
        const bool changed = !(m_controlLinesAttempted & 1) || m_requestedDtr != dtr ||
            (!m_hardwareFlow && (!(m_controlLinesAttempted & 2) || m_requestedRts != rts));
        if (!changed) return true;
        m_requestedDtr = dtr; m_requestedRts = rts; m_controlLinesAttempted = 3;
        const bool applied = applyControlLinesTogether(dtr, rts, m_hardwareFlow, error, warning);
        if (!applied && error != QSerialPort::NoError && error != QSerialPort::UnsupportedOperationError && error != QSerialPort::UnknownError) {
            warning = buildErrorMessage(error, warning); return false;
        }
        m_dtrState = applied ? (dtr ? 1 : 0) : -1;
        m_rtsState = m_hardwareFlow ? -2 : applied ? (rts ? 1 : 0) : -1;
        if (!applied) warning = QStringLiteral("DTR/RTS 合并设置未确认：%1").arg(warning);
        return true;
    }

    QStringList warnings;
    QSerialPort::SerialPortError fatalError = QSerialPort::NoError;
    QString fatalMessage;
    for (int line = 0; line < 2; ++line) {
        const bool isDtr = line == 0;
        if (!isDtr && m_hardwareFlow) {
            m_rtsState = -2; // Driver-managed RTS, not an unsupported manual line.
            continue;
        }
        const bool requested = isDtr ? dtr : rts;
        bool &previousRequest = isDtr ? m_requestedDtr : m_requestedRts;
        int &state = isDtr ? m_dtrState : m_rtsState;
        const int mask = 1 << line;
        // Changing RTS must not pulse DTR (and vice versa). An unsupported
        // line is not silently retried when the other checkbox changes.
        if ((m_controlLinesAttempted & mask) && previousRequest == requested) continue;
        previousRequest = requested;
        m_controlLinesAttempted |= mask;
        m_controlLineError = QSerialPort::NoError;
        m_controlLineErrorString.clear();
        m_isSettingControlLines = true;
        const bool applied = applyControlLine(isDtr, requested);
        m_isSettingControlLines = false;

        const auto error = m_controlLineError;
        // Unsupported control signals do not imply failed data communication.
        // Real I/O, permission and disconnection errors retain normal recovery.
        if (error != QSerialPort::NoError &&
            error != QSerialPort::UnsupportedOperationError &&
            error != QSerialPort::UnknownError) {
            fatalError = error;
            fatalMessage = buildErrorMessage(error, m_controlLineErrorString);
            break;
        }
        if (applied) {
            state = requested ? 1 : 0;
        } else {
            // A driver may fail after changing the native line. Do not claim
            // that either the old or new state was successfully established.
            state = -1;
            const QString detail = m_controlLineErrorString.isEmpty()
                ? QStringLiteral("驱动未确认此控制信号的设置") : m_controlLineErrorString;
            warnings << QStringLiteral("%1 设置失败：%2").arg(isDtr ? "DTR" : "RTS", detail);
        }
        m_serialPort->clearError();
    }
    if (fatalError != QSerialPort::NoError) {
        error = fatalError; warning = fatalMessage; return false;
    }
    warning = warnings.join('\n'); return true;
}

void SerialHandler::closePort()
{
    qDebug() << "SerialHandler::closePort START";
    closePortSafely();
    qDebug() << "SerialHandler::closePort END";
}

bool SerialHandler::applyBreakLine(bool enabled)
{
    return m_serialPort->setBreakEnabled(enabled);
}

bool SerialHandler::tryApplyBreak(bool enabled, QSerialPort::SerialPortError &error, QString &message)
{
    // Reuse the synchronous-setter error capture; do not recurse into close()
    // while a native Break call is still executing.
    m_controlLineError = QSerialPort::NoError;
    m_controlLineErrorString.clear();
    m_isSettingControlLines = true;
    const bool applied = applyBreakLine(enabled);
    m_isSettingControlLines = false;
    error = m_controlLineError;
    message = m_controlLineErrorString;
    m_serialPort->clearError();
    return applied && error == QSerialPort::NoError;
}

bool SerialHandler::initializeBreak(QString &warning, QSerialPort::SerialPortError &error)
{
    QString detail;
    m_breakSupported = tryApplyBreak(false, error, detail);
    if (!m_breakSupported && error != QSerialPort::NoError &&
        error != QSerialPort::UnsupportedOperationError && error != QSerialPort::UnknownError) {
        warning = buildErrorMessage(error, detail); return false;
    }
    // Startup/reconnection never restores an asserted Break, even if old
    // settings or an imported configuration contain a Break-enabled value.
    warning = m_breakSupported ? QString() : QStringLiteral(
        "驱动未确认 Break 控制，本次连接中已禁用此功能；软件不会自动开启 Break。\n%1").arg(detail);
    return true;
}

void SerialHandler::setBreakState(quint64 requestId, bool enabled, int durationMs)
{
    if (!requestId || requestId != m_activeRequestId || m_isOpening || m_isClosing || !m_serialPort->isOpen()) return;
    const auto report = [this, requestId](const QString &warning) {
        emit breakStateChanged(requestId, m_breakActive, m_breakPulse, m_breakSupported, warning);
    };
    if (durationMs < 0 || durationMs > 5000 || (enabled && !m_breakSupported)) {
        report(QStringLiteral("Break 未开启：持续时间无效或驱动不支持此功能。")); return;
    }
    if (enabled && (m_breakActive || pendingSendBytes() > 0 || m_serialPort->bytesToWrite() > 0)) {
        report(QStringLiteral("Break 未开启：已有 Break 正在执行，或串口仍有待发送数据。请等待发送完成后重试。")); return;
    }
    if (!enabled && !m_breakMayBeActive) { report(QString()); return; }
    m_breakTimer->stop();
    QSerialPort::SerialPortError error;
    QString detail;
    // A failed native request might have changed the line. Until release has
    // succeeded, ordinary writes must remain blocked and cleanup must try it.
    if (enabled) m_breakMayBeActive = true;
    if (tryApplyBreak(enabled, error, detail)) {
        m_breakActive = enabled;
        m_breakMayBeActive = enabled;
        m_breakPulse = enabled && durationMs > 0;
        if (m_breakPulse) m_breakTimer->start(durationMs);
        report(QString()); return;
    }
    if (enabled) {
        QSerialPort::SerialPortError releaseError;
        QString releaseDetail;
        const bool released = tryApplyBreak(false, releaseError, releaseDetail);
        if (released && (error == QSerialPort::NoError || error == QSerialPort::UnsupportedOperationError || error == QSerialPort::UnknownError)) {
            m_breakActive = m_breakMayBeActive = m_breakPulse = false;
            m_breakSupported = false;
            report(QStringLiteral("Break 开启失败，已确认释放，普通发送仍可使用。本次连接中已禁用 Break。\n%1").arg(detail)); return;
        }
        if (!released) { error = releaseError; detail += QStringLiteral("\n释放失败：%1").arg(releaseDetail); }
    }
    // An unconfirmed release cannot be treated as healthy/ready for sending.
    // Close the handle, stop recording through the established error path,
    // and never automatically resume or replay unsent commands.
    const QString message = QStringLiteral("Break 操作失败或无法确认释放，已安全关闭串口。\n%1")
        .arg(buildErrorMessage(error == QSerialPort::NoError ? QSerialPort::UnknownError : error, detail));
    closePortSafely();
    emit errorOccurred(message, static_cast<int>(error == QSerialPort::NoError ? QSerialPort::UnknownError : error));
}

void SerialHandler::closePortSafely()
{
    if (!m_serialPort || m_isClosing) return;

    m_isClosing = true;
    m_statusMonitor->setConnected(false);
    m_breakTimer->stop();
    invalidateSendQueue(true);
    m_activeRequestId = 0;
    m_controlLinesAttempted = 0;
    m_dtrState = m_rtsState = -1;
    {
        // Closing/clearing a failed Windows COM handle may emit another
        // errorOccurred signal. Block it so error recovery cannot recurse.
        const QSignalBlocker blocker(m_serialPort);
        if (m_serialPort->isOpen()) {
            if (m_breakMayBeActive) applyBreakLine(false);
            if (supportsCombinedControlLines()) {
                QSerialPort::SerialPortError error; QString warning;
                if (!applyControlLinesTogether(m_requestedDtr, m_requestedRts, m_hardwareFlow, error, warning))
                    qWarning() << "Unable to confirm DTR/RTS before closing:" << warning;
            }
            m_serialPort->close();
        }
        m_serialPort->clearError();
    }
    m_breakActive = m_breakMayBeActive = m_breakPulse = m_breakSupported = false;
    m_hardwareFlow = false; m_ctsState = -1;
    stopRecording();
    m_buffer.clear();
    m_isOpening = false;
    m_isClosing = false;
}

QString SerialHandler::buildErrorMessage(QSerialPort::SerialPortError error, const QString &systemMessage) const
{
    QString reason;
    switch (error) {
    case QSerialPort::DeviceNotFoundError:
        reason = QStringLiteral("设备不存在或已经断开连接");
        break;
    case QSerialPort::PermissionError:
        reason = QStringLiteral("访问被拒绝，串口可能正被其他程序占用或驱动尚未释放");
        break;
    case QSerialPort::OpenError:
        reason = QStringLiteral("串口已被占用，或当前进程无法再次打开该设备");
        break;
    case QSerialPort::ParityError:
        reason = QStringLiteral("接收到的数据发生校验位错误");
        break;
    case QSerialPort::FramingError:
        reason = QStringLiteral("接收到的数据发生帧格式错误");
        break;
    case QSerialPort::BreakConditionError:
        reason = QStringLiteral("串口检测到中断条件");
        break;
    case QSerialPort::ReadError:
        reason = QStringLiteral("读取串口数据失败");
        break;
    case QSerialPort::WriteError:
        reason = QStringLiteral("写入串口数据失败");
        break;
    case QSerialPort::ResourceError:
        reason = QStringLiteral("设备资源失效，设备可能被拔出、复位或重新枚举");
        break;
    case QSerialPort::UnsupportedOperationError:
        reason = QStringLiteral("当前串口或驱动不支持所请求的配置");
        break;
    case QSerialPort::TimeoutError:
        reason = QStringLiteral("串口操作超时");
        break;
    case QSerialPort::NotOpenError:
        reason = QStringLiteral("串口尚未打开");
        break;
    default:
        reason = QStringLiteral("发生未知串口错误");
        break;
    }

    const QString port = m_serialPort && !m_serialPort->portName().isEmpty()
        ? m_serialPort->portName()
        : QStringLiteral("未知端口");
    const QString nativeMessage = systemMessage.trimmed().isEmpty()
        ? QStringLiteral("系统未提供更多信息")
        : systemMessage.trimmed();
    return QStringLiteral("串口 %1：%2。\n系统信息：%3")
        .arg(port, reason, nativeMessage);
}

void SerialHandler::sendData(const QByteArray &data)
{
    quint64 generation;
    if (reserveSend(m_activeRequestId, data.size(), generation)) acceptQueuedSend(m_activeRequestId, generation, data);
}

qint64 SerialHandler::pendingSendBytes() const
{
    QMutexLocker lock(&m_sendMutex); return m_reservedSendBytes;
}

bool SerialHandler::sendSessionReady(quint64 requestId) const
{
    QMutexLocker lock(&m_sendMutex); return requestId && requestId == m_sendSession;
}

bool SerialHandler::reserveSend(quint64 requestId, qint64 size, quint64 &generation)
{
    bool full = false;
    {
        QMutexLocker lock(&m_sendMutex);
        if (!size || !requestId || requestId != m_sendSession) return false;
        full = size > SendQueueLimit - m_reservedSendBytes || m_reservedSendRequests >= SendRequestLimit;
        if (!full) { m_reservedSendBytes += size; ++m_reservedSendRequests; generation = m_sendGeneration; }
    }
    if (full) emit sendRejected(requestId, QStringLiteral("未发送：串口待发送队列已达到上限（256 KB 或 1024 条发送请求）。请等待设备就绪或取消待发送数据；命令循环已停止，本次命令不会自动重发。"));
    return !full;
}

bool SerialHandler::enqueueData(quint64 requestId, const QByteArray &data)
{
    quint64 generation;
    if (!reserveSend(requestId, data.size(), generation)) return false;
    const bool submitted = QMetaObject::invokeMethod(this, [this, requestId, generation, data]() {
        acceptQueuedSend(requestId, generation, data);
    }, Qt::QueuedConnection);
    if (!submitted) releaseSendBytes(generation, data.size(), true);
    return submitted;
}

void SerialHandler::releaseSendBytes(quint64 generation, qint64 count, bool completesRequest)
{
    QMutexLocker lock(&m_sendMutex);
    if (generation == m_sendGeneration) {
        m_reservedSendBytes = qMax<qint64>(0, m_reservedSendBytes - count);
        if (completesRequest) m_reservedSendRequests = qMax(0, m_reservedSendRequests - 1);
    }
}

void SerialHandler::acceptQueuedSend(quint64 requestId, quint64 generation, const QByteArray &data)
{
    { QMutexLocker lock(&m_sendMutex);
      if (requestId != m_sendSession || generation != m_sendGeneration) return; }
    if (m_breakMayBeActive) {
        releaseSendBytes(generation, data.size(), true);
        emit sendRejected(requestId, QStringLiteral("未发送：Break 正在保持发送线。请先释放 Break，再手动重新发送。"));
        return;
    }
    m_sendQueue.enqueue({data, 0});
    if (!m_sendTimer->isActive()) m_sendTimer->start();
    pumpSendQueue(); reportSendStatus();
}

void SerialHandler::pumpSendQueue()
{
    if (m_pumpingSend || !m_activeRequestId || !m_serialPort->isOpen() || m_breakMayBeActive || (m_sendQueue.isEmpty() && !m_inFlightSendBytes)) return;
    QScopedValueRollback<bool> guard(m_pumpingSend, true);
    if (m_hardwareFlow) {
        m_controlLineError = QSerialPort::NoError; m_controlLineErrorString.clear();
        m_isSettingControlLines = true;
        m_ctsState = readCtsState();
        m_isSettingControlLines = false;
        const auto error = m_controlLineError;
        if (error != QSerialPort::NoError) {
            const QString detail = m_controlLineErrorString;
            m_serialPort->clearError(); m_ctsState = -1;
            if (error != QSerialPort::UnsupportedOperationError && error != QSerialPort::UnknownError) {
                const QString message = buildErrorMessage(error, detail);
                closePortSafely(); emit errorOccurred(message, static_cast<int>(error)); return;
            }
        }
        // CTS querying is diagnostic only. The driver owns the handshake:
        // software polling must not miss short CTS-ready windows or reduce
        // throughput. Only one bounded chunk is submitted to the driver.
    }
    if (m_inFlightSendBytes > 0) return;
    if (m_sendQueue.isEmpty()) return;
    quint64 generation;
    { QMutexLocker lock(&m_sendMutex); generation = m_sendGeneration; }
    auto &front = m_sendQueue.head();
    const QByteArray chunk = front.data.mid(front.offset, 4096);
    front.offset += chunk.size();
    m_currentSendCompletesRequest = front.offset == front.data.size();
    if (m_currentSendCompletesRequest) m_sendQueue.dequeue();
    m_inFlightSendBytes = chunk.size();
    const qint64 accepted = m_serialPort->write(chunk);
    { QMutexLocker lock(&m_sendMutex); if (generation != m_sendGeneration) return; }
    if (accepted != chunk.size()) {
        // Do not silently truncate commands/files or report false completion.
        handleError(QSerialPort::WriteError); return;
    }
    if (!m_inFlightSendBytes && !m_sendQueue.isEmpty()) {
        QTimer::singleShot(0, this, [this, generation]() {
            { QMutexLocker lock(&m_sendMutex); if (generation != m_sendGeneration) return; }
            pumpSendQueue(); reportSendStatus();
        });
    }
}

void SerialHandler::reportSendStatus(bool force)
{
    if (!m_activeRequestId) return;
    const qint64 pending = pendingSendBytes();
    const int cts = m_hardwareFlow ? m_ctsState : -1;
    // Fast command loops must not flood the GUI with per-write label/layout
    // updates. The timer also delivers the final idle update after throttling.
    if (!force && cts == m_lastReportedCts && m_sendStatusClock.isValid() && m_sendStatusClock.elapsed() < 100) return;
    m_sendStatusClock.restart(); m_lastReportedCts = cts;
    if (!pending) m_sendTimer->stop();
    emit sendStatusChanged(m_activeRequestId, pending, cts);
}

void SerialHandler::invalidateSendQueue(bool closing)
{
    { QMutexLocker lock(&m_sendMutex);
      ++m_sendGeneration; m_reservedSendBytes = 0; m_reservedSendRequests = 0; if (closing) m_sendSession = 0; }
    m_sendQueue.clear(); m_inFlightSendBytes = 0; m_currentSendCompletesRequest = false;
    m_sendTimer->stop();
    m_sendStatusClock.invalidate(); m_lastReportedCts = -2;
}

void SerialHandler::cancelPendingSend(quint64 requestId)
{
    if (!requestId || requestId != m_activeRequestId || !m_serialPort->isOpen()) return;
    // Qt 5.14's Windows output purge can leave an aborted overlapped write.
    // If a write reached the driver, close the handle instead of pretending it
    // is safely reusable. Data still only queued in the app can be cancelled in place.
    const bool nativePending = m_inFlightSendBytes > 0 || m_serialPort->bytesToWrite() > 0;
    if (nativePending) closePortSafely();
    else { invalidateSendQueue(false); reportSendStatus(); }
    emit sendCancellationFinished(requestId, nativePending);
}

void SerialHandler::handleReadyRead()
{
    if (!m_serialPort) return;
    QByteArray data = m_serialPort->readAll();
    qDebug() << "handleReadyRead: read bytes:" << data.size();
    if (!data.isEmpty()) {
        // All frames parsed from this operating-system read belong to the same
        // receive batch and therefore retain the same raw host timestamp.
        m_currentReceiveTimestampNs = m_receiveEpochNs + m_receiveClock.nsecsElapsed();
        m_statusMonitor->setReceiveTimestamp(m_currentReceiveTimestampNs);
        m_statusMonitor->feedReceived(data,m_currentReceiveTimestampNs);
        m_rxCount.fetch_add(data.size());
        m_buffer.append(data);
        
        // ALWAYS emit raw data to UI so the console can show it, regardless of parser success!
        emit dataReceived(data);
        
        if (m_parser) {
            int consumed = m_parser->processData(m_buffer.constData(), m_buffer.size());
            if (consumed > 0) {
                m_buffer.remove(0, consumed);
            }
            
            // Buffer overflow protection (1MB threshold)
            if (m_buffer.size() > 1024 * 1024 && consumed == 0) {
                m_buffer.clear();
            }
        }
    }
}

void SerialHandler::forwardTextReady(const QByteArray &text)
{
    emit dataReceived(text);
}

void SerialHandler::forwardChannelsUpdated(const QVector<double> &data)
{
    m_plotFrames.push(data);
}

void SerialHandler::startRecording(const QString &filePath, double sampleRate)
{
    stopRecording();
    QString error;
    m_useWlrRecording = filePath.endsWith(QStringLiteral(".wlr"), Qt::CaseInsensitive);
    const auto names = m_parser ? m_parser->channelNames() : QStringList();
    const bool started = m_useWlrRecording
        ? m_wlrRecorder.start(filePath, error, sampleRate, names)
        : m_recorder.start(filePath, error, sampleRate, names);
    if (started) {
        loggedBytes = 0;
        emit recordingStartFinished(true, QString());
    } else {
        emit recordingStartFinished(false, error);
    }
}

void SerialHandler::stopRecording()
{
    const QString error = finishRecording();
    if (!error.isEmpty()) emit recordingError(error);
}

QString SerialHandler::finishRecording()
{
    QString csvError, wlrError;
    const bool csvOk = m_recorder.finish(csvError), wlrOk = m_wlrRecorder.finish(wlrError);
    m_recordingChannels.reset();
    return !csvOk ? csvError : !wlrOk ? wlrError : QString();
}

qint64 SerialHandler::getLoggedBytes() const
{
    return loggedBytes.load();
}

void SerialHandler::writeLogData(const QVector<double> &data)
{
    if ((m_recorder.isActive() || m_wlrRecorder.isActive()) && !data.isEmpty()) {
        if (!m_recordingChannels.accept(data.size())) return;
        QString error;
        const bool written = m_useWlrRecording
            ? m_wlrRecorder.append(data, error, m_currentReceiveTimestampNs)
            : m_recorder.append(data, error, m_currentReceiveTimestampNs);
        if (!written) {
            emit recordingError(error);
            return;
        }
        loggedBytes = m_useWlrRecording ? m_wlrRecorder.totalBytes() : m_recorder.totalBytes();
    }
}

void SerialHandler::handleError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || m_isClosing) return;

    const QString systemMessage = m_serialPort ? m_serialPort->errorString() : QString();
    if (m_isSettingControlLines) {
        // Qt emits errors synchronously inside setters, including its initial
        // pin-state query. Delay recovery until the setter has returned, and
        // never overwrite a genuine disconnect with a later unsupported error.
        if (m_controlLineError == QSerialPort::NoError ||
            m_controlLineError == QSerialPort::UnsupportedOperationError ||
            m_controlLineError == QSerialPort::UnknownError) {
            m_controlLineError = error;
            m_controlLineErrorString = systemMessage;
        }
        return;
    }
    if (m_isOpening) {
        // QSerialPort can emit errorOccurred synchronously from open(). Let
        // openPort() report that failure exactly once after open() returns.
        m_openError = error;
        m_openErrorString = systemMessage;
        return;
    }

    const QString message = buildErrorMessage(error, systemMessage);
    closePortSafely();
    emit errorOccurred(message, static_cast<int>(error));
}
