#ifndef DATAPROCESSOR_H
#define DATAPROCESSOR_H

#include <QVector>
#include <QMutex>

class DataProcessor {
public:
    DataProcessor();
    void setChannelCount(int count);
    
    // Process new incoming frame, applies any enabled filters,
    // stores rolling data, and returns data suitable for time-domain plot
    QVector<double> processFrame(const QVector<double>& input);
    
    // Get latest data for FFT (padded/truncated to power of 2)
    QVector<double> getRecentDataForFFT(int channelIdx, int points = 1024);

private:
    int m_channelCount;
    QMutex m_mutex;
    
    // Rolling buffer for each channel to support FFT
    QVector<QVector<double>> m_rollingBuffers;
    const int MAX_ROLLING_SIZE = 4096;
    
    // Future: Filter states can be added here
};

#endif // DATAPROCESSOR_H
