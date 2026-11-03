#include "ble/blefilesource.h"
#include "ble/bleipc.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <functional>
#include <iostream>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

#define VERIFY(x) do { if (!(x)) { std::cerr << "File source failure at " << __LINE__ << ": " #x << "\n"; return 1; } } while (0)
static bool until(const std::function<bool()> &condition, int timeout = 5000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(2); }
    return condition();
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    VERIFY(argc == 2 || argc == 3);
    const QString host = QString::fromLocal8Bit(argv[1]);
    const bool native = argc == 3;
    QTemporaryDir directory; VERIFY(directory.isValid());
    BleFileSource source(nullptr, native ? 5000 : 350);
    qint64 size = -1; QByteArray chunk; bool eof = false; int errors = 0, chunks = 0;
    QObject::connect(&source, &BleFileSource::opened, [&](qint64 value) { size = value; });
    QObject::connect(&source, &BleFileSource::chunkReady, [&](const QByteArray &bytes, bool end) { chunk = bytes; eof = end; ++chunks; });
    QObject::connect(&source, &BleFileSource::failed, [&](const QString &message) { ++errors; std::cout << "Expected error: " << message.toUtf8().constData() << "\n"; });
    QString error;
    auto cleaned = [&] { return !source.cleaningUp(); };
    int transfer = 0;
    for (const int length : {0, 1, 19, 20, 21, 4095, 4096, 4097, 4207}) {
        QFile file(directory.filePath(QStringLiteral("发送文件 %1.bin").arg(length)));
        VERIFY(file.open(QIODevice::WriteOnly));
        QByteArray input; for (int i = 0; i < length; ++i) input.append(char(i % 256));
        VERIFY(file.write(input) == input.size()); file.close();
        size = -1; VERIFY(source.start(host, file.fileName(), "s", QString::number(++transfer), error));
        VERIFY(until([&] { return size == length; }));
        QByteArray output;
        while (output.size() < length) {
            const int before = chunks;
            VERIFY(source.requestChunk(output.size(), qMin(4096, length - output.size())));
            VERIFY(!source.requestChunk(output.size(), 1)); // One demand in flight.
            VERIFY(until([&] { return chunks == before + 1; }));
            output += chunk; VERIFY(eof == (output.size() == length));
        }
        VERIFY(output == input && errors == 0);
        if (native && length) {
            QFile writer(file.fileName()); VERIFY(!writer.open(QIODevice::WriteOnly));
        }
        source.stop(); VERIFY(until(cleaned));
        if (native) { QFile writer(file.fileName()); VERIFY(writer.open(QIODevice::WriteOnly)); }
    }
    int before = errors;
    size = -1; VERIFY(source.start(host, directory.filePath("missing.bin"), "s", QString::number(++transfer), error));
    VERIFY(until([&] { return errors == before + 1 && cleaned(); }));
    if (!native) {
        for (const auto scenario : {"hang-open", "hang-read", "exit", "role", "read-error", "offset", "eof", "encoding", "early"}) {
            before = errors; size = -1;
            VERIFY(source.start(host, "fake:" + QString(scenario), "s", QString::number(++transfer), error));
            if (QString(scenario) != "hang-open" && QString(scenario) != "exit" && QString(scenario) != "role") {
                VERIFY(until([&] { return size == 21; })); VERIFY(source.requestChunk(0, 21));
            }
            VERIFY(until([&] { return errors == before + 1 && cleaned(); }));
        }
        size = -1; before = errors;
        VERIFY(source.start(host, "fake:stale", "s", QString::number(++transfer), error));
        VERIFY(until([&] { return size == 21; })); int oldChunks = chunks;
        VERIFY(source.requestChunk(0, 21)); VERIFY(until([&] { return chunks == oldChunks + 1; }));
        VERIFY(errors == before && chunk == QByteArray(21, 'y') && eof); source.stop(); VERIFY(until(cleaned));
        int beats = 0; QTimer heartbeat; heartbeat.setInterval(10);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; }); heartbeat.start();
        VERIFY(source.start(host, "fake:slow-open", "s", QString::number(++transfer), error));
        VERIFY(until([&] { return beats >= 8; }));
        QElapsedTimer clock; clock.start(); source.stop(); VERIFY(clock.elapsed() < 250);
        VERIFY(until(cleaned)); heartbeat.stop(); VERIFY(errors == before);
        auto destroying = new BleFileSource(nullptr, 5000);
        VERIFY(destroying->start(host, "fake:slow-open", "s", "destruction", error));
        auto child = destroying->findChild<QProcess *>(); VERIFY(child);
        VERIFY(until([&] { return child->state() == QProcess::Running; }));
#ifdef Q_OS_WIN
        HANDLE childExit = OpenProcess(SYNCHRONIZE, FALSE, DWORD(child->processId())); VERIFY(childExit);
#endif
        clock.restart(); delete destroying; VERIFY(clock.elapsed() < 250);
#ifdef Q_OS_WIN
        VERIFY(until([&] { return WaitForSingleObject(childExit, 0) == WAIT_OBJECT_0; })); CloseHandle(childExit);
#endif
        size = -1; VERIFY(source.start(host, "fake:large", "s", QString::number(++transfer), error));
        VERIFY(until([&] { return size == 4294967313LL; }));
        oldChunks = chunks; VERIFY(source.requestChunk(0, 4096)); VERIFY(until([&] { return chunks == oldChunks + 1; }));
        VERIFY(chunk.size() == 4096 && !eof); source.stop(); VERIFY(until(cleaned));
    }
#ifdef Q_OS_WIN
    if (native) {
        const QString path = directory.filePath("large-sparse.bin");
        HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        VERIFY(file != INVALID_HANDLE_VALUE); DWORD done = 0;
        const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &done, nullptr);
        LARGE_INTEGER position{}; position.QuadPart = 4294967313LL;
        const bool sized = sparse && SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && SetEndOfFile(file);
        CloseHandle(file); VERIFY(sized); // Never allocate a real 4 GB test file.
        size = -1; VERIFY(source.start(host, path, "s", QString::number(++transfer), error));
        VERIFY(until([&] { return size == position.QuadPart; }));
        int oldChunks = chunks; VERIFY(source.requestChunk(0, 4096)); VERIFY(until([&] { return chunks == oldChunks + 1; }));
        VERIFY(chunk == QByteArray(4096, '\0') && !eof); source.stop(); VERIFY(until(cleaned));
        // The native control thread notices parent pipe EOF even while its
        // worker is waiting for another read. No Bluetooth mode is started.
        QProcess child; child.start(host, {"--file-reader"});
        QByteArray input; bool hello = false, opened = false;
        auto consume = [&] {
            input += child.readAllStandardOutput(); QJsonObject obj; QString parseError;
            while (BleIpc::take(input, obj, parseError)) {
                if (obj.value("event") == "hello") {
                    hello = obj.value("role") == "file-reader";
                    child.write(BleIpc::encode({{"op", "open"}, {"path", path}, {"session", "s"}, {"transfer", "eof"}, {"request", "1"}}));
                } else if (obj.value("event") == "opened") opened = true;
            }
        };
        QObject::connect(&child, &QProcess::readyReadStandardOutput, consume);
        VERIFY(until([&] { return hello && opened; }));
        child.closeWriteChannel(); VERIFY(until([&] { return child.state() == QProcess::NotRunning; }));
        VERIFY(child.exitCode() == 0);
    }
#endif
    std::cout << (native ? "Native" : "Fake") << " isolated file source tests passed.\n";
    return 0;
}
