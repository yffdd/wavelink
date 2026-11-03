#include "offlinewavestore.h"
#include "filter.h"
#include <QDir>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int BlockRows = 4096;
bool fail(QString &error, const QString &text) { error = text; return false; }
}
OfflineWaveStore::OfflineWaveStore(const QString &directory, qint64 indexBudget)
    : m_baseDirectory(directory), m_directory(QDir(directory).absoluteFilePath("WaveLink_Browse_XXXXXX")), m_indexBudget(indexBudget) {}
QSharedPointer<OfflineWaveStore> OfflineWaveStore::create(const QString &directory, qint64 indexBudget, QString &error) {
    if (!QDir().mkpath(directory)) { error = "无法创建浏览缓存目录：" + directory; return {}; }
    QSharedPointer<OfflineWaveStore> store(new OfflineWaveStore(directory, indexBudget));
    store->m_file.setFileName(store->m_directory.filePath("samples.bin"));
    if (!store->m_directory.isValid() || !store->m_file.open(QIODevice::ReadWrite)) {
        error = "无法创建大文件浏览缓存，请检查缓存路径、写入权限和磁盘空间：" + directory; return {};
    }
    return store;
}
void OfflineWaveStore::add(Summary &s, const Point &p, bool first) {
    if (first) { s.first = p; s.minimum = p; s.maximum = p; }
    s.last = p;
    if (!std::isfinite(p.value)) { s.invalid = true; return; }
    if (!std::isfinite(s.minimum.value) || p.value < s.minimum.value) s.minimum = p;
    if (!std::isfinite(s.maximum.value) || p.value > s.maximum.value) s.maximum = p;
}
void OfflineWaveStore::merge(Summary &s, const Summary &b, bool first) {
    if (first) { s = b; return; }
    s.last = b.last; s.invalid = s.invalid || b.invalid;
    if (std::isfinite(b.minimum.value) && (!std::isfinite(s.minimum.value) || b.minimum.value < s.minimum.value)) s.minimum = b.minimum;
    if (std::isfinite(b.maximum.value) && (!std::isfinite(s.maximum.value) || b.maximum.value > s.maximum.value)) s.maximum = b.maximum;
}
void OfflineWaveStore::endTimeGroup() {
    const long double x = m_rows - 1, y = m_groupTime;
    ++m_groups; const auto dx = x - m_meanX, dy = y - m_meanY;
    m_meanX += dx / m_groups; m_meanY += dy / m_groups;
    m_covariance += dx * (y - m_meanY); m_variance += dx * (x - m_meanX);
}
bool OfflineWaveStore::append(double time, const QVector<double> &values, QString &error) {
    if (m_finished || !std::isfinite(time) || values.isEmpty() || values.size() > 256)
        return fail(error, "浏览缓存遇到无效时间、通道数量或写入状态。");
    if (m_rows == 0) { m_firstTime = time; m_groupTime = time; }
    else {
        if (time <= m_lastTime) m_badTime = true;
        if (std::abs(time - m_groupTime) > 1e-6) { endTimeGroup(); m_groupTime = time; }
    }
    if (m_block.count == 0) { m_block.offset = m_file.pos(); m_block.firstRow = m_rows; m_block.firstTime = time; }
    m_channels = qMax(m_channels, values.size());
    const int previousColumns = m_block.columns.size(); m_block.columns.resize(m_channels);
    for (int ch = previousColumns; ch < m_channels; ++ch) if (m_block.count) {
        auto &s = m_block.columns[ch]; s.invalid = true;
        s.first = {m_block.firstTime, qQNaN(), m_block.firstRow};
    }
    for (int ch = 0; ch < m_channels; ++ch)
        add(m_block.columns[ch], {time, ch < values.size() ? values[ch] : qQNaN(), m_rows}, m_block.count == 0);
    const quint32 count = quint32(values.size()); const int size = int(sizeof(count) + sizeof(double) * (count + 1));
    const int offset = m_buffer.size(); m_buffer.resize(offset + size);
    auto bytes = m_buffer.data() + offset; std::memcpy(bytes, &count, sizeof(count));
    std::memcpy(bytes + sizeof(count), &time, sizeof(time));
    std::memcpy(bytes + sizeof(count) + sizeof(time), values.constData(), count * sizeof(double));
    m_lastTime = time; m_block.lastTime = time; ++m_rows; ++m_block.count;
    return m_block.count < BlockRows || flushBlock(error);
}
bool OfflineWaveStore::flushBlock(QString &error) {
    if (!m_block.count) return true;
    const qint64 nextBytes = sizeof(Block) + qint64(m_block.columns.size()) * sizeof(Summary);
    if (m_indexBytes + nextBytes > m_indexBudget)
        return fail(error, "大文件索引已达到内存额度。请在“系统设置 → 绘图显示”提高离线完整加载上限，或将文件整理为较小的独立文件。");
    if (m_file.write(m_buffer) != m_buffer.size()) return fail(error, "写入浏览缓存失败：" + m_file.errorString());
    m_indexBytes += nextBytes; m_blocks.append(std::move(m_block)); m_block = {}; m_buffer.clear();
    return true;
}
bool OfflineWaveStore::finish(double sampleRateHint, double fallbackInterval, bool hostTime, bool forceUniform, QString &error) {
    if (m_finished || !m_rows) return fail(error, "浏览缓存为空或已经完成。");
    endTimeGroup();
    if (!flushBlock(error) || !m_file.flush()) return fail(error, error.isEmpty() ? "浏览缓存保存失败：" + m_file.errorString() : error);
    if (hostTime || forceUniform || m_badTime) {
        m_interval = hostTime && m_variance > 0 ? double(m_covariance / m_variance) : 0;
        if (!(m_interval > 0) && sampleRateHint > 0) m_interval = 1.0 / sampleRateHint;
        if (!(m_interval > 0) || !std::isfinite(m_interval)) m_interval = fallbackInterval;
        if (!(m_interval > 0) || !std::isfinite(m_interval)) return fail(error, "无法重建文件时间轴，请核对采样率。");
        m_uniform = true;
    }
    m_finished = true; m_file.close(); return true;
}
bool OfflineWaveStore::readBlock(int index, const Visitor &visitor, QString &error) const {
    if (!m_finished || index < 0 || index >= m_blocks.size()) return fail(error, "浏览缓存索引无效。");
    QFile input(m_file.fileName()); const auto &block = m_blocks[index];
    const qint64 end = index + 1 < m_blocks.size() ? m_blocks[index + 1].offset : input.size();
    if (!input.open(QIODevice::ReadOnly) || !input.seek(block.offset)) return fail(error, "无法读取浏览缓存，请重新导入文件。");
    // QFile::size also works before open; validate the exact block length.
    const auto bytes = input.read(end - block.offset); int pos = 0; QVector<double> values;
    for (int i = 0; i < block.count; ++i) {
        if (bytes.size() - pos < int(sizeof(quint32) + sizeof(double))) return fail(error, "浏览缓存被截断，请重新导入。");
        quint32 count; double time; std::memcpy(&count, bytes.constData() + pos, sizeof(count)); pos += sizeof(count);
        std::memcpy(&time, bytes.constData() + pos, sizeof(time)); pos += sizeof(time);
        if (!count || count > quint32(m_channels) || bytes.size() - pos < qint64(count) * qint64(sizeof(double)))
            return fail(error, "浏览缓存内容无效，请重新导入。");
        values.fill(qQNaN(), m_channels); std::memcpy(values.data(), bytes.constData() + pos, count * sizeof(double)); pos += count * sizeof(double);
        const qint64 row = block.firstRow + i;
        if (!visitor(row, m_uniform ? row * m_interval : time, values)) return false;
    }
    return true;
}
QPair<int, int> OfflineWaveStore::blocksFor(double lower, double upper) const {
    const auto first = std::lower_bound(m_blocks.cbegin(), m_blocks.cend(), lower, [this](const Block &b, double t) { return blockLast(b) < t; });
    const auto end = std::upper_bound(first, m_blocks.cend(), upper, [this](double t, const Block &b) { return t < blockFirst(b); });
    return {int(first - m_blocks.cbegin()), int(end - m_blocks.cbegin())};
}
bool OfflineWaveStore::visit(const Visitor &visitor, QString &error) const {
    for (int b = 0; b < m_blocks.size(); ++b) if (!readBlock(b, visitor, error)) return false;
    return true;
}
bool OfflineWaveStore::exactChannel(int channel, double lower, double upper, int maximum, QVector<Point> &points, QString &error, bool newest) const {
    points.clear(); if (channel < 0 || channel >= m_channels || maximum <= 0) return fail(error, "读取的通道或点数上限无效。");
    const auto range = blocksFor(lower, upper);
    if (!newest) {
        qint64 estimated = 0;
        for (int b = range.first; b < range.second; ++b) {
            estimated += m_blocks[b].count;
            if (estimated > qint64(maximum) + 2 * BlockRows)
                return fail(error, QString("区间超过 %1 个原始采样点，请缩小时间范围后测量或分析。").arg(maximum));
        }
    }
    if (newest) {
        for (int b = range.second - 1; b >= range.first && points.size() < maximum; --b) {
            QVector<Point> part;
            if (!readBlock(b, [&](qint64 row, double t, const QVector<double> &v) { if (t >= lower && t <= upper) part.append({t, v[channel], row}); return true; }, error)) return false;
            for (int i = part.size() - 1; i >= 0 && points.size() < maximum; --i) points.append(part[i]);
        }
        std::reverse(points.begin(), points.end()); return true;
    }
    for (int b = range.first; b < range.second; ++b) if (!readBlock(b, [&](qint64 row, double t, const QVector<double> &v) {
        if (t < lower || t > upper) return true;
        if (points.size() >= maximum) return fail(error, QString("区间超过 %1 个原始采样点，请缩小时间范围后测量或分析。").arg(maximum));
        points.append({t, v[channel], row}); return true;
    }, error)) { points.clear(); return false; }
    return true;
}
bool OfflineWaveStore::nearby(int channel, double time, QVector<Point> &points, QString &error) const {
    points.clear(); if (channel < 0 || channel >= m_channels || m_blocks.isEmpty()) return false;
    const int at = qBound(0, blocksFor(time, time).first, m_blocks.size() - 1); QVector<Point> candidates;
    for (int b = qMax(0, at - 1); b <= qMin(m_blocks.size() - 1, at + 1); ++b)
        if (!readBlock(b, [&](qint64 row, double t, const QVector<double> &v) { candidates.append({t, v[channel], row}); return true; }, error)) return false;
    const auto it = std::lower_bound(candidates.cbegin(), candidates.cend(), time, [](const Point &p, double t) { return p.key < t; });
    const int i = int(it - candidates.cbegin());
    for (int j = qMax(0, i - 2); j < qMin(candidates.size(), i + 2); ++j) points.append(candidates[j]);
    return !points.isEmpty();
}
bool OfflineWaveStore::display(double lower, double upper, int maximumRows, QVector<QVector<Point>> &curves, bool &exact, QString &error) const {
    curves.clear(); curves.resize(m_channels); maximumRows = qMax(32, maximumRows);
    const auto range = blocksFor(lower, upper);
    qint64 count = 0; for (int b = range.first; b < range.second; ++b) count += m_blocks[b].count;
    QVector<int> edges;
    if (range.first < range.second) edges.append(range.first);
    if (range.second - range.first > 1) edges.append(range.second - 1);
    for (int b : edges) {
        if (blockFirst(m_blocks[b]) >= lower && blockLast(m_blocks[b]) <= upper) continue;
        qint64 inside = 0;
        if (!readBlock(b, [&](qint64, double t, const QVector<double> &) { if (t >= lower && t <= upper) ++inside; return true; }, error)) return false;
        count += inside - m_blocks[b].count;
    }
    exact = count <= maximumRows;
    if (exact) {
        for (int b = range.first; b < range.second; ++b) if (!readBlock(b, [&](qint64 row, double t, const QVector<double> &v) {
            if (t >= lower && t <= upper) for (int ch = 0; ch < m_channels; ++ch) curves[ch].append({t, v[ch], row}); return true;
        }, error)) return false;
        return true;
    }
    const int overviewRows = qMin(maximumRows, 4096);
    const int stride = qMax(1, (range.second - range.first + overviewRows / 8 - 1) / (overviewRows / 8));
    for (int start = range.first; start < range.second; start += stride) {
        QVector<Summary> summary(m_channels); bool first = true;
        for (int b = start; b < qMin(range.second, start + stride); ++b) {
            const auto &block = m_blocks[b];
            if (blockFirst(block) < lower || blockLast(block) > upper) {
                bool initial = true;
                if (!readBlock(b, [&](qint64 row, double t, const QVector<double> &v) {
                    if (t < lower || t > upper) return true;
                    for (int ch = 0; ch < m_channels; ++ch) add(summary[ch], {t, v[ch], row}, first && initial);
                    first = false; initial = false; return true;
                }, error)) return false;
            } else {
                for (int ch = 0; ch < m_channels; ++ch) {
                    Summary s;
                    if (ch < block.columns.size()) s = block.columns[ch];
                    else { s.first = {block.firstTime, qQNaN(), block.firstRow}; s.last = {block.lastTime, qQNaN(), block.firstRow + block.count - 1}; s.invalid = true; }
                    if (m_uniform) for (auto p : {&s.first, &s.last, &s.minimum, &s.maximum}) p->key = key(*p);
                    merge(summary[ch], s, first);
                }
                first = false;
            }
        }
        if (first) continue;
        for (int ch = 0; ch < m_channels; ++ch) {
            const auto &s = summary[ch];
            if (s.invalid) {
                // A peak envelope never draws a continuous line across an
                // unknown invalid run. Exact zooms preserve every original gap.
                const double center = (s.first.key + s.last.key) * .5;
                curves[ch].append({s.first.key, qQNaN(), s.first.row});
                curves[ch].append({center, s.minimum.value, s.minimum.row}); curves[ch].append({center, s.maximum.value, s.maximum.row});
                curves[ch].append({s.last.key, qQNaN(), s.last.row});
            } else {
                QVector<Point> p{s.first, s.minimum, s.maximum, s.last};
                std::sort(p.begin(), p.end(), [](const Point &a, const Point &b) { return a.row < b.row; });
                qint64 previous = -1; for (const auto &point : p) if (point.row != previous) { curves[ch].append(point); previous = point.row; }
            }
        }
    }
    return true;
}
QSharedPointer<OfflineWaveStore> OfflineWaveStore::filtered(const OfflineFilterSpec &spec, const Progress &progress, QString &error) const {
    auto result = create(m_baseDirectory, m_indexBudget, error); if (!result) return {};
    struct Filters { BiquadCascade notch, high, low; }; QVector<Filters> filters(m_channels);
    for (auto &f : filters) {
        if (spec.notch) f.notch.configure(BiquadFilter::NOTCH, spec.fs, spec.notchHz, spec.order);
        if (spec.highpass) f.high.configure(BiquadFilter::HIGHPASS, spec.fs, spec.highHz, spec.order);
        if (spec.lowpass) f.low.configure(BiquadFilter::LOWPASS, spec.fs, spec.lowHz, spec.order);
    }
    const bool ok = visit([&](qint64 row, double time, const QVector<double> &v) {
        if (row % BlockRows == 0 && progress && !progress(row, m_rows)) return false;
        auto values = v;
        for (int ch = 0; ch < m_channels; ++ch) {
            if (spec.notch) values[ch] = filters[ch].notch.process(values[ch]);
            if (spec.highpass) values[ch] = filters[ch].high.process(values[ch]);
            if (spec.lowpass) values[ch] = filters[ch].low.process(values[ch]);
        }
        return result->append(time, values, error);
    }, error);
    if (!ok || !result->finish(0, spec.fs > 0 ? 1.0 / spec.fs : .004, false, false, error)) return {};
    return result;
}
