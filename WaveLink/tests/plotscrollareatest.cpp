#include "plotscrollarea.h"
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QLabel>
#include <QScrollBar>
#include <QVBoxLayout>
#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #condition); \
    return 1; } } while (false)

static void settle()
{
    for (int i = 0; i < 12; ++i) QApplication::processEvents();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow outer;
    auto central = new QWidget;
    auto layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    auto scroll = new PlotScrollArea;
    auto inner = new QMainWindow;
    inner->setWindowFlags(Qt::Widget);
    inner->setDockNestingEnabled(true);
    auto dummy = new QWidget;
    dummy->setFixedSize(0, 0);
    inner->setCentralWidget(dummy);
    scroll->setWidget(inner);
    layout->addWidget(scroll, 1);
    auto footer = new QLabel("Time range");
    footer->setFixedHeight(25);
    layout->addWidget(footer);
    outer.setCentralWidget(central);

    QList<QDockWidget *> plots;
    auto addPlot = [&]() {
        auto dock = new QDockWidget("Plot", inner);
        dock->setObjectName(QString("Plot_%1").arg(plots.size()));
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
        auto content = new QLabel("Waveform");
        content->setMinimumHeight(180);
        dock->setWidget(content);
        if (plots.isEmpty()) inner->addDockWidget(Qt::TopDockWidgetArea, dock);
        else inner->splitDockWidget(plots.last(), dock, Qt::Vertical);
        plots.append(dock);
        scroll->setDetachedViewCount(plots.size() - 1);
    };
    addPlot();
    const QByteArray defaultInnerState = inner->saveState();
    QList<QDockWidget *> bottom;
    for (int i = 0; i < 3; ++i) {
        auto dock = new QDockWidget("Bottom panel", &outer);
        auto label = new QLabel("Data / Spectrum / Signal");
        label->setMinimumHeight(80);
        dock->setWidget(label);
        outer.addDockWidget(Qt::BottomDockWidgetArea, dock);
        if (!bottom.isEmpty()) outer.splitDockWidget(bottom.last(), dock, Qt::Horizontal);
        bottom.append(dock);
    }
    outer.resize(900, 700);
    outer.show();
    settle();
    const QSize originalSize = outer.size();
    const int bottomY = bottom.first()->y();
    const QSize originalMinimum = outer.minimumSizeHint();

    addPlot();
    addPlot();
    settle();
    CHECK(scroll->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    addPlot(); // Exactly three detached views: enable internal scrolling.
    settle();
    CHECK(scroll->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn);
    CHECK(scroll->verticalScrollBar()->maximum() > 0);
    CHECK(inner->height() >= 4 * 240);
    CHECK(outer.size() == originalSize);
    CHECK(outer.minimumSizeHint() == originalMinimum);
    for (auto dock : bottom) CHECK(dock->y() == bottomY);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    settle();
    CHECK(inner->y() < 0);

    // Reproduce the regression: default state predates all detached plots.
    // Rebuild from an intentionally horizontal arrangement after restoration.
    for (auto dock : plots) inner->removeDockWidget(dock);
    inner->addDockWidget(Qt::TopDockWidgetArea, plots.first());
    for (int i = 1; i < plots.size(); ++i)
        inner->splitDockWidget(plots[i - 1], plots[i], Qt::Horizontal);
    CHECK(inner->restoreState(defaultInnerState));
    stackPlotDocksVertically(inner, plots);
    settle();
    for (int i = 1; i < plots.size(); ++i) {
        CHECK(plots[i]->x() == plots.first()->x());
        CHECK(plots[i]->width() == plots.first()->width());
        CHECK(plots[i]->y() > plots[i - 1]->y());
        CHECK(qobject_cast<QLabel *>(plots[i]->widget())->text() == "Waveform");
    }
    CHECK(outer.size() == originalSize);
    for (auto dock : bottom) CHECK(dock->y() == bottomY);

    for (int i = 0; i < 20; ++i) addPlot();
    settle();
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK(inner->restoreState(defaultInnerState));
        stackPlotDocksVertically(inner, plots);
        settle();
        for (int i = 1; i < plots.size(); ++i) {
            CHECK(plots[i]->x() == plots.first()->x());
            CHECK(plots[i]->y() > plots[i - 1]->y());
        }
    }
    CHECK(outer.size() == originalSize);
    for (auto dock : bottom) CHECK(dock->y() == bottomY);
    while (plots.size() > 1) {
        auto dock = plots.takeLast();
        inner->removeDockWidget(dock);
        delete dock;
    }
    scroll->setDetachedViewCount(0);
    settle();
    CHECK(scroll->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    CHECK(scroll->verticalScrollBar()->maximum() == 0);
    CHECK(outer.size() == originalSize);
    std::puts("Plot scrolling tests passed: threshold, overflow, bottom docks, default-restack, shrink.");
    return 0;
}
