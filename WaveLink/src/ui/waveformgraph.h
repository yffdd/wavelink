#pragma once
#include "qcustomplot.h"
#include <functional>

// Display index. Paged files provide a separate exact-sample source; callers
// must use readExact/nearbySamples for analysis rather than overview vertices.
class WaveformGraph : public QCPGraph {
public:
    WaveformGraph(QCPAxis *keyAxis, QCPAxis *valueAxis) : QCPGraph(keyAxis, valueAxis) {
        setPen(displayPen(Qt::blue));
        setAntialiased(false);
        setAdaptiveSampling(true);
        // Qt's wide-polyline stroker is costly even for cosmetic pens. The
        // fast path draws cosmetic segments directly, including widths 2/3.
        parentPlot()->setPlottingHint(QCP::phFastPolylines, true);
    }
    static QPen displayPen(const QColor &color, int width = 1) {
        QPen pen(color);
        // Width is in device pixels, including HiDPI and CPU fallback. Keep
        // pens cosmetic: scaled wide pens are costly on dense, steep traces.
        pen.setWidthF(width >= 1 && width <= 3 ? width : 1);
        pen.setCosmetic(true);
        return pen;
    }
    // Names can repeat or change; grouped views route data/removal by channel.
    void setChannelIndex(int channel) { m_channelIndex = channel; }
    int channelIndex() const { return m_channelIndex; }
    using ExactReader = std::function<bool(double, double, int, QVector<QCPGraphData> &, QString &, bool)>;
    using NearbyReader = std::function<bool(double, QVector<QCPGraphData> &, QString &)>;
    void setOfflineSource(qint64 count, ExactReader reader, NearbyReader nearby) { m_sourceRows = count; m_exactReader = std::move(reader); m_nearbyReader = std::move(nearby); }
    void clearOfflineSource() { m_sourceRows = -1; m_exactReader = {}; m_nearbyReader = {}; }
    bool hasOfflineSource() const { return bool(m_exactReader); }
    qint64 sourceSampleCount() const { return hasOfflineSource() ? m_sourceRows : dataCount(); }
    bool readExact(double lower, double upper, int maximum, QVector<QCPGraphData> &points, QString &error, bool newest = false) const {
        if (m_exactReader) return m_exactReader(lower, upper, maximum, points, error, newest);
        points.clear(); const auto source = data(); auto first = source->findBegin(lower, false), end = source->findEnd(upper, false);
        if (end - first > maximum) {
            if (newest) first = end - maximum;
            else { error = QString("区间超过 %1 个原始采样点，请缩小范围。").arg(maximum); return false; }
        }
        points.reserve(int(end - first)); for (auto it = first; it != end; ++it) points.append(*it); return true;
    }
    bool nearbySamples(double time, QVector<QCPGraphData> &points, QString &error) const {
        if (m_nearbyReader) return m_nearbyReader(time, points, error);
        points.clear(); const auto source = data(); const int at = int(source->findBegin(time, false) - source->constBegin());
        for (int i = qMax(0, at - 2); i < qMin(source->size(), at + 2); ++i) points.append(*source->at(i));
        return !points.isEmpty();
    }
    static WaveformGraph *forChannel(QCustomPlot *plot, int channel) {
        if (!plot) return nullptr;
        for (int i = 0; i < plot->graphCount(); ++i) {
            auto graph = dynamic_cast<WaveformGraph *>(plot->graph(i));
            if (graph && graph->m_channelIndex == channel) return graph;
        }
        return nullptr;
    }
    QCPRange getValueRange(bool &found, QCP::SignDomain domain = QCP::sdBoth,
                           const QCPRange &keys = QCPRange()) const override;
    qint64 cacheBytes() const;
    qint64 projectedCacheBytes() const;
    void releaseDisplayCache() const;
    quint64 indexedSamples() const { return m_indexedSamples; }
    int lastRangeSamples() const { return m_rangeSamples; }
    int lastDrawSamples() const { return m_drawSamples; }
protected:
    void getOptimizedLineData(QVector<QCPGraphData> *output,
        const QCPGraphDataContainer::const_iterator &begin,
        const QCPGraphDataContainer::const_iterator &end) const override;
private:
    ExactReader m_exactReader;
    NearbyReader m_nearbyReader;
    qint64 m_sourceRows = -1;
    int m_channelIndex = -1;
    static constexpr int BlockSize = 8;
    struct Summary { int minimum = -1, maximum = -1, invalid = 0; };
    Summary sample(const QCPGraphDataContainer &data, int index) const;
    static Summary combine(const QCPGraphDataContainer &data, const Summary &a, const Summary &b);
    void synchronize() const;
    Summary summarize(int begin, int end) const;
    void emitNode(const QCPGraphDataContainer &source, QVector<QCPGraphData> &output, int level, int node, int begin, int end, double pixelWidth) const;
    mutable QWeakPointer<QCPGraphDataContainer> m_owner;
    mutable QVector<QVector<Summary>> m_levels;
    mutable quint64 m_revision = 0, m_indexedSamples = 0;
    mutable int m_count = 0, m_rangeSamples = 0, m_drawSamples = 0;
};
