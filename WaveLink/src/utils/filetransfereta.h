#pragma once
#include <QString>
#include <QtGlobal>
#include <cmath>
#include <deque>
#include <limits>

// Estimates from completed transport bytes, never bytes merely read/queued.
// The caller supplies monotonic elapsed time, which also makes stalls and
// very large files testable without sleeping or opening a real device.
class FileTransferEta {
public:
    void start() { reset(); m_active = true; m_samples.push_back({0, 0}); }
    void reset() { m_active = false; m_total = m_completed = m_lastProgress = 0; m_samples.clear(); }
    bool active() const { return m_active; }
    void observe(qint64 elapsedMs, qint64 completed, qint64 total) {
        if (!m_active || elapsedMs < 0 || total < 0) return;
        m_total = total;
        completed = qBound<qint64>(0, completed, total);
        if (completed > m_completed) m_lastProgress = elapsedMs;
        m_completed = qMax(m_completed, completed);
        if (m_samples.empty() || elapsedMs - m_samples.back().time >= 250)
            m_samples.push_back({elapsedMs, m_completed});
        // Keep one anchor just before the eight-second measurement window.
        while (m_samples.size() > 2 && elapsedMs - m_samples[1].time >= 8000) m_samples.pop_front();
    }
    QString text(qint64 elapsedMs) const {
        if (!m_active || (m_total > 0 && m_completed >= m_total)) return format(0);
        if (m_total <= 0 || m_completed <= 0 || m_samples.empty() || elapsedMs - m_lastProgress >= 8000)
            return QStringLiteral("--:--:--");
        const auto anchor = m_samples.front();
        const qint64 duration = elapsedMs - anchor.time, bytes = m_completed - anchor.bytes;
        if (duration < 250 || bytes <= 0) return QStringLiteral("--:--:--");
        const long double remaining = std::ceil((static_cast<long double>(m_total - m_completed) * duration) / bytes / 1000.0L);
        const auto seconds = remaining >= static_cast<long double>(std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max() : qMax<qint64>(1, static_cast<qint64>(remaining));
        return format(seconds);
    }
    static QString format(qint64 seconds) {
        seconds = qMax<qint64>(0, seconds);
        // Hours are not a clock-of-day: a >24-hour transfer must not wrap.
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
            .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
private:
    struct Sample { qint64 time, bytes; };
    bool m_active = false;
    qint64 m_total = 0, m_completed = 0, m_lastProgress = 0;
    std::deque<Sample> m_samples;
};
