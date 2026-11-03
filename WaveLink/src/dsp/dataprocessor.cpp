#include "dataprocessor.h"

DataProcessor::DataProcessor() : m_channelCount(0) {
}

void DataProcessor::setChannelCount(int count) {
    QMutexLocker locker(&m_mutex);
    if (m_channelCount != count) {
        m_channelCount = count;
        m_rollingBuffers.clear();
        m_rollingBuffers.resize(count);
    }
}

QVector<double> DataProcessor::processFrame(const QVector<double>& input) {
    QMutexLocker locker(&m_mutex);
    QVector<double> output = input;
    
    // Future: Apply digital filters here
    
    // Feed rolling buffer for FFT
    for (int i = 0; i < input.size() && i < m_rollingBuffers.size(); ++i) {
        m_rollingBuffers[i].append(output[i]);
        if (m_rollingBuffers[i].size() > MAX_ROLLING_SIZE) {
            // Keep recent data
            m_rollingBuffers[i].remove(0, m_rollingBuffers[i].size() - MAX_ROLLING_SIZE);
        }
    }
    
    return output;
}

QVector<double> DataProcessor::getRecentDataForFFT(int channelIdx, int points) {
    QMutexLocker locker(&m_mutex);
    QVector<double> res(points, 0.0);
    
    if (channelIdx >= 0 && channelIdx < m_rollingBuffers.size()) {
        const auto& buf = m_rollingBuffers[channelIdx];
        int start = qMax(0, buf.size() - points);
        int writeStart = points - (buf.size() - start);
        for (int i = start; i < buf.size(); ++i) {
            res[writeStart++] = buf[i];
        }
    }
    return res;
}
