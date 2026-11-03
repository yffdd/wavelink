#ifndef PLOTFRAMEBUFFER_H
#define PLOTFRAMEBUFFER_H

#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QVector>

// A bounded mailbox, not a queued signal per sample. Recording happens before
// enqueueing and is never affected by display overload. Only old display frames
// are discarded when the GUI cannot keep up with acquisition.
class PlotFrameBuffer
{
public:
    void push(const QVector<double> &frame)
    {
        if (frame.isEmpty() || frame.size() > 256) return;
        QMutexLocker lock(&m_mutex);
        const qint64 bytes = frame.size() * qint64(sizeof(double)) + 64;
        while (!m_frames.isEmpty() &&
               (m_bytes + bytes > 8 * 1024 * 1024 || m_frames.size() >= 8192)) {
            m_bytes -= m_frames.head().size() * qint64(sizeof(double)) + 64;
            m_frames.dequeue();
            ++m_dropped;
            ++m_totalDropped;
        }
        m_frames.enqueue(frame);
        m_bytes += bytes;
    }

    QVector<QVector<double>> take(int limit, quint64 &dropped)
    {
        QMutexLocker lock(&m_mutex);
        dropped = m_dropped;
        m_dropped = 0;
        QVector<QVector<double>> batch;
        const int count = qMin(qMax(0, limit), m_frames.size());
        batch.reserve(count);
        for (int i = 0; i < count; ++i) {
            m_bytes -= m_frames.head().size() * qint64(sizeof(double)) + 64;
            batch.append(m_frames.dequeue());
        }
        return batch;
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        m_frames.clear();
        m_bytes = 0;
        m_dropped = 0;
    }

    quint64 totalDropped() const { QMutexLocker lock(&m_mutex); return m_totalDropped; }
    void resetDropCount() { QMutexLocker lock(&m_mutex); m_totalDropped = 0; }

private:
    mutable QMutex m_mutex;
    QQueue<QVector<double>> m_frames;
    qint64 m_bytes = 0;
    quint64 m_dropped = 0;
    quint64 m_totalDropped = 0;
};

#endif
