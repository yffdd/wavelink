#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QApplication>
#include <QString>
#include <QColor>

class QCustomPlot;

class ThemeManager {
public:
    enum Theme {
        DarkTheme,
        LightTheme
    };

    static void applyTheme(QApplication *app, Theme theme);
    static void setupPlotTheme(QCustomPlot *plot, Theme theme, const QColor &background = QColor());

private:
    static QString getDarkStyleSheet();
    static QString getLightStyleSheet();
};

#endif // THEMEMANAGER_H
