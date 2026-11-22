#include "networkhandler.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QFileInfo>
#include <QTemporaryDir>
#include <functional>
#include <iostream>
#define VERIFY(c) do { if (!(c)) { std::cerr << "TCP bind failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(std::function<bool()> condition) {
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return condition();
}

static int testServerRecovery()
{
    NetworkHandler server;
    quint64 sendSession = 0;
    int connections = 0, failures = 0;
    QString disconnectReason, waitReason;
    QObject::connect(&server, &NetworkHandler::clientConnected, [&](const QString &, quint64 session) {
        sendSession = session;
        ++connections;
    });
    QObject::connect(&server, &NetworkHandler::clientDisconnected, [&](const QString &reason) { disconnectReason = reason; });
    QObject::connect(&server, &NetworkHandler::tcpServerWaitChanged, [&](const QString &reason) { waitReason = reason; });
    QObject::connect(&server, &NetworkHandler::errorOccurred, [&](const QString &) { ++failures; });
    server.setProtocol(1);
    VERIFY(server.startConnection(2, "127.0.0.1", 0));
    auto listener = server.findChild<QTcpServer *>();
    VERIFY(listener);
    const quint16 port = listener->serverPort();

    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, port);
    VERIFY(until([&] { return connections == 1 && server.sendReady(); }));
    const auto oldSession = sendSession;
    QTemporaryDir directory;
    VERIFY(directory.isValid());
    const auto path = directory.filePath("across-reconnect.wlr");
    server.startRecording(path, 250);
    const QByteArray partial = "1,2\n99,";
    VERIFY(first.write(partial) == partial.size());
    VERIFY(until([&] { return server.getRxCount() == partial.size(); }));
    quint64 dropped = 0;
    VERIFY(server.takePlotFrames(10, dropped) == QVector<QVector<double>>({{1, 2}}));
    auto accepted = listener->findChild<QTcpSocket *>();
    VERIFY(accepted && accepted->socketOption(QAbstractSocket::KeepAliveOption).toBool());
    VERIFY(server.enqueueData("old queued data", oldSession));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    accepted->errorOccurred(QAbstractSocket::NetworkError);
#else
    accepted->error(QAbstractSocket::NetworkError);
#endif
    VERIFY(!server.sendReady() && server.pendingSendBytes() == 0 && listener->isListening());
    VERIFY(!disconnectReason.isEmpty() && failures == 0);
    VERIFY(QFileInfo::exists(path + ".part") && !QFileInfo::exists(path));

    QTcpSocket second;
    second.connectToHost(QHostAddress::LocalHost, port);
    VERIFY(until([&] { return connections == 2 && server.sendReady(); }));
    VERIFY(sendSession != oldSession && !server.enqueueData("old session", oldSession));
    VERIFY(server.enqueueData("new session", sendSession));
    VERIFY(until([&] { return second.bytesAvailable() == 11; }));
    VERIFY(second.readAll() == "new session");
    VERIFY(second.write("3,4\n") == 4 && until([&] { return server.getRxCount() == partial.size() + 4; }));
    VERIFY(server.takePlotFrames(10, dropped) == QVector<QVector<double>>({{3, 4}}));
    second.disconnectFromHost();
    VERIFY(until([&] { return !server.sendReady(); }));
    VERIFY(QFileInfo::exists(path + ".part") && !QFileInfo::exists(path));
    VERIFY(server.finishRecording().isEmpty()); // Manual stop also works while waiting.
    QVector<QVector<double>> recorded;
    QString recordingError;
    VERIFY(WlrRecordingWriter::visitFiles({path}, [&](qint64, const QVector<double> &values, qint64) {
        recorded.append(values);
        return true;
    }, recordingError));
    VERIFY(recorded == QVector<QVector<double>>({{1, 2}, {3, 4}}));

    listener->pauseAccepting();
    listener->acceptError(QAbstractSocket::NetworkError);
    VERIFY(QMetaObject::invokeMethod(&server, "refreshTcpServer", Qt::DirectConnection,
        Q_ARG(QList<QHostAddress>, QList<QHostAddress>())));
    QTcpSocket third;
    third.connectToHost(QHostAddress::LocalHost, port);
    VERIFY(until([&] { return connections == 3 && server.sendReady(); }));
    third.disconnectFromHost();
    VERIFY(until([&] { return !server.sendReady(); }));

    listener->close();
    QTcpServer blocker;
    VERIFY(blocker.listen(QHostAddress::LocalHost, port));
    VERIFY(until([&] { return !waitReason.isEmpty(); }));
    VERIFY(!listener->isListening() && failures == 0);
    blocker.close();
    VERIFY(until([&] { return listener->isListening() && waitReason.isEmpty(); }));
    VERIFY(listener->serverPort() == port);
    server.stopConnection();
    VERIFY(QMetaObject::invokeMethod(&server, "refreshTcpServer", Qt::DirectConnection,
        Q_ARG(QList<QHostAddress>, QList<QHostAddress>())));
    VERIFY(!listener->isListening());
    return 0;
}

static int testLocalAddressLoss()
{
    QHostAddress local;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsRunning)) continue;
        for (const auto &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                local = entry.ip();
                break;
            }
        }
        if (!local.isNull()) break;
    }
    if (local.isNull()) {
        std::cout << "No active non-loopback IPv4 address; skipping address loss simulation.\n";
        return 0;
    }

    for (const bool wildcard : {false, true}) {
        NetworkHandler server;
        QString waitReason;
        QObject::connect(&server, &NetworkHandler::tcpServerWaitChanged, [&](const QString &reason) { waitReason = reason; });
        VERIFY(server.startConnection(2, wildcard ? QStringLiteral("0.0.0.0") : local.toString(), 0));
        const auto listener = server.findChild<QTcpServer *>();
        VERIFY(listener);
        const auto port = listener->serverPort();
        QTcpSocket peer;
        peer.setProxy(QNetworkProxy::NoProxy);
        peer.connectToHost(local, port);
        VERIFY(until([&] { return server.sendReady(); }));
        VERIFY(QMetaObject::invokeMethod(&server, "refreshTcpServer", Qt::DirectConnection,
            Q_ARG(QList<QHostAddress>, QList<QHostAddress>())));
        VERIFY(!server.sendReady() && listener->isListening() == wildcard && waitReason.isEmpty() == wildcard);
        VERIFY(QMetaObject::invokeMethod(&server, "refreshTcpServer", Qt::DirectConnection,
            Q_ARG(QList<QHostAddress>, QList<QHostAddress>{local})));
        VERIFY(listener->isListening() && listener->serverPort() == port && waitReason.isEmpty());
        QTcpSocket reconnected;
        reconnected.setProxy(QNetworkProxy::NoProxy);
        reconnected.connectToHost(local, port);
        VERIFY(until([&] { return server.sendReady(); }));
        server.stopConnection();
    }
    return 0;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    struct RestoreProxy { QNetworkProxy old = QNetworkProxy::applicationProxy(); ~RestoreProxy() { QNetworkProxy::setApplicationProxy(old); } } restore;
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 9));
    for (const auto &address : {QString("0.0.0.0"), QString("127.0.0.1")}) {
        NetworkHandler server; VERIFY(server.startConnection(2, address, 0));
        auto listener = server.findChild<QTcpServer *>();
        VERIFY(listener && listener->serverAddress() == QHostAddress(address) && listener->serverPort() > 0);
        QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        VERIFY(until([&] { return peer.state() == QAbstractSocket::ConnectedState && server.sendReady(); }));
        VERIFY(server.enqueueData("bound-server") && until([&] { return peer.bytesAvailable() == 12; }));
        VERIFY(peer.readAll() == "bound-server");
        QTcpSocket newcomer;
        bool connected = false;
        QObject::connect(&newcomer, &QTcpSocket::connected, [&] { connected = true; });
        newcomer.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        VERIFY(until([&] { return connected && newcomer.state() == QAbstractSocket::UnconnectedState; }));
        VERIFY(peer.state() == QAbstractSocket::ConnectedState && server.sendReady());
        VERIFY(server.enqueueData("still-first") && until([&] { return peer.bytesAvailable() == 11; }));
        VERIFY(peer.readAll() == "still-first"); server.stopConnection();
    }
    // Everything uses loopback/ephemeral ports; no external host is contacted.
    for (const auto &address : {QString("0.0.0.0"), QString("127.0.0.1")}) for (bool fixedPort : {false, true}) {
        QTcpServer remote; VERIFY(remote.listen(QHostAddress::LocalHost, 0));
        QTcpServer reserve; VERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const quint16 requested = fixedPort ? reserve.serverPort() : 0; reserve.close();
        NetworkHandler client; QString actualAddress; int actualPort = 0;
        QObject::connect(&client, &NetworkHandler::connectionAttemptFinished, [&](bool ok, const QString &message) {
            if (!ok) std::cerr << "Client bind " << address.toStdString() << ':' << requested << ": " << message.toStdString() << '\n';
        });
        QObject::connect(&client, &NetworkHandler::localEndpointChanged, [&](const QString &a, int p) { actualAddress = a; actualPort = p; });
        VERIFY(client.startConnection(1, fixedPort ? "localhost" : "127.0.0.1", remote.serverPort(), address, requested));
        VERIFY(until([&] { return remote.hasPendingConnections(); }));
        auto peer = remote.nextPendingConnection();
        VERIFY(QHostAddress(actualAddress) == QHostAddress::LocalHost && actualPort > 0 && peer->peerPort() == actualPort);
        VERIFY(!fixedPort || actualPort == requested);
        VERIFY(client.enqueueData("bound-client") && until([&] { return peer->bytesAvailable() == 12; }));
        VERIFY(peer->readAll() == "bound-client"); client.stopConnection();
    }
    NetworkHandler client; int attempts = 0; QString error;
    QObject::connect(&client, &NetworkHandler::connectionAttemptFinished, [&](bool ok, const QString &message) { if (!ok) { ++attempts; error = message; } });
    QTcpServer occupied, remote; VERIFY(occupied.listen(QHostAddress::LocalHost, 0) && remote.listen(QHostAddress::LocalHost, 0));
    VERIFY(!client.startConnection(1, "127.0.0.1", remote.serverPort(), "127.0.0.1", occupied.serverPort()));
    VERIFY(attempts == 1 && error.contains("绑定") && !client.isConnected() && !client.sendReady());
    VERIFY(!client.startConnection(2, "127.0.0.1", occupied.serverPort()) && attempts == 2 && error.contains("监听"));
    VERIFY(!client.startConnection(2, "not-an-ip", 12345) && attempts == 3 && error.contains("监听 IP"));
    VERIFY(!client.startConnection(1, "0.0.0.0", remote.serverPort()) && attempts == 4);
    VERIFY(!client.startConnection(1, "127.0.0.1", remote.serverPort(), "bad-local", 0) && attempts == 5);
    VERIFY(!client.startConnection(1, "127.0.0.1", 0) && attempts == 6);
    VERIFY(!client.startConnection(1, "127.0.0.1", remote.serverPort(), "::1", 0) && attempts == 7 && error.contains("类型不匹配"));
    QString absent = "192.0.2.254";
    if (QNetworkInterface::allAddresses().contains(QHostAddress(absent))) absent = "198.51.100.254";
    VERIFY(!client.startConnection(1, "127.0.0.1", remote.serverPort(), absent, 0) && attempts == 8 && error.contains("绑定"));
    VERIFY(!client.startConnection(2, absent, 0) && attempts == 9 && error.contains("监听"));
    VERIFY(!remote.hasPendingConnections());
    bool ok = false;
    VERIFY(QMetaObject::invokeMethod(&client, "startConnection", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
        Q_ARG(int, 1), Q_ARG(QString, QString("127.0.0.1")), Q_ARG(int, remote.serverPort()),
        Q_ARG(QString, QString("127.0.0.1")), Q_ARG(int, 0)) && ok);
    VERIFY(until([&] { return remote.hasPendingConnections(); })); client.stopConnection();
    VERIFY(!client.startConnection(2, "239.255.0.1", 0) && !client.isConnected());
    VERIFY(!client.startConnection(1, "127.0.0.1", remote.serverPort(), "255.255.255.255", 0));
    QTcpServer ipv6;
    if (ipv6.listen(QHostAddress::LocalHostIPv6, 0)) {
        for (const auto &local : {QString("0.0.0.0"), QString("::1")}) {
            NetworkHandler v6; VERIFY(v6.startConnection(1, "::1", ipv6.serverPort(), local, 0));
            VERIFY(until([&] { return ipv6.hasPendingConnections(); }));
            auto peer = ipv6.nextPendingConnection(); VERIFY(peer->peerAddress() == QHostAddress::LocalHostIPv6); v6.stopConnection();
        }
    }
    VERIFY(testServerRecovery() == 0);
    VERIFY(testLocalAddressLoss() == 0);
    std::cout << "TCP binding, IPv6, peer recovery, listen retry, continuous recording and send-session isolation passed.\n";
    return 0;
}
