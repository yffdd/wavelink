#pragma once

#include <QVector>
#include <QString>
#include <limits>

namespace ScopeMeasurements {
inline double unavailable() { return std::numeric_limits<double>::quiet_NaN(); }
struct Sample { double time = 0, value = 0; };
struct Options {
    bool automaticLevels = true;
    double lowLevel = 0, highLevel = 1;
    double lowReference = .10, middleReference = .50, highReference = .90;
    double hysteresis = .02;
    double periodTolerance = .20;
};
struct Result {
    int samples = 0, invalid = 0, cycles = 0;
    bool discontinuous = false;
    double minimum = unavailable(), maximum = unavailable(), peakToPeak = unavailable();
    double mean = unavailable(), rms = unavailable(), standardDeviation = unavailable();
    double lowLevel = unavailable(), highLevel = unavailable();
    double frequency = unavailable(), period = unavailable();
    double positiveDuty = unavailable(), negativeDuty = unavailable();
    double positiveWidth = unavailable(), negativeWidth = unavailable();
    double riseTime = unavailable(), fallTime = unavailable();
    QString timingStatus;
};
bool validate(const Options &options, QString &error);
Result calculate(const QVector<Sample> &samples, const Options &options = {});
}
