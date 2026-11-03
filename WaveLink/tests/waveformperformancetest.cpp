#include "waveformgraph.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <functional>

#define VERIFY(c) do { if (!(c)) { std::cerr << "Waveform failure " << __LINE__ << ": " #c << '\n'; return 1; } } while (0)
class TestGraph : public WaveformGraph {
public:
    using WaveformGraph::WaveformGraph;
    QVector<QCPGraphData> display() const {
        QVector<QCPGraphData> out;
        getOptimizedLineData(&out, data()->findBegin(keyAxis()->range().lower), data()->findEnd(keyAxis()->range().upper));
        return out;
    }
};
static void events(int milliseconds) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) { QCoreApplication::processEvents(); QThread::msleep(1); }
}
static bool exact(TestGraph *graph, QCPRange keys) {
    bool indexedFound = false, originalFound = false;
    const auto indexed = graph->getValueRange(indexedFound, QCP::sdBoth, keys);
    const auto original = graph->data()->valueRange(originalFound, QCP::sdBoth, keys);
    return indexedFound == originalFound && (!indexedFound || indexed == original);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCustomPlot plot; plot.resize(1100, 600);
    auto graph = new TestGraph(plot.xAxis, plot.yAxis);
    VERIFY(graph->pen().isCosmetic() && graph->pen().widthF() == 1.0 && !graph->antialiased());
    plot.xAxis->setRange(0, 10000); plot.yAxis->setRange(-5, 5); plot.replot();
    QVector<QCPGraphData> values;
    for (int i = 0; i < 100003; ++i) values.append({i * .01, std::sin(i * .03)});
    values[351].value = -700; values[511].value = 800;
    for (int i = 550; i < 710; ++i) values[i].value = qQNaN();
    graph->data()->set(values, true);
    std::mt19937 random(17);
    for (int i = 0; i < 1000; ++i) {
        const int first = random() % values.size(), count = random() % 10000;
        VERIFY(exact(graph, {first * .01 - .001, (first + count) * .01 + .003}));
        VERIFY(graph->lastRangeSamples() <= 14);
    }
    VERIFY(exact(graph, QCPRange()));
    VERIFY(graph->cacheBytes() <= graph->projectedCacheBytes());
    VERIFY(exact(graph, {5.6, 6.0})); // all invalid
    VERIFY(exact(graph, {2, 2})); // exact one sample
    VERIFY(exact(graph, {2000, 3000})); // outside history
    const quint64 indexedBefore = graph->indexedSamples();
    graph->addData(1000.04, 77);
    VERIFY(exact(graph, QCPRange()));
    VERIFY(graph->indexedSamples() - indexedBefore <= 8); // append never rebuilds old history
    plot.xAxis->setRange(0, 1000); plot.replot();
    const auto display = graph->display();
    VERIFY(display.size() < 15000);
    bool peak = false, trough = false, gap = false;
    double previousKey = -1;
    for (const auto &point : display) {
        VERIFY(point.key >= previousKey); previousKey = point.key;
        peak |= point.value == 800; trough |= point.value == -700; gap |= qIsNaN(point.value);
        const auto source = graph->data()->findBegin(point.key, false);
        VERIFY(source != graph->data()->constEnd() && source->key == point.key);
        VERIFY((qIsNaN(point.value) && qIsNaN(source->value)) || source->value == point.value);
    }
    VERIFY(peak && trough && gap);
    VERIFY(graph->dataCount() == values.size() + 1); // display never replaces raw samples
    // Invalidation: every supported way to change an existing prefix.
    graph->data()->set(values, true); VERIFY(exact(graph, QCPRange()));
    values[300].value = 12345;
    graph->data()->set(values, true); VERIFY(exact(graph, QCPRange()));
    graph->data()->removeBefore(50); graph->data()->squeeze(); VERIFY(exact(graph, QCPRange()));
    graph->data()->removeAfter(500); VERIFY(exact(graph, QCPRange()));
    graph->data()->remove(80, 100); VERIFY(exact(graph, QCPRange()));
    graph->data()->remove(120); VERIFY(exact(graph, QCPRange()));
    graph->data()->add(QCPGraphData(70.005, -5555)); VERIFY(exact(graph, QCPRange()));
    graph->data()->add(QVector<QCPGraphData>{{60.005, 7777}, {50.001, -8888}}, false); VERIFY(exact(graph, QCPRange()));
    graph->data()->begin()->value = 99999; VERIFY(exact(graph, QCPRange()));
    QCPGraphDataContainer replacement; replacement.set(values, true);
    replacement.setAutoSqueeze(false);
    *graph->data() = replacement; VERIFY(exact(graph, QCPRange()));
    VERIFY(!graph->data()->autoSqueeze());
    graph->data()->clear(); VERIFY(exact(graph, QCPRange()));
    graph->addData(1, 42); VERIFY(exact(graph, QCPRange()));
    QSharedPointer<QCPGraphDataContainer> swapped(new QCPGraphDataContainer(replacement));
    VERIFY(!swapped->autoSqueeze());
    graph->setData(swapped); VERIFY(exact(graph, QCPRange()));
    graph->releaseDisplayCache(); VERIFY(graph->cacheBytes() == 0 && exact(graph, QCPRange()));
    // Incremental appends across every leaf/parent boundary, including equal keys.
    graph->data()->clear();
    for (int i = 0; i < 160; ++i) {
        graph->addData(i / 2, (i % 5 == 0) ? qQNaN() : double(i % 31));
        VERIFY(exact(graph, QCPRange()));
    }
    // No curve may bridge invalid data, even when a long invalid run collapses.
    graph->data()->set(values, true); graph->display();
    values[2000].value = qQNaN(); graph->data()->set(values, true);
    bool isolatedGap = false; for (const auto &p : graph->display()) isolatedGap |= p.key == 20 && qIsNaN(p.value);
    VERIFY(isolatedGap);
    // Main/detached plots use coalesced refresh, while explicit export refresh is immediate.
    plot.setMinimumReplotInterval(33); int replots = 0;
    QObject::connect(&plot, &QCustomPlot::afterReplot, [&] { ++replots; });
    for (int i = 0; i < 300; ++i) { plot.xAxis->setRange(i, i + 60); plot.replot(QCustomPlot::rpQueuedReplot); }
    events(80); VERIFY(replots == 1 && plot.xAxis->range() == QCPRange(299, 359));
    replots = 0;
    QElapsedTimer duration; duration.start();
    while (duration.elapsed() < 210) { plot.replot(QCustomPlot::rpQueuedReplot); events(1); }
    events(60); VERIFY(replots >= 2 && replots <= 9);
    plot.replot(QCustomPlot::rpQueuedReplot); plot.replot(); const int immediate = replots;
    events(50); VERIFY(replots == immediate); // no stale queued duplicate

    // Repeatable CPU benchmark. Timings are reported, not asserted (machine dependent).
    // Same full data and exact visible-range autoscale; no OpenGL dependency.
    plot.clearGraphs(); QVector<TestGraph *> channels;
    values.clear(); values.reserve(300000);
    for (int i = 0; i < 300000; ++i) values.append({i * .01, std::sin(i * .025)});
    for (int ch = 0; ch < 12; ++ch) {
        auto g = new TestGraph(plot.xAxis, plot.yAxis); g->data()->set(values, true); channels.append(g);
    }
    for (int seconds : {60, 100, 1000}) {
        plot.xAxis->setRange(1000, 1000 + seconds);
        for (auto g : channels) { bool found; g->getValueRange(found, QCP::sdBoth, plot.xAxis->range()); }
        QElapsedTimer clock; clock.start();
        for (int frame = 0; frame < 80; ++frame) for (auto g : channels) {
            bool found; g->QCPGraph::getValueRange(found, QCP::sdBoth, {1000 + frame * .01, 1000 + seconds + frame * .01});
        }
        const double baseRangeMs = clock.nsecsElapsed() / 1e6;
        clock.restart();
        for (int frame = 0; frame < 80; ++frame) for (auto g : channels) {
            bool found; g->getValueRange(found, QCP::sdBoth, {1000 + frame * .01, 1000 + seconds + frame * .01});
            VERIFY(g->lastRangeSamples() <= 14 && found);
        }
        const double indexedRangeMs = clock.nsecsElapsed() / 1e6;
        for (auto g : channels) g->setAdaptiveSampling(false);
        // Drawing benchmark compares built-in adaptive sampling against the cached pyramid.
        QCustomPlot baseline; baseline.resize(1100, 600); baseline.xAxis->setRange(plot.xAxis->range()); baseline.yAxis->setRange(-5, 5);
        for (auto g : channels) baseline.addGraph()->setData(g->data());
        baseline.replot();
        clock.restart(); for (int i = 0; i < 12; ++i) baseline.replot(); const double baseDrawMs = clock.nsecsElapsed() / 1e6 / 12;
        for (auto g : channels) g->setAdaptiveSampling(true);
        plot.replot(); clock.restart(); for (int i = 0; i < 12; ++i) plot.replot(); const double indexedDrawMs = clock.nsecsElapsed() / 1e6 / 12;
        std::cout << "12ch x100Hz " << seconds << "s; history=300000/ch; autoscale80frames(ms): " << baseRangeMs << " -> " << indexedRangeMs
            << "; render/frame(ms): " << baseDrawMs << " -> " << indexedDrawMs << '\n';
        for (auto g : channels) VERIFY(g->dataCount() == 300000 && g->cacheBytes() <= g->projectedCacheBytes());
    }
    // User regression: 4 ms sampling, high-frequency sine, narrow and wide
    // windows, main/detached traces, HiDPI and exact original-data queries.
    plot.clearGraphs();
    plot.resize(1500, 900); plot.yAxis->setRange(-60,60);
    plot.setAntialiasedElements(QCP::aeNone); plot.setNotAntialiasedElements(QCP::aeAll);
    for (int channelCount : {2, 12}) {
        QVector<TestGraph *> traces;
        for (int ch=0; ch<channelCount; ++ch) {
            auto g=new TestGraph(plot.xAxis,plot.yAxis);
            g->setPen(WaveformGraph::displayPen(QColor::fromHsv(ch*360/channelCount,255,220)));
            QVector<QCPGraphData> input;
            for (int i=0;i<=25000;++i) input.append({i*.004,50*std::sin(2*3.141592653589793*100*(i*.004+ch*.05))*(1-ch*.02)});
            input[501].value=qQNaN(); input[503].value=59; input[504].value=-59;
            g->data()->set(input,true); traces.append(g);
        }
        for (double ratio : {1.0,1.5,2.0}) for (int seconds : {5,60,100}) {
            plot.setBufferDevicePixelRatio(ratio);
            plot.xAxis->setRange(0,seconds); plot.replot();
            for (auto g:traces) {
                VERIFY(exact(g,plot.xAxis->range()) && g->dataCount()==25001);
                const auto shown=g->display();
                bool gap=false,peak=false,trough=false;
                for (const auto &p:shown) {
                    auto original=g->data()->findBegin(p.key,false);
                    VERIFY(original!=g->data()->constEnd() && original->key==p.key);
                    VERIFY((qIsNaN(p.value)&&qIsNaN(original->value))||p.value==original->value);
                    gap |= p.key==2.004 && qIsNaN(p.value);
                    peak |= p.value==59; trough |= p.value==-59;
                }
                VERIFY(gap && peak && trough);
            }
            QElapsedTimer frameClock; frameClock.start();
            for (int frame=0;frame<5;++frame) {
                for(auto g:traces) { bool found; g->getValueRange(found,QCP::sdBoth,plot.xAxis->range()); VERIFY(found); }
                plot.xAxis->setRange(frame*.004,seconds+frame*.004); plot.replot();
            }
            std::cout << "High-frequency 100Hz/250sps " << channelCount << "ch " << seconds << "s DPR=" << ratio
                      << " render+autoscale ms/frame=" << frameClock.nsecsElapsed()/1e6/5 << '\n';
        }
        VERIFY(plot.plottingHints().testFlag(QCP::phFastPolylines));
        for(int width:{2,3}) {
            for(auto g:traces) g->setPen(WaveformGraph::displayPen(g->pen().color(),width));
            QElapsedTimer coarseClock; coarseClock.start();
            for(int frame=0;frame<3;++frame) plot.replot();
            std::cout << "Cosmetic width=" << width << " " << channelCount << "ch DPR=2 100s ms/frame=" << coarseClock.nsecsElapsed()/1e6/3 << '\n';
            for(auto g:traces) VERIFY(g->dataCount()==25001 && g->pen().isCosmetic() && g->pen().widthF()==width);
        }
        plot.clearGraphs();
    }
    std::cout << "Exact range, invalidation, incremental cache, peaks/NaN gaps, full data preservation and frame coalescing passed.\n";
    return 0;
}
