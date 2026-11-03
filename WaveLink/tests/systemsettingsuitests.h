#pragma once
#include <QShortcut>

static int systemSettingsUiTests(OffscreenMessages &messages)
{
    QTemporaryDir folder; VERIFY(folder.isValid()); QString error;
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    main.remove("Console/ReceiveMaxLines"); main.remove("Console/ReceiveMaxCharacters"); main.remove("Console/ReceiveRefreshMs"); main.remove("Display/PlotFps"); main.remove("Display/PlotLineWidth"); main.remove("Layout/DefaultBottomPercent");
    main.remove("windowState"); main.remove("innerWindowState"); main.remove("windowGeometry"); main.setValue("dataTransceiveModeActive", false);
    main.setValue("editCacheDir", folder.path()); main.sync(); advanced.setValue("InterfaceType", 0); advanced.sync();
    {
        MainWindow host(nullptr, new TestSerialHandler(new ReceivePort)); host.resize(1200, 900); host.show();
        host.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        QElapsedTimer startup; startup.start(); VERIFY(until([&] { return startup.elapsed() >= 200; }));
        auto config = host.findChild<QDockWidget *>("DockConfig");
        VERIFY(config && std::abs(config->width() - 400) <= 2);
        auto updateNotice = host.findChild<QPushButton *>("updateAvailableNotice"); VERIFY(updateNotice && !updateNotice->isVisible());
        auto action = host.findChild<QAction *>("actionSystemSettings"); auto quick = host.findChild<QComboBox *>("comboReceiveLines");
        auto receive = host.findChild<QPlainTextEdit *>("textReceive");
        VERIFY(action && quick && receive && receive->isReadOnly());
        VERIFY(action->text() == QStringLiteral("系统设置(&O)") && QKeySequence::mnemonic(action->text()) == QKeySequence("Alt+O"));
        VERIFY(host.findChild<QTimer *>("consoleReceiveUpdateTimer")->interval() == 100 && host.findChild<QTimer *>("waveformRefreshTimer")->interval() == 33);
        bool checked = false;
        QTimer::singleShot(0, &host, [&] {
            auto dialog = host.findChild<QDialog *>("systemSettingsDialog"); if (!dialog) return;
            auto categories = dialog->findChild<QListWidget *>("systemSettingsCategories"); auto pages = dialog->findChild<QStackedWidget *>("systemSettingsPages");
            auto lines = dialog->findChild<QComboBox *>("systemReceiveLines"); auto characters = dialog->findChild<QSpinBox *>("systemReceiveCharacters");
            auto refresh = dialog->findChild<QComboBox *>("systemReceiveRefreshMs"); auto fps = dialog->findChild<QComboBox *>("systemPlotFps");
            auto width = dialog->findChild<QComboBox *>("systemPlotLineWidth"); auto widthHint = dialog->findChild<QLabel *>("systemPlotLineWidthHint");
            if (!categories || !pages || !lines || !characters) { dialog->reject(); return; }
            checked = categories->count() == 4 && categories->currentRow() == 0 && lines->count() == 3 && lines->currentData().toInt() == 1000 && characters->value() == 5;
            auto automatic = dialog->findChild<QCheckBox *>("systemAutomaticUpdates");
            checked = checked && automatic && automatic->isChecked() && categories->item(3)->text() == QStringLiteral("软件更新");
            auto bottom = dialog->findChild<QSpinBox *>("systemDefaultBottomPercent"); auto preview = dialog->findChild<QLabel *>("systemDefaultLayoutPreview");
            checked = checked && bottom && preview && bottom->minimum() == 20 && bottom->maximum() == 50 && bottom->value() == 30 && preview->text().contains("70%") && preview->text().contains("30%");
            checked = checked && categories->item(0)->text() == QStringLiteral("界面设置") && categories->item(1)->text() == QStringLiteral("数据收发") && categories->item(2)->text() == QStringLiteral("绘图显示") && pages->currentWidget()->isAncestorOf(bottom);
            checked = checked && characters->singleStep() == 1 && characters->suffix().contains(QStringLiteral("万字符")) && characters->minimum() == 1 && characters->maximum() == 25 && (dialog->windowFlags() & Qt::WindowMinMaxButtonsHint) && !(dialog->windowFlags() & Qt::WindowContextHelpButtonHint);
            checked = checked && refresh && fps && !refresh->isEditable() && !fps->isEditable() && refresh->count() == 8 && fps->count() == 9
                && refresh->currentData().toInt() == 100 && refresh->currentText().contains(QStringLiteral("默认")) && fps->currentData().toInt() == 30 && fps->currentText().contains(QStringLiteral("默认"));
            checked = checked && width && widthHint && width->count()==3 && width->currentData().toInt()==1 && width->toolTip().contains("CSV/WLR");
            if(width && widthHint) {
                width->setCurrentIndex(2);
                checked = checked && widthHint->text().contains("卡顿") && widthHint->text().contains("高 DPI") && widthHint->font().bold();
                categories->setCurrentRow(2); dialog->findChild<QPushButton *>("systemSettingsDefaults")->click();
                checked = checked && width->currentData().toInt()==1 && !widthHint->font().bold();
                width->setCurrentIndex(1); // Cancel below must discard the draft width.
            }
            auto hint = dialog->findChild<QLabel *>("systemReceiveCharactersHint");
            checked = checked && hint && hint->text().contains("1–25") && hint->text().contains(QStringLiteral("建议")) && hint->text().contains("5 万");
            for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
                ThemeManager::applyTheme(qApp, theme);
                for (int category = 0; category < 4; ++category) {
                    categories->setCurrentRow(category); qApp->processEvents();
                    checked = checked && pages->currentIndex() == category;
                    auto expectedControl = category == 0 ? static_cast<QWidget *>(bottom) : category == 1 ? static_cast<QWidget *>(characters) : category == 2 ? static_cast<QWidget *>(fps) : static_cast<QWidget *>(automatic);
                    checked = checked && pages->currentWidget()->isAncestorOf(expectedControl);
                    auto scroll = qobject_cast<QScrollArea *>(pages->currentWidget()); checked = checked && scroll && scroll->horizontalScrollBar()->maximum() == 0;
                    if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + QString("/system-settings-%1-%2-%3.png").arg(theme).arg(category).arg(qApp->devicePixelRatio()));
                }
            }
            automatic->setChecked(false); characters->setValue(1); dialog->reject(); // Cancel must not save.
        }); action->trigger(); VERIFY(checked && SystemPreferences::read(main).receiveCharacters == 50000);
        VERIFY(SystemPreferences::read(main).automaticUpdates);
        VERIFY(SystemPreferences::read(main).plotLineWidth==1);
        bool versionPage = false;
        QTimer::singleShot(0, &host, [&] {
            auto dialog = host.findChild<QDialog *>("versionInfoDialog"); if (!dialog) return;
            versionPage = dialog->findChild<QPushButton *>("checkForUpdates") && dialog->findChild<QPlainTextEdit *>("updateReleaseNotes") &&
                (dialog->windowFlags() & Qt::WindowMinMaxButtonsHint) && !(dialog->windowFlags() & Qt::WindowContextHelpButtonHint);
            dialog->reject();
        });
        auto versionAction = host.findChild<QAction *>("actionVersionInfo"); VERIFY(versionAction); versionAction->trigger(); VERIFY(versionPage);
        bool receiveShortcut = false;
        QTimer::singleShot(0, &host, [&] {
            auto dialog = host.findChild<QDialog *>("systemSettingsDialog"); if (!dialog) return;
            auto categories = dialog->findChild<QListWidget *>("systemSettingsCategories");
            auto pages = dialog->findChild<QStackedWidget *>("systemSettingsPages"); auto chars = dialog->findChild<QSpinBox *>("systemReceiveCharacters");
            receiveShortcut = categories && pages && chars && categories->currentItem()->text() == QStringLiteral("数据收发") && pages->currentWidget()->isAncestorOf(chars);
            dialog->reject();
        });
        auto receiveSettings = host.findChild<QPushButton *>("btnReceiveSettings"); VERIFY(receiveSettings); receiveSettings->click(); VERIFY(receiveShortcut);
        receive->setPlainText(QString(80000, 'A') + "LATEST");
        VERIFY(editSystemSettings(host, 2000, 10000, 200, 15));
        VERIFY(quick->currentData().toInt() == 2000 && receive->document()->characterCount() - 1 <= 10000 && receive->toPlainText().endsWith("LATEST"));
        VERIFY(host.findChild<QTimer *>("consoleReceiveUpdateTimer")->interval() == 200 && host.findChild<QTimer *>("waveformRefreshTimer")->interval() == 67);
        QTimer::singleShot(0, &host, [&] {
            auto dialog = host.findChild<QDialog *>("systemSettingsDialog"); if (!dialog) return;
            auto defaults = dialog->findChild<QPushButton *>("systemSettingsDefaults"); auto shortcut = dialog->findChild<QShortcut *>("systemSettingsSaveShortcut");
            dialog->findChild<QListWidget *>("systemSettingsCategories")->setCurrentRow(1);
            dialog->findChild<QCheckBox *>("systemAutomaticUpdates")->setChecked(false);
            if (defaults && shortcut) { defaults->click(); QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection); checked = dialog->isVisible(); }
            else checked = false;
            dialog->reject();
        }); action->trigger();
        VERIFY(checked && quick->currentData().toInt() == 1000);
        VERIFY(!SystemPreferences::read(main).automaticUpdates);
        auto saved = SystemPreferences::read(main); VERIFY(saved.receiveCharacters == 50000 && saved.receiveRefreshMs == 100 && saved.plotFps == 15);
        quick->setCurrentIndex(quick->findData(5000)); VERIFY(SystemPreferences::read(main).receiveLines == 5000);
        // Ratio changes are deferred until explicit reset; the target uses
        // this window's work area and has no old 320-pixel ceiling.
        host.resize(1600, 1300); qApp->processEvents();
        auto reset = host.findChild<QToolButton *>("btnRestoreDefaultLayout"); auto console = host.findChild<QDockWidget *>("DockConsole");
        VERIFY(reset && console); reset->click(); qApp->processEvents();
        VERIFY(std::abs(config->width() - 400) <= 2);
        auto ratio = [&] { return double(console->height()) / qMax(1, console->height() + host.centralWidget()->height()); };
        if (std::abs(ratio() - .30) >= .04) std::cerr << "Default layout ratio=" << ratio() << " bottom=" << console->height() << " central=" << host.centralWidget()->height() << " window=" << host.height() << "\n";
        VERIFY(std::abs(ratio() - .30) < .04);
        const int previousHeight = console->height();
        VERIFY(editSystemSettings(host, 5000, 50000, 100, 15, 40)); qApp->processEvents();
        VERIFY(std::abs(console->height() - previousHeight) < 4); // Saving does not rearrange.
        reset->click(); qApp->processEvents();
        VERIFY(std::abs(ratio() - .40) < .04 && console->height() > previousHeight && console->height() > 320);
        for (int windowHeight : {1100, 1500}) {
            host.resize(1600, windowHeight); qApp->processEvents(); reset->click(); qApp->processEvents();
            VERIFY(std::abs(ratio() - .40) < .04);
        }
        if (!artifactDirectory.isEmpty()) host.grab().save(artifactDirectory + QString("/default-layout-40-%1.png").arg(qApp->devicePixelRatio()));
        // A manual divider adjustment must remain adjustable and survive restart.
        host.resizeDocks({config}, {520}, Qt::Horizontal); qApp->processEvents();
        VERIFY(std::abs(config->width() - 520) <= 2);
        host.close();
    }
    {
        MainWindow restart(nullptr, new TestSerialHandler(new ReceivePort));
        restart.show(); restart.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        QElapsedTimer startup; startup.start(); VERIFY(until([&] { return startup.elapsed() >= 200; }));
        auto config = restart.findChild<QDockWidget *>("DockConfig");
        VERIFY(config && std::abs(config->width() - 520) <= 2);
        VERIFY(restart.findChild<QComboBox *>("comboReceiveLines")->currentData().toInt() == 5000 && restart.findChild<QTimer *>("waveformRefreshTimer")->interval() == 67);
        VERIFY(SystemPreferences::read(main).defaultBottomPercent == 40);
        VERIFY(!SystemPreferences::read(main).automaticUpdates);
        restart.close();
    }
    {
        // Historical refresh values remain selectable. Character limits now
        // deliberately migrate to integer 万字符, including imported settings.
        SystemPreferences old; old.receiveCharacters = 12345; old.receiveRefreshMs = 75; old.plotFps = 17; old.defaultBottomPercent = 35; old.plotLineWidth = 99;
        SystemPreferences applied; bool received = false;
        SystemSettingsDialog legacy(old, [&](const SystemPreferences &value) { applied = value; received = true; });
        legacy.show(); qApp->processEvents();
        auto chars = legacy.findChild<QSpinBox *>("systemReceiveCharacters");
        auto refresh = legacy.findChild<QComboBox *>("systemReceiveRefreshMs"), fps = legacy.findChild<QComboBox *>("systemPlotFps");
        auto apply = legacy.findChild<QPushButton *>("systemSettingsApply");
        VERIFY(chars && refresh && fps && apply && refresh->currentData().toInt() == 75 && fps->currentData().toInt() == 17);
        VERIFY(refresh->currentText().contains(QStringLiteral("已保存")) && fps->currentText().contains(QStringLiteral("已保存")));
        VERIFY(legacy.findChild<QComboBox *>("systemPlotLineWidth")->currentData().toInt()==1);
        for (int count : {10000, 20000, 30000, 50000, 250000}) {
            received = false; chars->setValue(count / 10000); apply->click();
            VERIFY(received && applied.receiveCharacters == count && applied.receiveRefreshMs == 75 && applied.plotFps == 17 && applied.defaultBottomPercent == 35);
        }
        legacy.findChild<QListWidget *>("systemSettingsCategories")->setCurrentRow(0);
        legacy.findChild<QPushButton *>("systemSettingsDefaults")->click(); apply->click();
        VERIFY(applied.defaultBottomPercent == 30 && applied.plotFps == 17);
        legacy.close();
    }
    ProtocolProfile p; VERIFY(ProtocolProfileValidator::profile(ProtocolProfileValidator::keyValueTemplate(), p, error));
    ProtocolSnapshot source; source.profiles = {p}; source.shared.kind = "custom"; source.shared.id = p.id;
    VERIFY(configUiRoundTrip(folder.path(), source, messages) == 0);
    std::cout << "System settings: categories, both themes, defaults, cancellation, apply/Ctrl+S, immediate trim/timers, restart and actual global config UI export/import passed.\n";
    return 0;
}
