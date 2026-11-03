#include "waveformgraph.h"
#include <algorithm>
#include <cmath>

namespace {
int blockCapacity(int count) {
    int capacity = 1;
    while (capacity < count) capacity *= 2;
    return capacity;
}
}

WaveformGraph::Summary WaveformGraph::sample(const QCPGraphDataContainer &data, int index) const {
    return std::isfinite(data.at(index)->value) ? Summary{index, index, 0} : Summary{-1, -1, 1};
}
WaveformGraph::Summary WaveformGraph::combine(const QCPGraphDataContainer &data, const Summary &a, const Summary &b) {
    Summary out = a; out.invalid += b.invalid;
    if (b.minimum >= 0 && (out.minimum < 0 || data.at(b.minimum)->value < data.at(out.minimum)->value)) out.minimum = b.minimum;
    if (b.maximum >= 0 && (out.maximum < 0 || data.at(b.maximum)->value > data.at(out.maximum)->value)) out.maximum = b.maximum;
    return out;
}
void WaveformGraph::releaseDisplayCache() const {
    m_levels.clear(); m_levels.squeeze(); m_owner.clear(); m_count = 0;
}
qint64 WaveformGraph::cacheBytes() const {
    qint64 bytes = m_levels.capacity() * qint64(sizeof(QVector<Summary>));
    for (const auto &level : m_levels) bytes += level.capacity() * qint64(sizeof(Summary));
    return bytes;
}
qint64 WaveformGraph::projectedCacheBytes() const {
    // Pre-budget the lazy index, including power-of-two growth headroom, so
    // building it during a repaint cannot push history beyond its configured cap.
    if (data()->isEmpty()) return cacheBytes();
    const qint64 blocks = blockCapacity((data()->size() + BlockSize - 1) / BlockSize);
    return qMax(cacheBytes(), (2 * blocks - 1) * qint64(sizeof(Summary)) + 32 * qint64(sizeof(QVector<Summary>)));
}
void WaveformGraph::synchronize() const {
    const auto source = data();
    if (m_owner.toStrongRef() != source || m_revision != source->prefixRevision() || source->size() < m_count) releaseDisplayCache();
    if (source->size() == m_count) return;
    m_owner = source; m_revision = source->prefixRevision();
    int first = m_count / BlockSize;
    const int count = source->size(), blocks = (count + BlockSize - 1) / BlockSize;
    if (m_levels.isEmpty()) m_levels.resize(1);
    if (m_levels[0].capacity() < blocks) m_levels[0].reserve(blockCapacity(blocks));
    m_levels[0].resize(blocks);
    for (int block = first; block < blocks; ++block) {
        Summary summary;
        for (int i = block * BlockSize; i < qMin(count, (block + 1) * BlockSize); ++i) {
            summary = combine(*source, summary, sample(*source, i)); ++m_indexedSamples;
        }
        m_levels[0][block] = summary;
    }
    int width = blocks, level = 1;
    while (width > 1) {
        first /= 2;
        const int nextWidth = (width + 1) / 2;
        if (m_levels.size() <= level) m_levels.resize(level + 1);
        if (m_levels[level].capacity() < nextWidth) m_levels[level].reserve(blockCapacity(nextWidth));
        m_levels[level].resize(nextWidth);
        for (int i = first; i < nextWidth; ++i) m_levels[level][i] = combine(*source, m_levels[level - 1][2 * i],
            2 * i + 1 < width ? m_levels[level - 1][2 * i + 1] : Summary{});
        width = nextWidth; ++level;
    }
    m_count = count;
}
WaveformGraph::Summary WaveformGraph::summarize(int begin, int end) const {
    const auto source = data(); Summary result;
    auto addSample = [&](int index) { result = combine(*source, result, sample(*source, index)); ++m_rangeSamples; };
    while (begin < end && begin % BlockSize) addSample(begin++);
    const int tail = end - end % BlockSize;
    int left = begin / BlockSize, right = qMax(begin, tail) / BlockSize, level = 0;
    while (left < right) {
        if (left & 1) result = combine(*source, result, m_levels[level][left++]);
        if (right & 1) result = combine(*source, result, m_levels[level][--right]);
        left /= 2; right /= 2; ++level;
    }
    for (int i = qMax(begin, tail); i < end; ++i) addSample(i);
    return result;
}
QCPRange WaveformGraph::getValueRange(bool &found, QCP::SignDomain domain, const QCPRange &keys) const {
    if (domain != QCP::sdBoth) return QCPGraph::getValueRange(found, domain, keys);
    synchronize(); m_rangeSamples = 0;
    const auto source = data();
    const int begin = keys == QCPRange() ? 0 : int(source->findBegin(keys.lower, false) - source->constBegin());
    const int end = keys == QCPRange() ? source->size() : int(source->findEnd(keys.upper, false) - source->constBegin());
    const auto result = summarize(begin, end); found = result.minimum >= 0;
    return found ? QCPRange(source->at(result.minimum)->value, source->at(result.maximum)->value) : QCPRange();
}
void WaveformGraph::emitNode(const QCPGraphDataContainer &source, QVector<QCPGraphData> &output, int level, int node, int begin, int end, double pixelWidth) const {
    const qint64 width = qint64(BlockSize) << level;
    const int first = int(qMax<qint64>(begin, width * node));
    const int last = int(qMin<qint64>(end, qMin<qint64>(m_count, width * (node + 1)))) - 1;
    if (last < first) return;
    const auto &summary = m_levels[level][node];
    const bool complete = first == width * node && last + 1 == qMin<qint64>(m_count, width * (node + 1));
    // A summary is drawn only within one device pixel. Use original sample
    // positions, retain peaks/troughs and never join across an invalid run.
    if (complete && summary.minimum < 0) { output.append(QCPGraphData(source.at(first)->key, qQNaN())); return; }
    if (complete && summary.invalid == 0 && std::abs(keyAxis()->coordToPixel(source.at(last)->key) - keyAxis()->coordToPixel(source.at(first)->key)) <= pixelWidth) {
        int indices[]{first, summary.minimum, summary.maximum, last}; std::sort(std::begin(indices), std::end(indices));
        int previous = -1;
        for (int index : indices) if (index != previous) { output.append(*source.at(index)); previous = index; }
    } else if (level > 0) {
        emitNode(source, output, level - 1, node * 2, begin, end, pixelWidth);
        if (node * 2 + 1 < m_levels[level - 1].size()) emitNode(source, output, level - 1, node * 2 + 1, begin, end, pixelWidth);
    } else {
        for (int index = first; index <= last; ++index) {
            auto point = *source.at(index); if (!std::isfinite(point.value)) point.value = qQNaN();
            output.append(point); ++m_drawSamples;
        }
    }
}
void WaveformGraph::getOptimizedLineData(QVector<QCPGraphData> *output,
    const QCPGraphDataContainer::const_iterator &begin, const QCPGraphDataContainer::const_iterator &end) const {
    if (!output) return;
    m_drawSamples = 0;
    const int count = int(end - begin);
    if (!adaptiveSampling() || lineStyle() != lsLine || keyAxis()->scaleType() != QCPAxis::stLinear) {
        m_drawSamples = count; QCPGraph::getOptimizedLineData(output, begin, end); return;
    }
    const double ratio = qMax(1.0, parentPlot()->bufferDevicePixelRatio());
    const int pixels = qMax(1, qRound(keyAxis()->axisRect()->width() * ratio));
    if (count < qMax(256, pixels * 2)) {
        // Sparse traces need no decimation. Keep exact positions and gaps;
        // the base adaptive path can otherwise synthesize points/skip NaNs.
        output->clear(); output->reserve(count);
        for (auto it = begin; it != end; ++it) {
            auto point = *it;
            if (!std::isfinite(point.value)) point.value = qQNaN();
            output->append(point);
        }
        m_drawSamples = count;
        return;
    }
    synchronize(); output->clear();
    if (m_levels.isEmpty()) return;
    const auto source = data();
    emitNode(*source, *output, m_levels.size() - 1, 0, int(begin - source->constBegin()), int(end - source->constBegin()), 1.0 / ratio);
}
