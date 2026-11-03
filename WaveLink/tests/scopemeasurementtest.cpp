#include "scopemeasurements.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
using namespace ScopeMeasurements;
#define CHECK(c) do { if (!(c)) { std::cerr << "Scope measurement failure " << __LINE__ << ": " #c << '\n'; return 1; } } while (0)
static bool near(double a, double b, double tolerance = 1e-8) { return std::isfinite(a) && std::abs(a - b) <= tolerance; }
int main() {
    constexpr double pi = 3.14159265358979323846;
    QVector<Sample> sine;
    for (int i = 0; i < 4000; ++i) { const double t = i / 2000.; sine.append({1000000 + t, 3 + 2 * std::sin(2 * pi * 5 * t)}); }
    auto r = calculate(sine);
    CHECK(r.samples == 4000 && near(r.minimum, 1) && near(r.maximum, 5) && near(r.peakToPeak, 4));
    CHECK(near(r.mean, 3) && near(r.rms, std::sqrt(11)) && near(r.standardDeviation, std::sqrt(2)));
    CHECK(near(r.frequency, 5, 1e-6) && near(r.period, .2, 1e-8) && near(r.positiveDuty, 50, 1e-4));
    CHECK(near(r.riseTime, std::asin(.8) / (pi * 5), .001));
    CHECK(near(r.fallTime, r.riseTime, .00001));

    QVector<Sample> square;
    for (int i = 0; i < 20000; ++i) { const double t = (i + .25) / 10000.; square.append({t, std::fmod(t, .1) < .03 ? 3. : 1.}); }
    r = calculate(square);
    CHECK(near(r.mean, 1.6, .001) && near(r.rms, std::sqrt(3.4), .001));
    CHECK(near(r.frequency, 10, .01) && near(r.positiveDuty, 30, .1) && near(r.negativeDuty, 70, .1));
    CHECK(near(r.positiveWidth, .03, .0001) && near(r.negativeWidth, .07, .0001));
    CHECK(std::isnan(r.riseTime) && std::isnan(r.fallTime));
    Options manual; manual.automaticLevels = false; manual.lowLevel = 0; manual.highLevel = 1;
    QVector<Sample> trapezoid;
    for (int i = 0; i < 6000; ++i) {
        const double t = i / 2000., phase = std::fmod(t + .1, .5);
        const double v = phase < .05 ? phase / .05 : phase < .25 ? 1 : phase < .33 ? 1 - (phase - .25) / .08 : 0;
        trapezoid.append({t, v});
    }
    r = calculate(trapezoid, manual);
    CHECK(near(r.frequency, 2, .001) && near(r.riseTime, .04, 1e-6) && near(r.fallTime, .064, 1e-6));
    manual.lowReference = .2; manual.highReference = .8;
    r = calculate(trapezoid, manual);
    CHECK(near(r.riseTime, .03, 1e-6) && near(r.fallTime, .048, 1e-6));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    r = calculate({{0, 2}, {1, 2}, {2, 2}});
    CHECK(near(r.mean, 2) && near(r.rms, 2) && near(r.standardDeviation, 0) && std::isnan(r.frequency));
    r = calculate({{0, 0}, {1, nan}, {2, 10}});
    CHECK(r.samples == 2 && r.invalid == 1 && r.discontinuous && near(r.mean, 5) && near(r.rms, std::sqrt(50)));
    CHECK(std::isnan(r.frequency));
    CHECK(calculate({{0, nan}}).samples == 0 && std::isnan(calculate({}).mean));
    r = calculate({{0, -1e300}, {1, 1e300}});
    CHECK(near(r.mean, 0) && std::isfinite(r.rms) && std::isfinite(r.standardDeviation));
    CHECK(std::abs(r.rms / 1e300 - 1) < 1e-12);
    // A sequence of fragments cannot manufacture complete cycles across gaps.
    QVector<Sample> fragments;
    for (int i = 0; i < 5; ++i) {
        const double t = i;
        fragments << Sample{t, 0} << Sample{t + .1, 1} << Sample{t + .2, 0} << Sample{t + .3, nan};
    }
    r = calculate(fragments, Options{});
    CHECK(r.discontinuous && r.cycles == 0 && std::isnan(r.period));
    auto reversed = sine; std::reverse(reversed.begin(), reversed.end());
    r = calculate(reversed); CHECK(r.discontinuous && std::isnan(r.frequency));
    // Varying edge-to-edge periods must not produce a misleading mean.
    QVector<Sample> unstable; double time = 0;
    for (int cycle = 0; cycle < 8; ++cycle) {
        const double period = cycle % 3 == 2 ? .3 : .1;
        for (int i = 0; i < 100; ++i) unstable.append({time + period * i / 100., i < 50 ? 0. : 1.});
        time += period;
    }
    r = calculate(unstable); CHECK(r.cycles >= 2 && std::isnan(r.frequency) && std::isnan(r.positiveDuty));
    // A single pulse has a width, but no periodic frequency/duty measurement.
    QVector<Sample> pulse;
    for (int i = 0; i < 1000; ++i) { const double t = i / 1000.; pulse.append({t, t >= .3 && t < .6 ? 1. : 0.}); }
    r = calculate(pulse); CHECK(near(r.positiveWidth, .3, .002) && std::isnan(r.frequency));
    QString error; Options invalid; invalid.lowReference = .6;
    CHECK(!validate(invalid, error) && !error.isEmpty() && std::isnan(calculate(sine, invalid).frequency));
    std::cout << "Known sine/square/ramp, custom thresholds, DC, gaps, reversed time, unstable cycles and large-value statistics passed.\n";
    return 0;
}
