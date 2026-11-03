#pragma once
#include "wlrrecordingwriter.h"

static bool offlineHistoryFileAction(MainWindow &window, const QString &path, bool importing, bool cancel = false) {
    bool picked = false; QTimer driver; QElapsedTimer elapsed; elapsed.start();
    QObject::connect(&driver, &QTimer::timeout, [&] {
        if (cancel) for (auto progress : window.findChildren<QProgressDialog *>()) if (progress->windowTitle() == "导入数据") progress->cancel();
        for (auto widget : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QFileDialog *>(widget)) if (dialog->isVisible()) {
            if (!picked) { picked = true; dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); }
            else if (elapsed.elapsed() > 15000) dialog->reject();
        }
    }); driver.start(10);
    QPushButton *button = nullptr;
    const QString caption = importing ? QStringLiteral("导入数据") : QStringLiteral("导出当前波形");
    for (auto candidate : window.findChildren<QPushButton *>()) if (candidate->text().contains(caption)) button = candidate;
    if (!button) return false;
    button->click();
    return picked;
}

static int offlineHistoryUiTests(OffscreenMessages &messages, bool paged = false) {
    QTemporaryDir folder; VERIFY(folder.isValid()); QString error;
    QSettings settings("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"); settings.clear(); advanced.clear();
    settings.setValue("editCacheDir", folder.path()); settings.setValue("spinPlotHistoryLimit", 16);
    settings.setValue("Display/OfflineMemoryMB", paged ? 16 : 256);
    advanced.setValue("InterfaceType", 0); settings.sync(); advanced.sync();
    ProtocolProfileStore store; VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));

    // Two full-resolution graphs alone exceed the smallest real-time cap.
    constexpr int Rows = 800001;
    constexpr double Rate = 1000;
    const double lastTime = (Rows - 1) / Rate;
    VERIFY(qint64(Rows) * 2 * sizeof(QCPGraphData) > 16LL * 1024 * 1024);
    const QStringList names{"离线 A", "离线 B"};
    const auto csvPath = folder.filePath("complete.csv"), wlrPath = folder.filePath("complete.wlr");
    QFile source(csvPath); VERIFY(source.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&source); out.setCodec("UTF-8"); out.setGenerateByteOrderMark(true); out.setRealNumberPrecision(17);
    out << "Time(s)," << names.join(',') << '\n';
    WlrRecordingWriter writer; VERIFY(writer.start(wlrPath, error, Rate, names));
    QVector<double> frame(2);
    for (int i = 0; i < Rows; ++i) {
        frame[0] = i == 0 ? -1000 : i == Rows - 1 ? 2000 : i % 100;
        frame[1] = -i / 8.0;
        out << i / Rate << ',' << frame[0] << ',' << frame[1] << '\n';
        VERIFY(writer.append(frame, error, 1700000000000000000LL + qint64(i) * 1000000));
    }
    out.flush(); VERIFY(out.status() == QTextStream::Ok && writer.finish(error)); source.close();
    std::cout << "Large CSV/WLR offline fixtures written.\n";

    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1400, 1000); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    QElapsedTimer ready; ready.start(); VERIFY(until([&] { return ready.elapsed() > 180; }));
    for (auto timer : window.findChildren<QTimer *>())
        if (timer->objectName() == "serialHardwareCheckTimer" || timer->objectName() == "waveformRefreshTimer") timer->stop();
    QCheckBox *lowpass = nullptr;
    for (auto check : window.findChildren<QCheckBox *>()) {
        if (check->text().startsWith("低通")) lowpass = check;
        if (check->text().startsWith("陷波") || check->text().startsWith("高通") || check->text().startsWith("低通")) check->setChecked(false);
    }
    auto dock = window.findChild<QDockWidget *>(QStringLiteral("PlotDock_主视图")); VERIFY(dock && lowpass);
    auto main = dock->findChild<QCustomPlot *>(); VERIFY(main);
    auto limit = window.findChild<QSpinBox *>("spinPlotHistoryLimit");
    auto usage = window.findChild<QLabel *>("lblPlotHistoryUsage");
    auto slider = window.findChild<RangeSlider *>("timeRangeSlider"); VERIFY(limit && usage && slider && limit->value() == 16);
    const auto complete = [&](QCPGraph *graph) {
        if (paged) {
            auto indexed = dynamic_cast<WaveformGraph *>(graph); if (!indexed || !indexed->hasOfflineSource() || indexed->sourceSampleCount() != Rows) return false;
            QVector<QCPGraphData> nearby; QString problem;
            if (!indexed->nearbySamples(0, nearby, problem) || nearby.isEmpty() || std::abs(nearby.first().key) > 1e-9) return false;
            return indexed->nearbySamples(lastTime, nearby, problem) && !nearby.isEmpty() && std::abs(nearby.last().key - lastTime) < 1e-7;
        }
        return graph && graph->dataCount() == Rows && std::abs(graph->data()->constBegin()->key) < 1e-9 &&
            std::abs((graph->data()->constEnd() - 1)->key - lastTime) < 1e-7;
    };
    for (const auto &path : {csvPath, wlrPath}) {
        messages.lastTitle.clear(); const bool imported = offlineHistoryFileAction(window, path, true);
        if (!imported || messages.lastTitle != "导入成功") std::cerr << "Offline import picked=" << imported << " title=" << messages.lastTitle.toStdString() << " text=" << messages.lastText.toStdString() << '\n';
        VERIFY(imported && messages.lastTitle == "导入成功");
        VERIFY(main->graphCount() == 2 && complete(main->graph(0)) && complete(main->graph(1)));
        VERIFY(main->graph(0)->data()->constBegin()->value == -1000 && (main->graph(0)->data()->constEnd() - 1)->value == 2000);
        VERIFY(paged ? usage->text().contains("按需浏览") : usage->text().contains("完整保留"));
        if (paged) VERIFY(main->graph(0)->dataCount() < Rows / 10);
        VERIFY(std::abs(main->xAxis->range().lower) < 1e-9 && std::abs(main->xAxis->range().upper - lastTime) < 1e-7);
        VERIFY(std::abs(slider->lowerValue()) < 1e-9 && std::abs(slider->upperValue() - lastTime) < 1e-7);
        limit->setValue(32); limit->setValue(16);
        VERIFY(complete(main->graph(0)) && complete(main->graph(1)));
        auto scope = main->findChild<PlotMeasurementController *>(); VERIFY(scope); scope->setEnabled(true); scope->refresh();
        VERIFY(until([&] { return scope->lastResult().samples == Rows; }));
        VERIFY(scope->lastResult().minimum == -1000 && scope->lastResult().maximum == 2000); scope->setEnabled(false);
        QListWidget *channels = nullptr;
        for (auto list : window.findChildren<QListWidget *>()) if (list->count() == 2 && list->itemWidget(list->item(0))) channels = list;
        VERIFY(channels && detachPlotTestChannel(window, channels, 0));
        auto separateDock = window.findChild<QDockWidget *>("PlotDock_独立视图: " + names[0]); VERIFY(separateDock);
        auto separate = separateDock->findChild<QCustomPlot *>(); VERIFY(separate && complete(separate->graph(0)));
        VERIFY(choosePlotChannelAction(separate, "添加通道显示", names[1]));
        VERIFY(separate->graphCount() == 2 && complete(separate->graph(0)) && complete(separate->graph(1)));
        lowpass->setChecked(true);
        for (auto plot : {main, separate}) for (int ch = 0; ch < plot->graphCount(); ++ch) VERIFY(complete(plot->graph(ch)));
        if (paged) {
            QDoubleSpinBox *frequency = nullptr;
            for (auto spin : window.findChildren<QDoubleSpinBox *>()) if (spin->value() == 10 && spin->suffix().contains("Hz") && spin->maximum() < 100000) frequency = spin;
            VERIFY(frequency); const double previous = frequency->value(); bool canceled = false; QTimer cancelFilter;
            QObject::connect(&cancelFilter, &QTimer::timeout, [&] { for (auto progress : window.findChildren<QProgressDialog *>()) if (progress->objectName() == "offlineFilterProgress") { canceled = true; progress->cancel(); } });
            cancelFilter.start(1); frequency->setValue(15); cancelFilter.stop();
            VERIFY(canceled && frequency->value() == previous && lowpass->isChecked() && complete(main->graph(0)));
        }
        lowpass->setChecked(false);
        VERIFY(main->graph(0)->data()->constBegin()->value == -1000 && (main->graph(0)->data()->constEnd() - 1)->value == 2000);
        if (paged) {
            QMouseEvent click(QEvent::MouseButtonDblClick, QPointF(separate->xAxis->coordToPixel(.5), separate->axisRect()->rect().center().y()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            separate->mouseDoubleClick(&click); bool marked = false;
            for (auto tracer : separate->findChildren<QCPItemTracer *>()) marked |= tracer->visible();
            VERIFY(marked && choosePlotChannelAction(separate, "移除通道显示", names[0]) && separate->graphCount() == 1);
            for (auto tracer : separate->findChildren<QCPItemTracer *>()) VERIFY(!tracer->visible());
            VERIFY(choosePlotChannelAction(separate, "添加通道显示", names[0]));
        }
        slider->rangeChanged(0, .01); QApplication::processEvents();
        if (paged) VERIFY(until([&] { return usage->text().contains("原始采样点") && main->graph(0)->dataCount() <= 32; }));
        VERIFY(complete(main->graph(0)) && main->xAxis->range().lower == 0 && separate->xAxis->range() == main->xAxis->range());
        slider->rangeChanged(lastTime - 1, lastTime); QApplication::processEvents();
        VERIFY(complete(main->graph(0)) && complete(separate->graph(0)));
        VERIFY(choosePlotChannelAction(separate, "移除通道显示", names[1]) && separate->graphCount() == 1 && complete(separate->graph(0)));
        limit->setValue(32); limit->setValue(16);
        for (auto plot : {main, separate}) for (int ch = 0; ch < plot->graphCount(); ++ch) VERIFY(complete(plot->graph(ch)));
        std::cout << "Complete " << QFileInfo(path).suffix().toStdString() << " survives limit changes, measurement, filtering, panning and detached views.\n";
    }
    // Export retains the first and last samples and every intervening row,
    // even when the visible time window shows only the end of the recording.
    const auto exportedPath = folder.filePath("all-imported.csv");
    messages.lastTitle.clear(); VERIFY(offlineHistoryFileAction(window, exportedPath, false));
    QFile exported(exportedPath); VERIFY(exported.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream in(&exported); in.setCodec("UTF-8"); VERIFY(in.readLine() == "Time(s)," + names.join(','));
    QString last; int exportedRows = 0;
    while (!in.atEnd()) {
        const auto row = in.readLine(); if (row.isEmpty()) continue;
        if (exportedRows == 0) VERIFY(row.split(',')[0].toDouble() == 0 && row.split(',')[1].toDouble() == -1000);
        last = row; ++exportedRows;
    }
    VERIFY(exportedRows == Rows && std::abs(last.split(',')[0].toDouble() - lastTime) < 1e-7 && last.split(',')[1].toDouble() == 2000);
    exported.close();
    if (paged) {
        const auto brokenPath = folder.filePath("broken.csv"); QFile broken(brokenPath); VERIFY(broken.open(QIODevice::WriteOnly)); broken.write("Time(s),A,B\n0,1\n"); broken.close();
        bool settingsOpened = false; QTimer adviceDriver;
        QObject::connect(&adviceDriver, &QTimer::timeout, [&] { if (auto dialog = window.findChild<QDialog *>("systemSettingsDialog")) if (dialog->isVisible()) {
            settingsOpened = dialog->findChild<QListWidget *>("systemSettingsCategories")->currentRow() == 2 && dialog->findChild<QSpinBox *>("systemOfflineMemoryMB"); dialog->reject();
        } }); adviceDriver.start(5); messages.openOfflineSettings = true;
        messages.lastTitle.clear(); VERIFY(offlineHistoryFileAction(window, brokenPath, true));
        messages.openOfflineSettings = false; adviceDriver.stop();
        VERIFY(settingsOpened && messages.lastTitle == "导入失败" && messages.lastText.contains("系统设置 → 绘图显示") && messages.lastText.contains("建议") && complete(main->graph(0)));
        messages.lastTitle.clear(); VERIFY(offlineHistoryFileAction(window, wlrPath, true, true));
        VERIFY(messages.lastTitle != "导入成功" && complete(main->graph(0)));
        bool adjusted = false;
        QTimer::singleShot(0, [&] { if (auto dialog = window.findChild<QDialog *>("systemSettingsDialog")) {
            auto categories = dialog->findChild<QListWidget *>("systemSettingsCategories"); auto memory = dialog->findChild<QSpinBox *>("systemOfflineMemoryMB");
            categories->setCurrentRow(2); memory->setValue(512); dialog->findChild<QPushButton *>("systemSettingsApply")->click();
            adjusted = dialog->isVisible() && settings.value("Display/OfflineMemoryMB").toInt() == 512;
            // On a small HiDPI screen, verify the new setting can be reached
            // through the page's scroll area rather than being clipped.
            auto pages = dialog->findChild<QStackedWidget *>("systemSettingsPages");
            auto scroll = qobject_cast<QScrollArea *>(pages->currentWidget()); QApplication::processEvents();
            if (scroll) {
                scroll->ensureWidgetVisible(memory); QApplication::processEvents();
                adjusted = adjusted && scroll->viewport()->rect().contains(QRect(memory->mapTo(scroll->viewport(), QPoint()), memory->size()));
            } else adjusted = false;
            if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + "/offline-memory-settings.png"); dialog->reject();
        } });
        VERIFY(QMetaObject::invokeMethod(&window, "showSystemSettings") && adjusted);
        VERIFY(complete(main->graph(0)));
        VERIFY(offlineHistoryFileAction(window, wlrPath, true));
        VERIFY(main->graph(0)->dataCount() == Rows && !dynamic_cast<WaveformGraph *>(main->graph(0))->hasOfflineSource());
        VERIFY(usage->text().contains("完整保留"));
        VERIFY(QDir(folder.path()).entryList({"WaveLink_Browse_*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }
    if (!artifactDirectory.isEmpty()) window.grab().save(artifactDirectory + "/offline-complete-history.png");

    // Clearing/leaving offline mode restores the bounded real-time policy.
    VERIFY(QMetaObject::invokeMethod(&window, "onClearPlots"));
    VERIFY(!usage->text().contains("离线波形") && !usage->text().contains("完整保留"));
    QVector<QCPGraphData> live; live.reserve(Rows);
    for (int i = 0; i < Rows; ++i) live.append({i / Rate, double(i)});
    limit->setValue(64);
    for (int ch = 0; ch < main->graphCount(); ++ch) main->graph(ch)->data()->set(live, true);
    limit->setValue(16);
    for (int ch = 0; ch < main->graphCount(); ++ch) {
        VERIFY(main->graph(ch)->dataCount() > 0 && main->graph(ch)->dataCount() < Rows);
        VERIFY(main->graph(ch)->data()->constBegin()->key > 0 && std::abs((main->graph(ch)->data()->constEnd() - 1)->key - lastTime) < 1e-7);
    }
    window.close();
    VERIFY(QDir(folder.path()).entryList({"WaveLink_Browse_*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    std::cout << "Full/paged offline CSV/WLR display/export, exact analysis, settings, cancellation, cache cleanup and bounded real-time history passed.\n";
    return 0;
}
