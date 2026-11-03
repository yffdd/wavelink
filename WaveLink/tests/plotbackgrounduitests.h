#pragma once

#include <QColorDialog>

static bool editPlotBackground(QCustomPlot *plot, const std::function<bool(QDialog *)> &edit) {
    bool opened = false, passed = false;
    QTimer::singleShot(0, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
            for (auto action : menu->actions()) if (action->objectName() == "plotBackgroundAction") {
                QTimer::singleShot(0, [&] {
                    for (auto top : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QDialog *>(top))
                        if (dialog->objectName() == "plotBackgroundDialog" && dialog->isVisible()) {
                            opened = true; passed = edit(dialog);
                            if (dialog->isVisible()) dialog->reject();
                        }
                });
                action->trigger();
                break;
            }
            menu->close();
        }
    });
    plot->customContextMenuRequested(plot->axisRect()->rect().center());
    return opened && passed;
}

static int plotBackgroundUiTests() {
    QTemporaryDir directory; VERIFY(directory.isValid());
    QSettings settings("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    settings.clear(); advanced.clear(); settings.setValue("editCacheDir", directory.path());
    advanced.setValue("InterfaceType", 0); settings.sync(); advanced.sync();
    ProtocolProfileStore store; QString error;
    VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    {
        MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1500, 950); window.show();
        VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
        window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        window.findChild<QTimer *>("waveformRefreshTimer")->stop();
        VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({1, 2, 3}))));
        auto mainDock = window.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图")); VERIFY(mainDock);
        auto main = mainDock->findChild<QCustomPlot *>(); VERIFY(main && main->graphCount() == 3);
        auto theme = window.findChild<QComboBox *>("comboTheme"); VERIFY(theme);
        QListWidget *channels = nullptr;
        for (auto list : window.findChildren<QListWidget *>()) if (list->count() == 3 && list->itemWidget(list->item(0))) channels = list;
        VERIFY(channels && detachPlotTestChannel(window, channels, 0));
        auto detachedDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + main->graph(0)->name()); VERIFY(detachedDock);
        auto detached = detachedDock->findChild<QCustomPlot *>(); VERIFY(detached);
        const QPen curve = main->graph(0)->pen();
        theme->setCurrentIndex(1); QApplication::processEvents();
        VERIFY(main->axisRect()->backgroundBrush().color() == Qt::white);
        // Entry exists only in the main view, where both view types are configured.
        bool unexpected = false;
        QTimer::singleShot(0, [&] {
            for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
                for (auto action : menu->actions()) if (action->objectName() == "plotBackgroundAction") unexpected = true;
                menu->close();
            }
        });
        detached->customContextMenuRequested(detached->axisRect()->rect().center()); VERIFY(!unexpected);
        VERIFY(editPlotBackground(main, [&](QDialog *dialog) {
            auto a = dialog->findChild<QComboBox *>("mainPlotBackgroundChoice");
            auto b = dialog->findChild<QComboBox *>("detachedPlotBackgroundChoice");
            if (!a || !b || a->currentIndex() != 0 || b->currentIndex() != 0) return false;
            a->setCurrentIndex(1); b->setCurrentIndex(2); QApplication::processEvents();
            const bool ok = main->axisRect()->backgroundBrush().color() == QColor("#1e1e1e") &&
                detached->axisRect()->backgroundBrush().color() == Qt::white && main->graph(0)->pen() == curve;
            if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + "/background-dialog-light.png");
            dialog->reject(); return ok;
        }));
        VERIFY(main->axisRect()->backgroundBrush().color() == Qt::white && !settings.contains("PlotBackground/Main"));
        VERIFY(editPlotBackground(main, [&](QDialog *dialog) {
            auto a = dialog->findChild<QComboBox *>("mainPlotBackgroundChoice");
            auto b = dialog->findChild<QComboBox *>("detachedPlotBackgroundChoice");
            if (!a || !b) return false;
            a->setCurrentIndex(1); b->setCurrentIndex(2); dialog->accept(); return true;
        }));
        settings.sync(); VERIFY(settings.value("PlotBackground/Main").toString() == "#1e1e1e");
        VERIFY(settings.value("PlotBackground/Detached").toString() == "#ffffff");
        theme->setCurrentIndex(0); QApplication::processEvents();
        VERIFY(main->axisRect()->backgroundBrush().color() == QColor("#1e1e1e") && detached->axisRect()->backgroundBrush().color() == Qt::white);
        VERIFY(main->xAxis->tickLabelColor().lightness() > 200 && detached->xAxis->tickLabelColor().lightness() < 60);
        VERIFY(detachPlotTestChannel(window, channels, 1));
        auto newDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + main->graph(1)->name()); VERIFY(newDock);
        VERIFY(newDock->findChild<QCustomPlot *>()->axisRect()->backgroundBrush().color() == Qt::white);
        VERIFY(editPlotBackground(main, [&](QDialog *dialog) {
            auto picker = dialog->findChild<QPushButton *>("mainPlotBackgroundPicker");
            if (!picker) return false;
            bool chosen = false;
            QTimer::singleShot(0, [&] {
                for (auto top : QApplication::topLevelWidgets()) if (auto colors = qobject_cast<QColorDialog *>(top)) if (colors->isVisible()) {
                    colors->setCurrentColor(QColor("#425363")); chosen = true; colors->accept();
                }
            });
            picker->click(); QApplication::processEvents();
            const bool ok = chosen && main->axisRect()->backgroundBrush().color() == QColor("#425363") &&
                detached->axisRect()->backgroundBrush().color() == Qt::white && main->graph(0)->pen() == curve;
            if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + "/background-dialog-dark.png");
            dialog->accept(); return ok;
        }));
        window.close();
    }
    {
        // Actual restart restores colors, and newly detached plots inherit them.
        MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1500, 950); window.show();
        VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
        window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        window.findChild<QTimer *>("waveformRefreshTimer")->stop();
        VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({1, 2, 3}))));
        auto main = window.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图"))->findChild<QCustomPlot *>(); VERIFY(main);
        VERIFY(main->axisRect()->backgroundBrush().color() == QColor("#425363"));
        auto theme = window.findChild<QComboBox *>("comboTheme"); VERIFY(theme);
        theme->setCurrentIndex(1); QApplication::processEvents();
        VERIFY(main->axisRect()->backgroundBrush().color() == QColor("#425363"));
        QListWidget *channels = nullptr;
        for (auto list : window.findChildren<QListWidget *>()) if (list->count() == 3 && list->itemWidget(list->item(0))) channels = list;
        VERIFY(channels && detachPlotTestChannel(window, channels, 0));
        auto detached = window.findChild<QDockWidget *>("PlotDock_独立视图: " + main->graph(0)->name())->findChild<QCustomPlot *>(); VERIFY(detached);
        VERIFY(detached->axisRect()->backgroundBrush().color() == Qt::white);
        VERIFY(editPlotBackground(main, [&](QDialog *dialog) {
            auto buttons = dialog->findChild<QDialogButtonBox *>("plotBackgroundButtons");
            if (!buttons) return false;
            buttons->button(QDialogButtonBox::RestoreDefaults)->click();
            const bool ok = main->axisRect()->backgroundBrush().color() == Qt::white && detached->axisRect()->backgroundBrush().color() == Qt::white;
            dialog->accept(); return ok;
        }));
        settings.sync(); VERIFY(!settings.contains("PlotBackground/Main") && !settings.contains("PlotBackground/Detached"));
        theme->setCurrentIndex(0); QApplication::processEvents();
        VERIFY(main->axisRect()->backgroundBrush().color() == QColor("#1e1e1e") && detached->axisRect()->backgroundBrush().color() == QColor("#1e1e1e"));
        window.close();
    }
    std::cout << "Plot backgrounds: scoped menu, presets, custom picker, preview cancellation, restart, new views and theme following passed.\n";
    return 0;
}
