// Probe the real native helper. Optional --scan checks advertisement reception;
// neither mode connects to devices or writes GATT characteristics.
#include "ble/bleipc.h"
#include <QCoreApplication>
#include <QProcess>
#include <QElapsedTimer>
#include <iostream>
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2 && argc != 3) return 1;
    const bool scanRequested = argc == 3 && QString::fromLocal8Bit(argv[2]) == "--scan";
    QProcess host; host.start(QString::fromLocal8Bit(argv[1]), QStringList());
    if (!host.waitForStarted(3000)) return 1;
    QByteArray buffer; QElapsedTimer clock; clock.start(); bool hello = false, scanning = false, stopped = false;
    int advertisements = 0;
    while (clock.elapsed() < 10000) {
        host.waitForReadyRead(100); buffer += host.readAllStandardOutput();
        QJsonObject object; QString error;
        while (BleIpc::take(buffer, object, error)) {
            const auto type = object.value("event").toString();
            if (type == "hello") {
                if (object.value("version").toInt() != 1 || hello || object.value("role").toString() != "ble" || object.value("sendControlVersion") != QJsonValue(1)) return 1;
                hello = true; host.write(BleIpc::encode({{"op", "probe"}, {"session", "0"}, {"request", "1"}}));
            } else if ((type == "adapter" || type == "error") && hello && object.value("request").toString() == "1") {
                std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << "\n";
                if (type == "adapter" && scanRequested) {
                    host.write(BleIpc::encode({{"op", "scan"}, {"session", "0"}, {"request", "2"}})); continue;
                }
                host.kill(); host.waitForFinished(1000); return 0;
            } else if (type == "scanning" && object.value("request").toString() == "2") {
                scanning = true;
            } else if (type == "device" && object.value("request").toString() == "2") {
                ++advertisements;
            } else if (type == "error" && scanRequested) {
                std::cerr << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << "\n";
                host.kill(); host.waitForFinished(1000); return 1;
            } else if (type == "scanStopped" && object.value("request").toString() == "3" && scanning) {
                std::cout << "Native BLE scan started/stopped successfully; advertisement updates: " << advertisements << "\n";
                host.kill(); host.waitForFinished(1000); return 0;
            }
        }
        if (!error.isEmpty() || host.state() == QProcess::NotRunning) return 1;
        if (scanRequested && scanning && !stopped && clock.elapsed() > 2500) {
            stopped = true; host.write(BleIpc::encode({{"op", "stopScan"}, {"session", "0"}, {"request", "3"}}));
        }
    }
    host.kill(); host.waitForFinished(1000); return 1;
}
