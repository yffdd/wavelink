#include "qtfontwarningfilter.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVector>
#include <QDebug>
#include <atomic>

#define CHECK(c) do { if (!(c)) { qCritical() << "Font warning filter failure" << __LINE__ << #c; return 1; } } while (0)
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString warning = QStringLiteral("  OpenType support missing for \"Consolas\", script 66");
    QtFontWarningFilter filter;
    const auto first = filter.messageForLog(QtWarningMsg, warning);
    CHECK(first.contains(warning.trimmed()) && first.contains("仅记录一次") && first.contains("HEX 接收"));
    CHECK(filter.messageForLog(QtWarningMsg, warning).isEmpty());
    // All fallback fonts and scripts share a bounded, per-process diagnostic.
    for (const auto &font : {"SimSun", "Arial", "MS UI Gothic", "Arial Unicode MS", "Segoe UI Emoji", "Segoe UI Symbol", "Ebrima", ""})
        for (int script : {0, 66, 132})
            CHECK(filter.messageForLog(QtWarningMsg, QString("OpenType support missing for \"%1\", script %2").arg(font).arg(script)).isEmpty());
    // No blanket filtering of other warnings, malformed messages or critical/fatal failures.
    for (const auto &other : {QStringLiteral("串口 COM3 读取失败"), QStringLiteral("录制文件写入失败"),
                            QStringLiteral("OpenType support missing for \"font\", script invalid"),
                            QStringLiteral("OpenType support missing for \"font\", script 66; additional failure")})
        CHECK(filter.messageForLog(QtWarningMsg, other) == other);
    for (auto type : {QtCriticalMsg, QtFatalMsg, QtInfoMsg, QtDebugMsg})
        CHECK(filter.messageForLog(type, warning) == warning);
    QtFontWarningFilter nextRun;
    CHECK(!nextRun.messageForLog(QtWarningMsg, warning).isEmpty());
    QtFontWarningFilter concurrent;
    std::atomic<int> recorded {0}; QVector<QThread *> threads;
    for (int i = 0; i < 8; ++i) {
        auto thread = QThread::create([&] {
            for (int n = 0; n < 2500; ++n)
                if (!concurrent.messageForLog(QtWarningMsg, warning).isEmpty()) ++recorded;
        });
        threads.append(thread); thread->start();
    }
    for (auto thread : threads) { thread->wait(); delete thread; }
    CHECK(recorded == 1);
    QElapsedTimer elapsed; elapsed.start();
    for (int n = 0; n < 100000; ++n) CHECK(filter.messageForLog(QtWarningMsg, warning).isEmpty());
    qInfo() << "PASS: one font advisory, other warnings/errors preserved, 20000 concurrent messages; 100000 repeats in" << elapsed.elapsed() << "ms";
}
