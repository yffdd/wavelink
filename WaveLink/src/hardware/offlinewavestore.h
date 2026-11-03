#pragma once
#include <QFile>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <QVector>
#include <functional>
#include <limits>

struct OfflineFilterSpec {
    bool notch = false, highpass = false, lowpass = false;
    bool automaticRate = true;
    double fs = 250, notchHz = 50, highHz = .1, lowHz = 10;
    int order = 2;
    bool active() const { return notch || highpass || lowpass; }
    bool operator==(const OfflineFilterSpec &b) const {
        return notch == b.notch && highpass == b.highpass && lowpass == b.lowpass && automaticRate == b.automaticRate && fs == b.fs &&
            notchHz == b.notchHz && highHz == b.highHz && lowHz == b.lowHz && order == b.order;
    }
};

// Immutable after finish(). Original doubles live on disk; the sparse peak
// index and one display window are the only persistent memory allocations.
// A store owns a uniquely created temporary directory, never the source file.
class OfflineWaveStore {
public:
    struct Point { double key = 0, value = std::numeric_limits<double>::quiet_NaN(); qint64 row = 0; };
    using Visitor = std::function<bool(qint64, double, const QVector<double> &)>;
    using Progress = std::function<bool(qint64, qint64)>;
    static QSharedPointer<OfflineWaveStore> create(const QString &directory, qint64 indexBudget, QString &error);
    bool append(double time, const QVector<double> &values, QString &error);
    bool finish(double sampleRateHint, double fallbackInterval, bool hostTime, bool forceUniform, QString &error);
    qint64 rows() const { return m_rows; }
    int channels() const { return m_channels; }
    double firstTime() const { return m_uniform ? 0 : m_firstTime; }
    double lastTime() const { return m_uniform ? (m_rows - 1) * m_interval : m_lastTime; }
    qint64 diskBytes() const { return m_file.size(); }
    qint64 indexBytes() const { return m_indexBytes; }
    bool visit(const Visitor &visitor, QString &error) const;
    bool exactChannel(int channel, double lower, double upper, int maximum, QVector<Point> &points, QString &error, bool newest = false) const;
    bool nearby(int channel, double time, QVector<Point> &points, QString &error) const;
    bool display(double lower, double upper, int maximumRows, QVector<QVector<Point>> &curves, bool &exact, QString &error) const;
    QSharedPointer<OfflineWaveStore> filtered(const OfflineFilterSpec &spec, const Progress &progress, QString &error) const;
private:
    struct Summary { Point first, last, minimum, maximum; bool invalid = false; };
    struct Block { qint64 offset = 0, firstRow = 0; int count = 0; double firstTime = 0, lastTime = 0; QVector<Summary> columns; };
    explicit OfflineWaveStore(const QString &directory, qint64 indexBudget);
    bool flushBlock(QString &error);
    bool readBlock(int index, const Visitor &visitor, QString &error) const;
    QPair<int, int> blocksFor(double lower, double upper) const;
    double key(const Point &point) const { return m_uniform ? point.row * m_interval : point.key; }
    double blockFirst(const Block &block) const { return m_uniform ? block.firstRow * m_interval : block.firstTime; }
    double blockLast(const Block &block) const { return m_uniform ? (block.firstRow + block.count - 1) * m_interval : block.lastTime; }
    void endTimeGroup();
    static void add(Summary &summary, const Point &point, bool first);
    static void merge(Summary &summary, const Summary &other, bool first);
    QString m_baseDirectory;
    QTemporaryDir m_directory;
    QFile m_file;
    QByteArray m_buffer;
    QVector<Block> m_blocks;
    Block m_block;
    qint64 m_rows = 0, m_indexBytes = 0, m_indexBudget = 0;
    int m_channels = 0;
    bool m_finished = false, m_badTime = false, m_uniform = false;
    double m_firstTime = 0, m_lastTime = 0, m_interval = 0, m_groupTime = 0;
    qint64 m_groups = 0;
    long double m_meanX = 0, m_meanY = 0, m_covariance = 0, m_variance = 0;
};
