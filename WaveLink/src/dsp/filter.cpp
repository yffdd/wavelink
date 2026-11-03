#include "filter.h"
#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

BiquadFilter::BiquadFilter() {
    reset();
}

void BiquadFilter::reset() {
    b[0] = 1.0; b[1] = 0.0; b[2] = 0.0;
    a[0] = 1.0; a[1] = 0.0; a[2] = 0.0;
    x[0] = 0.0; x[1] = 0.0;
    y[0] = 0.0; y[1] = 0.0;
    m_type = LOWPASS;
    m_isInitialized = false;
}

void BiquadFilter::configure(FilterType type, double fs, double f0, int order, double Q) {
    reset();
    m_type = type;

    if (fs <= 0 || f0 <= 0 || f0 >= fs / 2.0) {
        return;
    }

    double w0 = 2.0 * M_PI * f0 / fs;
    double alpha = sin(w0) / (2.0 * Q);
    double cosw0 = cos(w0);
    double a0 = 1.0 + alpha;

    if (type == NOTCH) {
        b[0] = 1.0;
        b[1] = -2.0 * cosw0;
        b[2] = 1.0;
        a[1] = -2.0 * cosw0;
        a[2] = 1.0 - alpha;
    } else if (type == LOWPASS) {
        b[0] = (1.0 - cosw0) / 2.0;
        b[1] = 1.0 - cosw0;
        b[2] = (1.0 - cosw0) / 2.0;
        a[1] = -2.0 * cosw0;
        a[2] = 1.0 - alpha;
    } else if (type == HIGHPASS) {
        b[0] = (1.0 + cosw0) / 2.0;
        b[1] = -1.0 - cosw0;
        b[2] = (1.0 + cosw0) / 2.0;
        a[1] = -2.0 * cosw0;
        a[2] = 1.0 - alpha;
    } else if (type == BANDPASS) {
        b[0] = alpha;
        b[1] = 0.0;
        b[2] = -alpha;
        a[1] = -2.0 * cosw0;
        a[2] = 1.0 - alpha;
    }

    if (a0 != 0.0) {
        b[0] /= a0;
        b[1] /= a0;
        b[2] /= a0;
        a[1] /= a0;
        a[2] /= a0;
    }
}

double BiquadFilter::process(double in) {
    // A missing/invalid sample is a gap, not zero. Clear only delay state;
    // retain the configured coefficients and reinitialize at the next valid
    // sample so one NaN cannot poison all subsequent output on this channel.
    if (!std::isfinite(in)) {
        x[0] = x[1] = y[0] = y[1] = 0; m_isInitialized = false;
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (!m_isInitialized) {
        if (m_type == HIGHPASS) {
            x[0] = in; x[1] = in;
            y[0] = 0.0; y[1] = 0.0;
        } else {
            x[0] = in; x[1] = in;
            y[0] = in; y[1] = in;
        }
        m_isInitialized = true;
    }

    // Direct Form I difference equation matching filter_v1.3 100%
    double output = b[0] * in 
                  + b[1] * x[0] 
                  + b[2] * x[1] 
                  - a[1] * y[0] 
                  - a[2] * y[1];

    if (!std::isfinite(output)) {
        x[0] = x[1] = y[0] = y[1] = 0; m_isInitialized = false;
        return std::numeric_limits<double>::quiet_NaN();
    }
    x[1] = x[0];
    y[1] = y[0];
    x[0] = in;
    y[0] = output;

    return output;
}


BiquadCascade::BiquadCascade() {
}

void BiquadCascade::configure(BiquadFilter::FilterType type, double fs, double f0, int order, double Q) {
    stages.clear();
    if (order <= 0 || order % 2 != 0) order = 2; // Default 2nd order
    
    int numStages = order / 2;
    stages.resize(numStages);

    for (int i = 0; i < numStages; ++i) {
        double stageQ = Q;
        if (order == 4 && Q == 0.707) {
            if (i == 0) stageQ = 0.5412;
            else if (i == 1) stageQ = 1.3066;
        }
        stages[i].configure(type, fs, f0, order, stageQ);
    }
}

double BiquadCascade::process(double in) {
    double val = in;
    for (auto &stage : stages) {
        val = stage.process(val);
    }
    return val;
}

void BiquadCascade::reset() {
    for (auto &stage : stages) {
        stage.reset();
    }
}
