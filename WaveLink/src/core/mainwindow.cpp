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
#include "statusmonitorwindow.h"
#include "helpdialog.h"
#include "ProtocolConfigTransfer.h"
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
#include <QDialogButtonBox>
#include <QAbstractSpinBox>
#include <QSignalBlocker>
#include <QNetworkInterface>
#include <QCryptographicHash>
#include <QSaveFile>
#include "receiveconsolebuffer.h"
#include "connectionhints.h"
#include <QPainter>
#include <QPainterPath>

namespace {
QIcon layoutActionIcon(bool restore) {
    QIcon icon;
    // Native vector drawing, with multiple raster sizes for high-DPI displays.
    for (int size : {16, 20, 24, 32, 40, 48, 64}) {
        QPixmap pixmap(size, size); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 20.0, size / 20.0);
        painter.setPen(QPen(Qt::white, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (restore) {
            QPainterPath arc; arc.moveTo(4, 7); arc.cubicTo(7, 1, 17, 3, 17, 10);
            arc.cubicTo(17, 17, 7, 19, 3, 13); painter.drawPath(arc);
            painter.drawPolyline(QPolygonF({QPointF(4, 3), QPointF(4, 7), QPointF(8, 7)}));
        } else {
            painter.drawLine(QPointF(3, 6), QPointF(17, 6));
            painter.drawPolyline(QPolygonF({QPointF(13, 2), QPointF(17, 6), QPointF(13, 10)}));
            painter.drawLine(QPointF(17, 14), QPointF(3, 14));
            painter.drawPolyline(QPolygonF({QPointF(7, 10), QPointF(3, 14), QPointF(7, 18)}));
        }
        painter.end(); icon.addPixmap(pixmap);
    }
    return icon;
}
}

MainWindow::MainWindow(QWidget *parent, SerialHandler *serialBackend)
    : QMainWindow(parent)
    , isRecording(false)
    , isPortOpen(false)
{
    spinDecimals = nullptr;
    chkAutoScaleY = nullptr;
    chkShowGrid = nullptr;
    demoLoggedBytes = 0;
    
    customTicker = QSharedPointer<RightAlignedTicker>(new RightAlignedTicker());
    QString recoveryError;
    const auto commandsPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("cmd_panels.json");
    const bool configRecovered = DataProtocols::ProtocolConfigTransfer::recover(DataProtocols::ProtocolProfileStore::defaultPath(), commandsPath, recoveryError);
    m_configRecoveryPending = !configRecovered;
    
    loadCustomColors();
    // Range/visibility signals can fire while UI and saved layout are restored.
    // Construct the coalescer before connecting those widgets.
    yScaleTimer = new QTimer(this);
    yScaleTimer->setSingleShot(true);
    connect(yScaleTimer, &QTimer::timeout, this, &MainWindow::autoScaleY);
    m_offlineViewTimer.setSingleShot(true);
    connect(&m_offlineViewTimer, &QTimer::timeout, this, &MainWindow::refreshPagedView);
    setupUi();
    qApp->installEventFilter(this);
    
    cmdWindow = new CommandPanelWindow(this);
    connect(cmdWindow, &CommandPanelWindow::sendDataRequested, this, &MainWindow::sendPanelData);
    cmdWindow->setSwitchSendHandler([this](const QByteArray &data) { return sendPanelData(data, false); });
    
    qRegisterMetaType<QVector<double>>("QVector<double>");
    
    // Setup Serial Handler in a separate thread
    serialThread = new QThread(this);
    serialHandler = serialBackend ? serialBackend : new SerialHandler();
    serialHandler->moveToThread(serialThread);

    // Setup Network Handler in a separate thread
    networkThread = new QThread(this);
    networkHandler = new NetworkHandler();
    networkHandler->moveToThread(networkThread);
    
    sampleCount = 0;
    frameCountForRate = 0;
    currentWindowSize = 10000;
    lastLatestTime = 0.0;
    m_autoScroll = true;
    
    // Connect thread management signals
    connect(serialThread, &QThread::finished, serialHandler, &QObject::deleteLater);
    connect(networkThread, &QThread::finished, networkHandler, &QObject::deleteLater);

    // Connect UI to SerialHandler (Cross-thread signals)
    connect(this, &MainWindow::destroyed, serialHandler, &SerialHandler::closePort);
    connect(this, &MainWindow::destroyed, networkHandler, &NetworkHandler::stopConnection);
    
    connect(serialHandler, &SerialHandler::dataReceived, this, &MainWindow::handleSerialDataReceived);
    connect(serialHandler, &SerialHandler::portOpenFinished, this, &MainWindow::handleSerialOpenFinished);
    connect(serialHandler, &SerialHandler::errorOccurred, this, &MainWindow::handleSerialError);
    connect(serialHandler, &SerialHandler::controlLinesFinished, this, &MainWindow::handleSerialControlLinesFinished);
    connect(serialHandler, &SerialHandler::breakStateChanged, this, &MainWindow::handleSerialBreakChanged);
    connect(serialHandler, &SerialHandler::sendStatusChanged, this, &MainWindow::handleSerialSendStatus);
    connect(serialHandler, &SerialHandler::sendCancellationFinished, this, &MainWindow::handleSerialSendCancelled);
    connect(serialHandler, &SerialHandler::sendRejected, this, [this](quint64 requestId, const QString &reason) {
        if (!connectionIsReady() || m_activeInterfaceType != 0 || requestId != m_serialOpenRequestId) return;
        if (cmdWindow) cmdWindow->forceStopAllTimers();
        stopConsolePeriodicSend(reason);
        if (fileSendTimer && fileSendTimer->isActive()) cancelSerialSend();
        statusBar()->showMessage(reason, 6000);
        DebugLogger::instance().log(reason, "WARN");
    });
    connect(serialHandler, &SerialHandler::recordingStartFinished, this, &MainWindow::handleRecordingStartFinished);
    connect(serialHandler, &SerialHandler::recordingError, this, &MainWindow::handleRecordingError);

    connect(networkHandler, &NetworkHandler::dataReceived, this, &MainWindow::handleSerialDataReceived);
    connect(networkHandler, &NetworkHandler::connectionStateChanged, this, &MainWindow::handleNetworkStateChanged);
    connect(networkHandler, &NetworkHandler::clientConnected, this, &MainWindow::handleClientConnected);
    connect(networkHandler, &NetworkHandler::clientDisconnected, this, &MainWindow::handleClientDisconnected);
    connect(networkHandler, &NetworkHandler::localEndpointChanged, this, [this](const QString &address, int port) {
        const int mode = comboInterface->currentData().toInt();
        auto label = mode == 5 ? lblUdpEndpoint : lblClientEndpoint;
        if ((mode != 1 && mode != 5) || !label || m_activeInterfaceType != mode
            || (m_connectionState != ConnectionState::Connecting && !connectionIsReady())) return;
        label->setText(QString(mode == 5 ? "实际监听：%1:%2" : "实际本机端点：%1:%2").arg(address.contains(':') ? "[" + address + "]" : address).arg(port));
        label->show(); updateInterfaceStackHeight();
    });
    connect(networkHandler, &NetworkHandler::errorOccurred, this, &MainWindow::handleNetworkError);
    connect(networkHandler, &NetworkHandler::connectionAttemptFinished, this, &MainWindow::handleNetworkConnectionAttemptFinished);
    connect(networkHandler, &NetworkHandler::recordingStartFinished, this, &MainWindow::handleRecordingStartFinished);
    connect(networkHandler, &NetworkHandler::recordingError, this, &MainWindow::handleRecordingError);

    serialThread->start();
    networkThread->start();
    setupBleBackend();
    setupStatusMonitor();
    
    m_portCheckTimer = new QTimer(this);
    m_portCheckTimer->setObjectName(QStringLiteral("serialHardwareCheckTimer"));
    connect(m_portCheckTimer, &QTimer::timeout, this, [this]() {
        // Demo and network modes also use isPortOpen to mean "running". Only
        // a real serial connection should participate in hardware polling.
        if (isPortOpen && comboInterface && comboInterface->currentData().toInt() == 0 && comboPorts) {
            QString portText = comboPorts->currentText().trimmed();
            QString purePortName = portText.split(' ').first();
            bool found = false;
            for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
                if (info.portName() == purePortName || info.portName() == portText) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                DebugLogger::instance().log(QString("检测到串口硬件掉线: %1").arg(purePortName), "WARN");
                handleSerialError(
                    QString("串口 %1 硬件物理连接已断开！").arg(purePortName),
                    static_cast<int>(QSerialPort::ResourceError));
            }
        }
    });
    m_portCheckTimer->start(1000);
    
    DebugLogger::instance().log("WaveLink 系统核心初始化完成", "INFO");
    loadConfig();
    setConnectionState(ConnectionState::Disconnected,
                       comboInterface ? comboInterface->currentData().toInt() : 0);
    QTimer::singleShot(0, this, &MainWindow::recoverInterruptedRecordings);
    
    defaultColors << QColor("#00ffcc") // Neon Cyan
                  << QColor("#ff00ff") // Neon Magenta
                  << QColor("#ffff00") // Neon Yellow
                  << QColor("#ff5500") // Neon Orange
                  << QColor("#00aaff") // Neon Blue
                  << QColor("#00ff00");    // Setup Timers
    plotTimer = new QTimer(this);
    connect(plotTimer, &QTimer::timeout, this, &MainWindow::onPlotTimer);
    plotTimer->setObjectName("waveformRefreshTimer");
    plotTimer->start(qRound(1000.0 / m_systemPreferences.plotFps)); // Display only.
    
    rateTimer = new QTimer(this);
    connect(rateTimer, &QTimer::timeout, this, &MainWindow::onRateTimer);
    rateTimer->start(1000);

    spectrumTimer = new QTimer(this);
    connect(spectrumTimer, &QTimer::timeout, this, &MainWindow::onSpectrumTimer);
    
    measureTimer = new QTimer(this);
    connect(measureTimer, &QTimer::timeout, this, &MainWindow::onMeasureTimer);
    measureTimer->start(500);
    spectrumTimer->start(100); // 10 FPS

    
    textUpdateTimer = new QTimer(this);
    textUpdateTimer->setObjectName("consoleReceiveUpdateTimer");
    connect(textUpdateTimer, &QTimer::timeout, this, &MainWindow::flushReceiveText);
    textUpdateTimer->start(m_systemPreferences.receiveRefreshMs);
    
    recordTimer = new QTimer(this);
    connect(recordTimer, &QTimer::timeout, this, &MainWindow::onRecordTimer);
    
    portRefreshTimer = new QTimer(this);
    connect(portRefreshTimer, &QTimer::timeout, this, &MainWindow::onRefreshPorts);
    portRefreshTimer->start(1000);
    
    
    initializeProtocols();
    initializeUpdates();
    if (!configRecovered) QTimer::singleShot(0, this, [this, recoveryError] {
        QMessageBox::warning(this, QStringLiteral("全局配置恢复未完成"), recoveryError + QStringLiteral("\n请保留恢复记录，暂不要导入其他配置。"));
    });
    
    // Initial port population
    onRefreshPorts();
    onInterfaceChanged(comboInterface->currentIndex());

    QList<QDockWidget*> allDocks = findChildren<QDockWidget*>();
    for (QDockWidget *dock : allDocks) {
        connect(dock, &QDockWidget::topLevelChanged, this, [dock](bool floating){
            if (floating) {
                dock->showNormal();
            }
        });
    }
}

MainWindow::~MainWindow()
{
    if (consoleSendTimer) consoleSendTimer->stop();
    if (bleClient) bleClient->disconnectDevice();
    if (bleWorker && bleThread && bleThread->isRunning()) {
        bleWorker->endSession();
        QMetaObject::invokeMethod(bleWorker, "stopRecordingNow", Qt::BlockingQueuedConnection);
        bleThread->quit(); bleThread->wait();
    }
    auto stopWorkerThread = [](QThread *thread, const char *name) {
        if (!thread || !thread->isRunning()) return;
        thread->requestInterruption();
        thread->quit();
        if (thread->wait(2000)) return;

        // A defective device driver can leave a synchronous OS call stuck.
        // Do not make application shutdown wait forever in that situation.
        qCritical() << name << "did not stop in time; terminating worker thread";
        thread->terminate();
        thread->wait(2000);
    };
    stopWorkerThread(serialThread, "Serial worker");
    stopWorkerThread(networkThread, "Network worker");
}

void MainWindow::centerWindowOnScreen()
{
    if (isMaximized() || isFullScreen() || isMinimized()) {
        return;
    }

    QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return;
    }

    const QRect availableGeometry = screen->availableGeometry();
    const QRect currentFrame = frameGeometry();
    const int x = availableGeometry.x() +
                  (availableGeometry.width() - currentFrame.width()) / 2;
    const int y = availableGeometry.y() +
                  (availableGeometry.height() - currentFrame.height()) / 2;
    move(x, y);
}

void MainWindow::setupUi()
{
    setWindowTitle("WaveLink");
    setWindowIcon(QIcon(":/app.ico"));
    resize(1280, 800);
    QScreen *primaryScreen = QGuiApplication::primaryScreen();
    if (primaryScreen) {
        QRect screenGeometry = primaryScreen->availableGeometry();
        int x = screenGeometry.x() + (screenGeometry.width() - width()) / 2;
        int y = screenGeometry.y() + (screenGeometry.height() - height()) / 2;
        move(x, y);
    }
    
    // 1. Central Widget: Nested QMainWindow for docking plots
    QWidget *mainCentral = new QWidget(this);
    mainCentral->setFocusPolicy(Qt::StrongFocus);
    QVBoxLayout *mainLayout = new QVBoxLayout(mainCentral);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    innerMainWindow = new QMainWindow(this);
    innerMainWindow->setWindowFlags(Qt::Widget);
    innerMainWindow->setDockNestingEnabled(true);
    QWidget *dummy = new QWidget();
    dummy->setFixedSize(0,0);
    innerMainWindow->setCentralWidget(dummy);
    
    m_rangeSlider = new RangeSlider(this);
    m_rangeSlider->setObjectName("timeRangeSlider");
    m_rangeSlider->setRange(0, 10);
    m_rangeSlider->setValues(0, 10);
    connect(m_rangeSlider, &RangeSlider::rangeChanged, this, [this](double lower, double upper) {
        onXAxisRangeChanged(QCPRange(lower, upper));
    });

    QHBoxLayout *sliderLayout = new QHBoxLayout();
    sliderLayout->setContentsMargins(10, 0, 10, 5);
    
    QDoubleSpinBox *spinStep = new QDoubleSpinBox(this);
    spinStep->setRange(0.01, 1000.0);
    spinStep->setValue(1.0);
    spinStep->setSingleStep(0.1);
    spinStep->setDecimals(2);
    spinStep->setSuffix(" s");
    spinStep->setToolTip("步长(Step)");
    spinStep->setFixedWidth(100);
    spinStep->setFocusPolicy(Qt::ClickFocus);
    if (QLineEdit *spinStepEdit = spinStep->findChild<QLineEdit*>()) {
        connect(spinStepEdit, &QLineEdit::returnPressed, this, [spinStep]() {
            spinStep->clearFocus();
        });
    }
    
    QPushButton *btnLeft = new QPushButton("<", this);
    btnLeft->setFixedWidth(30);
    
    QPushButton *btnRight = new QPushButton(">", this);
    btnRight->setFixedWidth(30);
    auto btnTimeLatest = new QPushButton(">>", this);
    btnTimeLatest->setObjectName("btnTimeScrollToLatest"); btnTimeLatest->setFixedWidth(40);
    btnTimeLatest->setToolTip("跳转至最新时刻（恢复自动跟随波形）");
    btnTimeLatest->setAccessibleName("跳转至最新时刻"); btnTimeLatest->setCursor(Qt::PointingHandCursor);
    connect(btnTimeLatest, &QPushButton::clicked, this, &MainWindow::scrollToLatest);
    
    connect(btnLeft, &QPushButton::clicked, this, [this, spinStep]() {
        double step = spinStep->value();
        double currentLower = m_rangeSlider->lowerValue();
        double currentUpper = m_rangeSlider->upperValue();
        const double minTime = m_rangeSlider->minimum();
        if (currentLower - step >= minTime) {
            m_rangeSlider->setValues(currentLower - step, currentUpper - step);
        } else {
            double diff = currentUpper - currentLower;
            m_rangeSlider->setValues(minTime, minTime + diff);
        }
    });

    connect(btnRight, &QPushButton::clicked, this, [this, spinStep]() {
        double step = spinStep->value();
        double currentLower = m_rangeSlider->lowerValue();
        double currentUpper = m_rangeSlider->upperValue();
        double maxTime = m_rangeSlider->maximum();
        if (currentUpper + step <= maxTime) {
            m_rangeSlider->setValues(currentLower + step, currentUpper + step);
        } else {
            double diff = currentUpper - currentLower;
            m_rangeSlider->setValues(maxTime - diff, maxTime);
        }
    });

    sliderLayout->addWidget(spinStep);
    sliderLayout->addSpacing(5);
    sliderLayout->addWidget(btnLeft);
    sliderLayout->addSpacing(15);
    sliderLayout->addWidget(m_rangeSlider, 1);
    sliderLayout->addSpacing(15);
    sliderLayout->addWidget(btnRight);
    sliderLayout->addSpacing(5);
    sliderLayout->addWidget(btnTimeLatest);
    
    plotScrollArea = new PlotScrollArea(mainCentral);
    plotScrollArea->setWidget(innerMainWindow);
    connect(plotScrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        // A newly revealed detached plot may have been skipped while clipped.
        for (const auto &ctx : allPlots)
            if (ctx.plot->isVisible() && !ctx.plot->visibleRegion().isEmpty()) ctx.plot->replot(QCustomPlot::rpQueuedReplot);
        if (chkAutoScaleY && chkAutoScaleY->isChecked()) yScaleTimer->start(0);
    });
    mainLayout->addWidget(plotScrollArea, 1);
    mainLayout->addLayout(sliderLayout, 0);
    
    setCentralWidget(mainCentral);
    
    PlotContext ctx = createNewPlotContext("主视图", true);
    allPlots.append(ctx);
    innerMainWindow->addDockWidget(Qt::TopDockWidgetArea, ctx.dock);
    
    // 2. Setup Dock Widgets
    setupDockWidgets();
    
    // 3. Setup Menu Bar & Status Bar
    setupMenuAndStatusBar();
}

void MainWindow::setupMenuAndStatusBar()
{
    // Status Bar
    QStatusBar *sBar = new QStatusBar(this);
    setStatusBar(sBar);
    
    lblStatusMessage = new QLabel("Welcome to WaveLink");
    sBar->addWidget(lblStatusMessage, 1);
    lblRecordingWarning = new QLabel;
    lblRecordingWarning->setObjectName("lblRecordingWarning");
    lblRecordingWarning->setToolTip("本次录制以首个有效数值帧的通道数为准；数量不符的帧仅跳过录制，实时显示不受影响。再次录制会重新确定通道数并清零计数。");
    sBar->addPermanentWidget(lblRecordingWarning); lblRecordingWarning->hide();
    
    lblSystemTime = new QLabel();
    lblSystemTime->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sBar->addPermanentWidget(lblSystemTime);
    
    systemTimeTimer = new QTimer(this);
    connect(systemTimeTimer, &QTimer::timeout, this, &MainWindow::updateSystemTime);
    systemTimeTimer->start(1000);
    updateSystemTime();
    
    // Menu Bar
    QMenuBar *mBar = new QMenuBar(this);
    setMenuBar(mBar);
    
    m_fileMenu = mBar->addMenu(QString::fromUtf8("文件(&F)"));
    QMenu *menuFile = m_fileMenu;
    QAction *actSave = menuFile->addAction(QString::fromUtf8("保存数据(&S)"));
    connect(actSave, &QAction::triggered, this, &MainWindow::onExportData);
    QAction *actImport = menuFile->addAction(QString::fromUtf8("导入数据(&I)"));
    connect(actImport, &QAction::triggered, this, &MainWindow::onImportData);
    QAction *actScreenshot = menuFile->addAction(QString::fromUtf8("界面截图(&P)"));
    connect(actScreenshot, &QAction::triggered, this, [this](){
        QPixmap pixmap = this->grab();
        QSettings settings("WaveLink", "AdvancedApp");
        QString lastDir = settings.value("LastDataDir", QDir::homePath()).toString();
        QString defaultName = QDir(lastDir).filePath(QString("WaveLink_Shot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
        QString fileName = QFileDialog::getSaveFileName(this, QString::fromUtf8("保存截图"), defaultName, "Images (*.png *.jpg)");
        if (!fileName.isEmpty()) {
            settings.setValue("LastDataDir", QFileInfo(fileName).absolutePath());
            pixmap.save(fileName);
            statusBar()->showMessage(QString::fromUtf8("截图已保存: ") + fileName, 3000);
        }
    });
    menuFile->addSeparator();
    QAction *actExit = menuFile->addAction(QString::fromUtf8("退出(&X)"));
    connect(actExit, &QAction::triggered, qApp, &QCoreApplication::quit);
    
    m_settingsMenu = new QMenu(QString::fromUtf8("设置(&S)"), mBar);
    QMenu *menuSettings = m_settingsMenu;
    auto systemSettings = menuSettings->addAction(QStringLiteral("系统设置(&O)"));
    systemSettings->setObjectName("actionSystemSettings");
    connect(systemSettings, &QAction::triggered, this, &MainWindow::showSystemSettings);
    menuSettings->addSeparator();
    QMenu *menuTheme = menuSettings->addMenu(QString::fromUtf8("主题(&T)"));
    
    QAction *actThemeDark = menuTheme->addAction(QString::fromUtf8("深色科技 (暗黑)"));
    connect(actThemeDark, &QAction::triggered, this, [this](){
        if(comboTheme) comboTheme->setCurrentIndex(0);
    });
    QAction *actThemeLight = menuTheme->addAction(QString::fromUtf8("明亮简约 (浅白)"));
    connect(actThemeLight, &QAction::triggered, this, [this](){
        if(comboTheme) comboTheme->setCurrentIndex(1);
    });

    menuSettings->addSeparator();
    QAction *actExportConfig = menuSettings->addAction("导出配置(&E)");
    connect(actExportConfig, &QAction::triggered, this, &MainWindow::onExportConfig);
    QAction *actImportConfig = menuSettings->addAction("导入配置(&I)");
    connect(actImportConfig, &QAction::triggered, this, &MainWindow::onImportConfig);

    m_viewMenu = mBar->addMenu(QString::fromUtf8("视图(&V)"));
    QMenu *menuView = m_viewMenu;
    
    QAction *actPin = menuView->addAction(QString::fromUtf8("窗口置顶(&T)"));
    actPin->setCheckable(true);
    connect(actPin, &QAction::toggled, this, [this, actPin](bool checked) {
        if (checked) {
            this->setWindowFlag(Qt::WindowStaysOnTopHint, true);
            actPin->setText(QString::fromUtf8("取消置顶(&T)"));
        } else {
            this->setWindowFlag(Qt::WindowStaysOnTopHint, false);
            actPin->setText(QString::fromUtf8("窗口置顶(&T)"));
        }
        this->show(); // Re-show to apply window flag changes immediately
    });
    menuView->addSeparator();
    
    auto addViewAct = [menuView](QDockWidget* dock, const QString& shortcut) {
        if (dock) {
            QAction* act = dock->toggleViewAction();
            QString cleanTitle = act->text();
            act->setText(cleanTitle + "(" + shortcut + ")");
            act->setIconText(cleanTitle);
            menuView->addAction(act);
        }
    };
    
    addViewAct(configDock, "&C");
    addViewAct(channelDock, "&H");
    if (filterPanelAction) {
        menuView->addAction(filterPanelAction);
    }
    addViewAct(consoleDock, "&L");
    addViewAct(spectrumDock, "&S");
    addViewAct(measureDock, "&M");

    menuView->addSeparator();

    QAction *actMaximizeWindow = menuView->addAction(QString::fromUtf8("窗口最大化"));
    auto updateMaximizeWindowText = [this, actMaximizeWindow]() {
        actMaximizeWindow->setText(
            isMaximized()
                ? QString::fromUtf8("取消最大化(&X)")
                : QString::fromUtf8("窗口最大化(&X)"));
    };
    connect(actMaximizeWindow, &QAction::triggered, this, [this, updateMaximizeWindowText]() {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
        updateMaximizeWindowText();
    });
    updateMaximizeWindowText();

    QAction *actMinimizeWindow = menuView->addAction(QString::fromUtf8("窗口最小化(&N)"));
    connect(actMinimizeWindow, &QAction::triggered, this, &QWidget::showMinimized);
    
    m_toolsMenu = mBar->addMenu(QString::fromUtf8("工具(&T)"));
    QMenu *menuTool = m_toolsMenu;
    
    QAction *actCmdPanel = menuTool->addAction("命令交互面板(&M)");
    connect(actCmdPanel, &QAction::triggered, this, &MainWindow::showCommandPanel);
    auto actStatus=menuTool->addAction("状态监视面板(&S)");actStatus->setObjectName("actionStatusMonitor");
    connect(actStatus,&QAction::triggered,this,&MainWindow::showStatusMonitor);
    menuTool->addSeparator();

    QAction *actColorPicker = menuTool->addAction(QString::fromUtf8("颜色拾取器(&C)"));
    connect(actColorPicker, &QAction::triggered, this, [this]() {
        QColorDialog *dlg = createStyledColorDialog(QString::fromUtf8("颜色拾取器"), Qt::white, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowFlags(Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
        dlg->show();
    });
    
    QAction *actCoilCalc = menuTool->addAction(QString::fromUtf8("线圈辅助计算器(&L)"));
    connect(actCoilCalc, &QAction::triggered, this, [this]() {
        QFile resFile(":/CoilCalculator.exe");
        QByteArray executableData;
        if (resFile.open(QIODevice::ReadOnly)) executableData = resFile.readAll();
        const QString resourceHash = QString::fromLatin1(
            QCryptographicHash::hash(executableData, QCryptographicHash::Sha256).toHex().left(12));
        const QString tempPath = QDir(QDir::tempPath()).absoluteFilePath(
            QStringLiteral("WaveLink_CoilCalculator_%1.exe").arg(resourceHash));
        if (!executableData.isEmpty() && !QFile::exists(tempPath)) {
            QSaveFile outFile(tempPath);
            if (outFile.open(QIODevice::WriteOnly) &&
                outFile.write(executableData) == executableData.size()) {
                outFile.commit();
            }
        }
        if (QFile::exists(tempPath)) {
            QProcess::startDetached(tempPath, QStringList());
        } else {
            QMessageBox::warning(this, QString::fromUtf8("错误"), QString::fromUtf8("无法释放或启动计算器程序！"));
        }
    });
    
    QAction *actCH341 = menuTool->addAction(QString::fromUtf8("UART Driver (CH341) 下载(&D)"));
    connect(actCH341, &QAction::triggered, this, [this]() {
        QString savePath = QFileDialog::getSaveFileName(this, QString::fromUtf8("保存 CH341 驱动"), "CH341.zip", "ZIP (*.zip)");
        if (savePath.isEmpty()) return;
        QFile resFile(":/CH341.zip");
        if (resFile.open(QIODevice::ReadOnly)) {
            QFile outFile(savePath);
            if (outFile.open(QIODevice::WriteOnly)) {
                outFile.write(resFile.readAll());
                outFile.close();
                QMessageBox::information(this, QString::fromUtf8("下载完成"), QString::fromUtf8("CH341 驱动已保存至:\n%1").arg(savePath));
            } else {
                QMessageBox::warning(this, QString::fromUtf8("错误"), QString::fromUtf8("无法写入文件: %1").arg(savePath));
            }
            resFile.close();
        } else {
            QMessageBox::warning(this, QString::fromUtf8("错误"), QString::fromUtf8("资源包中未找到 CH341.zip"));
        }
    });
    
    mBar->addMenu(m_settingsMenu);
    m_helpMenu = mBar->addMenu(QString::fromUtf8("帮助(&H)"));
    QMenu *menuHelp = m_helpMenu;
    QAction *actHelp = menuHelp->addAction(QString::fromUtf8("帮助中心(&C)"));
    // connect to help dialog
    connect(actHelp, &QAction::triggered, this, [this](){
        HelpDialog dlg(!comboTheme || comboTheme->currentIndex() == 0, this);
        dlg.exec();
    });
    
    QAction *actAbout = menuHelp->addAction(QString::fromUtf8("版本信息(&A)"));
    actAbout->setObjectName("actionVersionInfo");
    actAbout->setToolTip(QStringLiteral("查看当前版本、检查更新及下载安装包。"));
    connect(actAbout, &QAction::triggered, this, &MainWindow::showVersionInfo);
}

void MainWindow::updateSystemTime()
{
    lblSystemTime->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
}

void MainWindow::setupDockWidgets()
{
    // --- Left Dock: Configuration ---
    configDock = new QDockWidget("参数设置", this);
    configDock->setObjectName("ConfigDock");
    configDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    configDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    
    QWidget *configWidget = new QWidget();
    configWidget->setMinimumWidth(340);
    QVBoxLayout *configLayout = new QVBoxLayout(configWidget);
    
    // Protocol Group
    QGroupBox *groupProtocol = new QGroupBox("数据协议");
    QVBoxLayout *protocolLayout = new QVBoxLayout(groupProtocol);
    protocolLayout->setSpacing(2);
    protocolLayout->setContentsMargins(5, 5, 5, 5);
    
    QHBoxLayout *protocolTopLayout = new QHBoxLayout();
    comboProtocol = new QComboBox();
    comboProtocol->setMinimumWidth(200); // Prevent text truncation
    comboProtocol->setObjectName("comboProtocol");
    comboProtocol->addItem("Raw (文本/Hex)", "builtin.raw");
    comboProtocol->addItem("FireWater (CSV 文本)", "builtin.firewater");
    comboProtocol->addItem("JustFloat (二进制浮点数)", "builtin.justfloat");
    comboProtocol->addItem("自定义协议", "custom");
    QSettings settings("WaveLink", "AdvancedApp");
    int protocolIdx = settings.value("Protocol", 0).toInt();
    if (protocolIdx >= 0 && protocolIdx <= 4) {
        comboProtocol->setCurrentIndex(protocolIdx >= 3 ? 3 : protocolIdx);
    }
    
    QPushButton *btnProtocolHelp = new QPushButton("❔");
    btnProtocolHelp->setToolTip("查看各数据协议的通讯规范及收发示例");
    btnProtocolHelp->setFixedWidth(30);
    
    protocolTopLayout->addWidget(comboProtocol);
    protocolTopLayout->addWidget(btnProtocolHelp);
    
    comboEndianness = new QComboBox();
    comboEndianness->addItems({"小端模式 (Little-Endian) (常用)", "大端模式 (Big-Endian)"});
    comboEndianness->setVisible(protocolIdx == 2); // Show only for JustFloat
    int endianIdx = settings.value("Endianness", 0).toInt();
    if (endianIdx >= 0 && endianIdx < comboEndianness->count()) {
        comboEndianness->setCurrentIndex(endianIdx);
    }
    
    protocolLayout->addLayout(protocolTopLayout);
    protocolLayout->addWidget(comboEndianness);
    comboCustomProfile = new QComboBox; comboCustomProfile->setObjectName("comboCustomProfile");
    comboCustomProfile->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    comboCustomProfile->setMinimumContentsLength(8); protocolLayout->addWidget(comboCustomProfile);
    lblProtocolSummary = new QLabel; lblProtocolSummary->setObjectName("lblProtocolSummary");
    lblProtocolSummary->setWordWrap(true); lblProtocolSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    protocolLayout->addWidget(lblProtocolSummary);
    comboCustomProfile->hide(); lblProtocolSummary->hide();
    connect(comboCustomProfile, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::handleCustomProfileChanged);
    btnCustomProtocols = new QPushButton(QStringLiteral("⚙ 管理自定义协议"));
    btnCustomProtocols->setObjectName("btnCustomProtocols");
    btnCustomProtocols->setToolTip(QStringLiteral("编辑、保存、导入导出和离线测试自定义文本/二进制协议方案"));
    btnCustomProtocols->setStyleSheet("QPushButton#btnCustomProtocols { background-color: #1675b5; color: white; border: 1px solid #3396d6; border-radius: 4px; padding: 5px 8px; font-weight: bold; }"
        "QPushButton#btnCustomProtocols:hover { background-color: #218bc9; } QPushButton#btnCustomProtocols:pressed { background-color: #125888; }"
        "QPushButton#btnCustomProtocols:disabled { background-color: #555555; color: #bbbbbb; border-color: #666666; }");
    btnCustomProtocols->setVisible(protocolIdx == 4);
    protocolLayout->addWidget(btnCustomProtocols);
    connect(btnCustomProtocols, &QPushButton::clicked, this, &MainWindow::showCustomProtocols);
    
    connect(btnProtocolHelp, &QPushButton::clicked, this, &MainWindow::showProtocolHelpDialog);
    configLayout->addWidget(groupProtocol);
    
    // Connection Group
    QGroupBox *groupConnection = new QGroupBox("设备连接");
    groupConnection->setObjectName(QStringLiteral("groupDeviceConnection"));
    // The stacked interface pages report an expanding size policy. Without an
    // explicit vertical policy this group absorbs all spare height on portrait
    // displays and QVBoxLayout spreads its rows apart. Keep the group at its
    // content height; the outer parameter-panel stretch/scroll area owns any
    // remaining vertical space.
    groupConnection->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    QVBoxLayout *connLayout = new QVBoxLayout(groupConnection);
    connLayout->setSpacing(2);
    connLayout->setContentsMargins(5, 5, 5, 5);
    connLayout->setAlignment(Qt::AlignTop);

    QHBoxLayout *interfaceLayout = new QHBoxLayout();
    auto interfaceLabel = new QLabel("数据接口");
    interfaceLabel->setObjectName("interfaceTypeLabel");
    interfaceLayout->addWidget(interfaceLabel);
    comboInterface = new QComboBox();
    comboInterface->setObjectName("comboInterface");
    comboInterface->addItem("串口", 0);
    comboInterface->addItem("TCP客户端", 1);
    comboInterface->addItem("TCP服务端", 2);
    comboInterface->addItem(QStringLiteral("UDP 用户数据报"), 5);
#if WAVELINK_ENABLE_BLE
    comboInterface->addItem(QStringLiteral("BLE 低功耗蓝牙"), 4);
#endif
    comboInterface->addItem("Demo数据", 3); // Display order does not change saved interface IDs.
    const QMap<int, QString> interfaceTips{
        {0, "串口：通过 COM 端口连接设备，通信参数须与设备一致。"},
        {1, "TCP 客户端：由电脑主动连接设备或服务器。"},
        {2, "TCP 服务端：由电脑监听端口，等待设备连接。"},
        {5, "UDP 用户数据报：按独立数据包收发，不保证到达或顺序。"},
        {4, "BLE 低功耗蓝牙：连接 BLE 外设，选择接收通知与发送特征。"},
        {3, "Demo 数据：软件生成模拟波形，无需连接硬件。"}
    };
    QStringList interfaceSummary;
    for (int row = 0; row < comboInterface->count(); ++row) {
        const auto tip = interfaceTips.value(comboInterface->itemData(row).toInt());
        comboInterface->setItemData(row, tip, Qt::ToolTipRole);
        interfaceSummary.append(tip);
    }
    const auto interfaceTip = "<qt>" + interfaceSummary.join("<br>") + "</qt>";
    interfaceLabel->setToolTip(interfaceTip);
    comboInterface->setToolTip(interfaceTip);
    comboInterface->setCurrentIndex(qMax(0, comboInterface->findData(settings.value("InterfaceType", 0).toInt())));
    interfaceLayout->addWidget(comboInterface);
    connLayout->addLayout(interfaceLayout);

    stackInterface = new QStackedWidget();

    setupDemoDataPanel();

    QWidget *wSerial = new QWidget();
    wSerial->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFormLayout *serialLayout = new QFormLayout(wSerial);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    // Match the Demo page's native form spacing while keeping the group
    // fixed to its content height on tall displays.
    serialLayout->setHorizontalSpacing(6);
    comboPorts = new QComboBox();
    comboPorts->setObjectName(QStringLiteral("comboPorts"));
    QString savedPort = settings.value("Port", "").toString();
    if (!savedPort.isEmpty()) {
        comboPorts->addItem(savedPort);
        comboPorts->setCurrentText(savedPort);
    }
    
    comboBaudRate = new QComboBox();
    comboBaudRate->addItems({"9600", "19200", "38400", "57600", "115200", "460800", "921600", "1000000", "2000000"});
    comboBaudRate->setCurrentText(settings.value("BaudRate", "115200").toString());
    comboBaudRate->setEditable(true);
    
    comboDataBits = new QComboBox();
    comboDataBits->addItems({"5", "6", "7", "8"});
    comboDataBits->setCurrentText(settings.value("DataBits", "8").toString());
    
    comboParity = new QComboBox();
    comboParity->addItems({"None", "Even", "Odd", "Space", "Mark"});
    comboParity->setCurrentText(settings.value("Parity", "None").toString());
    
    comboStopBits = new QComboBox();
    comboStopBits->addItems({"1", "1.5", "2"});
    comboStopBits->setCurrentText(settings.value("StopBits", "1").toString());
    
    serialLayout->addRow("端口号", comboPorts);
    
    lblBluetoothName = new QLabel("");
    lblBluetoothName->setStyleSheet("color: #aaaaaa;");
    lblBluetoothName->setWordWrap(true);
    lblBluetoothName->hide();
    serialLayout->addRow("", lblBluetoothName);
    
    serialLayout->addRow("波特率", comboBaudRate);
    serialLayout->addRow("数据位", comboDataBits);
    serialLayout->addRow("校验位", comboParity);
    serialLayout->addRow("停止位", comboStopBits);
    comboFlowControl = new QComboBox();
    comboFlowControl->setObjectName(QStringLiteral("comboFlowControl"));
    comboFlowControl->addItem(QStringLiteral("无流控（默认）"), 0);
    comboFlowControl->addItem(QStringLiteral("硬件流控（RTS/CTS）"), 1);
    comboFlowControl->setToolTip(QStringLiteral(
        "无流控适用于普通 TX/RX/GND 三线连接，首次使用默认选择此项。\n"
        "硬件流控需要转换器、设备及 RTS/CTS 接线均支持；RTS 由驱动自动控制，不能手动置位。\n"
        "CTS 未就绪时发送等待，接收和录制继续。队列上限 256 KB / 1024 条发送请求，满后拒绝新命令并停止命令循环。\n"
        "取消会丢弃软件中尚未发送的数据，不会自动重发；已提交到驱动或硬件的少量字节仍可能到达，无法撤回。\n"
        "若已有数据进入驱动，取消时会安全断开串口并停止录制，防止旧数据在 CTS 恢复后继续发出。\n"
        "设置自动保存，只能在未连接时修改，普通调试不需要开启。"));
    serialLayout->addRow(QStringLiteral("流控"), comboFlowControl);
    auto controlLinesLayout = new QHBoxLayout();
    chkDtr = new QCheckBox(QStringLiteral("DTR"));
    chkDtr->setObjectName(QStringLiteral("chkDtr"));
    chkRts = new QCheckBox(QStringLiteral("RTS"));
    chkRts->setObjectName(QStringLiteral("chkRts"));
    const QString controlLineTip = QStringLiteral(
        "勾选表示置位此串口控制信号，取消表示释放。默认关闭，设置会自动保存。\n"
        "未连接时设置供下次打开使用；已连接时在后台立即应用。\n"
        "需要串口芯片及设备支持，不代表启用 RTS/CTS 硬件流控。\n"
        "Windows 下先合并应用 DTR/RTS 再报告连接成功；关闭时核对状态，不恢复打开前的旧设置。\n"
        "驱动无法确认时会提示并显示未知状态；取消勾选不能保证驱动在打开/关闭瞬间完全没有电平变化。\n"
        "注意：部分开发板使用 DTR/RTS 控制复位或下载模式，切换可能使设备复位。");
    chkDtr->setToolTip(controlLineTip);
    chkRts->setToolTip(controlLineTip);
    controlLinesLayout->addWidget(chkDtr);
    controlLinesLayout->addWidget(chkRts);
    controlLinesLayout->addStretch();
    serialLayout->addRow(QStringLiteral("控制信号"), controlLinesLayout);
    auto breakLayout = new QHBoxLayout();
    breakLayout->setSpacing(6);
    chkSerialBreak = new QCheckBox(QStringLiteral("保持"));
    chkSerialBreak->setObjectName(QStringLiteral("chkSerialBreak"));
    chkSerialBreak->setEnabled(false);
    spinSerialBreakMs = new QSpinBox();
    spinSerialBreakMs->setObjectName(QStringLiteral("spinSerialBreakMs"));
    spinSerialBreakMs->setRange(1, 5000);
    spinSerialBreakMs->setSingleStep(10);
    spinSerialBreakMs->setSuffix(QStringLiteral(" ms"));
    spinSerialBreakMs->setValue(250);
    spinSerialBreakMs->setMinimumWidth(80);
    btnSerialBreak = new QPushButton(QStringLiteral("发送"));
    btnSerialBreak->setObjectName(QStringLiteral("btnSerialBreak"));
    btnSerialBreak->setEnabled(false);
    const QString breakTip = QStringLiteral(
        "Break 使串口 TX 持续处于逻辑 0，不是断开连接，也不是发送 0x00。\n"
        "勾选“保持”会持续开启；取消勾选或点击“释放”即可恢复普通发送。\n"
        "也可设置 1–5000 ms 后点击“发送”，到时由后台自动释放。\n"
        "只在串口连接成功、驱动支持且没有正在发送文件时可操作。\n"
        "开启或处理期间暂停普通数据/文件发送并停止命令循环，不会自动重发；接收和录制继续。\n"
        "每次启动、重连都默认释放，不保存开启状态；仅保存短时发送的时长。\n"
        "普通调试一般无需开启，请按设备协议使用，部分设备可能复位或执行特殊操作。\n"
        "持续时间受系统调度和驱动影响，不保证精密时序，不能代替完整的 LIN/DMX 接口。");
    chkSerialBreak->setToolTip(breakTip);
    spinSerialBreakMs->setToolTip(breakTip);
    btnSerialBreak->setToolTip(breakTip);
    auto breakLabel = new QLabel(QStringLiteral("Break"));
    breakLabel->setToolTip("让 TX 暂时保持逻辑 0，并非发送 0x00；普通调试通常无需开启，可能使设备复位。");
    breakLayout->addWidget(chkSerialBreak);
    breakLayout->addWidget(spinSerialBreakMs, 1);
    breakLayout->addWidget(btnSerialBreak, 1);
    serialLayout->addRow(breakLabel, breakLayout);
    serialSendStatusWidget = new QWidget();
    auto sendStatusLayout = new QHBoxLayout(serialSendStatusWidget);
    sendStatusLayout->setContentsMargins(0, 0, 0, 0);
    sendStatusLayout->setSpacing(6);
    lblSerialSendStatus = new QLabel(QStringLiteral("未连接"));
    lblSerialSendStatus->setObjectName(QStringLiteral("lblSerialSendStatus"));
    lblSerialSendStatus->setWordWrap(true);
    btnCancelSerialSend = new QPushButton(QStringLiteral("取消待发送"));
    btnCancelSerialSend->setObjectName(QStringLiteral("btnCancelSerialSend"));
    btnCancelSerialSend->setEnabled(false);
    btnCancelSerialSend->setToolTip(comboFlowControl->toolTip());
    sendStatusLayout->addWidget(lblSerialSendStatus, 1);
    sendStatusLayout->addWidget(btnCancelSerialSend);
    serialSendStatusWidget->hide();
    serialLayout->addRow(serialSendStatusWidget);
    ConnectionHints::row(serialLayout, comboPorts, "选择设备对应的 COM 端口；被其他程序占用时无法打开。");
    ConnectionHints::row(serialLayout, comboBaudRate, "串口通信速率，单位 bit/s，须与设备设置一致。");
    ConnectionHints::row(serialLayout, comboDataBits, "每个字符的数据位数，常用 8 位，须与设备一致。");
    ConnectionHints::row(serialLayout, comboParity, "校验方式须与设备一致：None 无、Even 偶、Odd 奇、Space 固定 0、Mark 固定 1。");
    ConnectionHints::row(serialLayout, comboStopBits, "字符结束的停止位长度，常用 1 位，须与设备一致。");
    ConnectionHints::row(serialLayout, comboFlowControl, "无流控适合普通三线连接；RTS/CTS 需设备及接线支持。");
    ConnectionHints::row(serialLayout, controlLinesLayout, "手动控制 DTR/RTS，部分开发板会因此复位；普通调试通常保持释放。");
    
    stackInterface->addWidget(wSerial);

    QWidget *wNetwork = new QWidget();
    wNetwork->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFormLayout *networkLayout = new QFormLayout(wNetwork);
    networkLayout->setContentsMargins(0, 0, 0, 0);
    networkLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    networkLayout->setHorizontalSpacing(6);
    editIP = new QLineEdit();
    editIP->setObjectName("editIP");
    editIP->setText(settings.value("NetworkIP", "192.168.4.1").toString());
    spinPort = new QSpinBox();
    spinPort->setObjectName("spinPort");
    spinPort->setRange(1, 65535);
    spinPort->setValue(settings.value("NetworkPort", 8080).toInt());
    editIP->setToolTip("设备或服务器的远端 IP，也可输入主机名。请填写实际目标地址，0.0.0.0 不能用作远端 IP。");
    editIP->setMaxLength(253);
    spinPort->setToolTip("目标设备的 TCP 监听端口，范围 1–65535。");
    auto addressChoice = [](const QString &name, const QString &address) {
        auto combo = new QComboBox; combo->setObjectName(name); combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert); combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8); combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->addItem("0.0.0.0"); combo->setItemData(0, "所有本机 IPv4 地址；客户端由系统选择出口地址。", Qt::ToolTipRole);
        combo->addItem("127.0.0.1");
        for (const auto &local : QNetworkInterface::allAddresses())
            if (local.protocol() == QAbstractSocket::IPv4Protocol && combo->findText(local.toString()) < 0) combo->addItem(local.toString());
        combo->lineEdit()->setMaxLength(128); combo->setCurrentText(address);
        connect(combo, QOverload<int>::of(&QComboBox::activated), combo, [combo](int row) { combo->setEditText(combo->itemText(row)); });
        return combo;
    };
    comboLocalIP = addressChoice("comboLocalIP", settings.value("NetworkLocalIP", "0.0.0.0").toString());
    comboLocalIP->setToolTip("客户端的本机绑定 IP。默认 0.0.0.0，由系统选择出口网卡；可手动输入或下拉选择本机 IP。指定 IP 必须属于当前电脑；IPv6 地址也可手动输入。");
    spinLocalPort = new QSpinBox; spinLocalPort->setObjectName("spinLocalPort"); spinLocalPort->setRange(0, 65535);
    spinLocalPort->setValue(qBound(0, settings.value("NetworkLocalPort", 0).toInt(), 65535));
    spinLocalPort->setSpecialValueText("0（自动分配）");
    spinLocalPort->setToolTip("客户端使用的本机源端口。默认 0，由系统自动分配；指定非零端口时必须未被占用。连接成功后下方显示实际本机 IP 和端口。");
    networkLayout->addRow("远端 IP", editIP); networkLayout->addRow("远端端口", spinPort);
    networkLayout->addRow("本机 IP", comboLocalIP); networkLayout->addRow("本机端口", spinLocalPort);
    ConnectionHints::row(networkLayout, editIP, "要连接的设备或服务器地址，可填写 IP 或主机名。");
    ConnectionHints::row(networkLayout, spinPort, "设备或服务器正在监听的 TCP 端口。");
    ConnectionHints::row(networkLayout, comboLocalIP, "电脑使用的本机地址；0.0.0.0 由系统选择出口网卡。");
    ConnectionHints::row(networkLayout, spinLocalPort, "电脑的本机源端口；0 表示由系统自动分配。");
    lblClientEndpoint = new QLabel; lblClientEndpoint->setObjectName("lblClientEndpoint"); lblClientEndpoint->setTextFormat(Qt::PlainText);
    lblClientEndpoint->setWordWrap(true); lblClientEndpoint->hide(); networkLayout->addRow(lblClientEndpoint);
    stackInterface->addWidget(wNetwork);

    // TCP Server page
    QWidget *wServer = new QWidget();
    wServer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFormLayout *serverLayout = new QFormLayout(wServer);
    serverLayout->setContentsMargins(0, 0, 0, 0);
    serverLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    serverLayout->setHorizontalSpacing(6);
    comboServerIP = addressChoice("comboServerIP", settings.value("ServerIP", "0.0.0.0").toString());
    comboServerIP->setToolTip("TCP 服务端的监听地址，可手动输入或下拉选择。0.0.0.0 监听所有本机 IPv4 地址；127.0.0.1 仅接受本机连接。指定地址必须属于当前电脑；IPv6 可输入 :: 或具体地址。");
    auto serverAddressRow = new QWidget; auto serverAddressLayout = new QVBoxLayout(serverAddressRow);
    serverAddressLayout->setContentsMargins(0, 0, 0, 0); serverAddressLayout->setSpacing(4);
    auto listenHint = new QLabel; listenHint->setObjectName("lblServerListenHint"); listenHint->setWordWrap(true); listenHint->setTextFormat(Qt::PlainText);
    serverAddressLayout->addWidget(comboServerIP); serverAddressLayout->addWidget(listenHint);
    auto updateListenHint = [this, listenHint] {
        const auto address = comboServerIP->currentText().trimmed();
        listenHint->setText(address == "0.0.0.0" ? "监听所有本机 IPv4 地址" : address == "::" ? "监听所有本机 IPv6 地址" : "仅监听指定的本机 IP，请确认此地址属于当前电脑。");
        QTimer::singleShot(0, this, &MainWindow::updateInterfaceStackHeight);
    };
    connect(comboServerIP, &QComboBox::currentTextChanged, this, updateListenHint); updateListenHint();
    spinServerPort = new QSpinBox();
    spinServerPort->setObjectName("spinServerPort");
    spinServerPort->setRange(1, 65535);
    spinServerPort->setValue(settings.value("ServerPort", 4970).toInt());
    serverLayout->addRow("监听 IP", serverAddressRow);
    serverLayout->addRow("监听端口", spinServerPort);
    
    lblServerConnCount = new QLabel("0");
    lblServerConnCount->setStyleSheet("color: #00ffcc; font-weight: bold;");
    lblServerCurrentConn = new QLabel("无");
    lblServerCurrentConn->setStyleSheet("color: #00aaff; font-weight: bold;");
    serverLayout->addRow("连接数量", lblServerConnCount);
    serverLayout->addRow("当前连接", lblServerCurrentConn);
    ConnectionHints::row(serverLayout, serverAddressRow, "电脑监听的本机地址；0.0.0.0 监听所有本机 IPv4 地址。");
    ConnectionHints::row(serverLayout, spinServerPort, "设备需连接到电脑的此 TCP 端口；须未被其他程序占用。");
    ConnectionHints::row(serverLayout, lblServerConnCount, "当前已连接的 TCP 客户端数量。");
    ConnectionHints::row(serverLayout, lblServerCurrentConn, "当前 TCP 客户端连接信息，无连接时显示“无”。");
    
    stackInterface->addWidget(wServer);
    stackInterface->addWidget(wDemo);
    stackInterface->addWidget(createBlePage());

    // Page ID 5 is stable; the dropdown displays UDP before BLE and Demo.
    auto wUdp = new QWidget; wUdp->setObjectName("udpSettingsPage");
    wUdp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto udpLayout = new QFormLayout(wUdp); udpLayout->setContentsMargins(0, 0, 0, 0);
    udpLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    udpLayout->setHorizontalSpacing(6); udpLayout->setVerticalSpacing(6);
    udpLayout->setFormAlignment(Qt::AlignTop);
    comboUdpLocalIP = addressChoice("comboUdpLocalIP", settings.value("Udp/LocalIP", "0.0.0.0").toString());
    comboUdpLocalIP->setToolTip("UDP 本机监听地址，0.0.0.0 监听所有本机 IPv4 地址；127.0.0.1 仅本机。可下拉选择本机 IP，IPv6 可手动输入 :: 或具体地址。地址必须属于本机；本版使用单播收发。");
    spinUdpLocalPort = new QSpinBox; spinUdpLocalPort->setObjectName("spinUdpLocalPort"); spinUdpLocalPort->setRange(0, 65535);
    spinUdpLocalPort->setSpecialValueText("0（自动分配）"); spinUdpLocalPort->setValue(settings.value("Udp/LocalPort", 4970).toInt());
    spinUdpLocalPort->setToolTip("设备应向本机此 UDP 端口发送数据，默认 4970；0 由系统自动分配，打开后显示实际端口。UDP 端口与 TCP 端口相互独立。");
    editUdpRemoteIP = new QLineEdit(settings.value("Udp/RemoteIP", "").toString()); editUdpRemoteIP->setObjectName("editUdpRemoteIP");
    editUdpRemoteIP->setMaxLength(128); editUdpRemoteIP->setPlaceholderText("仅接收可留空");
    editUdpRemoteIP->setToolTip("发送目标的单播 IP（不支持主机名、广播、组播）。发送时须与远端端口一起填写，不能填 0.0.0.0。仅接收可留空，并将远端端口设为 0。");
    spinUdpRemotePort = new QSpinBox; spinUdpRemotePort->setObjectName("spinUdpRemotePort"); spinUdpRemotePort->setRange(0, 65535);
    spinUdpRemotePort->setSpecialValueText("0（未设置）"); spinUdpRemotePort->setValue(settings.value("Udp/RemotePort", 0).toInt());
    spinUdpRemotePort->setToolTip("远端设备接收 UDP 数据的端口；填写目标 IP 时必须设置 1–65535。启用远端过滤后，这也是允许接收的远端源端口。");
    chkUdpFilterRemote = new QCheckBox("仅接收远端数据"); chkUdpFilterRemote->setObjectName("chkUdpFilterRemote");
    chkUdpFilterRemote->setChecked(settings.value("Udp/FilterRemote", true).toBool());
    chkUdpFilterRemote->setToolTip("填写远端 IP/端口后，只接收源 IP 和源端口均匹配的数据。未设置远端时不启用过滤，接收全部来源。请勿将多个设备的波形混在同一监听端口。");
    auto udpHint = new QLabel("0.0.0.0 监听所有本机 IPv4 地址。\n每包独立解析（需包含完整帧）；UDP 不保证到达和顺序。");
    udpHint->setWordWrap(true); udpHint->setTextFormat(Qt::PlainText);
    udpLayout->addRow("本机 IP", comboUdpLocalIP); udpLayout->addRow("本机端口", spinUdpLocalPort);
    udpLayout->addRow("远端 IP", editUdpRemoteIP); udpLayout->addRow("远端端口", spinUdpRemotePort);
    ConnectionHints::row(udpLayout, comboUdpLocalIP, "电脑接收 UDP 的本机地址；0.0.0.0 监听所有本机 IPv4 地址。");
    ConnectionHints::row(udpLayout, spinUdpLocalPort, "设备向电脑此端口发送 UDP 数据；0 表示自动分配。");
    ConnectionHints::row(udpLayout, editUdpRemoteIP, "发送目标的单播 IP；仅接收时可留空，不支持主机名。");
    ConnectionHints::row(udpLayout, spinUdpRemotePort, "远端设备接收 UDP 的端口；仅接收时可设为 0。");
    udpLayout->addRow(chkUdpFilterRemote); udpLayout->addRow(udpHint);
    lblUdpEndpoint = new QLabel; lblUdpEndpoint->setObjectName("lblUdpEndpoint"); lblUdpEndpoint->setTextFormat(Qt::PlainText);
    lblUdpEndpoint->setWordWrap(true); lblUdpEndpoint->hide(); udpLayout->addRow(lblUdpEndpoint);
    stackInterface->addWidget(wUdp);
    
    stackInterface->setObjectName("stackInterface");
    stackInterface->setCurrentIndex(comboInterface->currentData().toInt());
    stackInterface->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connLayout->addWidget(stackInterface);
    updateInterfaceStackHeight();

    btnOpenClose = new QPushButton(comboInterface->currentData().toInt() == 0 ? "🔌 打开串口" : "🔌 连接网络");
    btnOpenClose->setObjectName("btnOpenClose");
    btnOpenClose->setToolTip("按当前接口打开或关闭连接；TCP 服务端和 UDP 开始/停止监听，Demo 开始/停止模拟数据。");
    btnOpenClose->setStyleSheet("QPushButton { font-weight: bold; padding: 5px; background-color: #2a6a2a; color: #ccffcc; border: 1px solid #4a9a4a; border-radius: 4px; }"
                               "QPushButton:hover { background-color: #3a8a3a; }");
    connLayout->addWidget(btnOpenClose, 0, Qt::AlignTop);

    configLayout->addWidget(groupConnection);
    
    // 命令交互
    QGroupBox *groupCommand = new QGroupBox("设备交互");
    QVBoxLayout *cmdLayout = new QVBoxLayout(groupCommand);
    QPushButton *btnOpenCommandPanel = new QPushButton("🚀 命令交互面板");
    btnOpenCommandPanel->setStyleSheet("QPushButton { font-weight: bold; padding: 6px; background-color: #2a4a6a; color: #cceeff; border: 1px solid #4a7a9a; border-radius: 4px; } QPushButton:hover { background-color: #3a6a8a; }");
    cmdLayout->addWidget(btnOpenCommandPanel);
    configLayout->addWidget(groupCommand);
    
    connect(btnOpenCommandPanel, &QPushButton::clicked, this, &MainWindow::showCommandPanel);
    auto btnStatus=new QPushButton("状态监视面板");btnStatus->setObjectName("openStatusMonitor");
    btnStatus->setStyleSheet("QPushButton {font-weight:bold;padding:6px;background-color:#27634f;color:#e4fff3;border:1px solid #42866c;border-radius:4px;} QPushButton:hover {background-color:#347f65;}");
    btnStatus->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    btnStatus->setToolTip("根据通道数值或接收文本监视设备状态，支持映射、按位、阈值和文本反馈匹配。");
    groupCommand->layout()->addWidget(btnStatus);
    connect(btnStatus,&QPushButton::clicked,this,&MainWindow::showStatusMonitor);

    // 波形参数
    QGroupBox *groupPlot = new QGroupBox("波形参数");
    QFormLayout *plotLayout = new QFormLayout(groupPlot);
    
    spinFs = new QDoubleSpinBox();
    spinFs->setRange(1.0, 100000.0);
    spinFs->setValue(settings.value("Fs", 250.0).toDouble());
    spinFs->setSuffix(" Hz");
    
    lblDt = new QLabel();
    lblDt->setStyleSheet("color: #aaaaaa;");
    
    auto updateLblDt = [this]() {
        double fs = spinFs->value();
        double dt_ms = (fs > 0) ? (1000.0 / fs) : 0.0;
        lblDt->setText(QString("(Δt: %1 ms)").arg(dt_ms, 0, 'f', 2));
    };
    updateLblDt();
    connect(spinFs, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateLblDt);
    
    QHBoxLayout *fsLayout = new QHBoxLayout();
    fsLayout->addWidget(spinFs);
    fsLayout->addWidget(lblDt);
    
    plotLayout->addRow("采样率", fsLayout);
    
    lblDataRate = new QLabel("0 帧/秒");
    lblDataRate->setStyleSheet("color: #00ffcc; font-weight: bold;");
    plotLayout->addRow("实时速率", lblDataRate);
    
    configLayout->addWidget(groupPlot);
    
    // 数据存储
    QGroupBox *groupLog = new QGroupBox("数据存储");
    QFormLayout *logLayout = new QFormLayout(groupLog);
    
    QHBoxLayout *cacheDirLayout = new QHBoxLayout();
    editCacheDir = new QLineEdit();
    QString defaultPath = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/DataLogs");
    editCacheDir->setText(settings.value("CacheDir", defaultPath).toString());
    editCacheDir->setObjectName("editCacheDir");
    editCacheDir->setToolTip(QStringLiteral("缓存路径仅在本机保存，不随全局配置导入、导出。"));
    
    QPushButton *btnSelectCacheDir = new QPushButton("📁");
    btnSelectCacheDir->setObjectName("btnSelectCacheDir");
    connect(btnSelectCacheDir, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "选择缓存路径", editCacheDir->text());
        if (!dir.isEmpty()) {
            editCacheDir->setText(QDir::toNativeSeparators(dir));
            QSettings settings("MyCompany", "WaveLink");
            settings.setValue("CacheDir", editCacheDir->text());
        }
    });
    
    cacheDirLayout->addWidget(editCacheDir);
    cacheDirLayout->addWidget(btnSelectCacheDir);
    
    logLayout->addRow("缓存路径:", cacheDirLayout);
    
    connect(editCacheDir, &QLineEdit::textChanged, this, [](const QString &text){
        QSettings mainSettings("WaveLink", "WaveLink");
        mainSettings.setValue("editCacheDir", text);
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue("CacheDir", text);
    });

    QHBoxLayout *recordingModeLayout = new QHBoxLayout();
    comboRecordingMode = new QComboBox();
    comboRecordingMode->setObjectName("comboRecordingMode");
    comboRecordingMode->addItem(QStringLiteral("兼容模式（CSV）"));
    comboRecordingMode->addItem(QStringLiteral("高速模式（WLR）"));
    comboRecordingMode->setCurrentIndex(settings.value("RecordingMode", 0).toInt());
    comboRecordingMode->setToolTip(QStringLiteral("选择完整录制文件的保存格式"));
    QPushButton *btnRecordingModeHelp = new QPushButton("?");
    btnRecordingModeHelp->setFixedWidth(30);
    btnRecordingModeHelp->setToolTip(QStringLiteral("录制模式说明"));
    recordingModeLayout->addWidget(comboRecordingMode, 1);
    recordingModeLayout->addWidget(btnRecordingModeHelp);
    logLayout->addRow(QStringLiteral("录制模式:"), recordingModeLayout);
    connect(comboRecordingMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [](int index) {
        QSettings liveSettings("WaveLink", "AdvancedApp");
        liveSettings.setValue("RecordingMode", index);
        QSettings configSettings("WaveLink", "WaveLink");
        configSettings.setValue("comboRecordingMode", index);
    });
    connect(btnRecordingModeHelp, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(
            this, QStringLiteral("录制模式说明"),
            QStringLiteral(
                "兼容模式（CSV）\n"
                "• 以文本形式保存，可直接使用 Excel、记事本、MATLAB 或 Python 打开。\n"
                "• 通用性最好，但高采样率、多通道录制时文件更大、CPU 开销更高。\n\n"
                "高速模式（WLR）\n"
                "• 使用 WaveLink 二进制格式直接保存时间戳和双精度通道数据。\n"
                "• 文件更小、写入转换开销更低，适合高采样率和长时间录制。\n"
                "• 可直接导入 WaveLink；点击“保存数据”可转换并合并为 CSV。\n\n"
                "录制格式与数值类型相互独立：默认保存原始通道数值；自定义协议可在通道表上方选择保存换算后数值（含系数、偏移及超限 NaN）。两者都不含界面数字滤波，不是原始报文字节抓包。\n"
                "都支持 1 GB 自动分卷、写入失败保护和异常退出恢复。"));
    });
    
    QHBoxLayout *limitLayout = new QHBoxLayout();
    spinCacheLimit = new QSpinBox();
    spinCacheLimit->setRange(10, 10000);
    spinCacheLimit->setValue(settings.value("CacheLimit", 500).toInt());
    spinCacheLimit->setSuffix(" MB");
    btnClearCache = new QPushButton("清空缓存");
    btnClearCache->setObjectName("btnClearCache");
    btnClearCache->setToolTip("确认后删除 WaveLink_Cache 文件夹内的磁盘录制缓存，释放磁盘空间；无法撤销。不会清空当前波形或绘图内存，请先另存重要录制。");
    limitLayout->addWidget(spinCacheLimit);
    limitLayout->addWidget(btnClearCache);
    QPushButton *btnLogHelp = new QPushButton("?");
    btnLogHelp->setFixedWidth(30);
    btnLogHelp->setToolTip(QString::fromUtf8("数据存储使用帮助"));
    connect(btnLogHelp, &QPushButton::clicked, this, [this](){
        QDialog helpDialog(this);
        helpDialog.setWindowTitle(QString::fromUtf8("数据存储说明"));
        helpDialog.setWindowFlags(Qt::Dialog | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
        helpDialog.setMinimumSize(520, 360);
        helpDialog.setSizeGripEnabled(true);
        QVBoxLayout *helpLayout = new QVBoxLayout(&helpDialog);
        helpLayout->setContentsMargins(14, 14, 14, 14);
        helpLayout->setSpacing(10);

        QTextBrowser *helpText = new QTextBrowser(&helpDialog);
        helpText->setOpenExternalLinks(false);
        helpText->setLineWrapMode(QTextEdit::WidgetWidth);
        helpText->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        helpText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        helpText->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        QString html = QString::fromUtf8(R"(
<h3 style='color: #00ffcc;'>数据存储功能使用说明</h3>
<p style='font-size: 14px; line-height: 1.6;'>
<b>1. 缓存路径：</b> 录制文件保存在该路径下的 <code>WaveLink_Cache</code> 文件夹；导出当前波形时，默认从该路径选择文件名。<br><br>
<b>2. 录制模式：</b> CSV 兼容模式便于其他软件直接打开；WLR 高速模式以二进制保存，适合高采样率、多通道和长时间录制。两种格式都可以重新导入，WLR 也可通过“保存数据”转换为 CSV。<br><br>
<b>3. 磁盘缓存上限：</b> 用于检查 <code>WaveLink_Cache</code> 中的录制文件大小，也作为录制磁盘的最低安全剩余空间，不是绘图内存上限。<br>缓存超限后不会自动删除；下次开始录制时，可选择保留文件并继续、清空文件并继续，或打开文件夹自行处理。只有磁盘剩余空间低于该值时才会弹出空间不足提示并停止录制。<br><br>
<b>4. 录前清空：</b> 勾选后，开始录制时只会清空当前显示的波形，不会删除磁盘缓存文件。<br><br>
<b>5. 开始录制：</b> 连接成功后才能录制。CSV/WLR 默认保存通过协议验证的原始通道数值；自定义协议可在通道表上方的“录制保存数据类型”中选择换算后数值（含系数、偏移及超限 NaN），两者都不包含界面数字滤波，不是原始报文字节抓包。该选择随协议保存，仅影响后续录制。“保存数据”合并/转换文件时保留已有值，不再次换算。数据持续写入磁盘，不受绘图限流和波形历史清理影响。单个文件达到 1 GB 后自动生成下一分卷；录制中的临时文件带有 <code>.part</code> 后缀，程序异常退出后会在下次启动时尝试恢复完整数据。<br><br>
<b>6. 导出当前波形：</b> 保存当前仍在绘图历史中的显示结果，包含已应用的通道缩放、偏移及滤波结果，不能代替原始数据的完整录制。<br><br>
<b>7. 导入数据：</b> 可将 CSV 或 WLR 文件重新加载，用于波形回放、滤波或分析。直接展示文件中保存的原始值或换算值，不自动套用当前协议缩放，不修改已有文件。<br><br>
<b>8. 清空缓存：</b> 弹窗说明路径、数量及影响，只有点击“确认清空”才删除所列文件，取消或关闭不清理。用于释放磁盘空间，缓存中的录制、分卷和未恢复临时文件将被删除，无法撤销；不清空当前波形、不删除外部另存文件、不递归清理子文件夹。请先另存重要录制。
</p>
)");
        helpText->setHtml(html);
        helpLayout->addWidget(helpText, 1);

        QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &helpDialog);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        connect(buttons, &QDialogButtonBox::rejected, &helpDialog, &QDialog::reject);
        helpLayout->addWidget(buttons);

        const bool darkTheme = !comboTheme || comboTheme->currentIndex() == 0;
        if (darkTheme) {
            helpDialog.setStyleSheet(
                "QDialog { background-color: #1e1e1e; color: #dddddd; }"
                "QTextBrowser { background-color: #252526; color: #dddddd; border: 1px solid #555555; padding: 12px; font-size: 14px; }"
                "QPushButton { padding: 6px 20px; }");
        } else {
            helpDialog.setStyleSheet(
                "QDialog { background-color: #f0f0f0; color: #333333; }"
                "QTextBrowser { background-color: #ffffff; color: #333333; border: 1px solid #cccccc; padding: 12px; font-size: 14px; }"
                "QPushButton { padding: 6px 20px; }");
        }

        QSettings helpSettings("WaveLink", "AdvancedApp");
        const QByteArray savedGeometry = helpSettings.value("StorageHelpGeometry").toByteArray();
        if (savedGeometry.isEmpty() || !helpDialog.restoreGeometry(savedGeometry)) {
            helpDialog.resize(760, 580);
        }
        helpDialog.exec();
        helpSettings.setValue("StorageHelpGeometry", helpDialog.saveGeometry());
    });
    limitLayout->addWidget(btnLogHelp);
    logLayout->addRow("缓存上限", limitLayout);
    
    // Row 1: Record button + clear-before-record checkbox
    QHBoxLayout *recordRowLayout = new QHBoxLayout();
    btnToggleRecord = new QPushButton("🔴 开始录制");
    btnToggleRecord->setObjectName("btnToggleRecord");
    chkClearBeforeRecord = new QCheckBox("录前清空");
    chkClearBeforeRecord->setChecked(true);
    chkClearBeforeRecord->setToolTip("开始录制时先清空波形显示，使界面与录制数据保持一致");
    recordRowLayout->addWidget(btnToggleRecord);
    recordRowLayout->addWidget(chkClearBeforeRecord);
    logLayout->addRow("录制", recordRowLayout);
    
    // Row 2: Export button + Import button
    QHBoxLayout *dataRowLayout = new QHBoxLayout();
    btnExportData = new QPushButton("💾 保存数据");
    btnImportData = new QPushButton("📂 导入数据");
    dataRowLayout->addWidget(btnExportData);
    dataRowLayout->addWidget(btnImportData);
    logLayout->addRow("数据", dataRowLayout);
    
    lblRecordStatus = new QLabel("00:00:00 | 0.00 MB");
    lblRecordStatus->setObjectName("lblRecordStatus");
    logLayout->addRow("状态", lblRecordStatus);
    
    configLayout->addWidget(groupLog);
    
    configLayout->addStretch();
    
    QScrollArea *configScrollArea = new QScrollArea();
    configScrollArea->setWidgetResizable(true);
    configScrollArea->setFrameShape(QFrame::NoFrame);
    configScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    configScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    configScrollArea->setWidget(configWidget);
    configDock->setMinimumWidth(360);
    configDock->setWidget(configScrollArea);
    addDockWidget(Qt::LeftDockWidgetArea, configDock);
    
    // Setup Settings Savers
    connect(editIP, &QLineEdit::textChanged, this, [](const QString &address) { QSettings("WaveLink", "AdvancedApp").setValue("NetworkIP", address.trimmed()); });
    connect(spinPort, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int port) { QSettings("WaveLink", "AdvancedApp").setValue("NetworkPort", port); });
    connect(comboLocalIP, &QComboBox::currentTextChanged, this, [](const QString &address) { QSettings("WaveLink", "AdvancedApp").setValue("NetworkLocalIP", address.trimmed()); });
    connect(spinLocalPort, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int port) { QSettings("WaveLink", "AdvancedApp").setValue("NetworkLocalPort", port); });
    connect(comboUdpLocalIP, &QComboBox::currentTextChanged, this, [](const QString &ip) { QSettings("WaveLink", "AdvancedApp").setValue("Udp/LocalIP", ip.trimmed()); });
    connect(spinUdpLocalPort, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int port) { QSettings("WaveLink", "AdvancedApp").setValue("Udp/LocalPort", port); });
    connect(editUdpRemoteIP, &QLineEdit::textChanged, this, [](const QString &ip) { QSettings("WaveLink", "AdvancedApp").setValue("Udp/RemoteIP", ip.trimmed()); });
    connect(spinUdpRemotePort, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int port) { QSettings("WaveLink", "AdvancedApp").setValue("Udp/RemotePort", port); });
    connect(chkUdpFilterRemote, &QCheckBox::toggled, this, [](bool filter) { QSettings("WaveLink", "AdvancedApp").setValue("Udp/FilterRemote", filter); });
    connect(comboServerIP, &QComboBox::currentTextChanged, this, [](const QString &address) { QSettings("WaveLink", "AdvancedApp").setValue("ServerIP", address.trimmed()); });
    connect(spinServerPort, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int port) { QSettings("WaveLink", "AdvancedApp").setValue("ServerPort", port); });
    // Protocol selection is persisted only after the worker acknowledges it.
    connect(comboPorts, &QComboBox::currentTextChanged, this, [this](const QString &text){
        if (!text.isEmpty()) {
            QSettings settings("WaveLink", "AdvancedApp");
            settings.setValue("Port", text);
            
            QString purePortName = text.split(' ').first();
            QString btName = getBluetoothNameForComPort(purePortName);
            if (!btName.isEmpty()) {
                lblBluetoothName->setText(QString::fromUtf8("蓝牙设备: ") + btName);
                lblBluetoothName->show();
            } else {
                lblBluetoothName->hide();
            }
        } else {
            lblBluetoothName->hide();
        }
        // Word-wrapped Bluetooth details can change the serial page height.
        // Recalculate after show/hide has reached the layout system.
        QTimer::singleShot(0, this, [this]() { updateInterfaceStackHeight(); });
    });
    connect(comboBaudRate, &QComboBox::currentTextChanged, this, [](const QString &text){
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue("BaudRate", text);
    });
    connect(comboDataBits, &QComboBox::currentTextChanged, this, [](const QString &text){
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue("DataBits", text);
    });
    connect(comboParity, &QComboBox::currentTextChanged, this, [](const QString &text){
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue("Parity", text);
    });
    connect(comboStopBits, &QComboBox::currentTextChanged, this, [](const QString &text){
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue("StopBits", text);
    });
    connect(chkDtr, &QCheckBox::toggled, this, &MainWindow::onSerialControlLinesChanged);
    connect(chkRts, &QCheckBox::toggled, this, &MainWindow::onSerialControlLinesChanged);
    connect(comboFlowControl, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        QSettings("WaveLink", "WaveLink").setValue("Serial/FlowControl", comboFlowControl->currentData());
        updateSerialFlowControls();
        updateInterfaceStackHeight();
    });
    connect(btnCancelSerialSend, &QPushButton::clicked, this, &MainWindow::cancelSerialSend);
    connect(chkSerialBreak, &QCheckBox::toggled, this, [this](bool enabled) { requestSerialBreak(enabled); });
    connect(btnSerialBreak, &QPushButton::clicked, this, [this]() {
        requestSerialBreak(!m_serialBreakActive, m_serialBreakActive ? 0 : spinSerialBreakMs->value());
    });
    connect(spinSerialBreakMs, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int duration) {
        QSettings("WaveLink", "WaveLink").setValue("Serial/BreakDurationMs", duration);
    });
    connect(spinFs, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value){
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue("Fs", value);
        // Changing a setting is not permission to rewrite existing timestamps.
        // New live samples use the new interval; the elapsed timeline remains
        // continuous. Retune filters/analysis without rescaling stored history.
        resetFilters();
        onSpectrumTimer();
    });
    connect(spinCacheLimit, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int value){
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue("CacheLimit", value);
    });
    
    // --- Bottom Dock: Console ---
    consoleDock = new QDockWidget("数据收发", this);
    consoleDock->setObjectName("ConsoleDock");
    consoleDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    consoleDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    
    QWidget *consoleWidget = new QWidget();
    QVBoxLayout *consoleLayout = new QVBoxLayout(consoleWidget);
    
        // RX Toolbar for console
    QHBoxLayout *rxToolbarLayout = new QHBoxLayout();
    chkShowReceiveData = new QCheckBox("显示接收数据");
    chkShowReceiveData->setChecked(true); // Default to showing data
    chkHexReceive = new QCheckBox("HEX 接收");
    chkHexReceive->setObjectName("chkHexReceive");
    btnClearReceive = new QPushButton("清空接收");
    btnClearReceive->setObjectName("btnClearReceive");
    btnSaveReceive = new QPushButton("保存接收");
    btnSaveReceive->setObjectName("btnSaveReceive");
    btnSaveReceive->setToolTip("将当前接收文本框内容保存为 UTF-8 TXT 文件，保留中文、HEX 和时间戳显示。仅保存当前显示内容，不是持续录制全部接收数据。");
    connect(btnSaveReceive, &QPushButton::clicked, this, &MainWindow::onSaveReceive);
    btnCopyReceive = new QPushButton("复制接收");
    btnCopyReceive->setObjectName("btnCopyReceive");
    btnCopyReceive->setToolTip("复制当前接收文本框的全部内容，保留中文、HEX、时间戳和换行；不影响接收内容或当前选区。");
    connect(btnCopyReceive, &QPushButton::clicked, this, &MainWindow::onCopyReceive);
    
    connect(chkHexReceive, &QCheckBox::clicked, this, [this](bool checked) {
        flushReceiveText(); // Convert pending display text together with the visible history.
        QString text = textReceive->toPlainText();
        if (text.isEmpty()) return;
        
        QTextCodec *codec = QTextCodec::codecForName(comboEncoding->currentText().toUtf8());
        if (!codec) codec = QTextCodec::codecForName("UTF-8");

        if (checked) {
            // Text to HEX
            QByteArray bytes = codec->fromUnicode(text);
            QString hexStr = bytes.toHex().toUpper();
            QString formatted;
            for (int i = 0; i < hexStr.length(); i += 2) {
                formatted += hexStr.mid(i, 2) + " ";
            }
            formatted = formatted.trimmed() + " ";
            ReceiveConsoleBuffer::trimPending(formatted, true, m_systemPreferences.receiveLines, m_systemPreferences.receiveCharacters);
            textReceive->setPlainText(formatted);
        } else {
            // HEX to Text
            QString hexStr = text.remove(" ").remove("\n").remove("\r");
            QByteArray bytes = QByteArray::fromHex(hexStr.toUtf8());
            auto decoded = codec->toUnicode(bytes);
            ReceiveConsoleBuffer::trimPending(decoded, false, m_systemPreferences.receiveLines, m_systemPreferences.receiveCharacters);
            textReceive->setPlainText(decoded);
        }
        
        QTextCursor cursor = textReceive->textCursor();
        cursor.movePosition(QTextCursor::End);
        textReceive->setTextCursor(cursor);
        trimReceiveText();
    });
    
    comboEncoding = new QComboBox();
    comboEncoding->addItems({"UTF-8", "GBK"});

    lblRxCount = new QLabel("Rx: 0");
    lblTxCount = new QLabel("Tx: 0");
    btnClearCounts = new QPushButton("清零计数");

    chkTimestamp = new QCheckBox("时间戳显示");
    chkTimestamp->setChecked(false);
    rxToolbarLayout->addWidget(chkShowReceiveData);
    rxToolbarLayout->addWidget(chkHexReceive);
    rxToolbarLayout->addWidget(chkTimestamp);
    rxToolbarLayout->addWidget(comboEncoding);
    rxToolbarLayout->addWidget(btnClearReceive);
    rxToolbarLayout->addStretch();
    rxToolbarLayout->addWidget(lblRxCount);
    rxToolbarLayout->addWidget(lblTxCount);
    rxToolbarLayout->addWidget(btnClearCounts);

    connect(btnClearCounts, &QPushButton::clicked, this, [this]() {
        if (comboInterface->currentData().toInt() == 4 && bleClient) {
            bleClient->resetCounters();
            lblRxCount->setText("Rx: 0"); lblTxCount->setText("Tx: 0");
            return;
        }
        if (serialHandler) {
            QMetaObject::invokeMethod(serialHandler, "resetCounters", Qt::QueuedConnection);
            lblRxCount->setText("Rx: 0");
            lblTxCount->setText("Tx: 0");
        }
        if (networkHandler) networkHandler->resetCounters();
    });

    // Text areas
    textReceive = new QPlainTextEdit();
    textReceive->setObjectName("textReceive");
    textReceive->setReadOnly(true); // Plain-text layout avoids rich-text console overhead.
    textReceive->document()->setMaximumBlockCount(ReceiveConsoleBuffer::DefaultLines);
    textReceive->setUndoRedoEnabled(false);
    // Keep the console compact; the splitter can still be enlarged manually.
    textReceive->setMinimumHeight(40);
    auto rxHistoryLayout = new QHBoxLayout;
    rxHistoryLayout->addWidget(btnSaveReceive);
    rxHistoryLayout->addWidget(btnCopyReceive);
    auto receiveCacheLabel = new QLabel("显示缓存");
    receiveCacheLabel->setObjectName("receiveCacheLabel");
    rxHistoryLayout->addWidget(receiveCacheLabel);
    comboReceiveLines = new QComboBox;
    comboReceiveLines->setObjectName("comboReceiveLines");
    comboReceiveLines->addItem("1000 行（默认）", 1000);
    comboReceiveLines->addItem("2000 行", 2000);
    comboReceiveLines->addItem("5000 行", 5000);
    comboReceiveLines->setCurrentIndex(0);
    comboReceiveLines->setToolTip("默认保留最近 1000 行、5 万字符。达到任一上限时，仅移除最旧内容，不整框清空。行数按换行分段计算，自动折行不额外计数。仅影响显示及保存/复制接收，不影响协议解析、波形或录制；完整数据请使用录制功能。字符上限和刷新间隔可在“设置 → 系统设置 → 数据收发”中调整。");
    receiveCacheLabel->setToolTip(comboReceiveLines->toolTip());
    rxHistoryLayout->addWidget(comboReceiveLines);
    auto receiveSettings = new QPushButton(QStringLiteral("接收设置")); receiveSettings->setObjectName("btnReceiveSettings");
    receiveSettings->setToolTip(QStringLiteral("打开系统设置，调整接收缓存行数、字符上限和显示刷新间隔。"));
    connect(receiveSettings, &QPushButton::clicked, this, &MainWindow::showSystemSettings); rxHistoryLayout->addWidget(receiveSettings);
    rxHistoryLayout->addStretch();
    connect(comboReceiveLines, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        const int lines = ReceiveConsoleBuffer::validLines(comboReceiveLines->currentData().toInt());
        m_systemPreferences.receiveLines = lines;
        applySystemPreferences(m_systemPreferences, true);
    });

    // TX Toolbar for console
    QHBoxLayout *txToolbarLayout = new QHBoxLayout();
    
    chkHexSend = new QCheckBox("HEX 发送");
    chkHexSend->setObjectName("chkHexSend");
    
    comboAppendNewline = new QComboBox();
    comboAppendNewline->setObjectName("comboAppendNewline");
    comboAppendNewline->addItems({"无追加", "发送追加 \\r\\n", "发送追加 \\n\\r", "发送追加 \\r", "发送追加 \\n"});
    comboAppendNewline->setMinimumWidth(120);
    comboAppendNewline->setCurrentIndex(1); // Default to \r\n
    
    btnClearSend = new QPushButton("清空发送");
    btnClearSend->setObjectName("btnClearSend");
    chkConsolePeriodicSend = new QCheckBox("定时发送");
    chkConsolePeriodicSend->setObjectName("chkConsolePeriodicSend");
    spinConsoleSendInterval = new QSpinBox;
    spinConsoleSendInterval->setObjectName("spinConsoleSendInterval");
    spinConsoleSendInterval->setRange(1, 3600000);
    spinConsoleSendInterval->setValue(1000);
    spinConsoleSendInterval->setSuffix(" ms");
    spinConsoleSendInterval->setKeyboardTracking(false);
    spinConsoleSendInterval->setToolTip("定时发送周期，1–3600000 ms，默认 1000 ms。运行时修改周期会重新计时；周期自动保存，但下次启动不会自动开启发送。普通系统定时器不保证精确毫秒或设备实际到达时间。");
    consoleSendTimer = new QTimer(this); consoleSendTimer->setObjectName("consoleSendTimer");
    consoleSendTimer->setTimerType(Qt::PreciseTimer);
    connect(consoleSendTimer, &QTimer::timeout, this, &MainWindow::onConsolePeriodicSendTimer);
    connect(chkConsolePeriodicSend, &QCheckBox::toggled, this, &MainWindow::onConsolePeriodicSendToggled);
    connect(spinConsoleSendInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int interval) {
        QSettings("WaveLink", "WaveLink").setValue("Console/SendIntervalMs", interval);
        if (consoleSendTimer->isActive()) consoleSendTimer->start(interval);
    });
    
    textSend = new QTextEdit();
    textSend->setObjectName("textSend");
    // Commands are plain text; pasted HTML must not carry fonts/backgrounds.
    textSend->setAcceptRichText(false);
    textSend->setMinimumHeight(40);
    connect(btnClearSend, &QPushButton::clicked, textSend, &QTextEdit::clear);

    txToolbarLayout->addWidget(chkHexSend);
    txToolbarLayout->addWidget(comboAppendNewline);
    txToolbarLayout->addWidget(chkConsolePeriodicSend);
    txToolbarLayout->addWidget(spinConsoleSendInterval);
    txToolbarLayout->addWidget(btnClearSend);
    txToolbarLayout->addStretch();

    QHBoxLayout *sendLayout = new QHBoxLayout();
    
    connect(textSend, &QTextEdit::textChanged, this, [this]() {
        if (chkHexSend && chkHexSend->isChecked()) {
            QString text = textSend->toPlainText();
            QString original = text;
            text = text.remove(" ");
            QString newText;
            for (QChar c : text) {
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                    newText += c.toUpper();
                }
            }
            QString formatted;
            for (int i = 0; i < newText.length(); i++) {
                formatted += newText[i];
                if ((i % 2) == 1 && i != newText.length() - 1) {
                    formatted += " ";
                }
            }
            if (formatted != original) {
                textSend->blockSignals(true);
                int cursor = textSend->textCursor().position();
                int origLen = original.length();
                textSend->setPlainText(formatted);
                QTextCursor tc = textSend->textCursor();
                if (cursor >= origLen) {
                    tc.movePosition(QTextCursor::End);
                } else {
                    tc.setPosition(qMin(cursor, formatted.length()));
                }
                textSend->setTextCursor(tc);
                textSend->blockSignals(false);
            }
        }
    });

    connect(textSend, &QTextEdit::textChanged, this, [this] {
        if (textSend->toPlainText().isEmpty()) stopConsolePeriodicSend("发送内容为空，定时发送已停止。");
    });
    btnSend = new QPushButton("发送");
    btnSend->setObjectName(QStringLiteral("btnSend"));
    btnSend->setMinimumWidth(96); // Logical pixels: scales with display DPI.
    btnSend->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);

    sendLayout->addWidget(textSend, 1);
    sendLayout->addWidget(btnSend);

    // --- File Send UI ---
    QHBoxLayout *fileSendLayout = new QHBoxLayout();
    txtSendFilePath = new QLineEdit();
    txtSendFilePath->setObjectName("txtSendFilePath");
    txtSendFilePath->setPlaceholderText("请选择要发送的文件...");
    
    btnOpenSendFile = new QPushButton("打开文件");
    btnSendFile = new QPushButton("发送文件");
    btnSendFile->setObjectName(QStringLiteral("btnSendFile"));
    btnStopSendFile = new QPushButton("停止发送");
    btnStopSendFile->setObjectName("btnStopSendFile");
    
    progressBarFile = new QProgressBar();
    progressBarFile->setObjectName("progressBarFile");
    progressBarFile->setRange(0, 100);
    progressBarFile->setValue(0);
    progressBarFile->setTextVisible(true);
    progressBarFile->setAlignment(Qt::AlignCenter);
    progressBarFile->installEventFilter(this);
    setFileSendProgressHint();
    lblFileSendEta = new QLabel(QStringLiteral("00:00:00"));
    lblFileSendEta->setObjectName("lblFileSendEta");
    lblFileSendEta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lblFileSendEta->setMinimumWidth(lblFileSendEta->fontMetrics().horizontalAdvance(QStringLiteral("000:00:00")) + 4);
    lblFileSendEta->setAccessibleName(QStringLiteral("文件发送预计剩余时间"));
    lblFileSendEta->setToolTip(QStringLiteral("预计剩余时间（时:分:秒），按最近已写出/提交的字节速度估算；没有有效速度或长时间等待时显示 --:--:--。停止、完成或断开后显示 00:00:00；不代表接收设备已确认文件完整。"));
    
    btnOpenSendFile->setToolTip(QStringLiteral("选择任意类型文件，包括图片、文档和压缩包。"));
    btnSendFile->setToolTip(QStringLiteral("按原始字节发送整个文件，不转换文本/HEX、不附加换行，也不发送文件名或自动添加文件传输协议；接收端须自行处理文件边界、校验和保存。"));
    btnStopSendFile->setToolTip(QString::fromUtf8("停止发送"));
    
    fileSendLayout->addWidget(txtSendFilePath, 1);
    fileSendLayout->addWidget(btnOpenSendFile);
    fileSendLayout->addWidget(btnSendFile);
    fileSendLayout->addWidget(btnStopSendFile);
    fileSendLayout->addWidget(progressBarFile);
    fileSendLayout->addWidget(lblFileSendEta);

    fileSendTimer = new QTimer(this);
    fileSendTimer->setInterval(20);
    fileToSend = new QFile(this);
    fileSendEtaTimer = new QTimer(this); fileSendEtaTimer->setObjectName("fileSendEtaTimer");
    fileSendEtaTimer->setInterval(1000);
    connect(fileSendEtaTimer, &QTimer::timeout, this, &MainWindow::refreshFileSendEta);
    
    connect(btnOpenSendFile, &QPushButton::clicked, this, &MainWindow::onOpenSendFile);
    connect(btnSendFile, &QPushButton::clicked, this, &MainWindow::onStartSendFile);
    connect(btnStopSendFile, &QPushButton::clicked, this, &MainWindow::onStopSendFile);
    connect(fileSendTimer, &QTimer::timeout, this, &MainWindow::onFileSendTimer);
    // --------------------

    connect(chkHexSend, &QCheckBox::clicked, this, [this](bool checked) {
        textSend->blockSignals(true);
        QString text = textSend->toPlainText();
        if (checked) {
            // Text to HEX
            QByteArray utf8Text = text.toUtf8();
            QString hexStr = utf8Text.toHex().toUpper();
            QString formatted;
            for (int i = 0; i < hexStr.length(); i += 2) {
                formatted += hexStr.mid(i, 2) + " ";
            }
            textSend->setPlainText(formatted.trimmed());
        } else {
            // HEX to Text
            QString hexStr = text.remove(" ").remove("\n").remove("\r");
            QByteArray bytes = QByteArray::fromHex(hexStr.toUtf8());
            textSend->setPlainText(QString::fromUtf8(bytes));
        }
        textSend->blockSignals(false);
    });

    consoleSplitter = new QSplitter(Qt::Vertical);
    consoleSplitter->setObjectName("ConsoleRxTxSplitter");
    
    QWidget *rxWidget = new QWidget();
    QVBoxLayout *rxLayout = new QVBoxLayout(rxWidget);
    rxLayout->setContentsMargins(0, 0, 0, 0);
    rxLayout->addLayout(rxToolbarLayout);
    rxLayout->addLayout(rxHistoryLayout);
    rxLayout->addWidget(textReceive);
    
    QWidget *txWidget = new QWidget();
    QVBoxLayout *txLayout = new QVBoxLayout(txWidget);
    txLayout->setContentsMargins(0, 0, 0, 0);
    txLayout->addLayout(txToolbarLayout);
    txLayout->addLayout(sendLayout);
    txLayout->addLayout(fileSendLayout);

    consoleSplitter->addWidget(rxWidget);
    consoleSplitter->addWidget(txWidget);
    consoleSplitter->setStretchFactor(0, 1);
    consoleSplitter->setStretchFactor(1, 1);
    
    consoleLayout->addWidget(consoleSplitter);
    
    consoleDock->setWidget(consoleWidget);
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);

    // --- Bottom Dock: Spectrum ---
    spectrumDock = new QDockWidget("频谱分析", this);
    spectrumDock->setObjectName("SpectrumDock");
    spectrumDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    spectrumDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    
    QWidget *spectrumWidget = new QWidget();
    QVBoxLayout *spectrumLayout = new QVBoxLayout(spectrumWidget);
    spectrumLayout->setSpacing(2);
    
    QHBoxLayout *spectrumControlLayout = new QHBoxLayout();
    spectrumControlLayout->setContentsMargins(0, 0, 0, 0);
    spectrumControlLayout->setSpacing(6);
    QLabel *lblSpectrumChannel = new QLabel("分析通道:");
    
    btnSpectrumChannels = new QToolButton();
    btnSpectrumChannels->setText("选择通道...");
    btnSpectrumChannels->setPopupMode(QToolButton::InstantPopup);
    btnSpectrumChannels->setObjectName("btnSpectrumChannels");
    menuSpectrumChannels = new QMenu(btnSpectrumChannels);
    btnSpectrumChannels->setMenu(menuSpectrumChannels);
    connect(menuSpectrumChannels, &QMenu::triggered, this, &MainWindow::updateSpectrumChannelMenu);
    
    QLabel *lblSampleRate = new QLabel("采样率 (Hz):");
    comboSampleRate = new QComboBox();
    comboSampleRate->setEditable(true);
    comboSampleRate->addItems({"Auto", "100", "500", "1000", "2000", "5000", "10000"});
    
    QLabel *lblFftPoints = new QLabel("FFT点数:");
    comboFftPoints = new QComboBox();
    comboFftPoints->addItems({"256", "512", "1024", "2048", "4096"});
    comboFftPoints->setCurrentText("256");
    
    chkRemoveDC = new QCheckBox("去除 0Hz");
    chkRemoveDC->setChecked(true);
    connect(chkRemoveDC, &QCheckBox::stateChanged, this, [this](int){ onSpectrumTimer(); });
    
    chkSpectrumAutoScale = new QCheckBox("自适应");
    chkSpectrumAutoScale->setObjectName("chkSpectrumAutoScale");
    chkSpectrumAutoScale->setChecked(true);
    
    chkSpectrumFixX = new QCheckBox("固定X轴:");
    spinSpectrumXMin = new QDoubleSpinBox();
    spinSpectrumXMin->setRange(0, 1e6);
    spinSpectrumXMin->setDecimals(1);
    spinSpectrumXMin->setValue(0);
    
    QLabel *lblSpectrumTo = new QLabel("-");
    
    spinSpectrumXMax = new QDoubleSpinBox();
    spinSpectrumXMax->setRange(0, 1e6);
    spinSpectrumXMax->setDecimals(1);
    spinSpectrumXMax->setValue(100);

    connect(chkSpectrumFixX, &QCheckBox::toggled, this, [this](bool) {
        onSpectrumTimer();
    });
    connect(spinSpectrumXMin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { onSpectrumTimer(); });
    connect(spinSpectrumXMax, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { onSpectrumTimer(); });
    
    QHBoxLayout *fixedXConfigLayout = new QHBoxLayout();
    fixedXConfigLayout->setContentsMargins(0, 0, 0, 0);
    fixedXConfigLayout->setSpacing(6);
    fixedXConfigLayout->addWidget(chkSpectrumFixX);
    fixedXConfigLayout->addWidget(spinSpectrumXMin);
    fixedXConfigLayout->addWidget(lblSpectrumTo);
    fixedXConfigLayout->addWidget(spinSpectrumXMax);

    spectrumControlLayout->addWidget(lblSpectrumChannel);
    spectrumControlLayout->addWidget(btnSpectrumChannels);
    spectrumControlLayout->addWidget(lblSampleRate);
    spectrumControlLayout->addWidget(comboSampleRate);
    spectrumControlLayout->addWidget(lblFftPoints);
    spectrumControlLayout->addWidget(comboFftPoints);
    spectrumControlLayout->addWidget(chkRemoveDC);
    spectrumControlLayout->addWidget(chkSpectrumAutoScale);
    spectrumControlLayout->addStretch();


    
    spectrumPlot = new QCustomPlot();
    spectrumPlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    spectrumPlot->installEventFilter(this);
    int themeIdx = settings.value("comboTheme", 0).toInt();
    ThemeManager::setupPlotTheme(spectrumPlot, static_cast<ThemeManager::Theme>(themeIdx));
    spectrumPlot->xAxis->setLabel("");
    spectrumPlot->yAxis->setLabel("幅度");
    spectrumPlot->xAxis->grid()->setPen(QPen(QColor("#333333"), 1, Qt::DashLine));
    spectrumPlot->yAxis->grid()->setPen(QPen(QColor("#333333"), 1, Qt::DashLine));
    
    spectrumPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    spectrumPlot->axisRect()->setRangeZoom(Qt::Horizontal);
    spectrumPlot->installEventFilter(this);
    
    spectrumTracer = new QCPItemTracer(spectrumPlot);
    spectrumTracer->setPen(QPen(Qt::white, 1, Qt::DashLine));
    spectrumTracer->setStyle(QCPItemTracer::tsCrosshair);
    spectrumTracer->setVisible(false);
    
    spectrumTracerText = new QCPItemText(spectrumPlot);
    spectrumTracerText->setColor(Qt::white);
    spectrumTracerText->setBrush(QBrush(QColor(0, 0, 0, 150)));
    spectrumTracerText->setPadding(QMargins(4, 4, 4, 4));
    spectrumTracerText->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    spectrumTracerText->position->setParentAnchor(spectrumTracer->position);
    spectrumTracerText->position->setCoords(0, -10);
    spectrumTracerText->setVisible(false);
    
    spectrumWarningText = new QCPItemText(spectrumPlot);
    spectrumWarningText->setColor(Qt::red);
    spectrumWarningText->setBrush(QBrush(QColor(0, 0, 0, 150)));
    spectrumWarningText->setPadding(QMargins(10, 10, 10, 10));
    spectrumWarningText->setPositionAlignment(Qt::AlignCenter);
    spectrumWarningText->position->setType(QCPItemPosition::ptAxisRectRatio);
    spectrumWarningText->position->setCoords(0.5, 0.5);
    spectrumWarningText->setFont(QFont("Arial", 12, QFont::Bold));
    spectrumWarningText->setVisible(false);
    
    hpfCutoffLine = new QCPItemLine(spectrumPlot);
    hpfCutoffLine->setPen(QPen(QColor("#00aaff"), 1.5, Qt::DashLine));
    hpfCutoffLine->setVisible(false);

    lpfCutoffLine = new QCPItemLine(spectrumPlot);
    lpfCutoffLine->setPen(QPen(QColor("#ffaa00"), 1.5, Qt::DashLine));
    lpfCutoffLine->setVisible(false);

    notchCutoffLine = new QCPItemLine(spectrumPlot);
    notchCutoffLine->setPen(QPen(QColor("#ff0055"), 1.5, Qt::DashLine));
    notchCutoffLine->setVisible(false);

    spectrumLegendText = new QCPItemText(spectrumPlot);
    spectrumLegendText->setColor(QColor("#ffffff"));
    spectrumLegendText->setBrush(QBrush(QColor(18, 18, 18, 200)));
    spectrumLegendText->setPen(QPen(QColor("#444444"), 1));
    spectrumLegendText->setPadding(QMargins(6, 4, 6, 4));
    spectrumLegendText->setPositionAlignment(Qt::AlignRight | Qt::AlignTop);
    spectrumLegendText->position->setType(QCPItemPosition::ptAxisRectRatio);
    spectrumLegendText->position->setCoords(0.98, 0.02);
    spectrumLegendText->setFont(QFont("Arial", 9, QFont::Bold));
    spectrumLegendText->setVisible(false);
    
    spectrumTracerFixed = false;

    spectrumXAxisLabel = new QLabel("频率 (Hz)");
    spectrumXAxisLabel->setAlignment(Qt::AlignCenter);
    spectrumXAxisLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    spectrumXAxisLabel->setStyleSheet("color: #aaaaaa; padding: 0 4px;");

    spectrumFooter = new QWidget();
    spectrumFooter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    spectrumFooter->setFixedHeight(32);
    spectrumFooterLayout = new QHBoxLayout(spectrumFooter);
    spectrumFooterLayout->setContentsMargins(0, 0, 0, 0);
    spectrumFooterLayout->setSpacing(6);
    spectrumFooterLayout->addLayout(fixedXConfigLayout);
    spectrumFooterLayout->addStretch(1);
    spectrumFooterLayout->addWidget(spectrumXAxisLabel);
    spectrumFooterLayout->addStretch(1);
    spectrumFooter->installEventFilter(this);
    
    connect(spectrumPlot, &QCustomPlot::mouseMove, this, [this](QMouseEvent *event) {
        if (!spectrumTracerFixed && spectrumGraphs.size() > 0) {
            double x = spectrumPlot->xAxis->pixelToCoord(event->pos().x());
            // Find closest active graph
            QCPGraph *bestGraph = nullptr;
            for (QCPGraph *g : spectrumGraphs) {
                if (g && g->dataCount() > 0 && g->visible()) {
                    bestGraph = g;
                    break;
                }
            }
            if (bestGraph) {
                spectrumTracer->setGraph(bestGraph);
                spectrumTracer->setGraphKey(x);
                spectrumTracer->setVisible(true);
                spectrumTracerText->setVisible(true);
                double freq = spectrumTracer->position->key();
                double mag = spectrumTracer->position->value();
                
                double yUpper = spectrumPlot->yAxis->range().upper;
                double yLower = spectrumPlot->yAxis->range().lower;
                if (mag > yLower + (yUpper - yLower) * 0.7) {
                    spectrumTracerText->setPositionAlignment(Qt::AlignTop | Qt::AlignHCenter);
                    spectrumTracerText->position->setCoords(0, 15);
                } else {
                    spectrumTracerText->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
                    spectrumTracerText->position->setCoords(0, -10);
                }
                
                spectrumTracerText->setText(QString("Freq: %1 Hz\nMag: %2").arg(freq, 0, 'f', 1).arg(mag, 0, 'f', 2));
                spectrumPlot->replot(QCustomPlot::rpQueuedReplot);
            }
        }
    });
    
    connect(spectrumPlot, &QCustomPlot::mouseDoubleClick, this, [this](QMouseEvent *event) {
        Q_UNUSED(event);
        spectrumTracerFixed = !spectrumTracerFixed;
        if (!spectrumTracerFixed) {
            spectrumTracer->setVisible(false);
            spectrumTracerText->setVisible(false);
            spectrumPlot->replot(QCustomPlot::rpQueuedReplot);
        }
    });
    connect(chkSpectrumAutoScale, &QCheckBox::clicked, this, [this](bool checked){
        if (checked) onSpectrumTimer();
    });

    
    
    spectrumLayout->addLayout(spectrumControlLayout, 0);
    spectrumLayout->addWidget(spectrumPlot, 1);
    spectrumLayout->addWidget(spectrumFooter, 0);
    spectrumDock->setWidget(spectrumWidget);

    
    // --- Right Dock: Channels ---
    channelDock = new QDockWidget("数据通道", this);
    channelDock->setObjectName("ChannelDock");
    channelDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    channelDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    channelDock->setMinimumWidth(360);
    
    QWidget *channelWidget = new QWidget();
    QVBoxLayout *channelLayout = new QVBoxLayout(channelWidget);

    // This is deliberately separate from the recording-folder cache limit.
    QHBoxLayout *historyLimitLayout = new QHBoxLayout();
    historyLimitLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *lblHistoryLimit = new QLabel(QStringLiteral("实时历史上限:"));
    spinPlotHistoryLimit = new QSpinBox();
    spinPlotHistoryLimit->setObjectName("spinPlotHistoryLimit");
    spinPlotHistoryLimit->setRange(16, 2048);
    spinPlotHistoryLimit->setValue(settings.value("PlotHistoryLimitMB", 64).toInt());
    spinPlotHistoryLimit->setSuffix(" MB");
    // Do not apply the limit after every digit typed. Applying it can scan and
    // trim millions of plot points, which interrupts manual editing. The typed
    // value is committed when Enter is pressed or the editor loses focus;
    // arrow-button changes still take effect immediately.
    spinPlotHistoryLimit->setKeyboardTracking(false);
    spinPlotHistoryLimit->setFocusPolicy(Qt::StrongFocus);
    spinPlotHistoryLimit->setMinimumWidth(100);
    spinPlotHistoryLimit->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    spinPlotHistoryLimit->setToolTip(QStringLiteral("仅限制实时采集时主视图和独立视图共同保留的历史数据。离线导入完整保留，不受此设置裁剪；离线时调整该值会用于下次实时采集。"));
    lblPlotHistoryUsage = new QLabel(
        QStringLiteral("当前已使用：0.00 MB / %1 MB").arg(spinPlotHistoryLimit->value()));
    lblPlotHistoryUsage->setObjectName("lblPlotHistoryUsage");
    lblPlotHistoryUsage->setToolTip(QStringLiteral("当前所有绘图曲线的数据与绘图加速索引的估算总量，未包含导入原始数据副本和界面等其他内存。离线导入完整保留，不受实时历史上限裁剪。"));
    // The usage value changes frequently. Keep it inside the available row
    // width so text updates never change the dock's layout or minimum width.
    lblPlotHistoryUsage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    lblPlotHistoryUsage->setMinimumWidth(0);
    QPushButton *btnHistoryLimitHelp = new QPushButton("?");
    btnHistoryLimitHelp->setFixedWidth(30);
    btnHistoryLimitHelp->setToolTip(QStringLiteral("波形历史上限说明"));
    historyLimitLayout->addWidget(lblHistoryLimit);
    historyLimitLayout->addWidget(spinPlotHistoryLimit);
    historyLimitLayout->addStretch();
    historyLimitLayout->addWidget(btnHistoryLimitHelp);
    QHBoxLayout *historyUsageLayout = new QHBoxLayout();
    historyUsageLayout->setContentsMargins(0, 0, 0, 0);
    historyUsageLayout->addWidget(lblPlotHistoryUsage, 1);
    connect(spinPlotHistoryLimit, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        QSettings liveSettings("WaveLink", "AdvancedApp");
        liveSettings.setValue("PlotHistoryLimitMB", value);
        QSettings configSettings("WaveLink", "WaveLink");
        configSettings.setValue("spinPlotHistoryLimit", value);
        enforcePlotHistoryLimit(true);
    });
    connect(btnHistoryLimitHelp, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(
            this, QStringLiteral("波形历史上限"),
            QStringLiteral(
                "此数值仅限制实时采集时主视图和独立视图共同保留的波形历史数据量。超出后会清理最旧的实时显示数据。\n\n"
                "离线导入成功后完整保留全部采样点，并默认展示完整时间范围。调整此上限、滤波或分离视图均不会裁剪导入的波形。\n\n"
                "离线完整加载上限在“系统设置 → 绘图显示”设置，默认 256 MB，可设置 16–4096 MB；同时保留可用内存保护。超过额度自动建立磁盘缓存并按需浏览，概览保留峰谷，放大后读取原始采样点；测量和导出也读取原始数据。大文件索引和滤波需时间及缓存磁盘空间，关闭数据后清理临时缓存。\n\n"
                "该设置不影响正在写入的完整录制文件，也不是数据存储面板中的磁盘缓存上限。\n\n"
                "右侧“已用”包含曲线数据和绘图加速索引的估算值；绘图库、导入数据副本和界面本身还会额外占用内存。\n\n"
                "实时历史上限默认 64 MB。设置较小时，可回看和导出的实时波形时长会缩短。"));
    });
    
    chkSelectAllChannels = new QCheckBox("全选/ 取消全选");
    chkSelectAllChannels->setChecked(true);
    
    chkPausePlot = new QCheckBox("暂停绘图");
    
    QLabel *lblDecimals = new QLabel("Y轴小数位数:");
    spinDecimals = new QSpinBox();
    spinDecimals->setRange(0, 9);
    spinDecimals->setValue(2);
    QHBoxLayout *decimalLayout = new QHBoxLayout();
    decimalLayout->addWidget(lblDecimals);
    decimalLayout->addWidget(spinDecimals);
    connect(spinDecimals, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val){
        for(PlotContext &ctx : allPlots) {
            ctx.plot->yAxis->setNumberPrecision(val);
            if (ctx.heightText) {
                ctx.heightText->setText(QString::fromUtf8("Y跨度: %1").arg(ctx.plot->yAxis->range().size(), 0, 'f', val));
            }
            ctx.plot->replot();
        }
    });

    chkAutoScaleY = new QCheckBox("Y轴自适应");
    chkAutoScaleY->setChecked(true);
    
    connect(chkAutoScaleY, &QCheckBox::toggled, this, [this](bool checked) {
        for (PlotContext &ctx : allPlots) {
            if (checked) {
                ctx.plot->axisRect()->setRangeDrag(Qt::Horizontal);
            } else {
                ctx.plot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
            }
        }
        if (checked) {
            autoScaleY();
        }
    });
    
    connect(chkPausePlot, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) {
            for (PlotContext &ctx : allPlots) {
                if (ctx.measureState != 0) {
                    ctx.tracer1->setVisible(false);
                    ctx.tracer2->setVisible(false);
                    ctx.measureLine->setVisible(false);
                    ctx.measureText->setVisible(false);
                    ctx.measureText2->setVisible(false);
                    if (ctx.measureDeltaText) ctx.measureDeltaText->setVisible(false);
                    ctx.measureState = 0;
                    ctx.plot->replot();
                }
            }
        }
    });
    
    listChannels = new QListWidget();
    listChannels->setObjectName("listChannels");
    listChannels->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(listChannels, &QListWidget::customContextMenuRequested, this, &MainWindow::onChannelContextMenu);
    connect(listChannels, &QListWidget::itemClicked, this, [](QListWidgetItem *item){
        QWidget *w = item->listWidget()->itemWidget(item);
        if (w) {
            QCheckBox *chk = w->findChild<QCheckBox*>("chkChannel");
            if (chk) chk->toggle();
        }
    });
    
    btnMergeAllViews = new QPushButton(QString::fromUtf8("📑 一键合并所有视图"));
    btnMergeAllViews->setObjectName("btnMergeAllViews");
    connect(btnMergeAllViews, &QPushButton::clicked, this, &MainWindow::onMergeAllViews);
    
    btnClearPlots = new QPushButton(QString::fromUtf8("🗑️ 清空波形"));
    btnClearPlots->setObjectName("btnClearPlots");
    connect(btnClearPlots, &QPushButton::clicked, this, &MainWindow::onRequestClearPlots);
    
    chkShowGrid = new QCheckBox("背景网格");
    chkShowGrid->setChecked(true);
    connect(chkShowGrid, &QCheckBox::toggled, this, [this](bool checked) {
        for (PlotContext &ctx : allPlots) {
            ctx.plot->xAxis->grid()->setVisible(checked);
            ctx.plot->yAxis->grid()->setVisible(checked);
            ctx.plot->replot();
        }
    });

    QHBoxLayout *chkTopLayout = new QHBoxLayout();
    chkTopLayout->addWidget(chkAutoScaleY);
    chkTopLayout->addWidget(chkPausePlot);
    chkTopLayout->addWidget(chkShowGrid);
    chkTopLayout->addStretch();
    
    channelLayout->addLayout(historyLimitLayout);
    channelLayout->addLayout(historyUsageLayout);
    channelLayout->addLayout(chkTopLayout);
    channelLayout->addLayout(decimalLayout);
    btnScrollToLatest = new QPushButton("跳转至最新时刻");
    btnScrollToLatest->setObjectName("btnScrollToLatest");
    btnScrollToLatest->setCursor(Qt::PointingHandCursor);
    btnScrollToLatest->setStyleSheet(
        "QPushButton {"
        "   background-color: #2b78e4;"
        "   color: white;"
        "   border: none;"
        "   border-radius: 4px;"
        "   padding: 6px 12px;"
        "   font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "   background-color: #3b8bf4;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #1a5cba;"
        "}"
    );
    connect(btnScrollToLatest, &QPushButton::clicked, this, &MainWindow::scrollToLatest);
    channelLayout->addWidget(btnScrollToLatest);
    channelLayout->addWidget(chkSelectAllChannels);
    channelLayout->addWidget(listChannels);
    
    btnExportCache = new QPushButton(QString::fromUtf8("📤 导出当前波形"));
    QPushButton *btnExportHelp = new QPushButton("?");
    btnExportHelp->setFixedWidth(30);
    btnExportHelp->setToolTip(QString::fromUtf8("该功能会将目前屏幕上所有通道的波形数据（包括滤波后的结果）保存为 CSV 文件。"));
    QHBoxLayout *exportLayout = new QHBoxLayout();
    exportLayout->addWidget(btnExportCache);
    exportLayout->addWidget(btnExportHelp);
    connect(btnExportCache, &QPushButton::clicked, this, &MainWindow::onExportCache);
    connect(btnExportHelp, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, QString::fromUtf8("保存波形帮助"), 
            QString::fromUtf8("作用：将当前窗口中存在的所有通道的波形数据保存到本地 CSV 文件中。\n\n"
            "说明：\n"
            "1. 如果您开启了数字滤波，保存的将是滤波后的纯净数据。\n"
            "2. 如果早期波形已经被绘图历史内存额度清理，则只会保存当前仍保留的波形。\n"
            "3. 时间轴(Time)将以当前波形的相对时间（秒）进行保存。"));
    });
    channelLayout->addLayout(exportLayout);
    channelLayout->addWidget(btnMergeAllViews);

    channelLayout->addWidget(btnClearPlots);
    channelWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    
    // --- Right Dock: Filter ---
    filterDock = new QDockWidget("数字滤波", this);
    filterDock->setObjectName("FilterDock");
    filterDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    filterDock->setVisible(false);
    
    filterDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    
    filterWidget = new QWidget();
    QVBoxLayout *filterLayout = new QVBoxLayout(filterWidget);
    
    // Notch
    QHBoxLayout *notchLayout = new QHBoxLayout();
    chkNotch = new QCheckBox("陷波（工频干扰）");
    spinNotchFreq = new QDoubleSpinBox();
    spinNotchFreq->setRange(1.0, 1000.0);
    spinNotchFreq->setValue(50.0);
    spinNotchFreq->setSuffix(" Hz");
    notchLayout->addWidget(chkNotch);
    notchLayout->addWidget(spinNotchFreq);
    filterLayout->addLayout(notchLayout);
    
    // High Pass
    QHBoxLayout *hpfLayout = new QHBoxLayout();
    chkHPF = new QCheckBox("高通（基线漂移）");
    spinHPFFreq = new QDoubleSpinBox();
    spinHPFFreq->setRange(0.01, 1000.0);
    spinHPFFreq->setValue(0.1);
    spinHPFFreq->setSuffix(" Hz");
    hpfLayout->addWidget(chkHPF);
    hpfLayout->addWidget(spinHPFFreq);
    filterLayout->addLayout(hpfLayout);
    
    // Low Pass
    QHBoxLayout *lpfLayout = new QHBoxLayout();
    chkLPF = new QCheckBox("低通（高频噪声）");
    spinLPFFreq = new QDoubleSpinBox();
    spinLPFFreq->setRange(1.0, 10000.0);
    spinLPFFreq->setValue(10.0);
    spinLPFFreq->setSuffix(" Hz");
    lpfLayout->addWidget(chkLPF);
    lpfLayout->addWidget(spinLPFFreq);
    filterLayout->addLayout(lpfLayout);
    
    // Order
    QHBoxLayout *orderLayout = new QHBoxLayout();
    orderLayout->addWidget(new QLabel("滤波器阶数:"));
    comboFilterOrder = new QComboBox();
    comboFilterOrder->addItems({"2阶", "4阶"});
    orderLayout->addWidget(comboFilterOrder);
    filterLayout->addLayout(orderLayout);
    
    filterLayout->addStretch();
    filterWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // Combine both right-side panels into one scrollable content area.
    filterSection = new QWidget();
    QVBoxLayout *filterSectionLayout = new QVBoxLayout(filterSection);
    filterSectionLayout->setContentsMargins(0, 8, 0, 0);
    filterSectionLayout->setSpacing(4);

    QHBoxLayout *filterHeaderLayout = new QHBoxLayout();
    filterHeaderLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *filterHeaderLabel = new QLabel(QString::fromUtf8("数字滤波"));
    filterHeaderLabel->setStyleSheet(
        "QLabel { font-size: 16px; font-weight: bold; padding: 2px 0; }");
    QToolButton *filterHeaderButton = new QToolButton();
    filterHeaderButton->setText(">>");
    filterHeaderButton->setAutoRaise(true);
    filterHeaderButton->setToolTip(QString::fromUtf8("展开或收起数字滤波"));
    filterHeaderLayout->addWidget(filterHeaderLabel);
    filterHeaderLayout->addStretch();
    filterHeaderLayout->addWidget(filterHeaderButton);
    filterSectionLayout->addLayout(filterHeaderLayout);
    filterSectionLayout->addWidget(filterWidget);

    rightSharedContent = new QWidget();
    QVBoxLayout *rightSharedLayout = new QVBoxLayout(rightSharedContent);
    rightSharedLayout->setContentsMargins(0, 0, 0, 0);
    rightSharedLayout->setSpacing(0);
    rightSharedLayout->addWidget(channelWidget);
    rightSharedLayout->addWidget(filterSection);
    rightSharedLayout->addStretch();
    rightSharedContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    rightSharedScrollArea = new QScrollArea(channelDock);
    rightSharedScrollArea->setObjectName("RightSharedScrollArea");
    rightSharedScrollArea->setWidgetResizable(true);
    rightSharedScrollArea->setFrameShape(QFrame::NoFrame);
    rightSharedScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    rightSharedScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rightSharedScrollArea->setWidget(rightSharedContent);
    channelDock->setWidget(rightSharedScrollArea);
    updateChannelDockMinimumWidth();
    addDockWidget(Qt::RightDockWidgetArea, channelDock);

    filterPanelAction = new QAction(QString::fromUtf8("数字滤波(&F)"), this);
    filterPanelAction->setCheckable(true);
    filterPanelAction->setChecked(true);
    filterPanelAction->setIconText(QString::fromUtf8("数字滤波"));
    connect(filterPanelAction, &QAction::toggled, this, [this](bool checked) {
        if (filterSection) {
            filterSection->setVisible(checked);
        }
        if (rightSharedContent) {
            rightSharedContent->adjustSize();
            rightSharedContent->updateGeometry();
        }
    });
    connect(filterHeaderButton, &QToolButton::clicked, this, [this, filterHeaderButton]() {
        const bool expanded = filterWidget && filterWidget->isVisible();
        if (filterWidget) {
            filterWidget->setVisible(!expanded);
        }
        filterHeaderButton->setText(expanded ? "<<" : ">>");
        if (filterPanelAction) {
            QSignalBlocker blocker(filterPanelAction);
            filterPanelAction->setChecked(!expanded);
        }
        if (rightSharedContent) {
            rightSharedContent->adjustSize();
            rightSharedContent->updateGeometry();
        }
    });
    
    // --- Bottom Dock: Measure ---
    measureDock = new QDockWidget("信号分析", this);
    measureDock->setObjectName("MeasureDock");
    measureDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    measureDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    QWidget *measureWidget = new QWidget();
    QVBoxLayout *measureLayout = new QVBoxLayout(measureWidget);
    
    QHBoxLayout *measureChLayout = new QHBoxLayout();
    measureChLayout->addWidget(new QLabel("测量通道:"));
    comboMeasureChannel = new QComboBox();
    measureChLayout->addWidget(comboMeasureChannel);
    measureLayout->addLayout(measureChLayout);
    
    QHBoxLayout *measureTypeLayout = new QHBoxLayout();
    measureTypeLayout->addWidget(new QLabel("信号类型:"));
    comboSignalType = new QComboBox();
    comboSignalType->addItems({"General", "50Hz", "ECG", "EEG", "EMG", "EOG", "PPG", "CBF"});
    measureTypeLayout->addWidget(comboSignalType);
    measureLayout->addLayout(measureTypeLayout);
    signalTypeHint = new QLabel();
    signalTypeHint->setWordWrap(true);
    signalTypeHint->setStyleSheet(QStringLiteral("color: #999999; font-size: 11px; padding-left: 2px;"));
    measureLayout->addWidget(signalTypeHint);
    
    measureMetricsLayout = new QVBoxLayout();
    measureLayout->addLayout(measureMetricsLayout);
    measureLayout->addStretch();
    
    connect(comboSignalType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::updateMeasureUI);
    
    measureWatcher = new QFutureWatcher<PhysioResult>(this);
    connect(measureWatcher, &QFutureWatcher<PhysioResult>::finished, this, &MainWindow::onMeasureFinished);
    
    updateMeasureUI(); // Initialize UI
    
    measureDock->setWidget(measureWidget);
    
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
    addDockWidget(Qt::BottomDockWidgetArea, spectrumDock);
    addDockWidget(Qt::BottomDockWidgetArea, measureDock);
    auto updateFilters = [this](){ this->resetFilters(); };
    connect(chkNotch, &QCheckBox::toggled, this, updateFilters);
    connect(spinNotchFreq, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateFilters);
    connect(chkHPF, &QCheckBox::toggled, this, updateFilters);
    connect(spinHPFFreq, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateFilters);
    connect(chkLPF, &QCheckBox::toggled, this, updateFilters);
    connect(spinLPFFreq, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateFilters);
    connect(comboFilterOrder, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateFilters);
    QToolBar *mainToolBar = addToolBar("视图控制");
    mainToolBar->setMovable(false);
    mainToolBar->setObjectName("mainToolBar");
    configDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    consoleDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    channelDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    
    // Enable native tabbing and nested docks
    setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    setDockNestingEnabled(true);
    
    applyCustomTitleBar(configDock, "参数设置", ">>", "<<");
    applyCustomTitleBar(consoleDock, "数据收发", "^", "v");
    applyCustomTitleBar(spectrumDock, "频谱分析", "^", "v");
    applyCustomTitleBar(measureDock, "信号分析", "^", "v");
    applyCustomTitleBar(channelDock, "数据通道", "<<", ">>");
    
    // Connect to topLevelChanged for native window bar
    QList<QDockWidget*> allDocks = { configDock, consoleDock, spectrumDock, measureDock, channelDock };
    for (QDockWidget *dock : allDocks) {
        // Connect to topLevelChanged for native window bar
        connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::onDockTopLevelChanged);
        connect(dock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea){ updateDockButtons(); });
        connect(dock, &QDockWidget::visibilityChanged, this, [this](bool){ updateDockButtons(); });
        
        // Set object names for state restoration
        if (dock == configDock) dock->setObjectName("DockConfig");
        else if (dock == consoleDock) dock->setObjectName("DockConsole");
        else if (dock == spectrumDock) dock->setObjectName("DockSpectrum");
        else if (dock == measureDock) dock->setObjectName("DockMeasure");
        else if (dock == channelDock) dock->setObjectName("DockChannel");
    }
    
    mainToolBar->addAction(configDock->toggleViewAction());
    mainToolBar->addAction(channelDock->toggleViewAction());

    measureDock->toggleViewAction()->setText("信号分析");
    connect(measureDock->toggleViewAction(), &QAction::toggled, this, [=](bool checked){
        if (checked && measureDock->widget() && measureDock->widget()->isHidden()) {
            measureDock->widget()->setMinimumHeight(0);
            measureDock->widget()->setMinimumWidth(0);
            measureDock->widget()->show();
        }
    });
    
    mainToolBar->addAction(filterPanelAction);
    mainToolBar->addAction(consoleDock->toggleViewAction());
    mainToolBar->addAction(spectrumDock->toggleViewAction());
    mainToolBar->addAction(measureDock->toggleViewAction());
    
    // Add stretch and quick layout controls
    QWidget *spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mainToolBar->addWidget(spacer);

    btnDataTransceiveMode = new QToolButton(this);
    btnDataTransceiveMode->setObjectName("btnDataTransceiveMode");
    btnDataTransceiveMode->setText(QStringLiteral("数据收发模式"));
    btnDataTransceiveMode->setToolTip(
        QStringLiteral("仅显示参数设置和数据收发面板；再次点击恢复之前的布局"));
    btnDataTransceiveMode->setCheckable(true);
    btnDataTransceiveMode->setIcon(layoutActionIcon(false));
    btnDataTransceiveMode->setIconSize(QSize(18, 18));
    btnDataTransceiveMode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mainToolBar->addWidget(btnDataTransceiveMode);
    connect(btnDataTransceiveMode, &QToolButton::toggled, this,
            [this](bool checked) { setDataTransceiveMode(checked); });

    QToolButton *btnRestoreLayout = new QToolButton(this);
    btnRestoreLayout->setObjectName("btnRestoreDefaultLayout");
    btnRestoreLayout->setText(QStringLiteral("恢复默认布局"));
    btnRestoreLayout->setToolTip(QStringLiteral("恢复所有窗口和面板的默认布局"));
    btnRestoreLayout->setIcon(layoutActionIcon(true));
    btnRestoreLayout->setIconSize(QSize(18, 18));
    btnRestoreLayout->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mainToolBar->addWidget(btnRestoreLayout);
    connect(btnRestoreLayout, &QToolButton::clicked, this, &MainWindow::restoreDefaultLayout);
    
    comboTheme = new QComboBox(this);comboTheme->setObjectName("comboTheme");
    comboTheme->addItem(QString::fromUtf8("深色科技 (暗黑)"), ThemeManager::DarkTheme);
    comboTheme->addItem(QString::fromUtf8("明亮简约 (浅白)"), ThemeManager::LightTheme);
    comboTheme->setFixedWidth(200);
    comboTheme->setToolTip(QString::fromUtf8("设置系统主题"));
    mainToolBar->addWidget(comboTheme);
    connect(comboTheme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        ThemeManager::Theme theme = static_cast<ThemeManager::Theme>(comboTheme->itemData(index).toInt());
        ThemeManager::applyTheme(qApp, theme);
        ThemeManager::setupPlotTheme(spectrumPlot, theme);
        updateWaveformPlotThemes();
        spectrumPlot->replot();
        if (cmdWindow) {
            cmdWindow->applyTheme(index == 0);
        }
        if(m_statusWindow)m_statusWindow->applyTheme(index==0);
    });
    QToolButton *btnPin = new QToolButton(this);
    btnPin->setText("📌");
    btnPin->setToolTip("窗口置顶 (未置顶)");
    btnPin->setCheckable(true);
    btnPin->setStyleSheet("QToolButton { background-color: transparent; border: 1px solid transparent; border-radius: 3px; padding: 4px 6px; font-size: 16px; margin-left: 8px; } QToolButton:hover { background-color: rgba(100, 100, 100, 0.3); border: 1px solid #777; } QToolButton:checked { background-color: rgba(255, 80, 80, 0.3); border: 1px solid #ff5555; }");
    connect(btnPin, &QToolButton::toggled, this, [this, btnPin](bool checked) {
        setWindowFlag(Qt::WindowStaysOnTopHint, checked);
        btnPin->setText(checked ? QString::fromUtf8("取消置顶") : QString::fromUtf8("窗口置顶"));
        show();
    });
    mainToolBar->addWidget(btnPin);
    
    QToolButton *btnScreenshot = new QToolButton(this);
    btnScreenshot->setText(QString::fromUtf8("📷"));
    btnScreenshot->setToolTip(QString::fromUtf8("保存当前主窗口界面截图"));
    btnScreenshot->setStyleSheet("QToolButton { background-color: transparent; border: 1px solid transparent; border-radius: 3px; padding: 0px 6px 8px 6px; font-size: 16px; margin-left: 8px; } QToolButton:hover { background-color: rgba(100, 100, 100, 0.3); border: 1px solid #777; }");
    connect(btnScreenshot, &QToolButton::clicked, this, [this](){
        QPixmap pixmap = this->grab();
        QSettings settings("WaveLink", "AdvancedApp");
        QString lastDir = settings.value("LastDataDir", QDir::homePath()).toString();
        QString defaultName = QDir(lastDir).filePath(QString("WaveLink_Shot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
        QString fileName = QFileDialog::getSaveFileName(this, QString::fromUtf8("保存截图"), defaultName, "Images (*.png *.jpg)");
        if (!fileName.isEmpty()) {
            settings.setValue("LastDataDir", QFileInfo(fileName).absolutePath());
            pixmap.save(fileName);
            statusBar()->showMessage(QString::fromUtf8("截图已保存: ") + fileName, 3000);
        }
    });
    mainToolBar->addWidget(btnScreenshot);

    QToolButton *btnHelp = new QToolButton(this);
    btnHelp->setText("❓");
    btnHelp->setToolTip("帮助与系统诊断中心（使用说明、更新日志、程序调试）");
    btnHelp->setStyleSheet("QToolButton { background-color: #0e639c; color: white; border-radius: 3px; padding: 4px 10px; font-weight: bold; margin-left: 8px; } QToolButton:hover { background-color: #1177bb; }");
    connect(btnHelp, &QToolButton::clicked, this, [this](){
        HelpDialog dlg(!comboTheme || comboTheme->currentIndex() == 0, this);
        dlg.exec();
    });
    mainToolBar->addWidget(btnHelp);
    
    // Ensure native behaviors when floating were handled previously

    // Save the default state for restoration BEFORE user settings corrupt it
    // Initial/default width only; loadConfig restores a user's saved dock sizes.
    resizeDocks({configDock}, {400}, Qt::Horizontal);
    // Set the initial bottom-row height before capturing the default state.
    resizeDocks({consoleDock}, {defaultBottomDockHeight()}, Qt::Vertical);
    m_defaultState = this->saveState();
    m_defaultInnerState = innerMainWindow->saveState();
    
    // --- Connections ---
    // --- Connections ---
    connect(comboProtocol, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::handleProtocolChanged);
    connect(comboEndianness, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        Q_UNUSED(index)
        handleProtocolChanged(comboProtocol->currentIndex());
    });
    connect(comboInterface, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onInterfaceChanged);
    connect(btnOpenClose, &QPushButton::clicked, this, &MainWindow::onOpenCloseConnection);
    connect(btnClearReceive, &QPushButton::clicked, this, &MainWindow::onClearReceive);
    connect(btnSend, &QPushButton::clicked, this, &MainWindow::onSend);
    connect(btnToggleRecord, &QPushButton::clicked, this, &MainWindow::onToggleRecord);
    connect(btnExportData, &QPushButton::clicked, this, &MainWindow::onExportData);
    connect(btnImportData, &QPushButton::clicked, this, &MainWindow::onImportData);
    connect(btnClearCache, &QPushButton::clicked, this, &MainWindow::onClearCache);
    connect(chkSelectAllChannels, &QCheckBox::stateChanged, this, &MainWindow::onSelectAllChannelsStateChanged);
    connect(listChannels, &QListWidget::itemChanged, this, &MainWindow::onChannelItemChanged);
}
