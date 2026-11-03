#include "filetransfereta.h"
#include <QCoreApplication>
#include <iostream>
#define VERIFY(c) do { if (!(c)) { std::cerr << "File ETA failure " << __LINE__ << ": " #c << '\n'; return 1; } } while (0)
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    FileTransferEta eta;
    VERIFY(eta.text(0) == "00:00:00" && !eta.active());
    eta.start(); VERIFY(eta.active() && eta.text(0) == "--:--:--");
    eta.observe(0, 0, -1); VERIFY(eta.text(100) == "--:--:--");
    eta.observe(1000, 1000, 10000); VERIFY(eta.text(1000) == "00:00:09");
    eta.observe(2000, 2000, 10000); VERIFY(eta.text(2000) == "00:00:08");
    // Waiting for CTS or GATT must not predict from bytes read ahead.
    eta.observe(3000, 2000, 10000); VERIFY(eta.text(3000) == "00:00:12");
    VERIFY(eta.text(10000) == "--:--:--"); // stalled, no fabricated zero/success
    eta.observe(11000, 9000, 10000); VERIFY(eta.text(11000) != "--:--:--");
    eta.observe(12000, 10000, 10000); VERIFY(eta.text(12000) == "00:00:00");
    eta.reset(); VERIFY(!eta.active() && eta.text(12000) == "00:00:00");
    eta.observe(13000, 1, 10000); VERIFY(eta.text(13000) == "00:00:00"); // late progress cannot restart a stopped send
    eta.start(); eta.observe(1000, 1024, 10LL * 1024 * 1024 * 1024);
    VERIFY(eta.text(1000) == FileTransferEta::format(10LL * 1024 * 1024 - 1));
    VERIFY(FileTransferEta::format(0) == "00:00:00" && FileTransferEta::format(3661) == "01:01:01");
    VERIFY(FileTransferEta::format(90061) == "25:01:01");
    VERIFY(FileTransferEta::format(-1) == "00:00:00");
    // The recent-rate window adapts after a speed change and stays bounded.
    eta.start();
    for (int t = 250; t <= 20000; t += 250) eta.observe(t, t / 10, 10000);
    VERIFY(eta.text(20000) == "00:01:20");
    for (int t = 20250; t <= 30000; t += 250) eta.observe(t, 2000 + (t - 20000) / 5, 10000);
    VERIFY(eta.text(30000) == "00:00:30");
    eta.reset(); VERIFY(eta.text(0) == "00:00:00");
    std::cout << "Transfer ETA: actual bytes, warm-up, stalls, speed changes, stop, restart, >24h and large files passed.\n";
}
