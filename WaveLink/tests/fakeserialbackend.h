#ifndef FAKESERIALBACKEND_H
#define FAKESERIALBACKEND_H

#include "serialhandler.h"
#include <atomic>

// No native handle is ever opened. Only the QIODevice layer and injected
// control-line implementations are used, including in full-window tests.
class FakePort : public QSerialPort {
public:
    bool open(OpenMode mode) override { return QIODevice::open(mode); }
    void close() override { pendingBytes = 0; nativeWaiting.clear(); QIODevice::close(); }
    qint64 bytesToWrite() const override { return pendingBytes; }
    qint64 pendingBytes = 0;
    QByteArray written;
    QByteArray nativeWaiting;
    int *cts = nullptr;
    bool holdWrites = false;
    void drainWrites() {
        const auto count = pendingBytes; pendingBytes = 0;
        written.append(nativeWaiting); nativeWaiting.clear();
        if (count) emit bytesWritten(count);
    }
    void fail(SerialPortError error) {
        setErrorString(QStringLiteral("Simulated driver error %1").arg(static_cast<int>(error)));
        emit errorOccurred(error);
    }
protected:
    qint64 writeData(const char *data, qint64 length) override {
        nativeWaiting.append(data, static_cast<int>(length)); pendingBytes += length;
        if (!holdWrites && !(flowControl() == HardwareControl && cts && *cts == 0)) drainWrites();
        return length;
    }
};

class TestSerialHandler : public SerialHandler {
public:
    explicit TestSerialHandler(FakePort *port) : SerialHandler(nullptr, port), fake(port) { fake->cts = &ctsState; }
    FakePort *fake;
    QVector<QPair<bool, bool>> calls;
    QSerialPort::SerialPortError dtrError = QSerialPort::NoError;
    QSerialPort::SerialPortError rtsError = QSerialPort::NoError;
    QSerialPort::SerialPortError breakEnableError = QSerialPort::NoError;
    QSerialPort::SerialPortError breakReleaseError = QSerialPort::NoError;
    bool failedAfterChange = false;
    int ctsState = 1;
    QSerialPort::SerialPortError flowError = QSerialPort::NoError;
    QSerialPort::SerialPortError ctsError = QSerialPort::NoError;
    QVector<QSerialPort::FlowControl> flowCalls;
    std::atomic<int> breakAssertions{0}, breakReleases{0};
    std::atomic<bool> breakLineState{false};
protected:
    bool configureFlowControl(QSerialPort::FlowControl flow) override {
        flowCalls.append(flow);
        if (flowError != QSerialPort::NoError) { fake->fail(flowError); return false; }
        return SerialHandler::configureFlowControl(flow);
    }
    int readCtsState() override {
        if (ctsError != QSerialPort::NoError) { fake->fail(ctsError); return -1; }
        if (ctsState == 1 && !fake->holdWrites && fake->pendingBytes > 0) fake->drainWrites();
        return ctsState;
    }
    bool applyControlLine(bool dtr, bool enabled) override {
        calls.append(qMakePair(dtr, enabled));
        const auto error = dtr ? dtrError : rtsError;
        if (error == QSerialPort::NoError) return true;
        fake->fail(error);
        if (failedAfterChange) fake->fail(QSerialPort::UnsupportedOperationError);
        return false;
    }
    bool applyBreakLine(bool enabled) override {
        if (enabled) ++breakAssertions; else ++breakReleases;
        // Simulate a driver failing AFTER changing the native signal too.
        breakLineState = enabled;
        const auto error = enabled ? breakEnableError : breakReleaseError;
        if (error == QSerialPort::NoError) return true;
        fake->fail(error);
        if (failedAfterChange) fake->fail(QSerialPort::UnsupportedOperationError);
        return false;
    }
};

#endif
