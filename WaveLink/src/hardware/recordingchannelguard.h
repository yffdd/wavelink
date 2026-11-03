#pragma once
#include <QtGlobal>
#include <atomic>

// Recording-only policy: one worker accepts/resets; GUI polls the atomic count.
// Parser/display channel counts are deliberately left untouched.
class RecordingChannelGuard {
public:
    void reset() { m_channels.store(0); m_skipped.store(0); }
    bool accept(int channels) {
        if (channels <= 0) return false;
        const int expected = m_channels.load();
        if (!expected) { m_channels.store(channels); return true; }
        if (channels == expected) return true;
        ++m_skipped; return false;
    }
    quint64 skippedFrames() const { return m_skipped.load(); }
    int channelCount() const { return m_channels.load(); }
private:
    std::atomic<int> m_channels{0};
    std::atomic<quint64> m_skipped{0};
};
