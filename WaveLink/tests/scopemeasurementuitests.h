#pragma once
#include "plotmeasurementcontroller.h"

static QAction *scopeAction(QMenu *menu, const QString &text) {
    for (auto action : menu->actions()) {
        if (action->text() == text) return action;
        if (action->menu()) if (auto found = scopeAction(action->menu(), text)) return found;
    }
    return nullptr;
}
static double scopeValue(QTableWidget *table, const QString &id) {
    for (int row = 0; row < table->rowCount(); ++row) for (int col : {0, 2})
        if (table->item(row, col) && table->item(row, col)->data(Qt::UserRole).toString() == id)
            return table->item(row, col + 1)->data(Qt::UserRole).toDouble();
    return std::numeric_limits<double>::quiet_NaN();
}
static bool scopeNear(double value, double expected, double tolerance = .005) {
    return std::isfinite(value) && std::abs(value - expected) < tolerance;
}
static bool openScope(QCustomPlot *plot) {
    bool opened = false;
    QTimer::singleShot(0, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
            if (auto action = scopeAction(menu, "显示测量栏")) { action->trigger(); opened = true; }
            menu->close();
        }
    });
    plot->customContextMenuRequested(plot->axisRect()->rect().center()); return opened;
}
static void dragScope(QCustomPlot *plot, QPointF from, QPointF to) {
    QMouseEvent press(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(plot, &press);
    QMouseEvent move(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(plot, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier); QApplication::sendEvent(plot, &release);
    plot->replot(); QApplication::processEvents();
}
static bool scopePlotAction(QCustomPlot *plot, const QString &name, bool mainView = true) {
    bool triggered = false, complete = false;
    QTimer::singleShot(0, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
            QAction *selected = nullptr; QSet<QString> actions;
            for (auto action : menu->actions()) {
                actions.insert(action->objectName());
                if (!name.isEmpty() && action->objectName() == name) selected = action;
            }
            complete = scopeAction(menu, "显示测量栏");
            if (mainView) complete = complete && actions.contains("plotClearAction") && actions.contains("plotImportAction") && actions.contains("plotExportAction");
            else {
                complete = complete && !actions.contains("plotClearAction") && !actions.contains("plotImportAction") && !actions.contains("plotExportAction") &&
                    scopeAction(menu, "合并回主视图") && scopeAction(menu, "添加通道显示") && scopeAction(menu, "移除通道显示");
                triggered = name.isEmpty();
            }
            if (selected) { triggered = true; selected->trigger(); }
            menu->close();
        }
    });
    plot->customContextMenuRequested(plot->axisRect()->rect().center());
    return triggered && complete;
}
static int scopeMeasurementUiTests() {
    QTemporaryDir folder; VERIFY(folder.isValid());
    QSettings settings("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"); settings.clear(); advanced.clear();
    settings.setValue("editCacheDir", folder.path()); advanced.setValue("InterfaceType", 0); settings.sync(); advanced.sync();
    ProtocolProfileStore store; QString error; VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1500, 1000); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    QElapsedTimer ready; ready.start(); VERIFY(until([&] { return ready.elapsed() > 180; }));
    for (auto timer : window.findChildren<QTimer *>())
        if (timer->objectName() == "serialHardwareCheckTimer" || timer->objectName() == "waveformRefreshTimer") timer->stop();
    for (auto check : window.findChildren<QCheckBox *>()) {
        if (check->text() == "Y轴自适应") check->setChecked(false);
        if (check->text() == "暂停绘图") check->setChecked(true);
    }
    VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({0, 0}))));
    auto dock = window.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图")); VERIFY(dock);
    auto main = dock->findChild<QCustomPlot *>(); VERIFY(main && main->graphCount() == 2);
    QVector<double> times, first, second;
    for (int i = 0; i <= 2000; ++i) { const double t = i / 1000.; times.append(t); first.append(std::sin(2 * M_PI * 5 * t)); second.append(1 + 2 * std::sin(2 * M_PI * 10 * t)); }
    main->graph(0)->setData(times, first); main->graph(1)->setData(times, second);
    main->xAxis->setRange(0, 1); main->yAxis->setRange(-2, 4); main->replot();
    auto scope = main->findChild<PlotMeasurementController *>(); VERIFY(scope && !scope->isEnabled());
    VERIFY(openScope(main) && scope->isEnabled());
    auto panel = scope->panel(); auto table = panel->findChild<QTableWidget *>("scopeResults");
    auto channel = panel->findChild<QComboBox *>("scopeChannel"), mode = panel->findChild<QComboBox *>("scopeCursorMode"), range = panel->findChild<QComboBox *>("scopeRange");
    VERIFY(table && channel && mode && range && channel->count() == 2);
    VERIFY(until([&] { return scopeNear(scope->lastResult().frequency, 5); }));
    VERIFY(scopeNear(scopeValue(table, "rms"), std::sqrt(.5), .002));
    const QString originalName = main->graph(1)->name(); main->graph(1)->setName(main->graph(0)->name()); scope->refresh();
    VERIFY(channel->itemText(0) != channel->itemText(1)); main->graph(1)->setName(originalName); scope->refresh();
    const auto originalRange = main->xAxis->range();
    auto line = main->findChild<QCPItemLine *>("scopeTimeCursorA"); VERIFY(line && line->visible());
    const double middleY = main->axisRect()->rect().center().y();
    dragScope(main, {line->start->pixelPosition().x(), middleY}, {main->xAxis->coordToPixel(.35), middleY});
    VERIFY(scopeNear(scopeValue(table, "timeA"), .35, .002) && main->xAxis->range() == originalRange);
    mode->setCurrentIndex(PlotMeasurementController::BothCursors); main->replot();
    auto horizontal = main->findChild<QCPItemLine *>("scopeAmplitudeCursorA"); VERIFY(horizontal && horizontal->visible());
    const double x = main->axisRect()->rect().right() - 20;
    dragScope(main, {x, horizontal->start->pixelPosition().y()}, {x, main->yAxis->coordToPixel(.25)});
    VERIFY(scopeNear(scopeValue(table, "amplitudeA"), .25, .02));
    auto snap = panel->findChild<QCheckBox *>("scopeSnap"); VERIFY(snap); snap->setChecked(false);
    dragScope(main, {line->start->pixelPosition().x(), middleY}, {main->xAxis->coordToPixel(.3537), middleY});
    VERIFY(scopeNear(scopeValue(table, "timeA"), .3537, 1e-7));
    VERIFY(scopeNear(scopeValue(table, "valueA"), std::sin(2 * M_PI * 5 * .3537), .001));
    range->setCurrentIndex(1); scope->refresh();
    VERIFY(until([&] { return scope->lastResult().samples > 390 && scope->lastResult().samples < 405; }));
    VERIFY(std::isnan(scope->lastResult().frequency));
    range->setCurrentIndex(0); channel->setCurrentIndex(channel->findData(1));
    VERIFY(until([&] { return scopeNear(scope->lastResult().frequency, 10); }));
    channel->setCurrentIndex(channel->findData(0));
    VERIFY(until([&] { return scopeNear(scope->lastResult().frequency, 5); }));
    // All statistic and automatic projects can be selected independently.
    QMenu projects; scope->appendMenu(&projects); auto all = scopeAction(&projects, "选择全部项目"); VERIFY(all); all->trigger();
    VERIFY(scopeNear(scopeValue(table, "minimum"), -1) && scopeNear(scopeValue(table, "maximum"), 1));
    VERIFY(scopeNear(scopeValue(table, "positiveWidth"), .1) && std::isfinite(scopeValue(table, "riseTime")));
    bool positions = false;
    QTimer::singleShot(0, [&] {
        if (auto dialog = window.findChild<QDialog *>("scopePositionDialog")) {
            dialog->findChild<QDoubleSpinBox *>("scopePosition0")->setValue(.2);
            dialog->findChild<QDoubleSpinBox *>("scopePosition1")->setValue(.8);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click(); positions = true;
        }
    });
    auto positionAction = scopeAction(&projects, "精确设置光标位置…"); VERIFY(positionAction); positionAction->trigger();
    VERIFY(positions && scopeNear(scopeValue(table, "deltaTime"), .6));
    bool rejected = false, thresholds = false;
    QTimer::singleShot(0, [&] {
        if (auto dialog = window.findChild<QDialog *>("scopeThresholdDialog")) {
            auto low = dialog->findChild<QDoubleSpinBox *>("scopeReference0"); auto buttons = dialog->findChild<QDialogButtonBox *>();
            low->setValue(60); buttons->button(QDialogButtonBox::Ok)->click();
            rejected = !dialog->findChild<QLabel *>("scopeThresholdError")->text().isEmpty();
            low->setValue(20); dialog->findChild<QDoubleSpinBox *>("scopeReference2")->setValue(80);
            auto automatic = dialog->findChild<QCheckBox *>("scopeAutomaticLevels"); automatic->setChecked(false);
            dialog->findChild<QLineEdit *>("scopeLowLevel")->clear(); automatic->setChecked(true);
            buttons->button(QDialogButtonBox::Ok)->click(); thresholds = true;
            if (dialog->isVisible()) { thresholds = false; dialog->reject(); }
        }
    });
    auto thresholdAction = scopeAction(&projects, "自动测量阈值设置…"); VERIFY(thresholdAction); thresholdAction->trigger();
    VERIFY(thresholds && rejected && until([&] { return scopeNear(scope->lastResult().frequency, 5); }));
    QListWidget *channels = nullptr;
    for (auto list : window.findChildren<QListWidget *>()) if (list->count() == 2 && list->itemWidget(list->item(0))) channels = list;
    VERIFY(channels); const auto firstName = channels->itemWidget(channels->item(0))->findChild<QCheckBox *>("chkChannel")->text();
    const auto secondName = channels->itemWidget(channels->item(1))->findChild<QCheckBox *>("chkChannel")->text();
    channels->itemWidget(channels->item(1))->findChild<QCheckBox *>("chkChannel")->setChecked(false);
    VERIFY(detachPlotTestChannel(window, channels, 1));
    auto detachedDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + secondName); VERIFY(detachedDock);
    auto detached = detachedDock->findChild<QCustomPlot *>(); auto separateScope = detached->findChild<PlotMeasurementController *>(); VERIFY(separateScope && !separateScope->isEnabled());
    detached->xAxis->setRange(0, 1); detached->yAxis->setRange(-2, 4); detached->replot();
    VERIFY(openScope(detached) && until([&] { return scopeNear(separateScope->lastResult().frequency, 10); }));
    VERIFY(mode->currentIndex() == PlotMeasurementController::BothCursors && scopeNear(scopeValue(table, "deltaTime"), .6));
    VERIFY(choosePlotChannelAction(detached, "添加通道显示", firstName));
    VERIFY(choosePlotChannelAction(detached, "移除通道显示", secondName) && detached->graphCount() == 1);
    VERIFY(until([&] { return scopeNear(separateScope->lastResult().frequency, 5); }));
    for (auto theme : {ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
        ThemeManager::applyTheme(qApp, theme);
        for (auto plot : {main, detached}) ThemeManager::setupPlotTheme(plot, theme);
        window.resize(1250, 1000); QApplication::processEvents(); main->replot(); detached->replot();
        for (auto measurement : {scope, separateScope}) {
            const auto view = measurement->panel();
            for (auto combo : view->findChildren<QComboBox *>()) if (combo->isVisible())
                VERIFY(combo->mapTo(view, QPoint()).x() + combo->width() <= view->width());
            const auto originalFont = view->font();
            auto results = view->findChild<QTableWidget *>("scopeResults");
            for (int points : {9, 11, 14, 9}) {
                auto font = originalFont; font.setPointSize(points); view->setFont(font);
                results->scrollToTop();
                VERIFY(until([&] {
                    auto header = results->horizontalHeader();
                    return header->height() >= header->sizeHint().height() &&
                        header->height() >= header->fontMetrics().height() + 8 &&
                        results->rowHeight(0) >= results->fontMetrics().height() + 2 &&
                        results->rowViewportPosition(2) + results->rowHeight(2) <= results->viewport()->height();
                }));
                if (points == 14 && measurement == scope && !artifactDirectory.isEmpty())
                    view->grab().save(artifactDirectory + (theme == ThemeManager::DarkTheme ? "/scope-header-dark-large.png" : "/scope-header-light-large.png"));
            }
            view->setFont(originalFont); QApplication::processEvents();
        }
        if (!artifactDirectory.isEmpty()) window.grab().save(artifactDirectory + (theme == ThemeManager::DarkTheme ? "/scope-dark.png" : "/scope-light.png"));
    }
    // Global data operations belong only to the main plot. Detached views keep
    // their measurement and channel-management menus.
    VERIFY(scopePlotAction(detached, {}, false));
    for (const auto &action : {QString("plotImportAction"), QString("plotExportAction")}) {
        bool picked = false; QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, [&] {
            for (auto widget : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QFileDialog *>(widget)) if (dialog->isVisible()) {
                picked = dialog->acceptMode() == (action == "plotImportAction" ? QFileDialog::AcceptOpen : QFileDialog::AcceptSave); dialog->reject();
            }
        }); driver.start(10);
        VERIFY(scopePlotAction(main, action)); driver.stop();
        VERIFY(picked && main->graph(0)->dataCount() == times.size() && detached->graph(0)->dataCount() == times.size());
    }
    const auto exportPath = folder.filePath("context-waveform.csv");
    bool exported = false; QTimer exportDriver;
    QObject::connect(&exportDriver, &QTimer::timeout, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QFileDialog *>(widget)) if (dialog->isVisible() && !exported) {
            exported = true; dialog->selectFile(exportPath); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        }
    }); exportDriver.start(10);
    VERIFY(scopePlotAction(main, "plotExportAction")); exportDriver.stop();
    QFile exportedFile(exportPath); VERIFY(exported && exportedFile.open(QIODevice::ReadOnly));
    const auto csv = exportedFile.readAll();
    VERIFY(csv.startsWith(RecordingChannels::csvUtf8Bom() + "Time(s),Ch1,Ch2") && csv.count('\n') == times.size() + 1);
    exportedFile.close();
    auto clearButton = window.findChild<QPushButton *>("btnClearPlots"); VERIFY(clearButton);
    const auto requestClear = [&](int source, bool accept) {
        bool seen = false, cancelDefault = false, readable = false; QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, [&] {
            auto dialog = window.findChild<QDialog *>("clearPlotsConfirmDialog");
            if (!dialog || !dialog->isVisible() || seen) return;
            seen = true;
            auto buttons = dialog->findChild<QDialogButtonBox *>(); auto message = dialog->findChild<QLabel *>();
            cancelDefault = buttons && buttons->button(QDialogButtonBox::Cancel)->isDefault();
            readable = message && message->height() >= message->heightForWidth(message->width());
            if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + "/plot-clear-confirm.png");
            if (buttons) buttons->button(accept ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
            else dialog->reject();
        }); driver.start(10);
        bool dispatched = true;
        if (source == 0) clearButton->click();
        else dispatched = scopePlotAction(main, "plotClearAction");
        driver.stop(); return dispatched && seen && cancelDefault && readable;
    };
    for (int source : {0, 1}) {
        VERIFY(requestClear(source, false));
        VERIFY(main->graph(0)->dataCount() == times.size() && detached->graph(0)->dataCount() == times.size());
        VERIFY(scope->lastResult().samples > 0 && separateScope->lastResult().samples > 0);
    }
    for (int source : {0, 1}) {
        VERIFY(requestClear(source, true));
        for (auto plot : {main, detached}) {
            for (int graph = 0; graph < plot->graphCount(); ++graph) VERIFY(plot->graph(graph)->dataCount() == 0);
            VERIFY(!plot->findChild<QCPItemLine *>("scopeTimeCursorA")->visible());
        }
        VERIFY(scope->lastResult().samples == 0 && separateScope->lastResult().samples == 0);
        main->graph(0)->setData(times, first); main->graph(1)->setData(times, second); detached->graph(0)->setData(times, first);
        for (auto plot : {main, detached}) plot->xAxis->setRange(0, 1);
        scope->refresh(); separateScope->refresh();
        VERIFY(until([&] { return scopeNear(scope->lastResult().frequency, 5) && scopeNear(separateScope->lastResult().frequency, 5); }));
    }
    auto fold = panel->findChild<QToolButton *>("scopeFold"); VERIFY(fold); fold->setChecked(false); VERIFY(!table->isVisible()); fold->setChecked(true);
    VERIFY(QMetaObject::invokeMethod(&window, "onClearPlots"));
    VERIFY(std::isnan(scopeValue(table, "frequency")) && scope->lastResult().samples == 0);
    auto clear = scopeAction(&projects, "清除测量"); VERIFY(clear); clear->trigger(); VERIFY(!scope->isEnabled() && !line->visible());
    scope->setEnabled(true); window.close();
    MainWindow restored(nullptr, new TestSerialHandler(new ReceivePort)); restored.show();
    auto restoredDock = restored.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图")); VERIFY(restoredDock);
    auto restoredScope = restoredDock->findChild<PlotMeasurementController *>(); VERIFY(restoredScope && restoredScope->isEnabled());
    VERIFY(restoredScope->panel()->findChild<QComboBox *>("scopeCursorMode")->currentIndex() == PlotMeasurementController::BothCursors);
    VERIFY(restoredScope->lastResult().samples == 0); restored.close();
    std::cout << "Per-view measurements, adaptive headers, main-only data menus, export, clear confirmation, themes and preferences passed.\n";
    return 0;
}
