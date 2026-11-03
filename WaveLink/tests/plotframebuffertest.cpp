#include "plotframebuffer.h"
#include <QCoreApplication>
#include <thread>
#include <atomic>
#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #condition); \
    return 1; } } while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    PlotFrameBuffer buffer;
    quint64 dropped = 0;
    buffer.push({});
    buffer.push(QVector<double>(257, 1.0));
    CHECK(buffer.take(128, dropped).isEmpty() && dropped == 0);
    for (int i = 0; i < 100; ++i) buffer.push({double(i), -double(i)});
    auto first = buffer.take(32, dropped);
    CHECK(first.size() == 32 && dropped == 0);
    for (int i = 0; i < first.size(); ++i) CHECK(first[i][0] == i);
    CHECK(buffer.take(0, dropped).isEmpty());
    CHECK(buffer.take(100, dropped).size() == 68);

    for (int i = 0; i < 10000; ++i) buffer.push({double(i)});
    auto bounded = buffer.take(20000, dropped);
    CHECK(bounded.size() == 8192 && dropped == 1808);
    CHECK(bounded.first()[0] == 1808 && bounded.last()[0] == 9999);
    for (int i = 0; i < 10000; ++i) buffer.push(QVector<double>(256, double(i)));
    bounded = buffer.take(20000, dropped);
    CHECK(bounded.size() * (256 * 8 + 64) <= 8 * 1024 * 1024);
    CHECK(quint64(bounded.size()) + dropped == 10000);
    buffer.push({1.0});
    buffer.clear();
    CHECK(buffer.take(128, dropped).isEmpty() && dropped == 0);

    std::atomic<bool> finished{false};
    std::thread producer([&]() {
        for (int i = 0; i < 100000; ++i) buffer.push({double(i)});
        finished.store(true);
    });
    quint64 accounted = 0;
    double previous = -1;
    bool ordered = true;
    for (;;) {
        const bool done = finished.load();
        const auto batch = buffer.take(128, dropped);
        accounted += dropped + batch.size();
        for (const auto &frame : batch) {
            ordered = ordered && frame[0] > previous;
            previous = frame[0];
        }
        if (done && batch.isEmpty()) break;
        std::this_thread::yield();
    }
    producer.join();
    CHECK(ordered && accounted == 100000);
    std::puts("Plot mailbox tests passed: order, frame/byte bounds, clear, concurrent delivery.");
    return 0;
}
