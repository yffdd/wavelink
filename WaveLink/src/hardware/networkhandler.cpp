#include "networkhandler.h"
#include "ProtocolParserFactory.h"
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QRegularExpression>
#include <QNetworkProxy>
#include <QTimer>

NetworkHandler::NetworkHandler(QObject *parent)
    : QObject(parent), m_tcpSocket(nullptr), m_tcpServer(new QTcpServer(this)), m_mode(1), m_parser(nullptr),
      loggedBytes(0)
{
    m_wlrRecorder.setErrorHandler([this](const QString &error) { emit recordingError(error); });
    m_receiveEpochNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    m_receiveClock.start();
    m_tcpServer->setProxy(QNetworkProxy::NoProxy);
    connect(m_tcpServer, &QTcpServer::newConnection, this, &NetworkHandler::onNewConnection);
}

NetworkHandler::~NetworkHandler()
{
    stopRecording();
    if (m_parser) {
        m_parser->deleteLater();
    }
    stopConnection();
}

void NetworkHandler::setProtocol(int index)
{
    applyProtocol(0, DataProtocols::ProtocolSelection::fromLegacy(index), {});
}
void NetworkHandler::applyProtocol(quint64 request, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile)
{
    if (m_recorder.isActive() || m_wlrRecorder.isActive() || pendingSendBytes() > 0 || (m_tcpSocket && m_tcpSocket->bytesToWrite() > 0)) {
        emit protocolApplyFinished(request, false, QStringLiteral("录制或发送期间不能切换协议。")); return;
    }
    QString error;
    auto candidate = DataProtocols::ProtocolParserFactory::create(selection, profile, this, error);
    if (!candidate) { emit protocolApplyFinished(request, false, error); return; }
    delete m_parser; m_parser = candidate; m_plotFrames.clear(); m_buffer.clear();
    m_statusMonitor->bindProtocol(m_parser, selection, profile);
    connect(m_parser, &ProtocolParser::textReady, this, &NetworkHandler::forwardTextReady);
    connect(m_parser, &ProtocolParser::channelsUpdated, this, &NetworkHandler::forwardChannelsUpdated);
    connect(m_parser, &ProtocolParser::recordingFrameReady, this, &NetworkHandler::writeLogData);
    emit protocolApplyFinished(request, true, QString());
}

void NetworkHandler::setEndianness(bool bigEndian)
{
    if (m_recorder.isActive() || m_wlrRecorder.isActive()) return;
    if (JustFloatParser *p = qobject_cast<JustFloatParser*>(m_parser)) {
        p->setBigEndian(bigEndian);
    }
}

bool NetworkHandler::startConnection(int mode, const QString &ip, int port, const QString &localIp, int localPort)
{
    stopConnection();
    m_mode = mode;
    m_statusMonitor->setTextTransport(false);
    auto failed = [this](const QString &message) {
        stopConnection(); emit connectionAttemptFinished(false, message); return false;
    };
    const QString addressText = ip.trimmed();
    if ((mode != 1 && mode != 2) || port < (mode == 1 ? 1 : 0) || port > 65535)
        return failed(QStringLiteral("TCP 模式或端口无效。远端端口必须为 1–65535。"));
    
    if (m_mode == 1) { // Client
        QHostAddress remoteAddress, localAddress;
        const bool remoteIsAddress = remoteAddress.setAddress(addressText);
        if (addressText.isEmpty() || addressText.contains(QRegularExpression("[\\s\\x00-\\x1F]"))
            || (remoteIsAddress && (remoteAddress == QHostAddress::AnyIPv4 || remoteAddress == QHostAddress::AnyIPv6)))
            return failed(QStringLiteral("请填写有效的远端 IP 或主机名，不能把 0.0.0.0 或 :: 用作远端地址。"));
        if (!localAddress.setAddress(localIp.trimmed()) || localPort < 0 || localPort > 65535)
            return failed(QStringLiteral("本机 IP 必须是有效地址；本机端口须为 0–65535，0 表示自动分配。"));
        if (localAddress.isMulticast() || localAddress == QHostAddress::Broadcast)
            return failed(QStringLiteral("TCP 本机地址不能是组播或广播地址。"));
        const bool automaticAddress = localAddress == QHostAddress::AnyIPv4 || localAddress == QHostAddress::AnyIPv6;
        if (!automaticAddress && remoteIsAddress && localAddress.protocol() != remoteAddress.protocol())
            return failed(QStringLiteral("本机 IP 与远端 IP 的地址类型不匹配，请同时使用 IPv4 或 IPv6。"));
        m_tcpSocket = new QTcpSocket(this);
        // Device TCP is a direct connection. An ambient HTTP proxy cannot
        // honor a local bind, and must not receive hardware debug traffic.
        m_tcpSocket->setProxy(QNetworkProxy::NoProxy);
        connectSendProgress(m_tcpSocket);
        connect(m_tcpSocket, &QTcpSocket::readyRead, this, &NetworkHandler::onReadyRead);
        connect(m_tcpSocket, &QTcpSocket::connected, this, &NetworkHandler::onConnected);
        connect(m_tcpSocket, &QTcpSocket::disconnected, this, &NetworkHandler::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(m_tcpSocket, &QTcpSocket::errorOccurred, this, &NetworkHandler::onError);
#else
        connect(m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this, &NetworkHandler::onError);
#endif
        // With wildcard/port 0 Qt/the OS choose the source address and port.
        auto protocol = QAbstractSocket::AnyIPProtocol;
        if (!automaticAddress || localPort != 0) {
            QHostAddress bindAddress = localAddress;
            if (automaticAddress) {
                // Match literal peers; for hostnames use the selected local
                // address family (0.0.0.0 = IPv4, :: = IPv6).
                protocol = remoteIsAddress ? remoteAddress.protocol() : localAddress.protocol();
                bindAddress = protocol == QAbstractSocket::IPv6Protocol ? QHostAddress(QHostAddress::AnyIPv6) : QHostAddress(QHostAddress::AnyIPv4);
            } else protocol = localAddress.protocol();
            if (!m_tcpSocket->bind(bindAddress, quint16(localPort), QAbstractSocket::DontShareAddress))
                return failed(QStringLiteral("无法绑定本机地址 %1:%2：%3。请确认 IP 属于当前电脑、端口未被占用。")
                    .arg(localIp).arg(localPort).arg(m_tcpSocket->errorString()));
        }
        m_tcpSocket->connectToHost(addressText, quint16(port), QIODevice::ReadWrite, protocol);
        if (!m_tcpSocket->waitForConnected(3000)) {
            const QString error = m_tcpSocket->errorString();
            return failed(QStringLiteral("无法连接远端 %1:%2：%3").arg(addressText).arg(port).arg(error));
        }
        emit connectionAttemptFinished(true, QString());
        return true;
    } else if (m_mode == 2) { // Server
        QHostAddress listenAddress;
        if (!listenAddress.setAddress(addressText)) return failed(QStringLiteral("监听 IP 无效，请输入本机 IP；0.0.0.0 表示监听所有本机 IPv4 地址。"));
        if (listenAddress.isMulticast() || listenAddress == QHostAddress::Broadcast)
            return failed(QStringLiteral("TCP 监听 IP 不能是组播或广播地址。"));
        if (!m_tcpServer->listen(listenAddress, quint16(port)))
            return failed(QStringLiteral("无法监听 %1:%2：%3。请确认 IP 属于当前电脑、端口未被占用。")
                .arg(addressText).arg(port).arg(m_tcpServer->errorString()));
        emit connectionStateChanged(true); // Treat listening as "connected" for UI purposes
        emit connectionAttemptFinished(true, QString());
        return true;
    }
    return false;
}

bool NetworkHandler::startUdp(const QString &localIp, int localPort, const QString &remoteIp, int remotePort, bool filterRemote)
{
    stopConnection(); m_mode = 5;
    m_statusMonitor->setTextTransport(true);
    auto failed = [this](const QString &message) {
        stopConnection(); emit connectionAttemptFinished(false, message); return false;
    };
    QHostAddress local, remote;
    if (!local.setAddress(localIp.trimmed()) || local.isMulticast() || local == QHostAddress::Broadcast
        || localPort < 0 || localPort > 65535)
        return failed(QStringLiteral("UDP 本机 IP 必须是本机单播地址或 0.0.0.0 / ::；本机端口范围为 0–65535，0 表示自动分配。"));
    const QString target = remoteIp.trimmed();
    const bool hasTarget = !target.isEmpty() || remotePort != 0;
    if (hasTarget && (!remote.setAddress(target) || remotePort < 1 || remotePort > 65535
        || remote == QHostAddress::AnyIPv4 || remote == QHostAddress::AnyIPv6 || remote.isMulticast() || remote == QHostAddress::Broadcast))
        return failed(QStringLiteral("UDP 远端 IP 和端口须同时填写，使用有效的单播 IP 和 1–65535 端口；仅接收时请将远端 IP 留空、远端端口设为 0。本版不支持广播或组播。"));
    if (hasTarget && local.protocol() != remote.protocol())
        return failed(QStringLiteral("UDP 本机 IP 与远端 IP 须使用同一地址类型：IPv4 或 IPv6。"));
    m_udpRemoteAddress = remote; m_udpRemotePort = quint16(remotePort);
    m_udpFilterRemote = filterRemote && hasTarget;
    m_udpSocket = new QUdpSocket(this); m_udpSocket->setProxy(QNetworkProxy::NoProxy);
    auto socket = m_udpSocket;
    if (!socket->bind(local, quint16(localPort), QAbstractSocket::DontShareAddress))
        return failed(QStringLiteral("无法监听 UDP %1:%2：%3。请确认 IP 属于当前电脑、端口未被其他 UDP 程序占用。")
                      .arg(localIp).arg(localPort).arg(socket->errorString()));
    // Only hook runtime errors after successful bind. Opening failures have
    // exactly one consolidated connection result.
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QUdpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QUdpSocket::error), this, [this, socket](QAbstractSocket::SocketError) {
#endif
        if (socket == m_udpSocket) {
            resetSendSession(false); emit errorOccurred(QStringLiteral("UDP 收发失败：") + socket->errorString());
        }
    });
    connect(socket, &QUdpSocket::readyRead, this, [this, socket] { if (socket == m_udpSocket) onUdpReadyRead(); });
    m_plotFrames.clear(); resetSendSession(hasTarget, true);
    m_statusMonitor->setConnected(true);
    emit localEndpointChanged(socket->localAddress().toString(), socket->localPort());
    emit connectionStateChanged(true); emit connectionAttemptFinished(true, {});
    return true;
}

void NetworkHandler::stopConnection()
{
    m_statusMonitor->setConnected(false);
    resetSendSession(false);
    ++m_udpSession;
    if (m_udpSocket) {
        QObject::disconnect(m_udpSocket, nullptr, this, nullptr);
        m_udpSocket->close(); m_udpSocket->deleteLater(); m_udpSocket = nullptr;
    }
    m_udpRemoteAddress.clear(); m_udpRemotePort = 0; m_udpFilterRemote = false;
    m_udpLastSender.clear(); m_udpLastSenderPort = 0;
    m_buffer.clear();
    if (m_parser) m_parser->reset();
    if (m_mode == 1 && m_tcpSocket) {
        QObject::disconnect(m_tcpSocket, nullptr, this, nullptr);
        if (m_tcpSocket->state() == QAbstractSocket::ConnectedState || m_tcpSocket->state() == QAbstractSocket::ClosingState) {
            m_tcpSocket->disconnectFromHost();
            if (m_tcpSocket->state() != QAbstractSocket::UnconnectedState) m_tcpSocket->waitForDisconnected(1000);
        } else m_tcpSocket->abort();
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    } else if (m_mode == 2) {
        if (m_tcpServer->isListening()) {
            m_tcpServer->close();
        }
        if (m_tcpSocket) {
            QObject::disconnect(m_tcpSocket, nullptr, this, nullptr);
            m_tcpSocket->disconnectFromHost();
            m_tcpSocket->deleteLater();
            m_tcpSocket = nullptr;
        }
        emit connectionStateChanged(false);
    }
}

bool NetworkHandler::isConnected() const
{
    if (m_mode == 5) return m_udpSocket && m_udpSocket->state() == QAbstractSocket::BoundState;
    if (m_mode == 1 && m_tcpSocket) {
        return m_tcpSocket->state() == QAbstractSocket::ConnectedState;
    } else if (m_mode == 2) {
        return m_tcpServer->isListening();
    }
    return false;
}

void NetworkHandler::onNewConnection()
{
    // A recording belongs to one peer. Never let another device silently
    // replace an active client and write into that peer's recording.
    if (m_tcpSocket && m_tcpSocket->state() != QAbstractSocket::UnconnectedState) {
        while (m_tcpServer->hasPendingConnections()) {
            auto rejected = m_tcpServer->nextPendingConnection();
            rejected->abort();
            rejected->deleteLater();
        }
        return;
    }
    if (m_tcpSocket) {
        // Disconnect old client before replacing it.
        QObject::disconnect(m_tcpSocket, nullptr, this, nullptr);
        m_tcpSocket->disconnectFromHost();
        m_tcpSocket->deleteLater();
    }
    m_buffer.clear();
    if (m_parser) m_parser->reset();
    m_tcpSocket = m_tcpServer->nextPendingConnection();
    if (!m_tcpSocket) return;
    m_statusMonitor->setConnected(true);
    resetSendSession(true);
    connectSendProgress(m_tcpSocket);
    connect(m_tcpSocket, &QTcpSocket::readyRead, this, &NetworkHandler::onReadyRead);
    connect(m_tcpSocket, &QTcpSocket::disconnected, this, &NetworkHandler::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_tcpSocket, &QTcpSocket::errorOccurred, this, &NetworkHandler::onError);
#else
    connect(m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this, &NetworkHandler::onError);
#endif
    
    emit clientConnected(m_tcpSocket->peerAddress().toString());
    // Optionally emit a signal that a client connected
}

void NetworkHandler::onReadyRead()
{
    if (!m_tcpSocket || sender() != m_tcpSocket) return;
    QByteArray data = m_tcpSocket->readAll();
    if (!data.isEmpty()) {
        m_currentReceiveTimestampNs = m_receiveEpochNs + m_receiveClock.nsecsElapsed();
        m_statusMonitor->setReceiveTimestamp(m_currentReceiveTimestampNs);
        m_statusMonitor->feedReceived(data,m_currentReceiveTimestampNs);
        m_rxCount.fetch_add(data.size());
        emit dataReceived(data); // Pass raw data for UI logging
        
        // OS reads may contain megabytes of VALID frames. Parse bounded
        // chunks before testing the unconsumed backlog, never discard a valid
        // burst simply because TCP coalesced it into one read.
        for (int offset = 0; offset < data.size(); offset += 64 * 1024) {
            m_buffer.append(data.constData() + offset, qMin(64 * 1024, data.size() - offset));
            if (m_parser) {
                const int consumed = m_parser->processData(m_buffer.constData(), m_buffer.size());
                if (consumed > 0 && consumed <= m_buffer.size()) m_buffer.remove(0, consumed);
            }
            if (m_buffer.size() > 1024 * 1024) {
                m_buffer.clear();
                emit errorOccurred(QStringLiteral("未能解析的网络数据超过 1 MB，已停止采集。请检查数据协议。"));
                return;
            }
        }
    }
}

void NetworkHandler::onUdpReadyRead()
{
    auto socket = m_udpSocket;
    if (!socket || m_mode != 5) return;
    QElapsedTimer budget; budget.start(); int packets = 0;
    while (socket == m_udpSocket && socket->hasPendingDatagrams()) {
        QByteArray data; data.resize(int(qBound<qint64>(0, socket->pendingDatagramSize(), 65535)));
        QHostAddress sender; quint16 senderPort = 0;
        const auto count = socket->readDatagram(data.data(), data.size(), &sender, &senderPort);
        if (count < 0) { emit errorOccurred(QStringLiteral("UDP 接收失败：") + socket->errorString()); return; }
        data.resize(int(count));
        if (!m_udpFilterRemote || (sender == m_udpRemoteAddress && senderPort == m_udpRemotePort)) {
            if (sender != m_udpLastSender || senderPort != m_udpLastSenderPort) {
                m_statusMonitor->restart();
                if (m_parser) m_parser->reset();
                m_udpLastSender = sender; m_udpLastSenderPort = senderPort;
            }
            m_currentReceiveTimestampNs = m_receiveEpochNs + m_receiveClock.nsecsElapsed();
            m_statusMonitor->setReceiveTimestamp(m_currentReceiveTimestampNs);
            m_statusMonitor->feedReceived(data,m_currentReceiveTimestampNs,true);
            m_rxCount.fetch_add(data.size());
            if (!data.isEmpty()) emit dataReceived(data);
            if (m_parser) {
                const int consumed = data.isEmpty() ? 0 : m_parser->processData(data.constData(), data.size());
                // Incomplete bytes/key groups at this boundary are discarded,
                // not concatenated with a later (possibly unrelated) packet.
                m_parser->endDatagram(consumed == data.size());
            }
        }
        if (++packets >= 128 || budget.elapsed() >= 4) {
            // Yield to queued send/stop/recording operations under sustained RX.
            const auto session = m_udpSession;
            if (socket->hasPendingDatagrams()) QTimer::singleShot(0, this, [this, socket, session] {
                if (socket == m_udpSocket && session == m_udpSession) onUdpReadyRead();
            });
            return;
        }
    }
}

qint64 NetworkHandler::getLoggedBytes() const
{
    return loggedBytes.load();
}

void NetworkHandler::sendData(const QByteArray &data)
{
    enqueueData(data);
}

void NetworkHandler::resetSendSession(bool ready, bool datagram)
{
    QMutexLocker lock(&m_sendMutex);
    ++m_sendGeneration; m_pendingSendBytes = 0; m_pendingSendRequests = 0; m_sendReady = ready; m_datagramSend = datagram;
}

void NetworkHandler::connectSendProgress(QTcpSocket *socket)
{
    connect(socket, &QTcpSocket::bytesWritten, this, [this, socket](qint64 count) {
        if (socket != m_tcpSocket) return;
        { QMutexLocker lock(&m_sendMutex); m_pendingSendBytes = qMax<qint64>(0, m_pendingSendBytes - count); }
        m_txCount.fetch_add(count);
    });
}

bool NetworkHandler::enqueueData(const QByteArray &data)
{
    if (data.isEmpty()) return false;
    quint64 generation;
    {
        QMutexLocker lock(&m_sendMutex);
        if (!m_sendReady || data.size() > 256 * 1024 - m_pendingSendBytes || m_pendingSendRequests >= 1024) return false;
        if (m_datagramSend && data.size() > 65507) return false; // Never silently split a UDP command.
        generation = m_sendGeneration; m_pendingSendBytes += data.size(); ++m_pendingSendRequests;
    }
    const bool submitted = QMetaObject::invokeMethod(this, [this, generation, data]() {
        { QMutexLocker lock(&m_sendMutex); if (generation != m_sendGeneration) return; --m_pendingSendRequests; }
        if (m_mode == 5) {
            if (!m_udpSocket || m_udpSocket->state() != QAbstractSocket::BoundState || m_udpRemotePort == 0) {
                resetSendSession(false); emit errorOccurred("UDP 发送已停止：监听已关闭或没有远端目标。"); return;
            }
            const auto written = m_udpSocket->writeDatagram(data, m_udpRemoteAddress, m_udpRemotePort);
            if (written != data.size()) {
                const auto detail = m_udpSocket->errorString(); resetSendSession(false);
                emit errorOccurred("UDP 写入失败，发送已停止（不自动重发）：" + detail); return;
            }
            { QMutexLocker lock(&m_sendMutex); if (generation == m_sendGeneration) m_pendingSendBytes -= data.size(); }
            m_txCount.fetch_add(written); // Accepted by OS, NOT acknowledged by the peer.
            return;
        }
        if (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
            resetSendSession(false); emit errorOccurred("网络发送已停止：客户端已经断开，未完成数据不会自动重发。"); return;
        }
        if (m_tcpSocket->write(data) != data.size()) {
            const QString detail = m_tcpSocket->errorString(); resetSendSession(false);
            emit errorOccurred("网络写入失败，发送已停止：" + detail);
        }
    }, Qt::QueuedConnection);
    if (!submitted) {
        QMutexLocker lock(&m_sendMutex);
        if (generation == m_sendGeneration) { m_pendingSendBytes -= data.size(); --m_pendingSendRequests; }
    }
    return submitted;
}

void NetworkHandler::onConnected()
{
    m_statusMonitor->setConnected(true);
    resetSendSession(true);
    emit localEndpointChanged(m_tcpSocket->localAddress().toString(), m_tcpSocket->localPort());
    emit connectionStateChanged(true);
}

void NetworkHandler::onDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket && socket != m_tcpSocket) return;
    m_statusMonitor->setConnected(false);
    resetSendSession(false);
    if (m_mode == 1) {
        emit connectionStateChanged(false);
    } else {
        emit clientDisconnected();
    }
}

void NetworkHandler::onError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    if (sender() != m_tcpSocket) return; // Ignore a retired socket's late error.
    if (m_mode == 2 && socketError == QAbstractSocket::RemoteHostClosedError) return;
    if (m_tcpSocket) {
        emit errorOccurred(m_tcpSocket->errorString());
    }
}

void NetworkHandler::forwardChannelsUpdated(const QVector<double> &data)
{
    m_plotFrames.push(data);
}

void NetworkHandler::startRecording(const QString &filePath, double sampleRate)
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

void NetworkHandler::stopRecording()
{
    const QString error = finishRecording();
    if (!error.isEmpty()) emit recordingError(error);
}

QString NetworkHandler::finishRecording()
{
    QString csvError, wlrError;
    const bool csvOk = m_recorder.finish(csvError), wlrOk = m_wlrRecorder.finish(wlrError);
    m_recordingChannels.reset();
    return !csvOk ? csvError : !wlrOk ? wlrError : QString();
}

void NetworkHandler::writeLogData(const QVector<double> &data)
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
