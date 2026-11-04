#pragma once

// Exercise the actual mouse events and native text-item geometry, including
// relayout of already-visible labels and graphs created from hidden channels.
static bool plotReadoutsInside(QCustomPlot *plot, int minimumLabels = 1) {
    const QRectF bounds(plot->axisRect()->rect());
    int labels = 0;
    QVector<QRectF> readouts;
    for (int i = 0; i < plot->itemCount(); ++i) {
        auto text = qobject_cast<QCPItemText *>(plot->item(i));
        if (!text || !text->visible() || (!text->text().startsWith("X:") && !text->text().startsWith("Δt:"))) continue;
        const QRectF rect(text->topLeft->pixelPosition(), text->bottomRight->pixelPosition());
        if (!bounds.contains(rect)) {
            std::cerr << "Readout clipped: " << text->text().toStdString()
                << " box=" << rect.x() << ',' << rect.y() << ',' << rect.width() << ',' << rect.height()
                << " axes=" << bounds.x() << ',' << bounds.y() << ',' << bounds.width() << ',' << bounds.height() << '\n';
            return false;
        }
        for (const auto &other : readouts) if (rect.intersects(other)) {
            std::cerr << "Readout labels overlap\n"; return false;
        }
        readouts.append(rect);
        ++labels;
    }
    return labels >= minimumLabels;
}

static bool detachPlotTestChannel(MainWindow &window, QListWidget *channels, int channel) {
    bool detached = false;
    QTimer::singleShot(0, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
            for (auto action : menu->actions()) if (action->text() == "分离到新视图") {
                action->trigger(); detached = true; break;
            }
            menu->close();
        }
    });
    return QMetaObject::invokeMethod(&window, "onChannelContextMenu",
        Q_ARG(QPoint, channels->visualItemRect(channels->item(channel)).center())) && detached;
}

static bool choosePlotChannelAction(QCustomPlot *plot, const QString &submenu, const QString &channel) {
    bool triggered = false;
    QTimer::singleShot(0, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
            for (auto action : menu->actions()) if (action->text() == submenu && action->menu()) {
                for (auto entry : action->menu()->actions()) if (entry->text() == channel) {
                    entry->trigger(); triggered = true; break;
                }
            }
            menu->close();
        }
    });
    plot->customContextMenuRequested(plot->axisRect()->rect().center());
    return triggered;
}

static bool detachedPlotPositionActions(QCustomPlot *plot, bool canMoveUp, bool canMoveDown, int direction = 0)
{
    bool verified = false;
    QTimer::singleShot(0, [&] {
        auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        const auto actions = menu->actions();
        for (int i = 0; i + 2 < actions.size(); ++i) {
            if (actions[i]->text() != QStringLiteral("合并回主视图")) continue;
            auto up = actions[i + 1];
            auto down = actions[i + 2];
            verified = up->text() == QStringLiteral("视图位置上移") && up->isEnabled() == canMoveUp &&
                       down->text() == QStringLiteral("视图位置下移") && down->isEnabled() == canMoveDown;
            if (direction != 0) {
                auto move = direction < 0 ? up : down;
                verified = verified && move->isEnabled();
                if (verified) move->trigger();
            }
            break;
        }
        menu->close();
    });
    plot->customContextMenuRequested(plot->axisRect()->rect().center());
    return verified;
}

static int detachedChannelRemovalUiTests() {
    QTemporaryDir directory; VERIFY(directory.isValid());
    QSettings settings("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    settings.clear(); advanced.clear(); settings.setValue("editCacheDir", directory.path());
    advanced.setValue("InterfaceType", 0); settings.sync(); advanced.sync();
    ProtocolProfileStore store; QString error;
    VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1500, 950); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    QElapsedTimer ready; ready.start(); VERIFY(until([&] { return ready.elapsed() > 180; }));
    window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    window.findChild<QTimer *>("waveformRefreshTimer")->stop();
    for (auto check : window.findChildren<QCheckBox *>()) {
        if (check->text() == "Y轴自适应") check->setChecked(false);
    }
    window.findChild<QPushButton *>("btnPausePlot")->setChecked(false);
    const auto feed = [&](const QVector<double> &values) {
        return QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, values)) &&
               QMetaObject::invokeMethod(&window, "onPlotTimer");
    };
    VERIFY(feed({10, 20, 30, 40}));
    auto mainDock = window.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图")); VERIFY(mainDock);
    auto main = mainDock->findChild<QCustomPlot *>(); VERIFY(main && main->graphCount() == 4);
    QListWidget *channels = nullptr;
    for (auto list : window.findChildren<QListWidget *>()) if (list->count() == 4 && list->itemWidget(list->item(0))) channels = list;
    VERIFY(channels); QStringList names;
    for (int i = 0; i < 4; ++i) {
        auto check = channels->itemWidget(channels->item(i))->findChild<QCheckBox *>("chkChannel"); VERIFY(check);
        names.append(check->text()); check->setChecked(false);
    }
    VERIFY(detachPlotTestChannel(window, channels, 0));
    QPointer<QDockWidget> groupDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + names[0]); VERIFY(groupDock);
    QPointer<QCustomPlot> group = groupDock->findChild<QCustomPlot *>(); VERIFY(group);
    VERIFY(detachedPlotPositionActions(group, false, false));
    VERIFY(choosePlotChannelAction(group, "添加通道显示", names[1]));
    VERIFY(choosePlotChannelAction(group, "添加通道显示", names[2]) && group->graphCount() == 3);
    VERIFY(detachPlotTestChannel(window, channels, 3));
    QPointer<QDockWidget> otherDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + names[3]); VERIFY(otherDock);
    QPointer<QCustomPlot> other = otherDock->findChild<QCustomPlot *>(); VERIFY(other);
    QPointer<QCPGraph> first = group->graph(0), middle = group->graph(1), last = group->graph(2);
    QPointer<QCPGraph> untouched = other->graph(0);
    // Channel names are editable metadata and need not be unique.
    middle->setName(first->name()); main->graph(1)->setName(main->graph(0)->name());
    group->xAxis->setRange(0, 1); group->yAxis->setRange(0, 100); group->replot();
    const QPointF point(group->xAxis->coordToPixel(0), group->yAxis->coordToPixel(10));
    QMouseEvent mark(QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(group, &mark); group->replot();
    const auto hasMarker = [&] {
        if (!group) return false;
        for (int i = 0; i < group->itemCount(); ++i) if (auto tracer = qobject_cast<QCPItemTracer *>(group->item(i)))
            if (tracer->visible() && tracer->graph() == first) return true;
        return false;
    };
    VERIFY(hasMarker());
    VERIFY(until([&] { return mainDock->y() < groupDock->y() && groupDock->y() < otherDock->y(); }));
    VERIFY(detachedPlotPositionActions(other, true, false, -1));
    VERIFY(until([&] { return mainDock->y() < otherDock->y() && otherDock->y() < groupDock->y(); }));
    VERIFY(detachedPlotPositionActions(other, false, true, 1));
    VERIFY(until([&] { return mainDock->y() < groupDock->y() && groupDock->y() < otherDock->y(); }));
    VERIFY(detachedPlotPositionActions(group, false, true, 1));
    VERIFY(until([&] { return mainDock->y() < otherDock->y() && otherDock->y() < groupDock->y(); }));
    VERIFY(detachedPlotPositionActions(group, true, false));
    VERIFY(group->graphCount() == 3 && group->graph(0) == first && other->graph(0) == untouched && hasMarker());
    // Remove the middle curve: retain both remaining graph objects/history,
    // their view, the unrelated view, and the other curve's point marker.
    VERIFY(choosePlotChannelAction(group, "移除通道显示", names[1]));
    VERIFY(group && groupDock->isVisible() && group->graphCount() == 2);
    VERIFY(!middle && group->graph(0) == first && group->graph(1) == last && hasMarker());
    VERIFY(other && otherDock->isVisible() && other->graph(0) == untouched);
    VERIFY(groupDock->windowTitle().contains(names[0]) && groupDock->windowTitle().contains(names[2]) && !groupDock->windowTitle().contains(names[1]));
    VERIFY(feed({11, 21, 31, 41}));
    const auto sameData = [](QCPGraph *a, QCPGraph *b) {
        if (!a || !b || a->dataCount() != b->dataCount()) return false;
        auto ai = a->data()->constBegin(), bi = b->data()->constBegin();
        for (; ai != a->data()->constEnd(); ++ai, ++bi) if (ai->key != bi->key || ai->value != bi->value) return false;
        return true;
    };
    VERIFY(sameData(first, main->graph(0)) && sameData(last, main->graph(2)) && sameData(untouched, main->graph(3)));
    // Removing the marked curve clears its annotations, preserving the last.
    VERIFY(choosePlotChannelAction(group, "移除通道显示", names[0]));
    VERIFY(!first && group && group->graphCount() == 1 && group->graph(0) == last && groupDock->isVisible());
    for (int i = 0; i < group->itemCount(); ++i) if (auto text = qobject_cast<QCPItemText *>(group->item(i)))
        if (text->text().startsWith("X:") || text->text().startsWith("Δt:")) VERIFY(!text->visible());
    VERIFY(feed({12, 22, 32, 42}) && sameData(last, main->graph(2)));
    VERIFY(choosePlotChannelAction(group, "移除通道显示", names[2]));
    VERIFY((!groupDock || !groupDock->isVisible()) && otherDock && otherDock->isVisible());
    VERIFY(feed({13, 23, 33, 43}) && sameData(untouched, main->graph(3)));
    // Re-detaching proves channel mappings were moved individually. The
    // channel-list merge action shares the same removal behavior.
    VERIFY(detachPlotTestChannel(window, channels, 0));
    QPointer<QDockWidget> againDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + names[0]); VERIFY(againDock);
    auto again = againDock->findChild<QCustomPlot *>(); VERIFY(again);
    VERIFY(choosePlotChannelAction(again, "添加通道显示", names[1]) && again->graphCount() == 2);
    bool merged = false;
    QTimer::singleShot(0, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
            for (auto action : menu->actions()) if (action->text() == "合并回主视图") { action->trigger(); merged = true; break; }
            menu->close();
        }
    });
    VERIFY(QMetaObject::invokeMethod(&window, "onChannelContextMenu", Q_ARG(QPoint, channels->visualItemRect(channels->item(0)).center())) && merged);
    VERIFY(againDock && againDock->isVisible() && again->graphCount() == 1);
    for (int i = 0; i < 4; ++i) VERIFY(!main->graph(i)->visible());
    VERIFY(other && otherDock->isVisible() && other->graph(0) == untouched);
    window.close();
    std::cout << "Removing one grouped channel preserves the view, other histories/markers and ongoing per-channel updates; only an empty view closes.\n";
    return 0;
}

static int plotReadoutUiTests() {
    QTemporaryDir directory; VERIFY(directory.isValid());
    QSettings mainSettings("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    mainSettings.clear(); advanced.clear();
    mainSettings.setValue("editCacheDir", directory.path()); advanced.setValue("InterfaceType", 0);
    mainSettings.setValue("chkPausePlot", true);
    mainSettings.sync(); advanced.sync();
    ProtocolProfileStore store; QString error;
    VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1500, 950); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    QElapsedTimer ready; ready.start(); VERIFY(until([&] { return ready.elapsed() > 180; }));
    window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    window.findChild<QTimer *>("waveformRefreshTimer")->stop();
    auto paused = window.findChild<QPushButton *>("btnPausePlot");
    auto latest = window.findChild<QPushButton *>("btnScrollToLatest");
    VERIFY(paused && latest && paused->isVisible() && paused->isCheckable() && paused->isChecked());
    VERIFY(paused->parentWidget() == latest->parentWidget() && paused->geometry().bottom() < latest->y());
    QCheckBox *automatic = nullptr, *selectAll = nullptr;
    for (auto check : window.findChildren<QCheckBox *>()) {
        if (check->text() == "Y轴自适应") automatic = check;
        if (check->text().startsWith("全选/")) selectAll = check;
    }
    VERIFY(automatic && selectAll); automatic->setChecked(false);
    VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({1, 2}))));
    auto mainDock = window.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图")); VERIFY(mainDock);
    auto main = mainDock->findChild<QCustomPlot *>(); VERIFY(main && main->graphCount() == 2);
    QListWidget *channels = nullptr;
    for (auto list : window.findChildren<QListWidget *>()) if (list->count() == 2 && list->itemWidget(list->item(0))) channels = list;
    VERIFY(channels);
    auto first = channels->itemWidget(channels->item(0))->findChild<QCheckBox *>("chkChannel");
    auto second = channels->itemWidget(channels->item(1))->findChild<QCheckBox *>("chkChannel");
    VERIFY(first && second); first->setChecked(true); second->setChecked(false);
    VERIFY(detachPlotTestChannel(window, channels, 0));
    auto detachedDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + main->graph(0)->name()); VERIFY(detachedDock);
    auto detached = detachedDock->findChild<QCustomPlot *>(); VERIFY(detached && detached->graphCount() == 1);
    VERIFY(until([&] { return detached->isVisible() && detached->height() > 120; }));

    const auto pauseButtonFill = [&] {
        const QImage image = paused->grab().toImage();
        return image.pixelColor(image.width() / 10, image.height() / 2);
    };
    for (auto theme : {ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
        ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents();
        const bool dark = theme == ThemeManager::DarkTheme;
        VERIFY(paused->text() == "继续绘图（已暂停）" && !paused->icon().isNull());
        VERIFY(pauseButtonFill() == QColor(dark ? "#68431c" : "#85530f"));
        const auto resumeIcon = paused->icon().cacheKey();
        paused->click();
        VERIFY(!paused->isChecked() && paused->text() == "暂停绘图" && !paused->icon().isNull() && paused->icon().cacheKey() != resumeIcon);
        VERIFY(pauseButtonFill() == QColor(dark ? "#214b42" : "#236954"));
        paused->click();
        VERIFY(paused->isChecked() && paused->icon().cacheKey() == resumeIcon);
        for (auto plot : {main, detached}) {
            // Peaks/troughs at all four corners.
            for (const QPointF point : {QPointF(.02, 99.9), QPointF(9.98, 99.9), QPointF(.02, .1), QPointF(9.98, .1)}) {
                plot->graph(0)->setData(QVector<double>{point.x()}, QVector<double>{point.y()});
                plot->xAxis->setRange(0, 10); plot->yAxis->setRange(0, 100); plot->replot();
                QElapsedTimer hover; hover.start(); VERIFY(until([&] { return hover.elapsed() > 36; }));
                const QPointF pixel(plot->xAxis->coordToPixel(point.x()), plot->yAxis->coordToPixel(point.y()));
                QMouseEvent move(QEvent::MouseMove, pixel, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(plot, &move); plot->replot();
                VERIFY(plotReadoutsInside(plot));
                bool correct = false;
                for (int i = 0; i < plot->itemCount(); ++i) if (auto text = qobject_cast<QCPItemText *>(plot->item(i)))
                    if (text->visible() && text->text().startsWith("X:") && text->color() == QColor(Qt::green)) {
                        correct = text->text().contains(QString("Y: %1").arg(point.y(), 0, 'f', 2));
                        const QRectF rect(text->topLeft->pixelPosition(), text->bottomRight->pixelPosition());
                        if (point.y() > 90) VERIFY(rect.top() > pixel.y());
                        if (point.x() > 9) VERIFY(rect.right() < pixel.x());
                    }
                VERIFY(correct);
            }
            // Both point markers and their difference label sit near the top.
            plot->graph(0)->setData(QVector<double>{.02, 9.98}, QVector<double>{99.9, 99.8}); plot->replot();
            for (const QPointF point : {QPointF(.02, 99.9), QPointF(9.98, 99.8)}) {
                const QPointF pixel(plot->xAxis->coordToPixel(point.x()), plot->yAxis->coordToPixel(point.y()));
                QMouseEvent press(QEvent::MouseButtonPress, pixel, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(plot, &press);
                QMouseEvent click(QEvent::MouseButtonDblClick, pixel, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(plot, &click); plot->replot();
                QMouseEvent release(QEvent::MouseButtonRelease, pixel, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(plot, &release);
            }
            VERIFY(plotReadoutsInside(plot, 3));
            bool deltaCorrect = false;
            for (int i = 0; i < plot->itemCount(); ++i) if (auto text = qobject_cast<QCPItemText *>(plot->item(i)))
                if (text->visible() && text->text().startsWith("Δt:")) deltaCorrect = text->text().contains("Δy: 0.10");
            VERIFY(deltaCorrect);
            window.resize(1120, 740); QCoreApplication::processEvents();
            plot->xAxis->setRange(0, 20); plot->yAxis->setRange(-10, 99.95); plot->replot();
            VERIFY(plotReadoutsInside(plot, 3));
            plot->replot(); VERIFY(plotReadoutsInside(plot, 3));
            if (!artifactDirectory.isEmpty()) plot->grab().save(artifactDirectory +
                QString("/readouts-%1-%2.png").arg(plot == main ? "main" : "detached").arg(theme == ThemeManager::DarkTheme ? "dark" : "light"));
            // Third double click clears markers before the next theme/run.
            const QPointF center(plot->axisRect()->rect().center());
            QMouseEvent clear(QEvent::MouseButtonDblClick, center, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(plot, &clear);
            window.resize(1500, 950); QCoreApplication::processEvents();
        }
    }

    // Neither individual checkboxes nor select-all may hide detached graphs.
    first->setChecked(false); VERIFY(!main->graph(0)->visible() && detached->graph(0)->visible());
    selectAll->setChecked(true); VERIFY(main->graph(0)->visible() && main->graph(1)->visible() && detached->graph(0)->visible());
    selectAll->setChecked(false); VERIFY(!main->graph(0)->visible() && !main->graph(1)->visible() && detached->graph(0)->visible());
    const int mainCount = main->graph(0)->dataCount(), detachedCount = detached->graph(0)->dataCount();
    VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({16, 22}))));
    VERIFY(QMetaObject::invokeMethod(&window, "onPlotTimer"));
    VERIFY(main->graph(0)->dataCount() == mainCount && detached->graph(0)->dataCount() == detachedCount);
    paused->click();
    VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({17, 23}))));
    VERIFY(QMetaObject::invokeMethod(&window, "onPlotTimer"));
    VERIFY(main->graph(0)->dataCount() == mainCount + 1 && detached->graph(0)->dataCount() == detachedCount + 1);
    VERIFY(!main->graph(0)->visible() && detached->graph(0)->visible());
    // A channel unchecked BEFORE detaching also starts visible in its view.
    VERIFY(!second->isChecked() && detachPlotTestChannel(window, channels, 1));
    auto secondDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + main->graph(1)->name()); VERIFY(secondDock);
    VERIFY(secondDock->findChild<QCustomPlot *>()->graph(0)->visible());
    // Moving another unchecked channel into an existing detached view uses a
    // separate graph-creation route and must preserve the same rule.
    bool moved = false;
    QTimer::singleShot(0, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
            for (auto action : menu->actions()) if (action->menu() && action->text() == "添加通道显示") {
                for (auto add : action->menu()->actions()) if (add->text() == main->graph(1)->name()) { add->trigger(); moved = true; break; }
            }
            menu->close();
        }
    });
    detached->customContextMenuRequested(detached->axisRect()->rect().center());
    VERIFY(moved && detached->graphCount() == 2);
    for (int i = 0; i < detached->graphCount(); ++i) VERIFY(detached->graph(i)->visible());
    selectAll->setChecked(true); selectAll->setChecked(false);
    for (int i = 0; i < detached->graphCount(); ++i) VERIFY(detached->graph(i)->visible() && !main->graph(i)->visible());
    auto merge = window.findChild<QPushButton *>("btnMergeAllViews"); VERIFY(merge); merge->click();
    VERIFY(!main->graph(0)->visible() && !main->graph(1)->visible() && !first->isChecked() && !second->isChecked());
    window.close();
    std::cout << "Main/detached readouts stay inside after hover, marking, resize and zoom; detached visibility is independent.\n";
    return 0;
}
