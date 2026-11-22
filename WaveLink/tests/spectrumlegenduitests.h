#pragma once

static int spectrumLegendUiTests() {
    QTemporaryDir directory; VERIFY(directory.isValid());
    QSettings settings("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    settings.clear(); advanced.clear(); settings.setValue("editCacheDir", directory.path());
    advanced.setValue("InterfaceType", 0); settings.sync(); advanced.sync();
    ProtocolProfileStore store; QString error;
    VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1500, 950); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>(8, 0))));
    VERIFY(QMetaObject::invokeMethod(&window, "onPlotTimer"));
    auto dock = window.findChild<QDockWidget *>("DockSpectrum");
    auto spectrum = window.findChild<QCustomPlot *>("spectrumPlot");
    auto mainDock = window.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图"));
    auto selector = window.findChild<QToolButton *>("btnSpectrumChannels");
    VERIFY(dock && spectrum && mainDock && selector && selector->menu()->actions().size() == 8);
    auto main = mainDock->findChild<QCustomPlot *>(); VERIFY(main && main->graphCount() == 8);
    for (auto timer : window.findChildren<QTimer *>()) timer->stop();
    for (auto check : window.findChildren<QCheckBox *>())
        if (check->text().contains("陷波") || check->text().contains("高通") || check->text().contains("低通")) check->setChecked(true);
    QStringList names;
    for (int ch = 0; ch < 8; ++ch) {
        names.append(ch == 0 ? QStringLiteral("EEG1") : QStringLiteral("脑电监测通道_%1_很长的通道名称 (uV)").arg(ch + 1));
        main->graph(ch)->setName(names.last());
        main->graph(ch)->setPen(QPen(QColor::fromHsv(ch * 40, 220, 180)));
        QVector<double> keys, values;
        for (int i = 0; i < 512; ++i) { keys.append(i / 250.0); values.append((ch + 1) * std::sin(2 * M_PI * (ch + 5) * i / 250)); }
        main->graph(ch)->setData(keys, values);
        selector->menu()->actions()[ch]->setChecked(true);
    }
    main->xAxis->setRange(0, 2.1);
    dock->setFloating(true); dock->resize(1050, 450); dock->show();
    VERIFY(QMetaObject::invokeMethod(&window, "onSpectrumTimer"));
    QCoreApplication::processEvents();
    QCPItemText *filters = nullptr;
    for (int i = 0; i < spectrum->itemCount(); ++i) if (auto text = qobject_cast<QCPItemText *>(spectrum->item(i)))
        if (text->visible() && text->text().contains("陷波:")) filters = text;
    VERIFY(filters && filters->text().contains("高通:") && filters->text().contains("低通:"));
    for (auto theme : {ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
        ThemeManager::applyTheme(qApp, theme); ThemeManager::setupPlotTheme(spectrum, theme);
        for (int width : {980, 300}) {
            spectrum->setFixedSize(width, 240); QCoreApplication::processEvents(); spectrum->replot();
            VERIFY(spectrum->legend->visible() && spectrum->legend->itemCount() == 8);
            const QRectF axes(spectrum->axisRect()->rect()), legend(spectrum->legend->outerRect());
            const QRectF badge(filters->topLeft->pixelPosition(), filters->bottomRight->pixelPosition());
            if (!axes.contains(legend) || !axes.contains(badge) || legend.intersects(badge))
                std::cerr << "Spectrum overlays: width=" << width << " axes=" << axes.x() << ',' << axes.y() << ',' << axes.width() << ',' << axes.height()
                    << " legend=" << legend.x() << ',' << legend.y() << ',' << legend.width() << ',' << legend.height()
                    << " filters=" << badge.x() << ',' << badge.y() << ',' << badge.width() << ',' << badge.height() << '\n';
            VERIFY(axes.contains(legend) && axes.contains(badge) && !legend.intersects(badge));
            VERIFY((spectrum->legend->textColor().lightness() > 128) == (theme == ThemeManager::DarkTheme));
            VERIFY(filters->color() == spectrum->legend->textColor());
            for (int ch = 0; ch < 8; ++ch) {
                auto graph = spectrum->graph(ch);
                VERIFY(graph->visible() && spectrum->legend->hasItemWithPlottable(graph));
                VERIFY(graph->property("spectrumChannelName").toString() == names[ch]);
                VERIFY(graph->pen().color() == main->graph(ch)->pen().color());
            }
            QMouseEvent hover(QEvent::MouseMove, legend.center(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(spectrum, &hover);
            VERIFY(spectrum->toolTip().contains(names[7]));
            if (!artifactDirectory.isEmpty()) spectrum->grab().save(artifactDirectory + QString("/spectrum-legend-%1-%2.png").arg(int(theme)).arg(width));
        }
    }
    // Selection and invalid data remove both the curve and its stale legend entry.
    for (int ch = 1; ch < 8; ++ch) selector->menu()->actions()[ch]->setChecked(false);
    main->graph(0)->setName(QStringLiteral("重新命名的通道")); main->graph(0)->setPen(QPen(QColor("#1682cc")));
    VERIFY(QMetaObject::invokeMethod(&window, "onSpectrumTimer")); spectrum->replot();
    VERIFY(spectrum->legend->itemCount() == 1 && spectrum->graph(0)->property("spectrumChannelName").toString() == QStringLiteral("重新命名的通道"));
    VERIFY(spectrum->graph(0)->pen().color() == QColor("#1682cc"));
    main->graph(0)->data()->clear();
    VERIFY(QMetaObject::invokeMethod(&window, "onSpectrumTimer")); spectrum->replot();
    VERIFY(!spectrum->legend->visible() && spectrum->legend->itemCount() == 0);
    selector->menu()->actions()[0]->setChecked(false);
    VERIFY(QMetaObject::invokeMethod(&window, "onSpectrumTimer")); spectrum->replot();
    VERIFY(!spectrum->legend->visible());
    window.close(); dock->close();
    std::cout << "Spectrum channel legends: colors, names, selection, empty data, filter separation, narrow plots and both themes passed.\n";
    return 0;
}
