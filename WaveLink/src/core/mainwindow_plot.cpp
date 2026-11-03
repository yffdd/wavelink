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
#include <QDialog>
#include <QDialogButtonBox>
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
#include "waveformgraph.h"
#include "plotmeasurementcontroller.h"
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
#include <QPointer>
#include <QStatusBar>
#include <QMenuBar>
#include <QToolBar>
#include <QStyle>

namespace {
// Use QCP's own padded text bounds, in logical pixels, after axis layout.
// The caller supplies the tracer anchor again on every render so clamping
// never changes the measured data point or accumulates after zoom/resize.
void placePlotValueLabel(QCPItemText *label, const QPointF &anchor, Qt::Alignment preferred, QVector<QRectF> &occupied)
{
    if (!label || !label->visible() || !qIsFinite(anchor.x()) || !qIsFinite(anchor.y())) return;
    auto plot = label->parentPlot();
    QRectF bounds(plot->axisRect()->rect().intersected(plot->viewport()));
    if (bounds.isEmpty()) return;

    label->position->setType(QCPItemPosition::ptAbsolute);
    auto align = preferred;
    auto position = [&]() {
        const double dx = align.testFlag(Qt::AlignLeft) ? 8 : align.testFlag(Qt::AlignRight) ? -8 : 0;
        const double dy = align.testFlag(Qt::AlignBottom) ? -8 : align.testFlag(Qt::AlignTop) ? 8 : 0;
        label->setPositionAlignment(align);
        label->position->setPixelPosition(anchor + QPointF(dx, dy));
        return QRectF(label->topLeft->pixelPosition(), label->bottomRight->pixelPosition());
    };
    QRectF rect = position();
    // Small split views may fit the text but not the usual extra gutter.
    const double marginX = qMin(5.0, qMax(0.0, (bounds.width() - rect.width()) * 0.5));
    const double marginY = qMin(5.0, qMax(0.0, (bounds.height() - rect.height()) * 0.5));
    bounds.adjust(marginX, marginY, -marginX, -marginY);
    if (align.testFlag(Qt::AlignBottom) && rect.top() < bounds.top()) {
        align = (align & ~Qt::AlignBottom) | Qt::AlignTop;
        rect = position();
    }
    if (align.testFlag(Qt::AlignLeft) && rect.right() > bounds.right()) {
        align = (align & ~Qt::AlignLeft) | Qt::AlignRight;
        rect = position();
    }
    const double left = qBound(bounds.left(), rect.left(), qMax(bounds.left(), bounds.right() - rect.width()));
    const double top = qBound(bounds.top(), rect.top(), qMax(bounds.top(), bounds.bottom() - rect.height()));
    label->position->setPixelPosition(label->position->pixelPosition() + QPointF(left - rect.left(), top - rect.top()));
    rect = QRectF(label->topLeft->pixelPosition(), label->bottomRight->pixelPosition());
    // Prefer nearby free space when point/difference labels or the span
    // readouts would cover each other, especially in short split views.
    QVector<double> xs{rect.left(), bounds.left(), bounds.right() - rect.width()};
    QVector<double> ys{rect.top(), bounds.top(), bounds.bottom() - rect.height()};
    for (const auto &other : occupied) {
        xs << other.left() - rect.width() - 4 << other.right() + 4;
        ys << other.top() - rect.height() - 4 << other.bottom() + 4;
    }
    QPointF best = rect.topLeft();
    double bestOverlap = std::numeric_limits<double>::max(), bestDistance = std::numeric_limits<double>::max();
    for (double x : xs) for (double y : ys) {
        const QPointF candidate(qBound(bounds.left(), x, qMax(bounds.left(), bounds.right() - rect.width())),
                                qBound(bounds.top(), y, qMax(bounds.top(), bounds.bottom() - rect.height())));
        const QRectF box(candidate, rect.size());
        double overlap = 0;
        for (const auto &other : occupied) {
            const auto intersection = box.intersected(other.adjusted(-2, -2, 2, 2));
            if (!intersection.isEmpty()) overlap += intersection.width() * intersection.height();
        }
        const auto delta = candidate - rect.topLeft();
        const double distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (overlap < bestOverlap || (overlap == bestOverlap && distance < bestDistance)) {
            best = candidate; bestOverlap = overlap; bestDistance = distance;
        }
    }
    label->position->setPixelPosition(label->position->pixelPosition() + best - rect.topLeft());
    occupied.append(QRectF(label->topLeft->pixelPosition(), label->bottomRight->pixelPosition()));
}
}

MainWindow::PlotContext MainWindow::createNewPlotContext(const QString &title, bool isMain)
{
    PlotContext ctx;
    ctx.plot = new QCustomPlot();
    ctx.plot->setMinimumReplotInterval(33);
    ctx.plot->setCursor(Qt::ArrowCursor); // Ensure cursor resets when hovering from splitter
    
    QPen axisPen(QColor("#888888"));
    axisPen.setWidth(1);
    QPen gridPen(QColor("#333333"));
    gridPen.setStyle(Qt::DashLine);
    // X Axis
    ctx.plot->xAxis->setBasePen(axisPen);
    ctx.plot->xAxis->setTickPen(axisPen);
    ctx.plot->xAxis->setSubTickPen(axisPen);
    ctx.plot->xAxis->setTickLabelColor(QColor("#888888"));
    ctx.plot->xAxis->setLabelColor(QColor("#aaaaaa"));
    ctx.plot->xAxis->grid()->setPen(gridPen);
    ctx.plot->xAxis->grid()->setZeroLinePen(Qt::NoPen);
    // Y Axis
    ctx.plot->yAxis->setBasePen(axisPen);
    ctx.plot->yAxis->setTickPen(axisPen);
    ctx.plot->yAxis->setSubTickPen(axisPen);
    ctx.plot->yAxis->setTickLabelColor(QColor("#888888"));
    ctx.plot->yAxis->setLabelColor(QColor("#aaaaaa"));
    ctx.plot->yAxis->grid()->setPen(gridPen);
    ctx.plot->yAxis->grid()->setZeroLinePen(Qt::NoPen);
    
    bool showGrid = chkShowGrid ? chkShowGrid->isChecked() : true;
    ctx.plot->xAxis->grid()->setVisible(showGrid);
    ctx.plot->yAxis->grid()->setVisible(showGrid);
    
    ctx.plot->yAxis->setNumberFormat("f"); // Disable scientific notation
    ctx.plot->yAxis->setNumberPrecision(spinDecimals ? spinDecimals->value() : 2);
    ctx.plot->installEventFilter(this);
    ctx.plot->xAxis->setLabel("时间 (s)");
    ctx.plot->yAxis->setLabel("数据值");
    // Set custom ticker for relative time
    ctx.ticker = QSharedPointer<RightAlignedTicker>(new RightAlignedTicker());
    ctx.plot->xAxis->setTicker(ctx.ticker);
    ctx.plot->xAxis->setRange(0, 10);
    ctx.plot->yAxis->setRange(-5, 5);
    // Restrict zoom to horizontal (X-axis) only, allow drag based on chkAutoScaleY
    ctx.plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    ctx.plot->axisRect()->setRangeZoom(Qt::Horizontal);
    if (chkAutoScaleY && chkAutoScaleY->isChecked()) {
        ctx.plot->axisRect()->setRangeDrag(Qt::Horizontal);
    } else {
        ctx.plot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    }
    // --- Performance: OpenGL GPU rendering (with safety fallback) ---
    if (isMain) {
        // Check if OpenGL is available before enabling
        QOpenGLContext testCtx;
        if (testCtx.create()) {
            // Waveforms use un-antialiased cosmetic lines. Multisampling adds
            // fill cost without adding useful detail to dense traces.
            ctx.plot->setOpenGl(true, 0);
            if (!ctx.plot->openGl()) {
                // OpenGL init failed internally, fallback to CPU rendering
                qWarning() << "OpenGL acceleration unavailable, using CPU rendering.";
                ctx.plot->setOpenGl(false);
            }
        } else {
            qWarning() << "No OpenGL context available, using CPU rendering.";
        }
    }
    // Disable expensive anti-aliasing on elements we don't need it for
    ctx.plot->setAntialiasedElements(QCP::aeNone);
    ctx.plot->setNotAntialiasedElements(QCP::aeAll);
    // Auto margins but force right margin to fit 0.00
    ctx.plot->axisRect()->setAutoMargins(QCP::msLeft | QCP::msTop | QCP::msBottom);
    ctx.plot->axisRect()->setMargins(QMargins(0, 0, 30, 0));
    // Measurement setup
    ctx.measureState = false;
    ctx.tracer1 = new QCPItemTracer(ctx.plot);
    ctx.tracer2 = new QCPItemTracer(ctx.plot);
    ctx.hoverTracer = new QCPItemTracer(ctx.plot);
    ctx.measureLine = new QCPItemLine(ctx.plot);
    ctx.measureText = new QCPItemText(ctx.plot);
    ctx.measureText2 = new QCPItemText(ctx.plot);
    ctx.measureDeltaText = new QCPItemText(ctx.plot);
    ctx.widthText = new QCPItemText(ctx.plot);
    ctx.widthText->setPositionAlignment(Qt::AlignTop | Qt::AlignLeft);
    ctx.widthText->position->setType(QCPItemPosition::ptAxisRectRatio);
    ctx.widthText->position->setCoords(0.02, 0.02);
    ctx.widthText->setText(QString("X跨度: %1 s").arg(ctx.plot->xAxis->range().size(), 0, 'f', 2));
    ctx.widthText->setColor(QColor(200, 200, 200, 150));
    ctx.widthText->setFont(QFont("Arial", 9));
    
    ctx.heightText = new QCPItemText(ctx.plot);
    ctx.heightText->setPositionAlignment(Qt::AlignTop | Qt::AlignLeft);
    // Anchor the second line to the first line in pixels. Percentage-based
    // offsets overlap when detached plots become short.
    ctx.heightText->position->setType(QCPItemPosition::ptAbsolute);
    ctx.heightText->position->setParentAnchor(ctx.widthText->bottomLeft);
    ctx.heightText->position->setCoords(0, 3);
    ctx.heightText->setText(QString("Y跨度: %1").arg(ctx.plot->yAxis->range().size(), 0, 'f', spinDecimals ? spinDecimals->value() : 2));
    ctx.heightText->setColor(QColor(200, 200, 200, 150));
    ctx.heightText->setFont(QFont("Arial", 9));
    ctx.tracer1->setStyle(QCPItemTracer::tsCircle);
    ctx.tracer1->setPen(QPen(Qt::red, 2));
    ctx.tracer1->setBrush(QBrush(QColor(255, 0, 0, 100)));
    ctx.tracer1->setSize(12);
    ctx.tracer1->setVisible(false);
    ctx.tracer2->setStyle(QCPItemTracer::tsCircle);
    ctx.tracer2->setPen(QPen(Qt::yellow, 2));
    ctx.tracer2->setBrush(QBrush(QColor(255, 255, 0, 100)));
    ctx.tracer2->setSize(12);
    ctx.tracer2->setVisible(false);
    ctx.measureLine->setPen(QPen(Qt::white, 1, Qt::DashLine));
    ctx.measureLine->setVisible(false);
    ctx.measureText->setColor(Qt::white);
    ctx.measureText->setFont(QFont("Arial", 11, QFont::Bold));
    ctx.measureText->setBrush(QBrush(QColor(0, 0, 0, 150)));
    ctx.measureText->setPadding(QMargins(5, 5, 5, 5));
    ctx.measureText->setVisible(false);
    
    ctx.measureText2->setColor(Qt::white);
    ctx.measureText2->setFont(QFont("Arial", 11, QFont::Bold));
    ctx.measureText2->setBrush(QBrush(QColor(0, 0, 0, 150)));
    ctx.measureText2->setPadding(QMargins(5, 5, 5, 5));
    ctx.measureText2->setVisible(false);
    
    ctx.measureDeltaText->setColor(Qt::yellow);
    ctx.measureDeltaText->setFont(QFont("Arial", 11, QFont::Bold));
    ctx.measureDeltaText->setBrush(QBrush(QColor(0, 0, 0, 200)));
    ctx.measureDeltaText->setPadding(QMargins(5, 5, 5, 5));
    ctx.measureDeltaText->setVisible(false);
    ctx.measureText->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    ctx.measureText2->setColor(Qt::white);
    ctx.measureText2->setFont(QFont("Arial", 11, QFont::Bold));
    ctx.measureText2->setBrush(QBrush(QColor(0, 0, 0, 150)));
    ctx.measureText2->setPadding(QMargins(5, 5, 5, 5));
    ctx.measureText2->setVisible(false);
    ctx.measureText2->setPositionAlignment(Qt::AlignTop | Qt::AlignHCenter);
    ctx.hoverTracer->setStyle(QCPItemTracer::tsCircle);
    ctx.hoverTracer->setPen(QPen(Qt::green, 1));
    ctx.hoverTracer->setBrush(QBrush(QColor(0, 255, 0, 50)));
    ctx.hoverTracer->setSize(15);
    ctx.hoverTracer->setVisible(false);
    
    ctx.hoverText = new QCPItemText(ctx.plot);
    ctx.hoverText->setColor(Qt::green);
    ctx.hoverText->setFont(QFont("Arial", 11, QFont::Bold));
    ctx.hoverText->setPadding(QMargins(3, 3, 3, 3));
    ctx.hoverText->setBrush(QBrush(QColor(0, 0, 0, 150)));
    ctx.hoverText->setVisible(false);
    ctx.hoverText->setPositionAlignment(Qt::AlignBottom | Qt::AlignLeft);

    connect(ctx.plot, &QCustomPlot::afterLayout, this, [this, plot = ctx.plot]() {
        for (auto &context : allPlots) if (context.plot == plot) {
            QVector<QRectF> occupied;
            for (auto span : {context.widthText, context.heightText}) if (span->visible())
                occupied.append(QRectF(span->topLeft->pixelPosition(), span->bottomRight->pixelPosition()));
            if (context.measureText->visible()) {
                context.tracer1->updatePosition();
                placePlotValueLabel(context.measureText, context.tracer1->position->pixelPosition(), Qt::AlignBottom | Qt::AlignHCenter, occupied);
            }
            if (context.measureText2->visible()) {
                context.tracer2->updatePosition();
                placePlotValueLabel(context.measureText2, context.tracer2->position->pixelPosition(), Qt::AlignBottom | Qt::AlignHCenter, occupied);
            }
            if (context.measureDeltaText->visible()) {
                const QPointF midpoint = (context.tracer1->position->pixelPosition() + context.tracer2->position->pixelPosition()) * 0.5;
                placePlotValueLabel(context.measureDeltaText, midpoint, Qt::AlignCenter, occupied);
            }
            if (context.hoverText->visible()) {
                context.hoverTracer->updatePosition();
                placePlotValueLabel(context.hoverText, context.hoverTracer->position->pixelPosition(), Qt::AlignBottom | Qt::AlignLeft, occupied);
            }
            break;
        }
    });
    
     connect(ctx.plot, &QCustomPlot::mouseDoubleClick, this, &MainWindow::onPlotMouseDoubleClick);
    connect(ctx.plot, &QCustomPlot::mouseMove, this, &MainWindow::onPlotMouseMove);
    connect(ctx.plot, &QCustomPlot::mousePress, this, [this, plot = ctx.plot](QMouseEvent *) {
        for (auto &context : allPlots) if (context.plot == plot) {
            const bool hovered = context.hoverTracer->visible() || context.hoverText->visible();
            context.hoverTracer->setVisible(false); context.hoverText->setVisible(false);
            if (hovered) plot->replot(QCustomPlot::rpQueuedReplot);
            break;
        }
    });
    connect(ctx.plot, &QCustomPlot::mouseRelease, this, [this](QMouseEvent *) {
        if (chkAutoScaleY && chkAutoScaleY->isChecked()) yScaleTimer->start(0);
    });
    ctx.plot->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ctx.plot, &QCustomPlot::customContextMenuRequested, this, &MainWindow::onPlotContextMenu);
    connect(ctx.plot->xAxis, QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged), this, &MainWindow::onXAxisRangeChanged);
    connect(ctx.plot->yAxis, QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged), this, [this, plot = ctx.plot](const QCPRange &newRange) {
        for (PlotContext &context : allPlots) {
            if (context.plot == plot && context.heightText) {
                context.heightText->setText(QString::fromUtf8("Y跨度: %1").arg(newRange.size(), 0, 'f', spinDecimals ? spinDecimals->value() : 2));
                break;
            }
        }
    });
    ctx.dock = new QDockWidget(title, innerMainWindow);
    ctx.dock->setObjectName(QString("PlotDock_%1").arg(title));
    auto frame = new QWidget(); auto frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(0, 0, 0, 0); frameLayout->setSpacing(0); frameLayout->addWidget(ctx.plot, 1);
    ctx.scope = new PlotMeasurementController(ctx.plot, frame, [this](int channel) { return channelUnit(channel); }, [this] { return lastLatestTime; });
    frameLayout->addWidget(ctx.scope->panel()); ctx.dock->setWidget(frame);
    connect(ctx.scope, &PlotMeasurementController::activated, this, [this, plot = ctx.plot] {
        for (auto &context : allPlots) if (context.plot == plot) {
            context.tracer1->setVisible(false); context.tracer2->setVisible(false); context.measureLine->setVisible(false);
            context.measureText->setVisible(false); context.measureText2->setVisible(false); context.measureDeltaText->setVisible(false); context.measureState = 0;
            break;
        }
    });
    connect(ctx.scope, &PlotMeasurementController::visibilityChanged, this, [this](bool visible) {
        bool any = visible;
        for (const auto &context : allPlots) if (context.scope && context.scope->isEnabled()) any = true;
        if (plotScrollArea) plotScrollArea->setMeasurementBarsVisible(any);
    });
    if (isMain) ctx.scope->setPreferenceKey("ScopeMeasurements/Main");
    if (isMain) {
        ctx.dock->setFeatures(QDockWidget::DockWidgetMovable);
    } else {
        ctx.dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
        ctx.dock->installEventFilter(this);
    }
    // Use the active selection, not an unrelated/stale settings namespace.
    // Apply after initializing axes so hard-coded setup colors cannot undo it.
    applyWaveformPlotTheme(ctx.plot, isMain);
    return ctx;
}

void MainWindow::applyWaveformPlotTheme(QCustomPlot *plot, bool isMain)
{
    const int theme = comboTheme ? comboTheme->currentData().toInt() : QSettings("WaveLink", "WaveLink").value("comboTheme", 0).toInt();
    ThemeManager::setupPlotTheme(plot, static_cast<ThemeManager::Theme>(theme), isMain ? m_mainPlotBackground : m_detachedPlotBackground);
}

void MainWindow::updateWaveformPlotThemes()
{
    for (int i = 0; i < allPlots.size(); ++i) {
        applyWaveformPlotTheme(allPlots[i].plot, i == 0);
        allPlots[i].plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void MainWindow::showPlotBackgroundSettings()
{
    const QColor original[] = {m_mainPlotBackground, m_detachedPlotBackground};
    QDialog dialog(this);
    dialog.setObjectName("plotBackgroundDialog");
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setWindowTitle(QStringLiteral("绘图背景颜色"));
    auto layout = new QVBoxLayout(&dialog);
    auto hint = new QLabel(QStringLiteral("分别设置主视图和所有独立视图的背景颜色。独立视图的设置也适用于以后新建的视图。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto form = new QGridLayout;
    layout->addLayout(form);
    QComboBox *choices[2];
    QPushButton *pickers[2];
    const auto swatch = [](const QColor &color) {
        QPixmap pixmap(24, 16); pixmap.fill(color); return QIcon(pixmap);
    };
    const auto themeColor = [this] {
        return comboTheme && comboTheme->currentData().toInt() == ThemeManager::LightTheme ? QColor("#ffffff") : QColor("#1e1e1e");
    };
    const auto preview = [&] {
        m_mainPlotBackground = choices[0]->currentData().value<QColor>();
        m_detachedPlotBackground = choices[1]->currentData().value<QColor>();
        for (int i = 0; i < 2; ++i) {
            QColor color = choices[i]->currentData().value<QColor>();
            if (!color.isValid()) color = themeColor();
            pickers[i]->setIcon(swatch(color));
            pickers[i]->setText(color.name().toUpper() + QStringLiteral("…"));
        }
        updateWaveformPlotThemes();
    };
    for (int i = 0; i < 2; ++i) {
        form->addWidget(new QLabel(i == 0 ? QStringLiteral("主视图") : QStringLiteral("独立视图"), &dialog), i, 0);
        auto choice = choices[i] = new QComboBox(&dialog);
        choice->setObjectName(i == 0 ? "mainPlotBackgroundChoice" : "detachedPlotBackgroundChoice");
        choice->addItem(QStringLiteral("跟随系统主题"), QColor());
        choice->addItem(swatch(QColor("#1e1e1e")), QStringLiteral("暗黑主题背景"), QColor("#1e1e1e"));
        choice->addItem(swatch(QColor("#ffffff")), QStringLiteral("浅白主题背景"), QColor("#ffffff"));
        choice->addItem(QStringLiteral("自定义颜色"), original[i].isValid() ? original[i] : themeColor());
        int selected = 0;
        if (original[i].isValid()) {
            selected = choice->findData(original[i]);
            if (selected < 0) selected = 3;
        }
        choice->setCurrentIndex(selected);
        form->addWidget(choice, i, 1);
        auto picker = pickers[i] = new QPushButton(&dialog);
        picker->setObjectName(i == 0 ? "mainPlotBackgroundPicker" : "detachedPlotBackgroundPicker");
        picker->setToolTip(QStringLiteral("选择自定义背景颜色"));
        form->addWidget(picker, i, 2);
        connect(picker, &QPushButton::clicked, &dialog, [&, i] {
            QColor current = choices[i]->currentData().value<QColor>();
            if (!current.isValid()) current = themeColor();
            QColorDialog colors(current, &dialog);
            colors.setObjectName("plotBackgroundColorPicker");
            colors.setWindowTitle(QStringLiteral("选择绘图背景颜色"));
            colors.setOption(QColorDialog::DontUseNativeDialog);
            if (auto colorButtons = colors.findChild<QDialogButtonBox *>()) {
                colorButtons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
                colorButtons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
            }
            if (colors.exec() != QDialog::Accepted) return;
            QColor selected = colors.selectedColor();
            if (!selected.isValid()) return;
            selected.setAlpha(255);
            const QSignalBlocker blocker(choices[i]);
            choices[i]->setItemData(3, selected);
            choices[i]->setCurrentIndex(3);
            preview();
        });
    }
    form->setColumnStretch(1, 1);
    auto note = new QLabel(QStringLiteral("点击右侧颜色按钮可自定义；选择后实时预览，确定保存，取消还原。"), &dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &dialog);
    buttons->setObjectName("plotBackgroundButtons");
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("恢复默认"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
        const QSignalBlocker a(choices[0]), b(choices[1]);
        choices[0]->setCurrentIndex(0); choices[1]->setCurrentIndex(0); preview();
    });
    for (auto choice : choices) connect(choice, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, preview);
    preview();
    dialog.resize(560, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) {
        m_mainPlotBackground = original[0]; m_detachedPlotBackground = original[1];
        updateWaveformPlotThemes();
        return;
    }
    QSettings settings("WaveLink", "WaveLink");
    const QColor saved[] = {m_mainPlotBackground, m_detachedPlotBackground};
    const QString keys[] = {QStringLiteral("PlotBackground/Main"), QStringLiteral("PlotBackground/Detached")};
    for (int i = 0; i < 2; ++i) {
        if (saved[i].isValid()) settings.setValue(keys[i], saved[i].name());
        else settings.remove(keys[i]);
    }
    settings.sync();
}

void MainWindow::syncChannelCount(int count)
{
    if (count < 0 || count > 256) {
        qWarning() << "Ignored suspicious channel count:" << count;
        return;
    }

    if (channelGraphs.size() < count) {
        checkAndAddChannel(count);
        return;
    }
    if (channelGraphs.size() == count) {
        return;
    }

    // Detached plots duplicate the main graphs, so merge them before removing
    // channels to keep the channel-to-plot mapping and dock list consistent.
    onMergeAllViews();

    QCustomPlot *mainPlot = (!allPlots.isEmpty() && allPlots[0].plot)
        ? allPlots[0].plot
        : nullptr;

    for (int i = channelGraphs.size() - 1; i >= count; --i) {
        QCPGraph *graph = channelGraphs.takeAt(i);
        if (mainPlot && graph) {
            mainPlot->removeGraph(graph);
        }

        if (i < channelBuffers.size()) {
            channelBuffers.removeAt(i);
        }
        if (i < channelValueLabels.size()) {
            channelValueLabels.removeAt(i);
        }
        if (i < channelColorButtons.size()) {
            channelColorButtons.removeAt(i);
        }

        if (listChannels && i < listChannels->count()) {
            QListWidgetItem *item = listChannels->takeItem(i);
            if (item) {
                QWidget *itemWidget = listChannels->itemWidget(item);
                if (itemWidget) {
                    listChannels->removeItemWidget(item);
                    delete itemWidget;
                }
                delete item;
            }
        }
    }

    for (auto it = channelToPlotMap.begin(); it != channelToPlotMap.end();) {
        if (it.key() >= count) {
            it = channelToPlotMap.erase(it);
        } else {
            it.value() = 0;
            ++it;
        }
    }

    while (menuSpectrumChannels
           && menuSpectrumChannels->actions().size() > count) {
        QAction *action = menuSpectrumChannels->actions().last();
        menuSpectrumChannels->removeAction(action);
        delete action;
    }

    if (comboMeasureChannel) {
        while (comboMeasureChannel->count() > count) {
            comboMeasureChannel->removeItem(comboMeasureChannel->count() - 1);
        }
    }

    while (spectrumGraphs.size() > count) {
        QCPGraph *graph = spectrumGraphs.takeLast();
        if (spectrumPlot && graph) {
            spectrumPlot->removeGraph(graph);
        }
    }

    filterStates.resize(count);
    m_dataProcessor.setChannelCount(count);
    updateSpectrumChannelMenu();
    updateChannelSelectionSummary();

    if (mainPlot) {
        mainPlot->replot();
    }
    if (spectrumPlot) {
        spectrumPlot->replot(QCustomPlot::rpQueuedReplot);
    }
    updateChannelListHeight();
}

void MainWindow::updateSpectrumFooterLayout()
{
    if (!spectrumFooter || !spectrumFooterLayout || !spectrumXAxisLabel) {
        return;
    }

    spectrumXAxisLabel->setMinimumWidth(0);
    const int titleWidth = spectrumXAxisLabel->sizeHint().width();
    spectrumXAxisLabel->setMinimumWidth(titleWidth);

    const int fixedControlsWidth =
        spectrumFooterLayout->itemAt(0)
            ? spectrumFooterLayout->itemAt(0)->sizeHint().width()
            : 0;
    const int availableWidth = spectrumFooter->contentsRect().width();
    const bool canCenter =
        availableWidth >= fixedControlsWidth + titleWidth + 24;

    spectrumFooterLayout->setStretch(1, canCenter ? 1 : 0);
    spectrumFooterLayout->setStretch(3, 1);
    spectrumXAxisLabel->setAlignment(
        canCenter ? Qt::AlignCenter : Qt::AlignLeft);
}

int MainWindow::defaultBottomDockHeight() const
{
    // QMainWindow::height excludes the native window frame, but includes menu,
    // toolbars and status bar. Never base this on the monitor's resolution.
    int availableHeight = contentsRect().height();
    if (menuWidget() && !menuWidget()->isHidden()) availableHeight -= menuWidget()->height();
    if (auto toolbar = findChild<QToolBar *>("mainToolBar")) {
        if (!toolbar->isHidden()) availableHeight -= toolbar->height();
    }
    if (auto status = findChild<QStatusBar *>()) {
        if (!status->isHidden()) availableHeight -= status->height();
    }
    availableHeight -= style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, this);
    return qMax(1, qRound(qMax(1, availableHeight) *
        SystemPreferences::validBottomPercent(m_systemPreferences.defaultBottomPercent) / 100.0));
}

void MainWindow::updateChannelListHeight()
{
    if (!listChannels) {
        return;
    }

    const int channelCount = listChannels->count();
    QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    const int screenHeight = screen ? screen->geometry().height() : 1080;
    const int minimumRows = screenHeight <= 1080 ? 6 : 10;
    const int visibleRows = qMax(channelCount, minimumRows);
    int rowHeight = channelCount > 0 ? listChannels->sizeHintForRow(0) : 0;
    if (rowHeight <= 0) {
        rowHeight = listChannels->fontMetrics().height() + 8;
    }

    const int listHeight =
        visibleRows * rowHeight + 2 * listChannels->frameWidth() + 2;
    listChannels->setMinimumHeight(listHeight);
    // The outer QScrollArea owns vertical scrolling for the complete right-side
    // panel, so the channel list itself should remain fully expanded.
    listChannels->setMaximumHeight(QWIDGETSIZE_MAX);
    listChannels->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listChannels->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    listChannels->updateGeometry();
}

void MainWindow::checkAndAddChannel(int count)
{
    if (count > 256) {
        qWarning() << "Ignored suspicious channel count:" << count;
        return;
    }
    // Dynamic protocols call this for every sample. Rebuilding the channel
    // panel's geometry/selection state is necessary only when a channel grows.
    if (count <= channelGraphs.size()) return;
    while (channelGraphs.size() < count) {
        int chIdx = channelGraphs.size();
        
        if (menuSpectrumChannels->actions().size() < count) {
            QAction *act = menuSpectrumChannels->addAction(channelDisplayName(chIdx));
            act->setCheckable(true);
            act->setData(chIdx);
            // Check first channel by default if it's the first one
            if (menuSpectrumChannels->actions().size() == 1) {
                act->setChecked(true);
            }
            updateSpectrumChannelMenu();
        }
        if (comboMeasureChannel && comboMeasureChannel->count() < count) {
            comboMeasureChannel->addItem(channelDisplayName(chIdx));
        }
        if (allPlots.isEmpty()) {
            PlotContext ctx = createNewPlotContext("主视图", true);
            allPlots.append(ctx);
            innerMainWindow->addDockWidget(Qt::TopDockWidgetArea, ctx.dock);
        }
        QCustomPlot *mainPlot = allPlots[0].plot;
        auto g = new WaveformGraph(mainPlot->xAxis, mainPlot->yAxis);
        g->setChannelIndex(chIdx);
        QSettings settings("WaveLink", "AdvancedApp");
    QColor graphColor = settings.value(channelSettingKey(chIdx, "Color"), defaultColors[chIdx % defaultColors.size()]).value<QColor>();
        QPen graphPen = WaveformGraph::displayPen(graphColor, m_systemPreferences.plotLineWidth);
        g->setPen(graphPen);
        g->setName(channelDisplayName(chIdx));
        g->setAntialiased(false);
        g->setAdaptiveSampling(true); // Key perf: auto-decimate to screen pixel count
        channelGraphs.append(g);
        channelToPlotMap[chIdx] = 0;
        QQueue<double> pending;
        for (int i = 0; i < timeBuffer.size(); ++i) pending.enqueue(qQNaN());
        channelBuffers.append(pending);
        QListWidgetItem *item = new QListWidgetItem(listChannels);
        QWidget *w = new QWidget();
        QHBoxLayout *hl = new QHBoxLayout(w);
        hl->setContentsMargins(5, 2, 5, 2);
        
        QCheckBox *chkBox = new QCheckBox(channelDisplayName(chIdx));
        chkBox->setObjectName("chkChannel");
        const bool channelVisible = settings.value(channelSettingKey(chIdx, "Visible"), true).toBool();
        chkBox->setChecked(channelVisible);
        g->setVisible(channelVisible);
        chkBox->setStyleSheet(QString("QCheckBox { color: %1; font-weight: bold; }").arg(graphColor.name()));
        
        QLabel *lblVal = new QLabel("0.00");
        lblVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lblVal->setAttribute(Qt::WA_TransparentForMouseEvents);
        QPushButton *btnColor = new QPushButton();
        btnColor->setFixedSize(16, 16);
        btnColor->setStyleSheet(QString("background-color: %1; border: 1px solid #777;").arg(graphColor.name()));
        hl->addWidget(chkBox);
        hl->addWidget(lblVal);
        hl->addWidget(btnColor);
        listChannels->setItemWidget(item, w);
        
        connect(chkBox, &QCheckBox::toggled, this, [this, item](bool checked) {
            onChannelItemChanged(item);
        });
        
        channelValueLabels.append(lblVal);
        connect(btnColor, &QPushButton::clicked, this, [this, chIdx, btnColor, chkBox]() {
            QColorDialog *dlg = createStyledColorDialog(QString::fromUtf8("选择波形颜色"), channelGraphs[chIdx]->pen().color(), this);
            int dlgResult = dlg->exec();
            
            if (dlgResult == QDialog::Accepted) {
                QColor newColor = dlg->currentColor();
                QPen p = channelGraphs[chIdx]->pen();
                p.setColor(newColor);
                channelGraphs[chIdx]->setPen(p);
                QSettings settings("WaveLink", "AdvancedApp");
                settings.setValue(channelSettingKey(chIdx, "Color"), newColor.name());
                btnColor->setStyleSheet(QString("background-color: %1; border: 1px solid #777;").arg(newColor.name()));
                chkBox->setStyleSheet(QString("QCheckBox { color: %1; font-weight: bold; }").arg(newColor.name()));
                int plotIdx = channelToPlotMap[chIdx];
                allPlots[plotIdx].plot->replot();
            }
            dlg->deleteLater();
        });
    }
    if (channelBuffers.size() != filterStates.size()) {
        filterStates.resize(channelBuffers.size());
        resetFilters();
    }
    updateChannelSelectionSummary();
    updateChannelListHeight();
    if (!allPlots.isEmpty() && allPlots[0].scope) allPlots[0].scope->refresh();
}

void MainWindow::handleChannelsUpdated(const QVector<double> &data)
{
    bool isDemoActive = (demoTimer && demoTimer->isActive());
    if (isRecording && isDemoActive) {
        writeDemoLogData(data);
    }
    
    if (data.isEmpty() || data.size() > 256) return;
    // Dynamic arrays can legitimately shrink for one frame and grow again.
    // Keep historical graphs/detached views and append NaN for absent values,
    // rather than deleting their previous data on every width transition.
    if (!isDemoActive && m_appliedProtocol.kind == "custom" && m_appliedProfile.isDynamicArray()) checkAndAddChannel(data.size());
    else syncChannelCount(data.size());

    // Optional device setup is separate from generic decoding. Apply it once
    // after the first complete frame, allowing the user to merge views later.
    if (!isDemoActive && m_devicePresetViewsPending && m_appliedProtocol.kind == "custom") {
        m_devicePresetViewsPending = false;
        for (int i = 0; i < data.size(); ++i) moveChannelToNewPlot(i);
    }

    if (chkPausePlot && chkPausePlot->isChecked()) {
        return;
    }
    
    // We intentionally removed the strict channel count filter here.
    // If a corrupted packet arrives, the channelBuffer handles missing data with qQNaN().
    

    // Bypass m_dataProcessor since it resets and drops data needlessly
    // m_dataProcessor.setChannelCount(data.size());
    // QVector<double> processedData = m_dataProcessor.processFrame(data);
    QVector<double> processedData = data;
    if (!isDemoActive && m_appliedProtocol.kind == "custom" && m_appliedProfile.isDynamicArray())
        while (processedData.size() < channelBuffers.size()) processedData.append(qQNaN());
    applyFilters(processedData);


    const double effectiveSampleRate = isDemoActive && spinDemoInterval && spinDemoInterval->value() > 0
        ? 1000.0 / spinDemoInterval->value()
        : spinFs->value();
    double dt = effectiveSampleRate > 0 ? (1.0 / effectiveSampleRate) : 0.001;
    const double t = m_nextPlotTime;
    m_nextPlotTime += dt; // A rate change must not rescale elapsed history.
    sampleCount++;
    frameCountForRate++;
    // Bound the UI-side backlog. Under sustained overload it is safer to drop
    // the oldest not-yet-plotted samples than to exhaust process memory.
    // Bound staging memory too, including the many-channel case.
    const int kMaxPendingPlotFrames = qMin(8192, qMax(256,
        (8 * 1024 * 1024) / (qMax(1, channelBuffers.size()) * 32)));
    while (timeBuffer.size() >= kMaxPendingPlotFrames) {
        timeBuffer.dequeue();
        for (auto &buffer : channelBuffers) {
            if (!buffer.isEmpty()) buffer.dequeue();
        }
    }
    timeBuffer.enqueue(t);
    for (int i = 0; i < channelBuffers.size(); ++i) {
        if (i < processedData.size()) {
            channelBuffers[i].enqueue(processedData[i]);
        } else {
            channelBuffers[i].enqueue(qQNaN()); // Missing data safety
        }
    }
}
void MainWindow::onRateTimer()
{
    if (m_isImportedData) {
        if (comboInterface->currentData().toInt() == 4 && bleClient) {
            lblRxCount->setText(QString("Rx: %1").arg(bleClient->rxCount()));
            lblTxCount->setText(QString("Tx: %1").arg(bleClient->txCount()));
        }
        return; // Preserve the imported file's sampling-rate display.
    }
    
    if (frameCountForRate > 0) {
        double actualDt = 1000.0 / frameCountForRate;
        lblDataRate->setText(QString("%1 帧/秒 (Δt: %2 ms)").arg(frameCountForRate).arg(actualDt, 0, 'f', 2));
    } else {
        lblDataRate->setText(QString("0 帧/秒"));
    }
    
    if (comboInterface->currentData().toInt() == 4 && bleClient) {
        lblRxCount->setText(QString("Rx: %1").arg(bleClient->rxCount()));
        lblTxCount->setText(QString("Tx: %1").arg(bleClient->txCount()));
    } else if (isNetworkInterface(comboInterface->currentData().toInt())) {
        lblRxCount->setText(QString("Rx: %1").arg(networkHandler->getRxCount()));
        lblTxCount->setText(QString("Tx: %1").arg(networkHandler->getTxCount()));
    } else if (lblRxCount && serialHandler) {
        lblRxCount->setText(QString("Rx: %1").arg(serialHandler->getRxCount()));
    }
    if (comboInterface->currentData().toInt() == 0 && lblTxCount && serialHandler) {
        lblTxCount->setText(QString("Tx: %1").arg(serialHandler->getTxCount()));
    }
    
    frameCountForRate = 0;
}
void MainWindow::onPlotTimer()
{
    updateProtocolControls();
    // Pull small batches under both a frame budget and a time budget. There is
    // no per-frame Qt event backlog; the worker mailbox bounds display overload.
    QElapsedTimer deliveryClock;
    deliveryClock.start();
    int delivered = 0;
    int graphCount = 0;
    for (const PlotContext &ctx : allPlots) graphCount += ctx.plot->graphCount();
    // A sample budget alone is not sufficient with hundreds of channels.
    const int frameBudget = qMin(1024, qMax(64, 131072 / qMax(1, graphCount)));
    auto deliver = [&](auto *handler) {
        while (handler && delivered < frameBudget && deliveryClock.elapsed() < 8) {
            quint64 dropped = 0;
            const auto batch = handler->takePlotFrames(qMin(32, frameBudget - delivered), dropped);
            if (dropped && !(chkPausePlot && chkPausePlot->isChecked())) {
                // Preserve the time gap when old display samples are discarded.
                sampleCount += dropped;
                const double dt = spinFs->value() > 0 ? 1.0 / spinFs->value() : 0.001;
                m_nextPlotTime += dropped * dt;
                // Do not draw an interpolated line across a missing interval.
                // Insert directly so NaNs never enter the stateful filters.
                if (!channelBuffers.isEmpty()) {
                    timeBuffer.enqueue(m_nextPlotTime - dt);
                    for (auto &buffer : channelBuffers) buffer.enqueue(qQNaN());
                }
                if (!m_plotOverloadClock.isValid() || m_plotOverloadClock.elapsed() >= 2000) {
                    statusBar()->showMessage(QStringLiteral("绘图负载过高：已跳过部分旧显示数据；录制数据不受此显示限流影响。"), 3000);
                    m_plotOverloadClock.start();
                }
            }
            if (batch.isEmpty()) break;
            for (const auto &frame : batch) handleChannelsUpdated(frame);
            delivered += batch.size();
        }
    };
    // Never deliver frames from an old interface or across a pending parser
    // boundary. The worker clears its mailbox before the successful ACK.
    if (!m_protocolApplyPending) {
        const int source = comboInterface->currentData().toInt();
        if (source == 0) deliver(serialHandler);
        else if (isNetworkInterface(source)) deliver(networkHandler);
        else if (source == 4) deliver(bleWorker);
    }
    if (timeBuffer.isEmpty()) return;
    const int pointsToAdd = qMin(timeBuffer.size(), frameBudget);
    const bool updateLabels = !m_plotLabelClock.isValid() || m_plotLabelClock.elapsed() >= 100;
    if (updateLabels) m_plotLabelClock.start();
    QVector<double> times(pointsToAdd);
    for (int i = 0; i < pointsToAdd; ++i) {
        times[i] = timeBuffer.dequeue();
    }
    
    for (int i = 0; i < channelBuffers.size(); ++i) {
        QVector<double> values(pointsToAdd);
        for (int j = 0; j < pointsToAdd; ++j) {
            if (!channelBuffers[i].isEmpty()) {
                double val = channelBuffers[i].dequeue();
                // Clamp to prevent UI layout breakage from garbage serial data
                if (val > 1e12) val = 1e12;
                if (val < -1e12) val = -1e12;
                values[j] = val;
            } else {
                values[j] = qQNaN();
            }
        }
        if (i < channelGraphs.size()) {
            channelGraphs[i]->addData(times, values, true);
            if (channelToPlotMap.value(i, 0) > 0) {
                int plotIdx = channelToPlotMap[i];
                if (plotIdx < allPlots.size() && allPlots[plotIdx].plot->graphCount() > 0) {
                    auto sepGraph = WaveformGraph::forChannel(allPlots[plotIdx].plot, i);
                    if (sepGraph) {
                        sepGraph->addData(times, values, true);
                    }
                }
            }
        }
        if (updateLabels && i < channelValueLabels.size()) {
            if (!values.isEmpty()) {
                const bool valid = qIsFinite(values.last());
                channelValueLabels[i]->setText(valid ? QString::number(values.last(), 'f', spinDecimals->value()) : "NaN");
                channelValueLabels[i]->setToolTip(valid ? QString() : "此时间点无有效数值（可能为超限标记或显示缺口），波形显示断点，不使用旧值替代。");
            }
        }
    }
    double currentT = times.last();
    lastLatestTime = currentT;
    enforcePlotHistoryLimit(updateLabels);
    if (chkPausePlot->isChecked()) {
        return;
    } else {
        for (PlotContext &ctx : allPlots) {
            if (ctx.tracer1->visible()) {
                ctx.tracer1->setVisible(false);
                ctx.tracer2->setVisible(false);
                ctx.measureLine->setVisible(false);
                ctx.measureText->setVisible(false);
                ctx.measureText2->setVisible(false);
                if (ctx.measureDeltaText) ctx.measureDeltaText->setVisible(false);
                ctx.measureState = 0;
            }
        }
    }
    for (PlotContext &ctx : allPlots) {
        ctx.ticker->setLatestTime(currentT);
        if (m_autoScroll) {
            double window = ctx.plot->xAxis->range().size();
            ctx.plot->xAxis->blockSignals(true);
            ctx.plot->xAxis->setRange(currentT - window, currentT);
            ctx.plot->xAxis->blockSignals(false);
        }
        // Maintain ranges for clipped detached views, but only paint visible
        // widgets. A stationary history window needs only its relative ticker.
        if (!ctx.plot->isVisible() || isMinimized() || ctx.plot->visibleRegion().isEmpty()) continue;
        if (updateLabels && ctx.heightText) {
            double rangeSize = ctx.plot->yAxis->range().size();
            ctx.heightText->setText(QString("Y跨度: %1").arg(rangeSize, 0, 'f', spinDecimals ? spinDecimals->value() : 2));
        }
        if (m_autoScroll || updateLabels) ctx.plot->replot(QCustomPlot::rpQueuedReplot);
    }
    if (chkAutoScaleY->isChecked() && !yScaleTimer->isActive()) yScaleTimer->start(100);
    if (!allPlots.isEmpty()) syncTimeRangeSlider(allPlots[0].plot->xAxis->range());
}

void MainWindow::enforcePlotHistoryLimit(bool updateUsageLabel)
{
    // Include invisible main graphs and detached copies in the shared limit.
    QList<QCPGraph *> historyGraphs;
    for (const PlotContext &ctx : allPlots) {
        for (int i = 0; i < ctx.plot->graphCount(); ++i) {
            QCPGraph *g = ctx.plot->graph(i);
            if (!historyGraphs.contains(g)) historyGraphs.append(g);
        }
    }
    const int limitMB = spinPlotHistoryLimit ? spinPlotHistoryLimit->value() : 64;
    const qint64 limitBytes = qint64(limitMB) * 1024LL * 1024LL;
    qint64 usedBytes = 0, budgetedBytes = 0;
    for (QCPGraph *g : historyGraphs) {
        usedBytes += qint64(g->data()->size()) * qint64(sizeof(QCPGraphData));
        budgetedBytes += qint64(g->data()->size()) * qint64(sizeof(QCPGraphData));
        if (auto indexed = dynamic_cast<WaveformGraph *>(g)) {
            usedBytes += indexed->cacheBytes();
            budgetedBytes += g->visible() ? indexed->projectedCacheBytes() : indexed->cacheBytes();
        }
    }

    bool trimmed = false;
    // Imported files are finite offline datasets. Keep every sample, including
    // detached copies and data rebuilt after filtering or changing this limit.
    if (!m_isImportedData && budgetedBytes > limitBytes && budgetedBytes > 0) {
        // Keep ten percent headroom to avoid reallocating on every refresh.
        // Applying one ratio to every curve preserves their relative history.
        const long double retainRatio =
            (static_cast<long double>(limitBytes) * 0.9L) /
            static_cast<long double>(budgetedBytes);
        for (QCPGraph *g : historyGraphs) {
            if (auto indexed = dynamic_cast<WaveformGraph *>(g)) indexed->releaseDisplayCache();
            const int oldSize = g->data()->size();
            if (oldSize <= 0) continue;
            const int retain = qMax(1, int(static_cast<long double>(oldSize) * retainRatio));
            if (retain >= oldSize) continue;
            const double cutoff = g->data()->at(oldSize - retain)->key;
            g->data()->removeBefore(cutoff);
            g->data()->squeeze();
            trimmed = true;
        }

        usedBytes = 0;
        for (QCPGraph *g : historyGraphs) {
            usedBytes += qint64(g->data()->size()) * qint64(sizeof(QCPGraphData));
        }
    }
    if (updateUsageLabel && lblPlotHistoryUsage) {
        const QString usageText = m_pagedData
            ? QStringLiteral("按需浏览：%1 MB · %2").arg(usedBytes / (1024.0 * 1024.0), 0, 'f', 2)
                .arg(m_pagedViewExact ? QStringLiteral("原始采样点") : QStringLiteral("峰谷概览"))
            : m_isImportedData
            ? QStringLiteral("离线波形：%1 MB（完整保留）").arg(usedBytes / (1024.0 * 1024.0), 0, 'f', 2)
            : QStringLiteral("当前已使用：%1 MB / %2 MB")
                .arg(usedBytes / (1024.0 * 1024.0), 0, 'f', 2)
                .arg(limitMB);
        if (lblPlotHistoryUsage->text() != usageText) {
            lblPlotHistoryUsage->setText(usageText);
        }
    }
    if (trimmed && statusBar()) {
        statusBar()->showMessage(
            QStringLiteral("波形历史已达到上限，已自动清理最旧的绘图数据；完整录制文件不受影响。"),
            5000);
    }
}
void MainWindow::onSelectAllChannelsStateChanged(int state)
{
    bool checked = (state == Qt::Checked);
    QSettings settings("WaveLink", "AdvancedApp");
    chkSelectAllChannels->blockSignals(true);
    for (int i = 0; i < listChannels->count(); ++i) {
        QListWidgetItem *item = listChannels->item(i);
        QWidget *w = listChannels->itemWidget(item);
        if (w) {
            QCheckBox *chk = w->findChild<QCheckBox*>("chkChannel");
            if (chk) {
                chk->blockSignals(true);
                chk->setChecked(checked);
                chk->blockSignals(false);
                settings.setValue(channelSettingKey(i, "Visible"), checked);
            }
        }
    }
    chkSelectAllChannels->blockSignals(false);
    for (QCPGraph *g : channelGraphs) {
        g->setVisible(checked);
    }
    // Channel checkboxes control only the retained graphs in the main view.
    if (!allPlots.isEmpty() && allPlots[0].scope) allPlots[0].scope->refresh();
    if (!allPlots.isEmpty()) allPlots[0].plot->replot();
    if (chkAutoScaleY && chkAutoScaleY->isChecked()) autoScaleY();
}

void MainWindow::onChannelItemChanged(QListWidgetItem *item)
{
    int index = listChannels->row(item);
    if (index >= 0 && index < channelGraphs.size()) {
        QWidget *w = listChannels->itemWidget(item);
        bool isChecked = false;
        if (w) {
            QCheckBox *chk = w->findChild<QCheckBox*>("chkChannel");
            if (chk) isChecked = chk->isChecked();
        }
        channelGraphs[index]->setVisible(isChecked);
        if (!allPlots.isEmpty() && allPlots[0].scope) allPlots[0].scope->refresh();
        QSettings settings("WaveLink", "AdvancedApp");
        settings.setValue(channelSettingKey(index, "Visible"), isChecked);
        // Always replot main view because visibility in main view is always toggled
        if (allPlots.size() > 0) {
            allPlots[0].plot->replot();
        }
        if (chkAutoScaleY && chkAutoScaleY->isChecked()) {
            autoScaleY();
        }
    }
    updateChannelSelectionSummary();
}
void MainWindow::updateChannelSelectionSummary()
{
    if (!chkSelectAllChannels || !listChannels) return;
    const QSignalBlocker blocker(chkSelectAllChannels);
    bool allChecked = true;
    for (int i = 0; i < listChannels->count(); ++i) {
        QWidget *w = listChannels->itemWidget(listChannels->item(i));
        if (w) {
            QCheckBox *chk = w->findChild<QCheckBox*>("chkChannel");
            if (chk && !chk->isChecked()) {
                allChecked = false;
                break;
            }
        }
    }
    chkSelectAllChannels->setChecked(allChecked);
}
void MainWindow::scrollToLatest()
{
    m_autoScroll = true;
    for (PlotContext &ctx : allPlots) {
        const double rangeSize = ctx.plot->xAxis->range().size();
        ctx.plot->xAxis->setRange(lastLatestTime - rangeSize, lastLatestTime);
        ctx.plot->replot();
    }
}
void MainWindow::syncTimeRangeSlider(const QCPRange &viewRange)
{
    if (!m_rangeSlider || !qIsFinite(viewRange.lower) || !qIsFinite(viewRange.upper)
        || viewRange.size() <= 0 || !qIsFinite(lastLatestTime)) return;
    // Include the ENTIRE view, not just the received history. At startup the
    // view is 0..10; during early acquisition it can extend before time zero.
    // Clamping either case to 0..1 (or to a short recording) made Range lie
    // about the actual X span and the next drag unexpectedly shrink the view.
    const QSignalBlocker blocker(m_rangeSlider);
    m_rangeSlider->setRange(qMin(0.0, viewRange.lower), qMax(lastLatestTime, viewRange.upper));
    m_rangeSlider->setRelativeOffset(isPortOpen ? lastLatestTime : 0.0);
    m_rangeSlider->setValues(viewRange.lower, viewRange.upper);
}
void MainWindow::onXAxisRangeChanged(const QCPRange &newRange)
{
    QCPRange clampedRange = newRange;
    if (newRange.upper > lastLatestTime) {
        clampedRange = QCPRange(lastLatestTime - newRange.size(), lastLatestTime);
    }
    if (clampedRange.lower < 0) {
        clampedRange = QCPRange(0, clampedRange.size());
    }

    if (clampedRange.upper >= lastLatestTime - 1e-2) {
        m_autoScroll = true;
    } else {
        m_autoScroll = false;
    }
    
    syncTimeRangeSlider(clampedRange);
    if (m_pagedData) m_offlineViewTimer.start(60);
    
    // Sync all plots
    for (PlotContext &ctx : allPlots) {
        if (ctx.widthText) {
            ctx.widthText->setText(QString("X跨度: %1 s").arg(clampedRange.size(), 0, 'f', 2));
        }
        if (ctx.plot->xAxis->range() != clampedRange) {
            ctx.plot->xAxis->blockSignals(true);
            ctx.plot->xAxis->setRange(clampedRange);
            ctx.plot->xAxis->blockSignals(false);
            ctx.plot->replot(QCustomPlot::rpQueuedReplot);
        }
    }
    // Throttle the heavy Y-axis rescaling
    if (!yScaleTimer->isActive()) {
        yScaleTimer->start(66); // coalesce range changes, keep autoscale active while dragging
    }
}
void MainWindow::autoScaleY()
{
    if (allPlots.isEmpty()) return;
    if (chkAutoScaleY && !chkAutoScaleY->isChecked()) {
        return;
    }
    for (PlotContext &ctx : allPlots) {
        if (!ctx.plot->isVisible() || isMinimized() || ctx.plot->visibleRegion().isEmpty()) continue;
        bool foundData = false;
        double minimum = std::numeric_limits<double>::max(), maximum = -std::numeric_limits<double>::max();
        for (int i = 0; i < ctx.plot->graphCount(); ++i) {
            QCPGraph *g = ctx.plot->graph(i);
            if (g->visible()) {
                bool found = false;
                const auto range = g->getValueRange(found, QCP::sdBoth, ctx.plot->xAxis->range());
                if (found && qIsFinite(range.lower) && qIsFinite(range.upper)) {
                    foundData = true;
                    minimum = qMin(minimum, range.lower); maximum = qMax(maximum, range.upper);
                }
            }
        }
        if (foundData) {
            const double span = qMax(2e-6, maximum - minimum);
            const double center = minimum + (maximum - minimum) * 0.5;
            const QCPRange target(center - span * 0.6, center + span * 0.6);
            const QCPRange previous = ctx.plot->yAxis->range();
            // Retain hysteresis against jitter, but never clip a new extreme.
            if (minimum < previous.lower || maximum > previous.upper ||
                qAbs(previous.lower - target.lower) > span * 0.02 ||
                qAbs(previous.upper - target.upper) > span * 0.02) {
                const QSignalBlocker blocker(ctx.plot->yAxis);
                ctx.plot->yAxis->setRange(target);
            }
        }
        if (ctx.heightText) ctx.heightText->setText(QString("Y跨度: %1").arg(ctx.plot->yAxis->range().size(), 0, 'f', spinDecimals ? spinDecimals->value() : 2));
        ctx.plot->replot(QCustomPlot::rpQueuedReplot);
    }
    syncTimeRangeSlider(allPlots[0].plot->xAxis->range());
}
void MainWindow::moveChannelToNewPlot(int chIdx)
{
    if (chIdx < 0 || chIdx >= channelGraphs.size()) return;
    if (channelToPlotMap[chIdx] != 0) return; // already detached
    QCPGraph *mainGraph = channelGraphs[chIdx];
    PlotContext ctx = createNewPlotContext(QString("独立视图: %1").arg(channelDisplayName(chIdx)), false);
    int newPlotIdx = allPlots.size();
    QDockWidget *prevDock = allPlots.last().dock;
    allPlots.append(ctx);
    if (newPlotIdx > 0 && prevDock) {
        Qt::Orientation splitDir = Qt::Vertical;
        innerMainWindow->splitDockWidget(prevDock, ctx.dock, splitDir);
    } else {
        innerMainWindow->addDockWidget(Qt::BottomDockWidgetArea, ctx.dock);
    }
    auto newGraph = new WaveformGraph(ctx.plot->xAxis, ctx.plot->yAxis);
    newGraph->setChannelIndex(chIdx);
    newGraph->setPen(mainGraph->pen());
    newGraph->setName(mainGraph->name());
    newGraph->setAntialiased(false);
    newGraph->setVisible(true);
    // DO NOT SHARE DATA CONTAINER to avoid QCustomPlot cross-widget memory bugs
    QSharedPointer<QCPGraphDataContainer> dataCopy(new QCPGraphDataContainer());
    dataCopy->set(*mainGraph->data());
    newGraph->setData(dataCopy);
    channelToPlotMap[chIdx] = newPlotIdx;
    ctx.scope->setPreferenceKey("ScopeMeasurements/Views/" + channelSettingKey(chIdx, "Measure"));
    if (plotScrollArea) plotScrollArea->setDetachedViewCount(allPlots.size() - 1);
    
    // Sync X axis with the main view (block signals to avoid re-entrancy crash!)
    ctx.plot->xAxis->blockSignals(true);
    ctx.plot->xAxis->setRange(allPlots[0].plot->xAxis->range());
    ctx.plot->xAxis->blockSignals(false);
    
    // Initial Y axis scale
    newGraph->rescaleValueAxis(false, true);
    QCPRange yRange = ctx.plot->yAxis->range();
    if (yRange.size() < 1e-9) {
        double center = (yRange.upper + yRange.lower) / 2.0;
        ctx.plot->yAxis->setRange(center - 1.0, center + 1.0);
    }
    
    ctx.plot->replot();
    enforcePlotHistoryLimit(true);
    if (m_pagedData) refreshPagedView();
    QTimer::singleShot(10, this, &MainWindow::equalizePlotHeights);
}
void MainWindow::moveChannelToMainPlot(int chIdx)
{
    if (chIdx < 0 || chIdx >= channelGraphs.size()) return;
    const int oldPlotIdx = channelToPlotMap.value(chIdx, 0);
    if (oldPlotIdx <= 0 || oldPlotIdx >= allPlots.size()) return;
    auto &context = allPlots[oldPlotIdx];
    QCustomPlot *oldPlot = context.plot;
    auto graph = WaveformGraph::forChannel(oldPlot, chIdx);
    if (!graph) return;

    const auto belongsToChannel = [&](QCPItemTracer *tracer) {
        return tracer->graph() == graph || (tracer->visible() && m_pagedData && tracer->property("offlineChannel").isValid() && tracer->property("offlineChannel").toInt() == chIdx);
    };
    if (belongsToChannel(context.hoverTracer)) {
        context.hoverTracer->setGraph(nullptr);
        context.hoverTracer->setVisible(false);
        context.hoverText->setVisible(false);
    }
    if (belongsToChannel(context.tracer1) || belongsToChannel(context.tracer2)) {
        context.tracer1->setGraph(nullptr); context.tracer2->setGraph(nullptr);
        context.tracer1->setVisible(false); context.tracer2->setVisible(false);
        context.measureLine->setVisible(false);
        context.measureText->setVisible(false); context.measureText2->setVisible(false);
        context.measureDeltaText->setVisible(false);
        context.measureState = 0;
    }
    // Main graphs retain their history and checkbox state. Remove only the
    // selected detached copy; other curves and their markers stay in place.
    oldPlot->removeGraph(graph);
    channelToPlotMap[chIdx] = 0;
    if (oldPlot->graphCount() == 0) {
        allPlots[oldPlotIdx].dock->close(); // This will trigger onPlotDockClosed
    } else {
        if (context.scope) context.scope->refresh();
        oldPlot->replot();
        updatePlotTitle(oldPlotIdx);
    }
    enforcePlotHistoryLimit(true);
    if (chkAutoScaleY && chkAutoScaleY->isChecked()) autoScaleY();
    QTimer::singleShot(10, this, &MainWindow::equalizePlotHeights);
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Show && qobject_cast<QCustomPlot *>(watched)) {
        for (const auto &ctx : allPlots) if (ctx.plot == watched) {
            ctx.plot->replot(QCustomPlot::rpQueuedReplot);
            if (chkAutoScaleY && chkAutoScaleY->isChecked()) yScaleTimer->start(0);
            break;
        }
    }
    if ((watched == rightSharedContent || watched == rightSharedScrollArea || watched == channelDock) &&
        (event->type() == QEvent::LayoutRequest || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)) {
        requestChannelDockWidthUpdate();
    }
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        QWidget *focusWidget = QApplication::focusWidget();

        if (keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Enter) {
            QAbstractSpinBox *spinBox = nullptr;
            for (QWidget *widget = focusWidget; widget; widget = widget->parentWidget()) {
                spinBox = qobject_cast<QAbstractSpinBox*>(widget);
                if (spinBox) {
                    break;
                }
            }
            if (spinBox) {
                spinBox->interpretText();
                if (focusWidget) {
                    focusWidget->clearFocus();
                }
                spinBox->clearFocus();
                return true;
            }
        }

        const bool isInputWidget =
            focusWidget &&
            (qobject_cast<QLineEdit*>(focusWidget) ||
             qobject_cast<QTextEdit*>(focusWidget) ||
             qobject_cast<QPlainTextEdit*>(focusWidget) ||
             qobject_cast<QComboBox*>(focusWidget) ||
             qobject_cast<QAbstractSpinBox*>(focusWidget));

        if (!isInputWidget && keyEvent->modifiers() == Qt::NoModifier &&
            !keyEvent->isAutoRepeat()) {
            QMenu *targetMenu = nullptr;
            if (keyEvent->key() == Qt::Key_F) {
                targetMenu = m_fileMenu;
            } else if (keyEvent->key() == Qt::Key_S) {
                targetMenu = m_settingsMenu;
            } else if (keyEvent->key() == Qt::Key_V) {
                targetMenu = m_viewMenu;
            } else if (keyEvent->key() == Qt::Key_T) {
                targetMenu = m_toolsMenu;
            } else if (keyEvent->key() == Qt::Key_H) {
                targetMenu = m_helpMenu;
            }

            if (targetMenu) {
                QAction *menuAction = targetMenu->menuAction();
                QRect actionRect = menuBar()->actionGeometry(menuAction);
                targetMenu->popup(menuBar()->mapToGlobal(actionRect.bottomLeft()));
                return true;
            }
        }
    }

    if (watched == progressBarFile && event->type() == QEvent::MouseButtonDblClick) {
        if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton) return true;
        if ((fileSendTimer && fileSendTimer->isActive()) || (bleClient && bleClient->fileSendBusy()) || m_serialSendCancelPending) return true;
        progressBarFile->setValue(0);
        resetFileSendEta();
        setFileSendProgressHint();
        return true;
    }

    if (event->type() == QEvent::Leave) {
        if (watched == spectrumPlot) {
            if (!spectrumTracerFixed) {
                spectrumTracer->setVisible(false);
                spectrumTracerText->setVisible(false);
                spectrumPlot->replot(QCustomPlot::rpQueuedReplot);
            }
        }
    }
    
    if (event->type() == QEvent::Resize) {
        if (watched == spectrumFooter) {
            updateSpectrumFooterLayout();
        }
        if (QDockWidget *dock = qobject_cast<QDockWidget*>(watched)) {
            if (!dock->isFloating()) {
                dock->setProperty("dockedSize", dock->size());
            }
        }
    }

    if (event->type() == QEvent::WindowStateChange) {
        if (QDockWidget *dock = qobject_cast<QDockWidget*>(watched)) {
            if (QToolButton *btnMax = dock->findChild<QToolButton*>("DockMaxBtn")) {
                btnMax->setText(dock->isMaximized() ? QString::fromUtf8("❐") : QString::fromUtf8("☐"));
            }
        }
    }
    
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (watched->objectName() == "customTitleBar") {
            QWidget *titleBar = qobject_cast<QWidget*>(watched);
            if (titleBar && titleBar->parentWidget()) {
                QDockWidget *dock = qobject_cast<QDockWidget*>(titleBar->parentWidget());
                if (dock && dock->isFloating()) {
                    if (dock->isMaximized()) {
                        dock->showNormal();
                    } else {
                        dock->showMaximized();
                    }
                    return true;
                }
            }
        }
    }
    if (event->type() == QEvent::ContextMenu) {
        if (QString(watched->metaObject()->className()) == "QTabBar") {
            QTabBar *tabBar = qobject_cast<QTabBar*>(watched);
            QContextMenuEvent *cme = static_cast<QContextMenuEvent*>(event);
            if (tabBar && cme) {
                int index = tabBar->tabAt(cme->pos());
                if (index >= 0) {
                    QString tabText = tabBar->tabText(index);
                    QMenu menu(this);
                    QAction *restoreAct = menu.addAction(QString("单独还原 '%1' 选项卡").arg(tabText));
                    QAction *res = menu.exec(cme->globalPos());
                    if (res == restoreAct) {
                        QList<QDockWidget*> allDocks = { configDock, consoleDock, spectrumDock, measureDock, channelDock };
                        for (QDockWidget *dock : allDocks) {
                            if (dock->windowTitle() == tabText) {
                                Qt::DockWidgetArea area = Qt::RightDockWidgetArea;
                                if (dock == configDock) area = Qt::LeftDockWidgetArea;
                                else if (dock == consoleDock || dock == measureDock || dock == spectrumDock) area = Qt::BottomDockWidgetArea;
                                else if (dock == channelDock) area = Qt::RightDockWidgetArea;
                                
                                this->addDockWidget(area, dock);
                                dock->show();
                                if (dock->widget()) dock->widget()->show();
                                break;
                            }
                        }
                    }
                    return true;
                }
            }
        }
    }
    
    if (event->type() == QEvent::Leave) {
        QCustomPlot *plot = qobject_cast<QCustomPlot*>(watched);
        if (plot) {
            for (auto &ctx : allPlots) {
                if (ctx.plot == plot) {
                    if (ctx.hoverTracer && ctx.hoverTracer->visible()) ctx.hoverTracer->setVisible(false);
                    if (ctx.hoverText && ctx.hoverText->visible()) ctx.hoverText->setVisible(false);
                    plot->replot(QCustomPlot::rpQueuedReplot);
                    break;
                }
            }
        }
    }
    if (event->type() == QEvent::Wheel) {
        QCustomPlot *plot = qobject_cast<QCustomPlot*>(watched);
        if (plot) {
            QWheelEvent *wheel = static_cast<QWheelEvent*>(event);
            if (wheel->modifiers() & Qt::ControlModifier) {
                if (plot != spectrumPlot && chkAutoScaleY && chkAutoScaleY->isChecked()) {
                    return true; // Ignore manual Y-zoom if auto scale is on (main plot only)
                }
                plot->axisRect()->setRangeZoom(Qt::Vertical);
                double overallMin = std::numeric_limits<double>::max();
                double overallMax = -std::numeric_limits<double>::max();
                bool foundValid = false;
                for (int i = 0; i < plot->graphCount(); ++i) {
                    if (plot->graph(i)->visible()) {
                        bool found;
                        QCPRange range = plot->graph(i)->getValueRange(found, QCP::sdBoth, plot->xAxis->range());
                        if (found) {
                            foundValid = true;
                            if (range.lower < overallMin) overallMin = range.lower;
                            if (range.upper > overallMax) overallMax = range.upper;
                        }
                    }
                }
                double center;
                if (foundValid) {
                    center = (overallMin + overallMax) / 2.0;
                } else {
                    center = plot->yAxis->range().center();
                }
                double factor = (wheel->angleDelta().y() > 0) ? 0.85 : 1.15;
                plot->yAxis->scaleRange(factor, center);
                for (PlotContext &ctx : allPlots) {
                    if (ctx.plot == plot && ctx.heightText) {
                        ctx.heightText->setText(QString("Y跨度: %1").arg(plot->yAxis->range().size(), 0, 'f', spinDecimals ? spinDecimals->value() : 2));
                        break;
                    }
                }
                plot->replot(QCustomPlot::rpQueuedReplot);
                return true;
            } else {
                plot->axisRect()->setRangeZoom(Qt::Horizontal);
            }
        }
    } else if (event->type() == QEvent::NonClientAreaMouseButtonDblClick || event->type() == QEvent::MouseButtonDblClick) {
        QDockWidget *dock = qobject_cast<QDockWidget*>(watched);
        if (dock && dock->isFloating()) {
            if (dock->isMaximized()) {
                dock->showNormal();
            } else {
                dock->showMaximized();
            }
            return true;
        }
    } else if (event->type() == QEvent::Close) {
        QDockWidget *dock = qobject_cast<QDockWidget*>(watched);
        if (dock) {
            bool isPlot = false;
            for (int i = 0; i < allPlots.size(); ++i) {
                if (allPlots[i].dock == dock) {
                    isPlot = true;
                    break;
                }
            }
            if (isPlot) {
                onPlotDockClosedEvent(dock);
                event->ignore();
                return true;
            } else {
                if (dock->isFloating()) {
                    dock->setFloating(false);
                    event->ignore();
                    return true;
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onMergeAllViews()
{
    // Iterate backwards and close all separated docks
    for (int i = allPlots.size() - 1; i > 0; --i) {
        QDockWidget *dockToClose = allPlots[i].dock;
        onPlotDockClosedEvent(dockToClose);
    }
}
void MainWindow::onPlotDockClosedEvent(QDockWidget *closedDock)
{
    if (!closedDock) return;
    int plotIdx = -1;
    for (int i = 0; i < allPlots.size(); ++i) {
        if (allPlots[i].dock == closedDock) {
            plotIdx = i;
            break;
        }
    }
    if (plotIdx <= 0) return; // 0 is main view or not found
    // Move all graphs back to plot 0
    QCustomPlot *oldPlot = allPlots[plotIdx].plot;
    QCustomPlot *mainPlot = allPlots[0].plot;
    QList<int> channelsToMove;
    for (auto it = channelToPlotMap.begin(); it != channelToPlotMap.end(); ++it) {
        if (it.value() == plotIdx) {
            channelsToMove.append(it.key());
        }
    }
    // Just update the map. mainGraph is already in mainPlot and receiving data.
    for (int chIdx : channelsToMove) {
        channelToPlotMap[chIdx] = 0;
    }
    if (!channelsToMove.isEmpty()) {
        mainPlot->replot();
    }
    // Remove synchronously so its old minimum height cannot linger until the
    // deferred QObject deletion. The scroll viewport, not outer docks, shrinks.
    innerMainWindow->removeDockWidget(closedDock);
    closedDock->hide();
    closedDock->deleteLater();
    allPlots.removeAt(plotIdx);
    // Update map indices for shifted plots
    for (auto it = channelToPlotMap.begin(); it != channelToPlotMap.end(); ++it) {
        if (it.value() > plotIdx) {
            it.value() = it.value() - 1;
        }
    }
    if (plotScrollArea) plotScrollArea->setDetachedViewCount(allPlots.size() - 1);
    enforcePlotHistoryLimit(true);
    QTimer::singleShot(0, this, &MainWindow::equalizePlotHeights);
}
void MainWindow::onChannelContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = listChannels->itemAt(pos);
    if (!item) return;
    int chIdx = listChannels->row(item);
    QMenu menu(this);
    if (channelToPlotMap[chIdx] == 0) {
        QAction *actMove = menu.addAction("分离到新视图");
        connect(actMove, &QAction::triggered, this, [this, chIdx]() {
            moveChannelToNewPlot(chIdx);
        });
    } else {
        QAction *actMove = menu.addAction("合并回主视图");
        connect(actMove, &QAction::triggered, this, [this, chIdx]() {
            moveChannelToMainPlot(chIdx);
        });
    }
    menu.exec(listChannels->mapToGlobal(pos));
}

void MainWindow::leaveImportedDataMode()
{
    m_offlineViewTimer.stop();
    for (const auto &context : allPlots) for (int ch = 0; ch < context.plot->graphCount(); ++ch)
        if (auto graph = dynamic_cast<WaveformGraph *>(context.plot->graph(ch))) graph->clearOfflineSource();
    m_pagedData.clear(); m_pagedRaw.clear();
    const bool hadImportedNames = m_isImportedData;
    m_isImportedData = false;
    m_importedChannelNames.clear();
    m_importedTimes.clear();
    m_importedData.clear();
    if (hadImportedNames) {
        for (int i = 0; i < channelGraphs.size(); ++i) {
            const auto name = channelDisplayName(i);
            if (channelGraphs[i]) channelGraphs[i]->setName(name);
            if (i < listChannels->count()) if (auto widget = listChannels->itemWidget(listChannels->item(i)))
                if (auto check = widget->findChild<QCheckBox *>("chkChannel")) check->setText(name);
            if (comboMeasureChannel && i < comboMeasureChannel->count()) comboMeasureChannel->setItemText(i, name);
            for (auto action : menuSpectrumChannels->actions()) if (action->data().toInt() == i) action->setText(name);
        }
        for (int i = 0; i < allPlots.size(); ++i) updatePlotTitle(i);
    }
}

void MainWindow::onRequestClearPlots()
{
    QDialog dialog(this);
    dialog.setObjectName("clearPlotsConfirmDialog");
    dialog.setWindowTitle(QStringLiteral("清空波形"));
    auto layout = new QVBoxLayout(&dialog);
    auto message = new QLabel(QStringLiteral("确定清空所有视图中的历史波形和测量结果吗？\n\n此操作无法撤销。已保存的数据文件不受影响，当前连接和录制将继续。"));
    message->setWordWrap(true);
    layout->addWidget(message);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定清空"));
    buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setFocus();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(qMax(500, dialog.sizeHint().width()), dialog.sizeHint().height());
    if (dialog.exec() == QDialog::Accepted) onClearPlots();
}

void MainWindow::onClearPlots()
{
    leaveImportedDataMode();

    for (int i = 0; i < channelGraphs.size(); ++i) {
        if (channelGraphs[i]) {
            channelGraphs[i]->data()->clear();
        }
    }
    for (int i = 0; i < channelBuffers.size(); ++i) {
        channelBuffers[i].clear();
    }
    timeBuffer.clear();
    lastLatestTime = 0.0;
    sampleCount = 0;
    m_nextPlotTime = 0.0;
    frameCountForRate = 0;
    
    for (PlotContext &ctx : allPlots) {
        if (ctx.plot) {
            if (ctx.scope) ctx.scope->resetData();
            ctx.measureState = 0;
            for (auto tracer : {ctx.tracer1, ctx.tracer2, ctx.hoverTracer}) if (tracer) {
                tracer->setVisible(false);
                tracer->setGraph(nullptr);
            }
            if (ctx.measureLine) ctx.measureLine->setVisible(false);
            for (auto text : {ctx.hoverText, ctx.measureText, ctx.measureText2, ctx.measureDeltaText, ctx.heightText, ctx.widthText})
                if (text) text->setVisible(false);
            for (int g = 0; g < ctx.plot->graphCount(); ++g) {
                if (ctx.plot->graph(g)) {
                    ctx.plot->graph(g)->data()->clear();
                }
            }
            if (ctx.ticker) ctx.ticker->setLatestTime(0);
            ctx.plot->xAxis->setRange(-10, 0);
            ctx.plot->replot();
        }
    }

    // Clear delay history while retaining the user's enabled filters and coefficients.
    resetFilters();
    if (!allPlots.isEmpty()) syncTimeRangeSlider(allPlots[0].plot->xAxis->range());
    enforcePlotHistoryLimit(true);
}
void MainWindow::equalizePlotHeights()
{
    bool measurements = false;
    for (const auto &context : allPlots) if (context.scope && context.scope->isEnabled()) measurements = true;
    if (plotScrollArea) plotScrollArea->setMeasurementBarsVisible(measurements);
    if (plotScrollArea) plotScrollArea->setDetachedViewCount(allPlots.size() - 1);
    QList<QDockWidget*> docks;
    for (const PlotContext &ctx : allPlots) {
        if (ctx.dock) {
            docks.append(ctx.dock);
        }
    }
    if (docks.isEmpty()) return;
    QList<int> sizes;
    int h = innerMainWindow->height() / docks.size();
    for (int i = 0; i < docks.size(); ++i) {
        sizes.append(h);
    }
    innerMainWindow->resizeDocks(docks, sizes, Qt::Vertical);
}






void MainWindow::onPlotMouseDoubleClick(QMouseEvent *event)
{
    QCustomPlot *clickedPlot = qobject_cast<QCustomPlot*>(sender());
    if (!clickedPlot) return;
    
    if (event->button() != Qt::LeftButton) return;
    
    PlotContext *ctxPtr = nullptr;
    for (int i = 0; i < allPlots.size(); ++i) {
        if (allPlots[i].plot == clickedPlot) {
            ctxPtr = &allPlots[i];
            break;
        }
    }
    if (!ctxPtr) return;
    PlotContext &ctx = *ctxPtr;
    
    if (ctx.measureState == 2) {
        ctx.tracer1->setVisible(false);
        ctx.tracer2->setVisible(false);
        ctx.measureLine->setVisible(false);
        ctx.measureText->setVisible(false);
        ctx.measureText2->setVisible(false);
        if (ctx.measureDeltaText) ctx.measureDeltaText->setVisible(false);
        ctx.measureState = 0;
        clickedPlot->replot();
        return;
    }
    
    double x = clickedPlot->xAxis->pixelToCoord(event->pos().x());
    
    QCPGraph *bestGraph = nullptr;
    double bestX = 0, bestY = 0;
    double minDistance = std::numeric_limits<double>::max();
    
    for (int i = 0; i < clickedPlot->graphCount(); ++i) {
        QCPGraph *g = clickedPlot->graph(i);
        if (!g->visible()) continue;
        
        if (auto indexed = dynamic_cast<WaveformGraph *>(g)) if (indexed->hasOfflineSource()) {
            QVector<QCPGraphData> points; QString error;
            if (indexed->nearbySamples(x, points, error)) for (const auto &point : points) {
                if (std::isfinite(point.value) && std::abs(point.key - x) < minDistance) {
                    minDistance = std::abs(point.key - x); bestGraph = g; bestX = point.key; bestY = point.value;
                }
            }
            continue;
        }
        auto iter = g->data()->findBegin(x, true);
        if (iter != g->data()->constEnd()) {
            double distance = qAbs(iter->key - x);
            if (distance < minDistance) {
                minDistance = distance;
                bestGraph = g;
                bestX = iter->key;
                bestY = iter->value;
            }
            auto nextIter = iter + 1;
            if (nextIter != g->data()->constEnd()) {
                double dist2 = qAbs(nextIter->key - x);
                if (dist2 < minDistance) {
                    minDistance = dist2;
                    bestGraph = g;
                    bestX = nextIter->key;
                    bestY = nextIter->value;
                }
            }
        }
    }
    
    if (bestGraph) {
        if (ctx.measureState == 0) {
            ctx.tracer1->setGraph(bestGraph);
            ctx.tracer1->setGraphKey(bestX);
            if (auto indexed = dynamic_cast<WaveformGraph *>(bestGraph)) if (indexed->hasOfflineSource()) { ctx.tracer1->setProperty("offlineChannel", indexed->channelIndex()); ctx.tracer1->setGraph(nullptr); ctx.tracer1->position->setCoords(bestX, bestY); }
            ctx.tracer1->setVisible(true);
            ctx.tracer2->setVisible(false);
            ctx.measureLine->setVisible(false);
            int decimals = spinDecimals ? spinDecimals->value() : 4;
            ctx.measureText->setText(QString("X: %1\nY: %2").arg(bestX - lastLatestTime, 0, 'f', decimals).arg(bestY, 0, 'f', decimals));
            ctx.measureText->setVisible(true);
            ctx.measureText2->setVisible(false);
            ctx.measureDeltaText->setVisible(false);
            ctx.measureState = 1;
        } else if (ctx.measureState == 1) {
            ctx.tracer2->setGraph(bestGraph);
            ctx.tracer2->setGraphKey(bestX);
            if (auto indexed = dynamic_cast<WaveformGraph *>(bestGraph)) if (indexed->hasOfflineSource()) { ctx.tracer2->setProperty("offlineChannel", indexed->channelIndex()); ctx.tracer2->setGraph(nullptr); ctx.tracer2->position->setCoords(bestX, bestY); }
            ctx.tracer2->setVisible(true);
            
            ctx.measureLine->start->setCoords(ctx.tracer1->position->key(), ctx.tracer1->position->value());
            ctx.measureLine->end->setCoords(bestX, bestY);
            ctx.measureLine->setVisible(true);
            
            double dx = bestX - ctx.tracer1->position->key();
            double dy = bestY - ctx.tracer1->position->value();
            
            int decimals = spinDecimals ? spinDecimals->value() : 4;
            ctx.measureText2->setText(QString("X: %1\nY: %2").arg(bestX - lastLatestTime, 0, 'f', decimals).arg(bestY, 0, 'f', decimals));
            ctx.measureText2->setVisible(true);
            
            ctx.measureDeltaText->setText(QString("Δt: %1 s\nΔy: %2").arg(qAbs(dx), 0, 'f', decimals).arg(qAbs(dy), 0, 'f', decimals));
            ctx.measureDeltaText->setVisible(true);
            
            ctx.measureState = 2;
        }
        clickedPlot->replot();
    }
}

void MainWindow::onPlotContextMenu(const QPoint &pos)
{
    QCustomPlot *clickedPlot = qobject_cast<QCustomPlot*>(sender());
    if (!clickedPlot) return;
    
    // Find the plot index
    int targetPlotIdx = -1;
    for (int i = 0; i < allPlots.size(); ++i) {
        if (allPlots[i].plot == clickedPlot) {
            targetPlotIdx = i;
            break;
        }
    }
    if (targetPlotIdx < 0) return;
    
    QMenu menu(this);
    if (allPlots[targetPlotIdx].scope) allPlots[targetPlotIdx].scope->appendMenu(&menu);
    if (targetPlotIdx == 0) {
        menu.addSeparator();
        menu.setToolTipsVisible(true);
        auto backgroundAction = menu.addAction(QStringLiteral("绘图背景颜色…"));
        backgroundAction->setObjectName("plotBackgroundAction");
        connect(backgroundAction, &QAction::triggered, this, &MainWindow::showPlotBackgroundSettings);
        menu.addSeparator();
        auto importAction = menu.addAction(QStringLiteral("导入数据…"));
        importAction->setObjectName("plotImportAction");
        importAction->setToolTip(QStringLiteral("导入 CSV 或 WLR 数据进行离线分析，与数据导入按钮相同。"));
        connect(importAction, &QAction::triggered, this, &MainWindow::onImportData);
        auto exportAction = menu.addAction(QStringLiteral("导出当前波形…"));
        exportAction->setObjectName("plotExportAction");
        exportAction->setToolTip(QStringLiteral("导出所有通道当前保留的波形数据为 CSV，与右侧导出按钮相同。"));
        connect(exportAction, &QAction::triggered, this, &MainWindow::onExportCache);
        menu.addSeparator();
        auto clearAction = menu.addAction(QStringLiteral("清空波形…"));
        clearAction->setObjectName("plotClearAction");
        clearAction->setToolTip(QStringLiteral("确认后清空所有视图中保留的历史波形和测量结果。"));
        connect(clearAction, &QAction::triggered, this, &MainWindow::onRequestClearPlots);
        menu.exec(clickedPlot->mapToGlobal(pos));
        return;
    }
    menu.addSeparator();
    QAction *mergeAction = menu.addAction(QStringLiteral("合并回主视图"));
    QPointer<QDockWidget> targetDock = allPlots[targetPlotIdx].dock;
    connect(mergeAction, &QAction::triggered, this, [this, targetDock]() {
        // Closing a detached view already moves all of its channel mappings
        // back to the retained main graphs. Never capture a mutable plot index.
        if (targetDock) targetDock->close();
    });
    menu.addSeparator();
    QMenu *addMenu = menu.addMenu("添加通道显示");
    QMenu *removeMenu = menu.addMenu("移除通道显示");
    
    QList<int> channelsInThisPlot;
    QList<int> channelsInOtherPlots;
    for (auto it = channelToPlotMap.begin(); it != channelToPlotMap.end(); ++it) {
        if (it.value() == targetPlotIdx) {
            channelsInThisPlot.append(it.key());
        } else {
            channelsInOtherPlots.append(it.key());
        }
    }
    
    for (int chIdx : channelsInOtherPlots) {
        QAction *act = addMenu->addAction(channelDisplayName(chIdx));
        QPointer<QCustomPlot> targetPlot = allPlots[targetPlotIdx].plot;
        connect(act, &QAction::triggered, this, [this, chIdx, targetPlot]() {
            if (!targetPlot || chIdx < 0 || chIdx >= channelGraphs.size()) return;
            QCPGraph *oldGraph = channelGraphs[chIdx];
            if (channelToPlotMap.value(chIdx, 0) != 0) {
                moveChannelToMainPlot(chIdx);
                if (channelToPlotMap.value(chIdx, 0) != 0) return;
            }
            
            int currentTargetIdx = -1;
            for (int i = 1; i < allPlots.size(); ++i) {
                if (allPlots[i].plot == targetPlot) {
                    currentTargetIdx = i;
                    break;
                }
            }
            if (currentTargetIdx < 1) return;
            QCustomPlot *newPlot = targetPlot;
            auto newGraph = new WaveformGraph(newPlot->xAxis, newPlot->yAxis);
            newGraph->setChannelIndex(chIdx);
            newGraph->setPen(oldGraph->pen());
            newGraph->setName(oldGraph->name());
            newGraph->setAntialiased(false);
            newGraph->setVisible(true);
            QSharedPointer<QCPGraphDataContainer> dataCopy(new QCPGraphDataContainer());
            dataCopy->set(*oldGraph->data());
            newGraph->setData(dataCopy);
            
            channelToPlotMap[chIdx] = currentTargetIdx;
            if (allPlots[currentTargetIdx].scope) allPlots[currentTargetIdx].scope->refresh();
            
            newPlot->replot();
            updatePlotTitle(currentTargetIdx);
            enforcePlotHistoryLimit(true);
            if (m_pagedData) refreshPagedView();
        });
    }
    
    for (int chIdx : channelsInThisPlot) {
        QAction *act = removeMenu->addAction(channelDisplayName(chIdx));
        QPointer<QCustomPlot> targetPlot = clickedPlot;
        connect(act, &QAction::triggered, this, [this, chIdx, targetPlot]() {
            moveChannelToMainPlot(chIdx);
            if (!targetPlot) return;
            for (int i = 1; i < allPlots.size(); ++i) {
                if (allPlots[i].plot == targetPlot) {
                    updatePlotTitle(i);
                    break;
                }
            }
        });
    }
    
    menu.exec(clickedPlot->mapToGlobal(pos));
}

void MainWindow::updatePlotTitle(int plotIdx)
{
    if (plotIdx <= 0 || plotIdx >= allPlots.size()) return;
    
    PlotContext &ctx = allPlots[plotIdx];
    if (!ctx.dock) return;
    
    QStringList chNames;
    for (auto it = channelToPlotMap.begin(); it != channelToPlotMap.end(); ++it) {
        if (it.value() == plotIdx) {
            chNames << channelDisplayName(it.key());
        }
    }
    if (chNames.isEmpty()) {
        ctx.dock->setWindowTitle("独立视图: (无)");
    } else {
        ctx.dock->setWindowTitle(QString("独立视图: %1").arg(chNames.join(", ")));
    }
}

void MainWindow::onPlotMouseMove(QMouseEvent *event)
{
    if (!chkPausePlot->isChecked() && isPortOpen) return; // Only hover snap when paused or disconnected
    if (event->buttons() != Qt::NoButton) return; // never scan samples while panning
    if (m_plotHoverClock.isValid() && m_plotHoverClock.elapsed() < 33) return;
    m_plotHoverClock.start();
    
    QCustomPlot *plot = qobject_cast<QCustomPlot*>(sender());
    if (!plot) return;
    
    PlotContext *ctxPtr = nullptr;
    for (int i = 0; i < allPlots.size(); ++i) {
        if (allPlots[i].plot == plot) {
            ctxPtr = &allPlots[i];
            break;
        }
    }
    if (!ctxPtr) return;
    PlotContext &ctx = *ctxPtr;
    
    double clickX = event->pos().x();
    double clickY = event->pos().y();
    
    QCPGraph *bestGraph = nullptr;
    double bestDistSq = 1e12;
    double bestX = 0, bestY = 0;
    
    double searchXMin = plot->xAxis->pixelToCoord(clickX - 20);
    double searchXMax = plot->xAxis->pixelToCoord(clickX + 20);
    if (searchXMin > searchXMax) qSwap(searchXMin, searchXMax);
    
    for (int i = 0; i < plot->graphCount(); ++i) {
        QCPGraph *g = plot->graph(i);
        if (!g->visible()) continue;
        
        auto source = g->data();
        if (auto indexed = dynamic_cast<WaveformGraph *>(g)) if (indexed->hasOfflineSource()) {
            QVector<QCPGraphData> points; QString error;
            if (!indexed->readExact(searchXMin, searchXMax, 50000, points, error))
                indexed->nearbySamples(plot->xAxis->pixelToCoord(clickX), points, error);
            source.reset(new QCPGraphDataContainer()); source->set(points, true);
        }
        auto it = source->findBegin(searchXMin);
        const auto endIt = source->findEnd(searchXMax);
        for (; it != endIt; ++it) {
            if (!qIsFinite(it->value)) continue;
            double px = plot->xAxis->coordToPixel(it->key);
            double py = plot->yAxis->coordToPixel(it->value);
            double distSq = (px - clickX) * (px - clickX) + (py - clickY) * (py - clickY);
            
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                bestX = it->key;
                bestY = it->value;
                bestGraph = g;
            }
            
        }
    }
    
    if (bestGraph) {
        ctx.hoverTracer->setGraph(bestGraph);
        ctx.hoverTracer->setGraphKey(bestX);
        if (auto indexed = dynamic_cast<WaveformGraph *>(bestGraph)) if (indexed->hasOfflineSource()) { ctx.hoverTracer->setProperty("offlineChannel", indexed->channelIndex()); ctx.hoverTracer->setGraph(nullptr); ctx.hoverTracer->position->setCoords(bestX, bestY); }
        if (!ctx.hoverTracer->visible()) {
            ctx.hoverTracer->setVisible(true);
        }
        if (ctx.hoverText) {
            ctx.hoverText->setText(QString("X: %1\nY: %2").arg(bestX - lastLatestTime, 0, 'f', spinDecimals ? spinDecimals->value() : 4).arg(bestY, 0, 'f', spinDecimals ? spinDecimals->value() : 4));
            if (!ctx.hoverText->visible()) {
                ctx.hoverText->setVisible(true);
            }
        }
    } else {
        if (ctx.hoverTracer->visible()) {
            ctx.hoverTracer->setVisible(false);
        }
        if (ctx.hoverText && ctx.hoverText->visible()) {
            ctx.hoverText->setVisible(false);
        }
    }
    plot->replot(QCustomPlot::rpQueuedReplot);
}
