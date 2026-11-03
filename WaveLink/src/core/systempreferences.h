#pragma once
#include "receiveconsolebuffer.h"
#include <QSettings>

// Display appearance/scheduling/retention only; never changes acquisition or recording.
struct SystemPreferences {
    int receiveLines = ReceiveConsoleBuffer::DefaultLines;
    int receiveCharacters = ReceiveConsoleBuffer::DefaultCharacters;
    int receiveRefreshMs = 100;
    int plotFps = 30;
    int plotLineWidth = 1;
    int offlineMemoryMB = 256;
    int defaultBottomPercent = 30;
    bool automaticUpdates = true;
    static int validRefresh(int value) { return value >= 50 && value <= 1000 ? value : 100; }
    static int validFps(int value) { return value >= 5 && value <= 60 ? value : 30; }
    static int validLineWidth(int value) { return value >= 1 && value <= 3 ? value : 1; }
    static int validOfflineMemoryMB(int value) { return value >= 16 && value <= 4096 ? value : 256; }
    static int validBottomPercent(int value) { return value >= 20 && value <= 50 ? value : 30; }
    static SystemPreferences read(QSettings &settings) {
        SystemPreferences p;
        p.receiveLines = ReceiveConsoleBuffer::validLines(settings.value("Console/ReceiveMaxLines", p.receiveLines).toInt());
        p.receiveCharacters = ReceiveConsoleBuffer::validCharacters(settings.value("Console/ReceiveMaxCharacters", p.receiveCharacters).toInt());
        p.receiveRefreshMs = validRefresh(settings.value("Console/ReceiveRefreshMs", p.receiveRefreshMs).toInt());
        p.plotFps = validFps(settings.value("Display/PlotFps", p.plotFps).toInt());
        p.plotLineWidth = validLineWidth(settings.value("Display/PlotLineWidth", p.plotLineWidth).toInt());
        p.offlineMemoryMB = validOfflineMemoryMB(settings.value("Display/OfflineMemoryMB", p.offlineMemoryMB).toInt());
        p.defaultBottomPercent = validBottomPercent(settings.value("Layout/DefaultBottomPercent", p.defaultBottomPercent).toInt());
        p.automaticUpdates = settings.value("Update/AutoCheck", true).toBool();
        return p;
    }
    void write(QSettings &settings) const {
        settings.setValue("Console/ReceiveMaxLines", ReceiveConsoleBuffer::validLines(receiveLines));
        settings.setValue("Console/ReceiveMaxCharacters", ReceiveConsoleBuffer::validCharacters(receiveCharacters));
        settings.setValue("Console/ReceiveRefreshMs", validRefresh(receiveRefreshMs));
        settings.setValue("Display/PlotFps", validFps(plotFps));
        settings.setValue("Display/PlotLineWidth", validLineWidth(plotLineWidth));
        settings.setValue("Display/OfflineMemoryMB", validOfflineMemoryMB(offlineMemoryMB));
        settings.setValue("Layout/DefaultBottomPercent", validBottomPercent(defaultBottomPercent));
        settings.setValue("Update/AutoCheck", automaticUpdates);
    }
};
