#ifndef FILTER_H
#define FILTER_H

#include <vector>
#include <cmath>

class BiquadFilter {
public:
    enum FilterType {
        NOTCH = 1,
        LOWPASS,
        HIGHPASS,
        BANDPASS,
        BANDSTOP
    };

    BiquadFilter();

    void configure(FilterType type, double fs, double f0, int order = 2, double Q = 0.707);
    double process(double in);
    void reset();

private:
    FilterType m_type;
    double b[3];
    double a[3];
    double x[2]; // x[n-1], x[n-2]
    double y[2]; // y[n-1], y[n-2]
    bool m_isInitialized;
};

class BiquadCascade {
public:
    BiquadCascade();
    
    void configure(BiquadFilter::FilterType type, double fs, double f0, int order = 2, double Q = 0.707);
    double process(double in);
    void reset();

private:
    std::vector<BiquadFilter> stages;
};

#endif // FILTER_H
