#include "mainwindow.h"
#include <QTime>
#include <QProcess>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include "ffthelper.h"
#include "demodatagenerator.h"
#include <QOpenGLContext>
#include "debuglogger.h"
#include "commandpanelwindow.h"
#include "helpdialog.h"
#include <QSerialPortInfo>
#include <QMessageBox>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollBar>
#include <QScrollArea>
#include <QStyle>
#include "plotscrollarea.h"
#include <QListWidget>
#include <QLineEdit>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFileDialog>
#include <QProgressBar>
#include <QTimer>
#include <QFile>
#include <QCoreApplication>
#include <QMenu>
#include <QSettings>
#include <QTextCodec>
#include <QColorDialog>
#include <QProgressDialog>
#include <QScreen>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QSignalBlocker>

class CustomTitleBar : public QWidget {
public:
    CustomTitleBar(QWidget *parent = nullptr) : QWidget(parent) {}
protected:
    void mousePressEvent(QMouseEvent *event) override {
        event->ignore(); // Pass mouse events to QDockWidget for dragging/tabbing
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        event->ignore();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        event->ignore();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        event->ignore();
    }
};

// QDockWidget deliberately gives a custom title bar only its sizeHint().
// Therefore a title-bar-only side dock can never fill the complete dock cell.
// Keep the real dock content visible and cover it with this opaque overlay
// while collapsed. The overlay follows the content rect, so it fills exactly
// the side dock cell and cannot overlap the bottom dock area.
class CollapsedSideOverlay : public QWidget {
public:
    explicit CollapsedSideOverlay(QWidget *host)
        : QWidget(host), m_host(host)
    {
        setAttribute(Qt::WA_StyledBackground, true);
        setAutoFillBackground(true);
        if (m_host) {
            m_host->installEventFilter(this);
            setGeometry(m_host->rect());
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_host
            && (event->type() == QEvent::Resize
                || event->type() == QEvent::Show
                || event->type() == QEvent::LayoutRequest)) {
            setGeometry(m_host->rect());
            if (isVisible()) raise();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QWidget *m_host = nullptr;
};

namespace {
// Leave enough horizontal room for both the vertical title and a native
// QToolButton arrow at high DPI. 36 px was too tight with some Qt styles and
// clipped the arrow against the outer dock edge.
constexpr int kCollapsedSideWidth = 48;
constexpr int kCollapsedBottomHeight = 30;

bool isSideDockArea(Qt::DockWidgetArea area)
{
    return area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
}

QString verticalDockTitle(const QString &title)
{
    QString result;
    for (const QChar character : title) {
        if (!result.isEmpty()) result.append(QLatin1Char('\n'));
        result.append(character);
    }
    return result;
}

void updateDockTitlePresentation(QDockWidget *dock)
{
    if (!dock || !dock->titleBarWidget()) return;

    QWidget *titleBar = dock->titleBarWidget();
    QLabel *titleLabel = titleBar->findChild<QLabel*>("DockTitleLabel");
    QToolButton *expandButton = titleBar->findChild<QToolButton*>("DockExpandBtn");
    QBoxLayout *titleLayout = qobject_cast<QBoxLayout*>(titleBar->layout());
    QMainWindow *host = qobject_cast<QMainWindow*>(dock->parentWidget());
    const Qt::DockWidgetArea area = host
        ? host->dockWidgetArea(dock)
        : Qt::LeftDockWidgetArea;
    const bool collapsed = dock->property("dockCollapsed").toBool();
    const bool sideCollapsed = collapsed && isSideDockArea(area);
    const QString title = titleBar->property("dockTitle").toString();

    if (titleLayout) {
        titleLayout->setDirection(QBoxLayout::LeftToRight);
        titleLayout->setContentsMargins(5, 2, 5, 2);
        if (expandButton) {
            titleLayout->removeWidget(expandButton);
            titleLayout->addWidget(expandButton);
        }
    }
    if (titleLabel) {
        titleLabel->setText(title);
        titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (expandButton) {
        Qt::ArrowType arrow = Qt::NoArrow;
        if (area == Qt::LeftDockWidgetArea) {
            arrow = collapsed ? Qt::RightArrow : Qt::LeftArrow;
        } else if (area == Qt::RightDockWidgetArea) {
            arrow = collapsed ? Qt::LeftArrow : Qt::RightArrow;
        } else if (area == Qt::TopDockWidgetArea) {
            arrow = collapsed ? Qt::DownArrow : Qt::UpArrow;
        } else {
            arrow = collapsed ? Qt::UpArrow : Qt::DownArrow;
        }
        expandButton->setText(QString());
        expandButton->setArrowType(arrow);
        expandButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        expandButton->setAccessibleName(collapsed
            ? QStringLiteral("展开%1面板").arg(title)
            : QStringLiteral("收起%1面板").arg(title));
    }

    QWidget *content = dock->widget();
    QWidget *sideOverlay = content
        ? content->findChild<QWidget*>("DockCollapsedSideOverlay",
                                      Qt::FindDirectChildrenOnly)
        : nullptr;
    if (sideOverlay) {
        QLabel *sideTitle = sideOverlay->findChild<QLabel*>("DockCollapsedSideTitle");
        QToolButton *sideExpand = sideOverlay->findChild<QToolButton*>("DockCollapsedSideExpand");
        if (sideTitle) sideTitle->setText(verticalDockTitle(title));
        if (sideExpand) {
            sideExpand->setArrowType(area == Qt::RightDockWidgetArea
                ? Qt::LeftArrow : Qt::RightArrow);
            sideExpand->setAccessibleName(
                QStringLiteral("展开%1面板").arg(title));
        }
        if (sideCollapsed) {
            sideOverlay->setGeometry(content->rect());
            sideOverlay->show();
            sideOverlay->raise();
        } else {
            sideOverlay->hide();
        }
    }

    titleBar->updateGeometry();
    if (dock->layout()) dock->layout()->invalidate();
    dock->updateGeometry();
}

void resetConsoleSplitterToFiveToOne(QSplitter *splitter)
{
    if (!splitter) return;
    const int availableHeight = qMax(
        6, splitter->height() - splitter->handleWidth());
    const int sendHeight = qMax(1, availableHeight / 6);
    splitter->setSizes(QList<int>()
        << availableHeight - sendHeight
        << sendHeight);
}
}


void MainWindow::requestChannelDockWidthUpdate()
{
    if (m_channelDockWidthUpdatePending || !rightSharedContent || !rightSharedScrollArea) return;
    m_channelDockWidthUpdatePending = true;
    // Wait until child controls have adopted the new theme/font and their
    // layouts have invalidated cached size hints. Coalesce repeated requests.
    QTimer::singleShot(0, this, [this]() {
        m_channelDockWidthUpdatePending = false;
        updateChannelDockMinimumWidth();
    });
}

void MainWindow::updateChannelDockMinimumWidth()
{
    if (!channelDock || !rightSharedScrollArea || !rightSharedContent) return;
    if (rightSharedContent->layout()) rightSharedContent->layout()->activate();
    const int contentWidth = qMax(rightSharedContent->minimumWidth(), rightSharedContent->minimumSizeHint().width());
    // A vertical scrollbar takes space away from the viewport. Reserve its
    // themed width even before it appears, so adding channels cannot crop the
    // help buttons, filter inputs or channel color controls at the right edge.
    const int scrollbarWidth = rightSharedScrollArea->verticalScrollBar()->sizeHint().width();
    const int frameWidth = 2 * (rightSharedScrollArea->frameWidth() +
        channelDock->style()->pixelMetric(QStyle::PM_DockWidgetFrameWidth, nullptr, channelDock));
    const int minimumWidth = qMax(360, contentWidth + scrollbarWidth + frameWidth);
    if (channelDock->property("collapseConstraintsSaved").toBool())
        channelDock->setProperty("collapseOriginalMinWidth", minimumWidth);
    // Do not expand the deliberately narrow collapsed side strip. Its saved
    // expanded constraint is updated above and takes effect when reopened.
    if (!channelDock->property("dockCollapsed").toBool() && channelDock->minimumWidth() != minimumWidth)
        channelDock->setMinimumWidth(minimumWidth);
}

void MainWindow::applyCustomTitleBar(QDockWidget *dock, const QString &title, const QString &iconText, const QString &expandText)
{
    CustomTitleBar *titleBar = new CustomTitleBar(dock);
    titleBar->setProperty("dockTitle", title);
    QHBoxLayout *layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(5, 2, 5, 2);
    layout->setSpacing(5);

    QLabel *lblTitle = new QLabel(title, titleBar);
    lblTitle->setObjectName("DockTitleLabel");

    QToolButton *btnExpand = new QToolButton(titleBar);
    btnExpand->setObjectName("DockExpandBtn");
    btnExpand->setText(expandText);
    btnExpand->setProperty("collapsedText", iconText);
    btnExpand->setProperty("expandedText", expandText);
    btnExpand->setToolTip("展开/收起");
    // Use a real QStyle arrow rather than text glyphs. This remains visible
    // across themes, DPI settings and narrow collapsed side strips.
    btnExpand->setFixedSize(28, 24);

    QToolButton *btnPin = new QToolButton(titleBar);
    btnPin->setObjectName("DockPinBtn");
    btnPin->setText(QString::fromUtf8("取消置顶"));
    btnPin->setToolTip("置顶/取消置顶");
    btnPin->setCheckable(true);
    btnPin->setChecked(true);
    
    // Optional distinct color for pin button if needed, but inheriting is fine.

    layout->addWidget(lblTitle);
    layout->addStretch();
    layout->addWidget(btnPin);
    layout->addWidget(btnExpand);

    bool isFloating = dock->isFloating();
    btnPin->setVisible(isFloating);
    btnExpand->setVisible(!isFloating);

    connect(dock, &QDockWidget::topLevelChanged, titleBar, [btnPin, btnExpand](bool floating){
        btnPin->setVisible(floating);
        btnExpand->setVisible(!floating);
    });
    
    connect(btnPin, &QToolButton::toggled, titleBar, [dock, btnPin](bool checked) {
        if (dock->isFloating()) {
            dock->setWindowFlag(Qt::WindowStaysOnTopHint, checked);
            dock->setWindowFlag(Qt::WindowSystemMenuHint, true);
            dock->setWindowFlag(Qt::WindowMinimizeButtonHint, true);
            dock->setWindowFlag(Qt::WindowMaximizeButtonHint, true);
            dock->setWindowFlag(Qt::WindowCloseButtonHint, true);
            // Keep the native frame so floating docks can be resized from
            // all four edges and corners.
            dock->setWindowFlag(Qt::FramelessWindowHint, false);
            btnPin->setText(checked ? QString::fromUtf8("取消置顶") : QString::fromUtf8("窗口置顶"));
            dock->show();
#ifdef Q_OS_WIN
            HWND hwnd = (HWND)dock->winId();
            ShowWindow(hwnd, SW_HIDE);
            SetParent(hwnd, NULL);
            SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, 0);
            
            LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_APPWINDOW;
            exStyle &= ~WS_EX_TOOLWINDOW;
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
            
            LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
            SetWindowLongPtr(hwnd, GWL_STYLE, style);
            
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            ShowWindow(hwnd, SW_SHOW);
#endif
        }
    });


    connect(btnExpand, &QToolButton::clicked, this, [this, dock]() {
        setDockCollapsed(dock, !dock->property("dockCollapsed").toBool());
    });

    if (QWidget *content = dock->widget()) {
        CollapsedSideOverlay *sideOverlay = new CollapsedSideOverlay(content);
        sideOverlay->setObjectName("DockCollapsedSideOverlay");
        sideOverlay->setStyleSheet(
            "QToolButton#DockCollapsedSideExpand { border: none; padding: 1px; margin: 0px; }"
            "QToolButton#DockCollapsedSideExpand:hover { background-color: rgba(128,128,128,0.2); }");

        QVBoxLayout *sideLayout = new QVBoxLayout(sideOverlay);
        sideLayout->setContentsMargins(6, 6, 6, 6);
        sideLayout->setSpacing(6);

        QToolButton *sideExpand = new QToolButton(sideOverlay);
        sideExpand->setObjectName("DockCollapsedSideExpand");
        sideExpand->setFixedSize(28, 24);
        sideExpand->setToolTip(QStringLiteral("展开%1面板").arg(title));
        sideExpand->setToolButtonStyle(Qt::ToolButtonIconOnly);

        QLabel *sideTitle = new QLabel(verticalDockTitle(title), sideOverlay);
        sideTitle->setObjectName("DockCollapsedSideTitle");
        sideTitle->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        sideTitle->setStyleSheet("font-weight: bold; font-size: 13pt;");

        sideLayout->addWidget(sideExpand, 0, Qt::AlignHCenter);
        sideLayout->addWidget(sideTitle, 0, Qt::AlignHCenter | Qt::AlignTop);
        sideLayout->addStretch();
        sideOverlay->hide();

        connect(sideExpand, &QToolButton::clicked, this, [this, dock]() {
            setDockCollapsed(dock, false);
        });
    }

    dock->setTitleBarWidget(titleBar);
    onDockTopLevelChanged(dock->isFloating());
}

void MainWindow::setDockCollapsed(QDockWidget *dock, bool collapsed,
                                  bool restoreExpandedSize)
{
    if (!dock || !dock->widget()) return;

    if (!dock->property("collapseConstraintsSaved").toBool()) {
        dock->setProperty("collapseOriginalMinWidth", dock->minimumWidth());
        dock->setProperty("collapseOriginalMaxWidth", dock->maximumWidth());
        dock->setProperty("collapseOriginalMinHeight", dock->minimumHeight());
        dock->setProperty("collapseOriginalMaxHeight", dock->maximumHeight());
        dock->setProperty("collapseConstraintsSaved", true);
    }

    const bool wasCollapsed = dock->property("dockCollapsed").toBool();
    if (collapsed && !wasCollapsed) {
        dock->setProperty("expandedWidth", dock->width());
        dock->setProperty("expandedHeight", dock->height());
    }

    dock->setProperty("dockCollapsed", collapsed);
    dock->setMinimumWidth(dock->property("collapseOriginalMinWidth").toInt());
    dock->setMaximumWidth(dock->property("collapseOriginalMaxWidth").toInt());
    dock->setMinimumHeight(dock->property("collapseOriginalMinHeight").toInt());
    dock->setMaximumHeight(dock->property("collapseOriginalMaxHeight").toInt());

    QMainWindow *host = qobject_cast<QMainWindow*>(dock->parentWidget());
    const Qt::DockWidgetArea area = host
        ? host->dockWidgetArea(dock)
        : Qt::LeftDockWidgetArea;
    const bool sideDock = isSideDockArea(area);

    QWidget *titleBar = dock->titleBarWidget();
    QWidget *sideOverlay = dock->widget()->findChild<QWidget*>(
        "DockCollapsedSideOverlay", Qt::FindDirectChildrenOnly);

    if (collapsed) {
        if (sideDock) {
            // Keep the content in the QDockWidget layout so Qt preserves the
            // complete side-cell height. The opaque overlay becomes the
            // collapsed strip and the small custom title bar is hidden.
            dock->widget()->show();
            if (titleBar) titleBar->hide();
            if (sideOverlay) {
                sideOverlay->setGeometry(dock->widget()->rect());
                sideOverlay->show();
                sideOverlay->raise();
            }
            dock->setMinimumWidth(kCollapsedSideWidth);
            dock->setMaximumWidth(kCollapsedSideWidth);
        } else {
            if (sideOverlay) sideOverlay->hide();
            if (titleBar) titleBar->show();
            dock->widget()->hide();
            dock->setMinimumHeight(kCollapsedBottomHeight);
            dock->setMaximumHeight(kCollapsedBottomHeight);
        }
        if (host && !dock->isFloating()) {
            host->resizeDocks({dock},
                              {sideDock ? kCollapsedSideWidth
                                        : kCollapsedBottomHeight},
                              sideDock ? Qt::Horizontal : Qt::Vertical);
        }
    } else {
        if (sideOverlay) sideOverlay->hide();
        if (titleBar) titleBar->show();
        dock->widget()->show();
    }

    updateDockTitlePresentation(dock);

    if (!collapsed && wasCollapsed && restoreExpandedSize
        && host && !dock->isFloating()) {
        const int expandedSize = sideDock
            ? dock->property("expandedWidth").toInt()
            : dock->property("expandedHeight").toInt();
        if (expandedSize > 0) {
            QTimer::singleShot(0, dock, [host, dock, expandedSize, sideDock]() {
                if (!dock->property("dockCollapsed").toBool()) {
                    host->resizeDocks({dock}, {expandedSize},
                                      sideDock ? Qt::Horizontal : Qt::Vertical);
                }
            });
        }
    }
}

QStringList MainWindow::collapsedDockNames() const
{
    QStringList names;
    const QList<QDockWidget*> docks = {
        configDock, consoleDock, spectrumDock, measureDock, channelDock
    };
    for (QDockWidget *dock : docks) {
        if (dock && dock->property("dockCollapsed").toBool()) {
            names.append(dock->objectName());
        }
    }
    return names;
}

void MainWindow::restoreDockCollapseStates(const QStringList &dockNames,
                                           bool restoreExpandedSizes)
{
    const QList<QDockWidget*> docks = {
        configDock, consoleDock, spectrumDock, measureDock, channelDock
    };
    for (QDockWidget *dock : docks) {
        if (dock) {
            setDockCollapsed(dock, dockNames.contains(dock->objectName()),
                             restoreExpandedSizes);
        }
    }
}

void MainWindow::updateDockButtons()
{
    QList<QDockWidget*> allDocks = { configDock, consoleDock, spectrumDock, measureDock, channelDock };
    for (QDockWidget *dock : allDocks) {
        if (!dock) continue;
        QWidget *titleBar = dock->titleBarWidget();
        if (!titleBar) continue;

        QToolButton *btnExpand = titleBar->findChild<QToolButton*>("DockExpandBtn");

        if (dock->isFloating()) {
            if (btnExpand) btnExpand->hide();
        } else {
            if (btnExpand) btnExpand->show();
        }
        if (dock->property("dockCollapsed").toBool() && !dock->isFloating()) {
            setDockCollapsed(dock, true, false);
        } else {
            updateDockTitlePresentation(dock);
        }
    }
}

void MainWindow::onDockTopLevelChanged(bool topLevel)
{
    QDockWidget *dock = qobject_cast<QDockWidget*>(sender());
    if (dock && topLevel && dock->property("dockCollapsed").toBool()) {
        setDockCollapsed(dock, false);
    }
    updateDockButtons();
    if (dock && topLevel) {
        // Change window flags to standard top-level window
        dock->setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint |
                             Qt::WindowSystemMenuHint | Qt::WindowTitleHint |
                             Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint |
                             Qt::WindowCloseButtonHint);
        
        QSize targetSize = dock->property("dockedSize").toSize();
        if (!targetSize.isValid()) {
            targetSize = dock->size();
        }
        
        // Force normal state in case Qt tried to restore it to maximized
        dock->setWindowState(dock->windowState() & ~Qt::WindowMaximized & ~Qt::WindowMinimized & ~Qt::WindowFullScreen);

        QScreen *screen = QApplication::primaryScreen();
        if (screen) {
            QRect screenGeom = screen->availableGeometry();
            int x = screenGeom.x() + (screenGeom.width() - targetSize.width()) / 2;
            int y = screenGeom.y() + (screenGeom.height() - targetSize.height()) / 2;
            QTimer::singleShot(50, dock, [=]() {
                dock->resize(targetSize);
                dock->move(x, y);
            });
        } else {
            dock->resize(targetSize);
        }
        dock->show();
#ifdef Q_OS_WIN
        // 1. Get HWND
        HWND hwnd = (HWND)dock->winId();
        // 2. Hide natively so Qt doesn't override our changes
        ShowWindow(hwnd, SW_HIDE);
        // 3. Break the parent relationship completely
        SetParent(hwnd, NULL);
        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, 0);
        
        // 4. Force into taskbar and add minimize capabilities
        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        exStyle |= WS_EX_APPWINDOW;
        exStyle &= ~WS_EX_TOOLWINDOW;
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
        
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        // 5. Show natively again
        ShowWindow(hwnd, SW_SHOW);
#endif
    } else if (dock && !topLevel) {
        // Save size before docking
    }
}

void MainWindow::setDataTransceiveMode(bool enabled, bool restoringSavedLayout)
{
    if (enabled == m_dataTransceiveMode) return;

    const bool updatesWereEnabled = updatesEnabled();
    setUpdatesEnabled(false);

    const QList<QAction*> regularPanelActions = {
        configDock ? configDock->toggleViewAction() : nullptr,
        channelDock ? channelDock->toggleViewAction() : nullptr,
        filterPanelAction,
        consoleDock ? consoleDock->toggleViewAction() : nullptr,
        spectrumDock ? spectrumDock->toggleViewAction() : nullptr,
        measureDock ? measureDock->toggleViewAction() : nullptr
    };

    if (enabled) {
        if (!restoringSavedLayout) {
            m_normalWindowStateBeforeDataMode = saveState();
            if (innerMainWindow) {
                m_normalInnerStateBeforeDataMode = innerMainWindow->saveState();
            }
            if (consoleSplitter) {
                m_normalConsoleSplitterState = consoleSplitter->saveState();
            }
            m_normalCollapsedDockNames = collapsedDockNames();
        }

        m_dataTransceiveMode = true;
        const bool hasSavedDataModeLayout = !m_dataModeWindowState.isEmpty();

        // Data mode always starts with both of its panels expanded.
        setDockCollapsed(configDock, false, false);
        setDockCollapsed(consoleDock, false, false);

        if (hasSavedDataModeLayout) {
            restoreState(m_dataModeWindowState);
        } else {
            configDock->setFloating(false);
            consoleDock->setFloating(false);
            removeDockWidget(configDock);
            removeDockWidget(consoleDock);
            addDockWidget(Qt::LeftDockWidgetArea, configDock);
            splitDockWidget(configDock, consoleDock, Qt::Horizontal);
        }

        if (centralWidget()) centralWidget()->hide();
        if (channelDock) channelDock->hide();
        if (spectrumDock) spectrumDock->hide();
        if (measureDock) measureDock->hide();
        for (PlotContext &ctx : allPlots) {
            if (ctx.dock) ctx.dock->hide();
        }

        configDock->setFloating(false);
        consoleDock->setFloating(false);
        configDock->show();
        consoleDock->show();
        setDockCollapsed(configDock, false, false);
        setDockCollapsed(consoleDock, false, false);

        for (QAction *action : regularPanelActions) {
            if (action) action->setVisible(false);
        }

        if (consoleSplitter) {
            resetConsoleSplitterToFiveToOne(consoleSplitter);
        }

        QTimer::singleShot(0, this, [this, hasSavedDataModeLayout]() {
            if (!m_dataTransceiveMode) return;
            if (!hasSavedDataModeLayout) {
                const int totalWidth = qMax(width(), 900);
                resizeDocks({configDock, consoleDock},
                            {qMax(300, totalWidth / 4), qMax(600, totalWidth * 3 / 4)},
                            Qt::Horizontal);
            }
            // Apply this after the outer dock layout settles so even a previously
            // zero-sized sending area is reopened on every entry.
            if (consoleSplitter) {
                resetConsoleSplitterToFiveToOne(consoleSplitter);
            }
        });
    } else {
        const int expandedConfigWidth = qMax(
            configDock->property("expandedWidth").toInt(), configDock->width());
        const int expandedConsoleWidth = qMax(
            consoleDock->property("expandedWidth").toInt(), consoleDock->width());
        setDockCollapsed(configDock, false, false);
        setDockCollapsed(consoleDock, false, false);
        resizeDocks({configDock, consoleDock},
                    {qMax(360, expandedConfigWidth), qMax(360, expandedConsoleWidth)},
                    Qt::Horizontal);
        m_dataModeWindowState = saveState();
        if (consoleSplitter) {
            m_dataModeConsoleSplitterState = consoleSplitter->saveState();
        }
        m_dataTransceiveMode = false;

        // Release data-mode constraints before restoring the normal dock state.
        restoreDockCollapseStates(QStringList(), false);

        for (QAction *action : regularPanelActions) {
            if (action) action->setVisible(true);
        }
        if (centralWidget()) centralWidget()->show();

        const QByteArray normalState = !m_normalWindowStateBeforeDataMode.isEmpty()
            ? m_normalWindowStateBeforeDataMode
            : m_defaultState;
        const QByteArray normalInnerState = !m_normalInnerStateBeforeDataMode.isEmpty()
            ? m_normalInnerStateBeforeDataMode
            : m_defaultInnerState;
        if (!normalState.isEmpty()) restoreState(normalState);
        if (innerMainWindow && !normalInnerState.isEmpty()) {
            innerMainWindow->restoreState(normalInnerState);
        }
        if (consoleSplitter && !m_normalConsoleSplitterState.isEmpty()) {
            consoleSplitter->restoreState(m_normalConsoleSplitterState);
        }
        restoreDockCollapseStates(m_normalCollapsedDockNames);
    }

    if (btnDataTransceiveMode && btnDataTransceiveMode->isChecked() != enabled) {
        const QSignalBlocker blocker(btnDataTransceiveMode);
        btnDataTransceiveMode->setChecked(enabled);
    }

    updateDockButtons();
    setUpdatesEnabled(updatesWereEnabled);
    update();
}

void MainWindow::restoreDefaultLayout() {
    if (m_dataTransceiveMode) {
        m_dataTransceiveMode = false;
        if (btnDataTransceiveMode) {
            const QSignalBlocker blocker(btnDataTransceiveMode);
            btnDataTransceiveMode->setChecked(false);
        }
        if (centralWidget()) centralWidget()->show();
        const QList<QAction*> regularPanelActions = {
            configDock->toggleViewAction(), channelDock->toggleViewAction(),
            filterPanelAction, consoleDock->toggleViewAction(),
            spectrumDock->toggleViewAction(), measureDock->toggleViewAction()
        };
        for (QAction *action : regularPanelActions) {
            if (action) action->setVisible(true);
        }
    }
    m_dataModeWindowState.clear();
    m_dataModeConsoleSplitterState.clear();
    m_normalCollapsedDockNames.clear();

    // Keep the current frame visible while Qt rebuilds the dock layout.
    QLabel *layoutShield = new QLabel(this);
    layoutShield->setAttribute(Qt::WA_TransparentForMouseEvents);
    layoutShield->setPixmap(grab());
    layoutShield->setGeometry(rect());
    layoutShield->raise();
    layoutShield->show();
    QApplication::processEvents();

    const bool wasUpdatesEnabled = updatesEnabled();
    const bool wasInnerUpdatesEnabled = innerMainWindow ? innerMainWindow->updatesEnabled() : true;
    setUpdatesEnabled(false);
    if (innerMainWindow) {
        innerMainWindow->setUpdatesEnabled(false);
    }

    restoreDockCollapseStates(QStringList(), false);

    if (!m_defaultState.isEmpty()) {
        restoreState(m_defaultState);
    }
    if (innerMainWindow && !m_defaultInnerState.isEmpty()) {
        innerMainWindow->restoreState(m_defaultInnerState);
    }
    // The default snapshot contains only the main plot. Keep every dynamic
    // view and its data, but rebuild its default arrangement as vertical rows.
    if (innerMainWindow) {
        QList<QDockWidget *> plotDocks;
        for (const PlotContext &ctx : allPlots) {
            if (ctx.dock) plotDocks.append(ctx.dock);
        }
        stackPlotDocksVertically(innerMainWindow, plotDocks);
        equalizePlotHeights();
    }

    QList<QDockWidget*> allDocks = { configDock, consoleDock, channelDock, measureDock, spectrumDock };
    for (QDockWidget* dock : allDocks) {
        dock->setFloating(false);
        dock->show();
        if (dock->widget()) {
            dock->widget()->show();
        }
        if (dock->toggleViewAction()) {
            dock->toggleViewAction()->setChecked(true);
        }
    }

    updateDockButtons();
    if (filterPanelAction) {
        filterPanelAction->setChecked(true);
    }
    m_channelDockUserResized = false;
    if (listChannels) {
        listChannels->setMinimumHeight(0);
    }
    resizeDocks({configDock}, {400}, Qt::Horizontal);
    resizeDocks({channelDock}, {360}, Qt::Horizontal);
    QScreen *layoutScreen = QGuiApplication::screenAt(frameGeometry().center());
    if (!layoutScreen) {
        layoutScreen = QGuiApplication::primaryScreen();
    }
    const int availableHeight = layoutScreen
        ? layoutScreen->availableGeometry().height()
        : 1080;
    const int defaultChannelHeight = qBound(360, qRound(availableHeight * 0.26), 420);
    resizeDocks({channelDock},
                {defaultChannelHeight},
                Qt::Vertical);
    resizeDocks({consoleDock, spectrumDock, measureDock}, {1000, 1000, 350}, Qt::Horizontal);
    resizeDocks({consoleDock}, {defaultBottomDockHeight()}, Qt::Vertical);

    if (consoleSplitter) {
        consoleSplitter->setSizes(QList<int>() << 200 << 200);
    }
    updateChannelListHeight();

    if (innerMainWindow) {
        innerMainWindow->setUpdatesEnabled(wasInnerUpdatesEnabled);
        innerMainWindow->update();
    }
    setUpdatesEnabled(wasUpdatesEnabled);
    update();
    QApplication::processEvents();

    layoutShield->hide();
    delete layoutShield;

    m_normalWindowStateBeforeDataMode = saveState();
    if (innerMainWindow) {
        m_normalInnerStateBeforeDataMode = innerMainWindow->saveState();
    }
    if (consoleSplitter) {
        m_normalConsoleSplitterState = consoleSplitter->saveState();
    }
}
