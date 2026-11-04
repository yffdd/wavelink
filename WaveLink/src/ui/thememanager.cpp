#include "thememanager.h"
#include "qcustomplot.h"
#include <QStyleFactory>
#include <QPalette>
#include <QColor>
#include <cmath>

void ThemeManager::applyTheme(QApplication *app, Theme theme)
{
    if (!app) return;
    
    app->setStyle(QStyleFactory::create("Fusion"));
    
    if (theme == DarkTheme) {
        app->setStyleSheet(getDarkStyleSheet());
    } else {
        app->setStyleSheet(getLightStyleSheet());
    }
    // Notify independently styled tool windows even when the theme selector
    // did not change (startup/config restore or a direct theme reload).
    app->setProperty("waveLinkDarkTheme",theme==DarkTheme);
}

void ThemeManager::setupPlotTheme(QCustomPlot *plot, Theme theme, const QColor &background)
{
    if (!plot) return;
    
    if (theme == DarkTheme) {
        plot->setBackground(QBrush(QColor("#1e1e1e")));
        plot->axisRect()->setBackground(QBrush(QColor("#1e1e1e")));
        
        QPen axisPen(QColor("#888888"));
        axisPen.setWidth(1);
        QPen gridPen(QColor("#333333"));
        gridPen.setStyle(Qt::DotLine);
        
        plot->xAxis->setBasePen(axisPen);
        plot->xAxis->setTickPen(axisPen);
        plot->xAxis->setSubTickPen(axisPen);
        plot->xAxis->setTickLabelColor(QColor("#dcdcdc"));
        plot->xAxis->setLabelColor(QColor("#dcdcdc"));
        plot->xAxis->grid()->setPen(gridPen);
        
        plot->yAxis->setBasePen(axisPen);
        plot->yAxis->setTickPen(axisPen);
        plot->yAxis->setSubTickPen(axisPen);
        plot->yAxis->setTickLabelColor(QColor("#dcdcdc"));
        plot->yAxis->setLabelColor(QColor("#dcdcdc"));
        plot->yAxis->grid()->setPen(gridPen);
    } else {
        plot->setBackground(QBrush(QColor("#f0f0f0")));
        plot->axisRect()->setBackground(QBrush(QColor("#ffffff")));
        
        QPen axisPen(QColor("#555555"));
        axisPen.setWidth(1);
        QPen gridPen(QColor("#e0e0e0"));
        gridPen.setStyle(Qt::DotLine);
        
        plot->xAxis->setBasePen(axisPen);
        plot->xAxis->setTickPen(axisPen);
        plot->xAxis->setSubTickPen(axisPen);
        plot->xAxis->setTickLabelColor(QColor("#333333"));
        plot->xAxis->setLabelColor(QColor("#333333"));
        plot->xAxis->grid()->setPen(gridPen);
        
        plot->yAxis->setBasePen(axisPen);
        plot->yAxis->setTickPen(axisPen);
        plot->yAxis->setSubTickPen(axisPen);
        plot->yAxis->setTickLabelColor(QColor("#333333"));
        plot->yAxis->setLabelColor(QColor("#333333"));
        plot->yAxis->grid()->setPen(gridPen);
    }
    if (background.isValid()) {
        QColor opaque = background;
        opaque.setAlpha(255);
        plot->setBackground(QBrush(opaque));
        plot->axisRect()->setBackground(QBrush(opaque));
        // Keep axes readable against an independently chosen background.
        // Curve colors remain exactly as configured by the user.
        const auto linear = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        const double luminance = 0.2126 * linear(opaque.redF()) + 0.7152 * linear(opaque.greenF()) + 0.0722 * linear(opaque.blueF());
        const QColor text = luminance > 0.179 ? QColor("#202020") : QColor("#eeeeee");
        const auto blend = [&](double amount) {
            return QColor(qRound(opaque.red() + (text.red() - opaque.red()) * amount),
                          qRound(opaque.green() + (text.green() - opaque.green()) * amount),
                          qRound(opaque.blue() + (text.blue() - opaque.blue()) * amount));
        };
        for (auto axis : {plot->xAxis, plot->yAxis}) {
            axis->setBasePen(QPen(blend(0.6)));
            axis->setTickPen(QPen(blend(0.6)));
            axis->setSubTickPen(QPen(blend(0.6)));
            axis->setTickLabelColor(text);
            axis->setLabelColor(text);
            axis->grid()->setPen(QPen(blend(0.18), 1, Qt::DotLine));
        }
    }
}

QString ThemeManager::getDarkStyleSheet()
{
    return QString(
        "QMainWindow { background-color: #1e1e1e; }"
        "QDialog { background-color: #1e1e1e; }"
        "QDockWidget { background-color: #1e1e1e; color: #dddddd; }"
        "QWidget#DockCollapsedSideOverlay { background-color: #1e1e1e; }"
        "QScrollArea { background-color: #1e1e1e; border: none; }"
        "QScrollArea > QWidget > QWidget { background-color: #1e1e1e; }"
        "QDockWidget::title { background: #2d2d2d; text-align: left; padding-left: 5px; }"
        "#DockTitleLabel { font-weight: bold; font-size: 13pt; color: #E0E0E0; }"
        "QToolButton#DockExpandBtn, QToolButton#DockMinBtn, QToolButton#DockMaxBtn, QToolButton#DockCloseBtn { border: none; color: #AAAAAA; padding: 1px; margin: 0px; }"
        "QToolButton#DockExpandBtn:hover, QToolButton#DockMinBtn:hover, QToolButton#DockMaxBtn:hover { color: #FFFFFF; background-color: rgba(255,255,255,0.1); }"
        "QToolButton#DockCloseBtn:hover { color: #FFFFFF; background-color: #FF4444; }"
        "QWidget { color: #dddddd; }"
        "QToolTip { background-color: #2b2b2b; color: #eeeeee; border: 1px solid #777777; padding: 8px; }"
        "QLabel#lblRecordingWarning { color: #ff8b8b; background: transparent; font-weight: bold; }"
        "QTextBrowser, QTextEdit, QPlainTextEdit, QListWidget, QLineEdit, QTableWidget, QTreeWidget { background-color: #252526; color: #d4d4d4; border: 1px solid #3c3c3c; selection-background-color: #264f78; selection-color: #ffffff; }"
        "QHeaderView { background-color: #333333; color: #dddddd; }"
        "QHeaderView::section { background-color: #333333; color: #dddddd; border: 1px solid #555555; padding: 4px; }"
        "QTableCornerButton::section { background-color: #333333; border: 1px solid #555555; }"
        "QPushButton { background-color: #333333; color: #ffffff; border: 1px solid #555555; padding: 5px; border-radius: 3px; }"
        "QPushButton:hover { background-color: #444444; }"
        "QPushButton:pressed { background-color: #222222; }"
        "QPushButton#btnPausePlot { color: white; background-color: #214b42; border: 1px solid #4f8c7b; border-radius: 4px; padding: 5px 12px; font-weight: bold; }"
        "QPushButton#btnPausePlot:hover { background-color: #2b5e53; }"
        "QPushButton#btnPausePlot:pressed { background-color: #183b33; }"
        "QPushButton#btnPausePlot:checked { background-color: #68431c; border-color: #bf9253; }"
        "QPushButton#btnPausePlot:checked:hover { background-color: #795025; }"
        "QPushButton#btnPausePlot:checked:pressed { background-color: #503315; }"
        "QPushButton#btnPausePlot:focus { border-color: #f0f0f0; }"
        "QComboBox, QSpinBox, QDoubleSpinBox { background-color: #333333; color: #ffffff; border: 1px solid #555555; padding: 2px; }"
        "QComboBox QAbstractItemView { background-color: #333333; color: #ffffff; selection-background-color: #00aaff; }"
        "QCheckBox { color: #cccccc; font-weight: bold; spacing: 6px; }"
        "QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #555555; border-radius: 3px; background-color: #2b2b2b; }"
        "QCheckBox::indicator:hover { border-color: #00aaff; }"
        "QCheckBox::indicator:checked { background-color: #00aaff; border: 1px solid #0088cc; image: url(:/checkmark.png); }"
        "QProgressBar { color: black; font-weight: bold; text-align: center; }"
        "QMessageBox { background-color: #252526; color: #d4d4d4; }"
        "QMessageBox QLabel { color: #d4d4d4; }"
        "QMessageBox QPushButton { background-color: #333333; color: #ffffff; border: 1px solid #555555; padding: 5px; min-width: 60px; }"
        "QMessageBox QPushButton:hover { background-color: #444444; }"
        "QGroupBox { border: 1px solid #444; margin-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; color: #aaa; }"
        "QTabBar { background: #1e1e1e; font-weight: bold; }"
        "QTabBar::tab { background: #252526; color: #999999; padding: 6px 14px; border: 1px solid #3e3e42; border-bottom: none; border-top-left-radius: 4px; border-top-right-radius: 4px; }"
        "QTabBar::tab:selected { background: #333333; color: #ffffff; border-top: 2px solid #0e639c; }"
        "QTabBar::tab:hover { background: #3e3e42; color: #ffffff; }"
        "QMenu { background-color: #2d2d2d; color: #ffffff; border: 1px solid #555; }"
        "QMenu::item:selected { background-color: #0e639c; }"
        "QMenuBar { background-color: #1e1e1e; color: #dddddd; }"
        "QMenuBar::item { background-color: transparent; padding: 4px 10px; }"
        "QMenuBar::item:selected { background-color: #3e3e42; }"
        "QToolBar { background: #252526; border: none; padding: 4px; border-bottom: 1px solid #333; }"
        "QToolButton { color: #cccccc; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 4px 12px; margin: 2px; font-weight: bold; font-family: Arial; }"
        "QToolButton:hover { background: #444444; border-color: #555555; }"
        "QToolButton:checked { background: #0e639c; color: white; border-color: #1177bb; }"
        
        "QLineEdit#editCacheDir { background-color: #2b2b2b; color: #dddddd; padding: 4px; }"
        "QPushButton#btnSelectCacheDir { padding: 4px; background-color: #3b3b3b; }"
        "QLabel#lblRecordStatus { color: #aaaaaa; }"
        "QPlainTextEdit#textReceive { background-color: #1e1e1e; color: #d4d4d4; font-family: Consolas; }"
        "QToolButton#btnSpectrumChannels { background-color: #2b2b2b; color: #dddddd; border: 1px solid #555555; padding: 4px; } QToolButton#btnSpectrumChannels::menu-indicator { image: none; }"
        "QPushButton#btnSpectrumAutoScale { font-weight: bold; background-color: #3e3e42; padding: 2px 8px; }"
        "QToolBar#mainToolBar { background: #252526; border-bottom: 1px solid #333; }"
        "QToolButton#btnDataTransceiveMode, QToolButton#btnRestoreDefaultLayout { color: #ffffff; padding: 4px 10px; margin: 2px 3px; border-radius: 4px; }"
        "QToolButton#btnDataTransceiveMode { background: #195f8e; border: 1px solid #407da5; }"
        "QToolButton#btnDataTransceiveMode:hover { background: #2273a5; border-color: #8bcfff; }"
        "QToolButton#btnDataTransceiveMode:checked { background: #123f60; border-color: #8bcfff; }"
        "QToolButton#btnDataTransceiveMode:checked:hover { background: #195577; }"
        "QToolButton#btnDataTransceiveMode:pressed { background: #10334e; }"
        "QToolButton#btnRestoreDefaultLayout { background: #265f52; border: 1px solid #4c8979; }"
        "QToolButton#btnRestoreDefaultLayout:hover { background: #2d7562; border-color: #91d3ba; }"
        "QToolButton#btnRestoreDefaultLayout:pressed { background: #194a3e; }"
        "QToolButton#btnDataTransceiveMode:focus, QToolButton#btnRestoreDefaultLayout:focus { border-color: #ffffff; }"
        "QToolButton#btnDataTransceiveMode:disabled, QToolButton#btnRestoreDefaultLayout:disabled { background: #363c42; color: #a7adb3; border-color: #555d65; }"
        "QWidget#customTitleBar { background: #2d2d30; color: #dcdcdc; border-bottom: 1px solid #1e1e1e; }"
        "QToolButton#customTitleBarBtn { background: transparent; border: none; color: #999999; }"
        "QToolButton#customTitleBarBtn:hover { color: #ffffff; background: #3e3e42; }"
        "QLabel#measureLabel { font-weight: bold; color: #00ffcc; background: #1e1e1e; padding: 4px; border-radius: 3px; border: 1px solid #333; }"
    );
}

QString ThemeManager::getLightStyleSheet()
{
    return QString(
        "QMainWindow { background-color: #f0f0f0; }"
        "QDialog { background-color: #f0f0f0; }"
        "QDockWidget { background-color: #f0f0f0; color: #333333; }"
        "QWidget#DockCollapsedSideOverlay { background-color: #f0f0f0; }"
        "QScrollArea { background-color: #f0f0f0; border: none; }"
        "QScrollArea > QWidget > QWidget { background-color: #f0f0f0; }"
        "QDockWidget::title { background: #e0e0e0; text-align: left; padding-left: 5px; color: #333333; }"
        "#DockTitleLabel { font-weight: bold; font-size: 13pt; color: #333333; }"
        "QToolButton#DockExpandBtn, QToolButton#DockMinBtn, QToolButton#DockMaxBtn, QToolButton#DockCloseBtn { border: none; color: #555555; padding: 1px; margin: 0px; }"
        "QToolButton#DockExpandBtn:hover, QToolButton#DockMinBtn:hover, QToolButton#DockMaxBtn:hover { color: #000000; background-color: rgba(128,128,128,0.2); }"
        "QToolButton#DockCloseBtn:hover { color: #FFFFFF; background-color: #FF4444; }"
        "QWidget { color: #333333; }"
        "QToolTip { background-color: #ffffff; color: #202020; border: 1px solid #9aa0a6; padding: 8px; }"
        "QLabel#lblRecordingWarning { color: #b42318; background: transparent; font-weight: bold; }"
        "QTextBrowser, QTextEdit, QPlainTextEdit, QListWidget, QLineEdit, QTableWidget, QTreeWidget { background-color: #ffffff; color: #333333; border: 1px solid #cccccc; selection-background-color: #0078d7; selection-color: #ffffff; }"
        "QHeaderView { background-color: #e1e1e1; color: #333333; }"
        "QHeaderView::section { background-color: #e1e1e1; color: #333333; border: 1px solid #cccccc; padding: 4px; }"
        "QTableCornerButton::section { background-color: #e1e1e1; border: 1px solid #cccccc; }"
        "QPushButton { background-color: #e1e1e1; color: #333333; border: 1px solid #adadad; padding: 5px; border-radius: 3px; }"
        "QPushButton:hover { background-color: #e5f1fb; border: 1px solid #0078d7; }"
        "QPushButton:pressed { background-color: #cce4f7; border: 1px solid #005499; }"
        "QPushButton#btnPausePlot { color: white; background-color: #236954; border: 1px solid #185141; border-radius: 4px; padding: 5px 12px; font-weight: bold; }"
        "QPushButton#btnPausePlot:hover { background-color: #29755e; }"
        "QPushButton#btnPausePlot:pressed { background-color: #185141; }"
        "QPushButton#btnPausePlot:checked { background-color: #85530f; border-color: #67400b; }"
        "QPushButton#btnPausePlot:checked:hover { background-color: #925d16; }"
        "QPushButton#btnPausePlot:checked:pressed { background-color: #67400b; }"
        "QPushButton#btnPausePlot:focus { border-color: #173a68; }"
        "QComboBox, QSpinBox, QDoubleSpinBox { background-color: #ffffff; color: #333333; border: 1px solid #cccccc; padding: 2px; }"
        "QComboBox QAbstractItemView { background-color: #ffffff; color: #333333; selection-background-color: #0078d7; selection-color: #ffffff; }"
        "QLabel { color: #333333; background: transparent; }"
        "QCheckBox { color: #333333; font-weight: bold; }"
        "QMessageBox { background-color: #f0f0f0; color: #333333; }"
        "QMessageBox QLabel { color: #333333; }"
        "QMessageBox QPushButton { background-color: #e1e1e1; color: #333333; border: 1px solid #adadad; padding: 5px; min-width: 60px; }"
        "QMessageBox QPushButton:hover { background-color: #e5f1fb; border: 1px solid #0078d7; }"
        "QGroupBox { border: 1px solid #cccccc; margin-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; color: #555555; }"
        "QTabBar { background: #f0f0f0; font-weight: bold; }"
        "QTabBar::tab { background: #e1e1e1; color: #555555; padding: 6px 14px; border: 1px solid #cccccc; border-bottom: none; border-top-left-radius: 4px; border-top-right-radius: 4px; }"
        "QTabBar::tab:selected { background: #ffffff; color: #000000; border-top: 2px solid #0078d7; }"
        "QTabBar::tab:hover { background: #e5f1fb; color: #333333; }"
        "QMenu { background-color: #f2f2f2; color: #333333; border: 1px solid #cccccc; }"
        "QMenu::item:selected { background-color: #91c9f7; }"
        "QMenuBar { background-color: #f0f0f0; color: #333333; }"
        "QMenuBar::item { background-color: transparent; padding: 4px 10px; }"
        "QMenuBar::item:selected { background-color: #e5f1fb; border: 1px solid #0078d7; }"
        "QToolBar { background: #f5f6f7; border: none; padding: 4px; border-bottom: 1px solid #dcdcdc; }"
        "QToolButton { color: #333333; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 4px 12px; margin: 2px; font-weight: bold; font-family: Arial; }"
        "QToolButton:hover { background: #e5f1fb; border-color: #0078d7; }"
        "QToolButton:checked { background: #cce4f7; color: #000000; border-color: #005499; }"
        
        "QLineEdit#editCacheDir { background-color: #ffffff; color: #333333; padding: 4px; }"
        "QPushButton#btnSelectCacheDir { padding: 4px; background-color: #e1e1e1; }"
        "QLabel#lblRecordStatus { color: #555555; }"
        "QPlainTextEdit#textReceive { background-color: #fafafa; color: #333333; font-family: Consolas; }"
        "QToolButton#btnSpectrumChannels { background-color: #e1e1e1; color: #333333; border: 1px solid #adadad; padding: 4px; } QToolButton#btnSpectrumChannels::menu-indicator { image: none; }"
        "QPushButton#btnSpectrumAutoScale { font-weight: bold; background-color: #e1e1e1; padding: 2px 8px; }"
        "QToolBar#mainToolBar { background: #f5f6f7; border-bottom: 1px solid #cccccc; }"
        "QToolButton#btnDataTransceiveMode, QToolButton#btnRestoreDefaultLayout { color: #ffffff; padding: 4px 10px; margin: 2px 3px; border-radius: 4px; }"
        "QToolButton#btnDataTransceiveMode { background: #086ca6; border: 1px solid #075c8e; }"
        "QToolButton#btnDataTransceiveMode:hover { background: #075c8e; border-color: #04446d; }"
        "QToolButton#btnDataTransceiveMode:checked { background: #083f63; border-color: #032e4a; }"
        "QToolButton#btnDataTransceiveMode:checked:hover { background: #063552; }"
        "QToolButton#btnDataTransceiveMode:pressed { background: #032e4a; }"
        "QToolButton#btnRestoreDefaultLayout { background: #237356; border: 1px solid #1a6046; }"
        "QToolButton#btnRestoreDefaultLayout:hover { background: #1b6047; border-color: #104a35; }"
        "QToolButton#btnRestoreDefaultLayout:pressed { background: #104a35; }"
        "QToolButton#btnDataTransceiveMode:focus, QToolButton#btnRestoreDefaultLayout:focus { border-color: #102c40; }"
        "QToolButton#btnDataTransceiveMode:disabled, QToolButton#btnRestoreDefaultLayout:disabled { background: #dce1e5; color: #737b82; border-color: #b5bec5; }"
        "QWidget#customTitleBar { background: #e5e5e5; color: #333333; border-bottom: 1px solid #cccccc; }"
        "QToolButton#customTitleBarBtn { background: transparent; border: none; color: #777777; }"
        "QToolButton#customTitleBarBtn:hover { color: #000000; background: #d0d0d0; }"
        "QLabel#measureLabel { font-weight: bold; color: #005499; background: #e5f1fb; padding: 4px; border-radius: 3px; border: 1px solid #0078d7; }"
    );
}
