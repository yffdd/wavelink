#include "offlinewavestore.h"
#include "waveimportbudget.h"
#include "filter.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#define VERIFY(c) do { if (!(c)) { std::cerr << "Offline store failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QTemporaryDir directory; VERIFY(directory.isValid()); QString error;
    QFile sentinel(directory.filePath("keep.txt")); VERIFY(sentinel.open(QIODevice::WriteOnly)); sentinel.write("KEEP"); sentinel.close();
    {
        auto raw = OfflineWaveStore::create(directory.path(), 16 * 1024 * 1024, error); VERIFY(raw);
        constexpr int Rows = 20000;
        for (int i = 0; i < Rows; ++i) {
            QVector<double> values{double(i == 5001 ? 123400 : i == 18000 ? -432100 : i), i >= 4093 && i <= 4100 ? qQNaN() : std::sin(i * .01)};
            if (i >= 10000) values.append(i * .5);
            VERIFY(raw->append(i * .001, values, error));
        }
        VERIFY(raw->finish(1000, .001, false, false, error));
        VERIFY(raw->rows() == Rows && raw->channels() == 3 && raw->firstTime() == 0 && std::abs(raw->lastTime() - 19.999) < 1e-12);
        QVector<QVector<OfflineWaveStore::Point>> curves; bool exact = true;
        VERIFY(raw->display(0, 20, 128, curves, exact, error) && !exact && curves.size() == 3);
        bool peak = false, trough = false, gap = false;
        for (const auto &p : curves[0]) { peak |= p.value == 123400; trough |= p.value == -432100; }
        for (const auto &p : curves[1]) gap |= std::isnan(p.value);
        VERIFY(peak && trough && gap && curves[0].size() < 128);
        VERIFY(raw->display(10.001, 10.02, 128, curves, exact, error) && exact && curves[0].size() == 20);
        VERIFY(curves[0].first().row == 10001 && curves[0].last().row == 10020);
        QVector<OfflineWaveStore::Point> points;
        VERIFY(raw->nearby(0, 4.096, points, error)); bool boundary = false;
        for (const auto &p : points) boundary |= p.row == 4096; VERIFY(boundary);
        VERIFY(raw->exactChannel(1, 4.09, 4.11, 100, points, error)); int invalid = 0;
        for (const auto &p : points) invalid += std::isnan(p.value); VERIFY(invalid == 8);
        VERIFY(raw->exactChannel(2, 0, 1, 2000, points, error)); for (const auto &p : points) VERIFY(std::isnan(p.value));
        VERIFY(raw->exactChannel(0, 0, 20, 32, points, error, true) && points.size() == 32 && points.first().row == Rows - 32);
        VERIFY(!raw->exactChannel(0, 0, 20, 10, points, error) && points.isEmpty() && !error.isEmpty()); error.clear();
        OfflineFilterSpec spec; spec.lowpass = true; spec.lowHz = 20; spec.fs = 1000; spec.order = 4;
        auto filtered = raw->filtered(spec, [](qint64, qint64) { return true; }, error); VERIFY(filtered);
        BiquadCascade reference; reference.configure(BiquadFilter::LOWPASS, 1000, 20, 4);
        VERIFY(filtered->exactChannel(0, 0, 20, Rows, points, error) && points.size() == Rows);
        for (int i = 0; i < Rows; ++i) {
            const double value = i == 5001 ? 123400 : i == 18000 ? -432100 : i;
            VERIFY(std::abs(points[i].value - reference.process(value)) < 1e-9);
        }
        VERIFY(!raw->filtered(spec, [](qint64 row, qint64) { return row < 4096; }, error));
        qint64 seen = 0; VERIFY(raw->visit([&](qint64 row, double t, const QVector<double> &v) {
            if (row == 0 && (t != 0 || v[0] != 0 || !std::isnan(v[2]))) return false;
            if (row == Rows - 1 && (v[0] != Rows - 1 || v[2] != (Rows - 1) * .5)) return false;
            ++seen; return true;
        }, error) && seen == Rows);
        auto host = OfflineWaveStore::create(directory.path(), 1024 * 1024, error); VERIFY(host);
        for (int i = 0; i < 20; ++i) VERIFY(host->append(1 + (i / 10) * .1, {double(i)}, error));
        VERIFY(host->finish(1000, .001, true, false, error) && std::abs(host->lastTime() - .19) < 1e-12);
        auto limited = OfflineWaveStore::create(directory.path(), 1, error); VERIFY(limited);
        VERIFY(limited->append(0, {1}, error) && !limited->finish(1, 1, false, false, error));
    }
    VERIFY(QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty() && sentinel.exists());
    VERIFY(WaveImportBudget::bytesForAvailableMemory(8ULL * 1024 * 1024 * 1024, 512ULL * 1024 * 1024) == 512LL * 1024 * 1024);
    std::cout << "Disk-backed exact reads, bounded peak overview, sparse boundaries, gaps, dynamic channels, continuous filtering, cancellation and cleanup passed.\n";
    return 0;
}
