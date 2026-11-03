#include "scopemeasurements.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace ScopeMeasurements {
namespace {
bool finite(double value) { return std::isfinite(value); }
double checked(long double value) {
    const double result = static_cast<double>(value);
    return finite(result) ? result : unavailable();
}
double median(QVector<double> values) {
    if (values.isEmpty()) return unavailable();
    const int middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if (values.size() % 2) return upper;
    return *std::max_element(values.begin(), values.begin() + middle) * .5 + upper * .5;
}
double crossing(const Sample &a, const Sample &b, double level) {
    const long double fraction = (static_cast<long double>(level) - a.value) /
                                (static_cast<long double>(b.value) - a.value);
    return checked(static_cast<long double>(a.time) + fraction * (static_cast<long double>(b.time) - a.time));
}
}

bool validate(const Options &o, QString &error) {
    if (!finite(o.lowLevel) || !finite(o.highLevel) || (!o.automaticLevels && o.lowLevel >= o.highLevel))
        error = QStringLiteral("低电平和高电平必须为有限数值，且低电平小于高电平。");
    else if (!finite(o.lowReference) || !finite(o.middleReference) || !finite(o.highReference) ||
             !(0 < o.lowReference && o.lowReference < o.middleReference && o.middleReference < o.highReference && o.highReference < 1))
        error = QStringLiteral("参考电平须满足：0% < 低参考 < 中参考 < 高参考 < 100%。");
    else if (!finite(o.hysteresis) || o.hysteresis < 0 || o.hysteresis > .2 ||
             o.middleReference - o.hysteresis <= 0 || o.middleReference + o.hysteresis >= 1)
        error = QStringLiteral("迟滞须为 0%–20%，且位于中参考电平两侧的有效电平范围内。");
    else if (!finite(o.periodTolerance) || o.periodTolerance < .01 || o.periodTolerance > .5)
        error = QStringLiteral("周期容差须为 1%–50%。");
    else { error.clear(); return true; }
    return false;
}

Result calculate(const QVector<Sample> &input, const Options &options) {
    Result r; QString error;
    if (!validate(options, error)) { r.timingStatus = error; return r; }
    double scale = 0;
    for (const auto &sample : input) {
        if (!finite(sample.time) || !finite(sample.value)) { ++r.invalid; continue; }
        if (!r.samples) r.minimum = r.maximum = sample.value;
        else { r.minimum = std::min(r.minimum, sample.value); r.maximum = std::max(r.maximum, sample.value); }
        scale = std::max(scale, std::abs(sample.value)); ++r.samples;
    }
    if (!r.samples) { r.timingStatus = QStringLiteral("范围内没有有效采样点。"); return r; }
    r.peakToPeak = checked(static_cast<long double>(r.maximum) - r.minimum);
    // Normalize before summing/squaring so large offsets do not overflow RMS.
    long double mean = 0, variance = 0, squares = 0; int count = 0;
    for (const auto &sample : input) if (finite(sample.time) && finite(sample.value)) {
        const long double value = scale ? static_cast<long double>(sample.value) / scale : 0;
        ++count; const long double delta = value - mean;
        mean += delta / count; variance += delta * (value - mean); squares += value * value;
    }
    r.mean = checked(mean * scale);
    r.rms = checked(std::sqrt(squares / count) * scale);
    r.standardDeviation = checked(std::sqrt(std::max(0.L, variance / count)) * scale);
    if (!(r.maximum > r.minimum) || !finite(r.peakToPeak)) {
        r.timingStatus = QStringLiteral("信号为常量或幅值范围不可用，不能测量周期与边沿。"); return r;
    }

    if (options.automaticLevels) {
        // Estimate the populated base/top of the waveform. Overshoot samples
        // do not automatically become the pulse's reference levels.
        constexpr int bins = 128;
        std::array<int, bins> counts{};
        std::array<long double, bins> sums{};
        const long double lower = static_cast<long double>(r.minimum) / scale;
        const long double span = static_cast<long double>(r.maximum) / scale - lower;
        for (const auto &sample : input) if (finite(sample.time) && finite(sample.value)) {
            const long double normalized = static_cast<long double>(sample.value) / scale;
            const int bin = std::max(0, std::min(bins - 1, int((normalized - lower) / span * bins)));
            ++counts[bin]; sums[bin] += normalized;
        }
        int low = 0, high = bins - 1;
        for (int i = 1; i < bins / 2; ++i) if (counts[i] > counts[low]) low = i;
        for (int i = bins - 2; i >= bins / 2; --i) if (counts[i] > counts[high]) high = i;
        r.lowLevel = counts[low] ? checked(sums[low] / counts[low] * scale) : r.minimum;
        r.highLevel = counts[high] ? checked(sums[high] / counts[high] * scale) : r.maximum;
    } else { r.lowLevel = options.lowLevel; r.highLevel = options.highLevel; }
    const double amplitude = r.highLevel - r.lowLevel;
    if (!(amplitude > 0) || !finite(amplitude)) {
        r.timingStatus = QStringLiteral("无法确定有效高低电平，请设置手动电平。"); return r;
    }
    const double low = r.lowLevel + amplitude * options.lowReference;
    const double mid = r.lowLevel + amplitude * options.middleReference;
    const double high = r.lowLevel + amplitude * options.highReference;
    const double hysteresis = amplitude * options.hysteresis;
    QVector<double> intervals;
    intervals.reserve(input.size());
    for (int i = 1; i < input.size(); ++i) {
        if (!finite(input[i - 1].time) || !finite(input[i].time) || !finite(input[i - 1].value) || !finite(input[i].value)) continue;
        const double dt = input[i].time - input[i - 1].time;
        if (dt > 0 && finite(dt)) intervals.append(dt);
    }
    const double typicalInterval = median(std::move(intervals));
    QVector<double> periods, positiveWidths, negativeWidths, rises, falls;
    long double cyclePositive = 0, cycleDuration = 0;
    int state = 0, lastRiseIndex = -1, lowIndex = -1, highIndex = -1;
    double lastRise = unavailable(), lastFall = unavailable();
    double pendingRise = unavailable(), pendingFall = unavailable(), lowCross = unavailable(), highCross = unavailable();
    const auto reset = [&] {
        state = 0; lastRiseIndex = lowIndex = highIndex = -1;
        lastRise = lastFall = pendingRise = pendingFall = lowCross = highCross = unavailable();
    };
    for (int i = 0; i < input.size(); ++i) {
        const auto &b = input[i];
        if (!finite(b.time) || !finite(b.value)) { r.discontinuous = true; reset(); continue; }
        if (i == 0 || !finite(input[i - 1].time) || !finite(input[i - 1].value)) {
            reset(); state = b.value >= mid + hysteresis ? 1 : b.value <= mid - hysteresis ? -1 : 0; continue;
        }
        const auto &a = input[i - 1]; const double dt = b.time - a.time;
        if (!(dt > 0) || !finite(dt) || (finite(typicalInterval) && dt > typicalInterval * 5)) {
            r.discontinuous = true; reset();
            state = b.value >= mid + hysteresis ? 1 : b.value <= mid - hysteresis ? -1 : 0; continue;
        }
        if (a.value <= low && b.value > low) { lowCross = crossing(a, b, low); lowIndex = i; }
        if (b.value < low) { lowCross = unavailable(); lowIndex = -1; }
        if (a.value < high && b.value >= high && finite(lowCross)) {
            // A one-sample jump cannot resolve its actual 10–90% edge time.
            if (i > lowIndex) rises.append(crossing(a, b, high) - lowCross);
            lowCross = unavailable(); lowIndex = -1;
        }
        if (a.value >= high && b.value < high) { highCross = crossing(a, b, high); highIndex = i; }
        if (b.value > high) { highCross = unavailable(); highIndex = -1; }
        if (a.value > low && b.value <= low && finite(highCross)) {
            if (i > highIndex) falls.append(crossing(a, b, low) - highCross);
            highCross = unavailable(); highIndex = -1;
        }
        if (a.value <= mid && b.value > mid) pendingRise = crossing(a, b, mid);
        if (a.value >= mid && b.value < mid) pendingFall = crossing(a, b, mid);
        if (state <= 0 && b.value >= mid + hysteresis && finite(pendingRise)) {
            const double time = pendingRise;
            if (finite(lastFall) && time > lastFall) negativeWidths.append(time - lastFall);
            if (finite(lastRise) && finite(lastFall) && lastFall > lastRise && time > lastFall && i - lastRiseIndex >= 8) {
                periods.append(time - lastRise);
                cyclePositive += lastFall - lastRise; cycleDuration += time - lastRise;
            }
            lastRise = time; lastRiseIndex = i; pendingRise = unavailable(); state = 1;
        } else if (state >= 0 && b.value <= mid - hysteresis && finite(pendingFall)) {
            const double time = pendingFall;
            if (finite(lastRise) && time > lastRise) positiveWidths.append(time - lastRise);
            lastFall = time; pendingFall = unavailable(); state = -1;
        }
    }
    r.positiveWidth = median(std::move(positiveWidths)); r.negativeWidth = median(std::move(negativeWidths));
    r.riseTime = median(std::move(rises)); r.fallTime = median(std::move(falls));
    r.cycles = periods.size();
    const double period = median(periods);
    bool stable = r.cycles >= 2 && finite(period) && period > 0;
    if (stable) for (double value : periods) if (std::abs(value - period) / period > options.periodTolerance) { stable = false; break; }
    if (stable) {
        r.period = period; r.frequency = 1 / period;
        r.positiveDuty = checked(cyclePositive / cycleDuration * 100); r.negativeDuty = 100 - r.positiveDuty;
        r.timingStatus = QStringLiteral("已识别 %1 个完整周期；边沿未解析时显示 --。").arg(r.cycles);
    } else r.timingStatus = r.cycles >= 2 ? QStringLiteral("周期不稳定，频率/周期/占空比显示 --；可调整阈值或缩小范围。")
                                         : QStringLiteral("频率/周期/占空比至少需要两个完整周期，每周期至少 8 个采样间隔。");
    return r;
}
}
