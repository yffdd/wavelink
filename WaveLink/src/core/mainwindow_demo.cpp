#include "mainwindow.h"
#include "connectionhints.h"
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

void MainWindow::setupDemoDataPanel()
{
    wDemo = new QWidget();
    wDemo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFormLayout *demoLayout = new QFormLayout(wDemo);
    demoLayout->setContentsMargins(0, 0, 0, 0);
    demoLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    comboDemoChannels = new QComboBox();
    comboDemoChannels->setObjectName("comboDemoChannels");
    for(int i = 1; i <= 8; ++i) {
        comboDemoChannels->addItem(QString("%1 通道").arg(i));
    }

    comboDemoProtocol = new QComboBox();
    comboDemoProtocol->addItem("FireWater");
    comboDemoProtocol->addItem("JustFloat");

    spinDemoInterval = new QSpinBox();
    spinDemoInterval->setObjectName("spinDemoInterval");
    spinDemoInterval->setRange(1, 1000);
    spinDemoInterval->setValue(4);
    spinDemoInterval->setSuffix(" ms");

    comboDemoType = new QComboBox();
    comboDemoType->addItems({"正弦波 (Sine)", "三角波 (Triangle)", "方波 (Square)", "脉冲 (Pulse)", 
                             "心电 (ECG)", "肌电 (EMG)", "脑电 (EEG)", "眼电 (EOG)", "脉搏波 (PPG)", "脑血流 (CBF)"});

    spinDemoFreq = new QDoubleSpinBox(); spinDemoFreq->setRange(0.01, 1000.0); spinDemoFreq->setValue(1.0); spinDemoFreq->setSuffix(" Hz"); spinDemoFreq->setSingleStep(0.1);
    spinDemoFreq->setObjectName("spinDemoFreq");
    spinDemoVpp = new QDoubleSpinBox(); spinDemoVpp->setRange(0.01, 1000.0); spinDemoVpp->setValue(100.0); spinDemoVpp->setSuffix("");
    spinDemoOffset = new QDoubleSpinBox(); spinDemoOffset->setRange(-1000.0, 1000.0); spinDemoOffset->setValue(0.0); spinDemoOffset->setSuffix("");
    spinDemoDuty = new QDoubleSpinBox(); spinDemoDuty->setRange(0.1, 99.9); spinDemoDuty->setValue(50.0); spinDemoDuty->setSuffix(" %");
    spinDemoPulseWidth = new QDoubleSpinBox(); spinDemoPulseWidth->setRange(0.1, 1000.0); spinDemoPulseWidth->setValue(10.0); spinDemoPulseWidth->setSuffix(" ms");
    spinDemoNoise = new QDoubleSpinBox(); spinDemoNoise->setRange(0.0, 1000.0); spinDemoNoise->setValue(0.0); spinDemoNoise->setSuffix("");

    demoLayout->addRow("波形类型", comboDemoType);
    demoLayout->addRow("通道数", comboDemoChannels);
    demoLayout->addRow("封装协议", comboDemoProtocol);
    demoLayout->addRow("采样间隔", spinDemoInterval);
    demoLayout->addRow("频率", spinDemoFreq);
    demoLayout->addRow("峰峰值", spinDemoVpp);
    demoLayout->addRow("占空比", spinDemoDuty);
    demoLayout->addRow("脉宽", spinDemoPulseWidth);
    demoLayout->addRow("直流偏移", spinDemoOffset);
    demoLayout->addRow("噪声水平", spinDemoNoise);
    ConnectionHints::row(demoLayout, comboDemoType, "选择软件生成的模拟波形，不是实际设备测量数据。");
    ConnectionHints::row(demoLayout, comboDemoChannels, "模拟数据的通道数量，无需连接硬件。");
    ConnectionHints::row(demoLayout, comboDemoProtocol, "选择模拟数据的封装格式，不改变真实设备的协议。");
    ConnectionHints::row(demoLayout, spinDemoInterval, "相邻模拟采样点的时间间隔；采样率为 1000 ÷ 间隔毫秒数。");
    ConnectionHints::row(demoLayout, spinDemoFreq, "模拟信号的频率，单位 Hz；含义随所选波形类型变化。");
    ConnectionHints::row(demoLayout, spinDemoVpp, "模拟信号的幅度设置；单位及具体含义随波形类型显示。");
    ConnectionHints::row(demoLayout, spinDemoDuty, "方波一个周期内高电平所占的百分比。");
    ConnectionHints::row(demoLayout, spinDemoPulseWidth, "脉冲保持的时长，单位毫秒。");
    ConnectionHints::row(demoLayout, spinDemoOffset, "叠加到模拟波形上的直流偏移量。");
    ConnectionHints::row(demoLayout, spinDemoNoise, "叠加到模拟波形上的随机噪声幅度；0 表示不叠加。");

    // Initial state
    spinDemoDuty->hide();
    demoLayout->labelForField(spinDemoDuty)->hide();
    spinDemoPulseWidth->hide();
    demoLayout->labelForField(spinDemoPulseWidth)->hide();

    connect(comboDemoType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, demoLayout](int index){
        bool showDuty = (index == DemoDataGenerator::Square);
        bool showPulse = (index == DemoDataGenerator::Pulse);
        bool isPhys = (index >= DemoDataGenerator::ECG && index <= DemoDataGenerator::CBF);
        
        spinDemoDuty->setVisible(showDuty);
        demoLayout->labelForField(spinDemoDuty)->setVisible(showDuty);
        
        spinDemoPulseWidth->setVisible(showPulse);
        demoLayout->labelForField(spinDemoPulseWidth)->setVisible(showPulse);
        
        if (isPhys) {
            spinDemoVpp->setSuffix(" uV");
            spinDemoOffset->setSuffix(" uV");
            spinDemoNoise->setSuffix(" uV");
            if (QLabel *lbl = qobject_cast<QLabel*>(demoLayout->labelForField(spinDemoNoise))) {
                lbl->setText(QString::fromUtf8("50Hz 干扰"));
            }
            
            if (index == DemoDataGenerator::ECG) {
                spinDemoFreq->setValue(1.2);
                spinDemoVpp->setValue(1500.0);
            } else if (index == DemoDataGenerator::EEG) {
                spinDemoFreq->setValue(10.0);
                spinDemoVpp->setValue(50.0);
            } else if (index == DemoDataGenerator::EMG) {
                spinDemoFreq->setValue(0.5);
                spinDemoVpp->setValue(800.0);
            } else if (index == DemoDataGenerator::EOG) {
                spinDemoFreq->setValue(0.3);
                spinDemoVpp->setValue(500.0);
            } else if (index == DemoDataGenerator::PPG) {
                spinDemoFreq->setValue(1.2);
                spinDemoVpp->setValue(1000.0);
            } else if (index == DemoDataGenerator::CBF) {
                spinDemoFreq->setValue(1.2);
                spinDemoVpp->setValue(400.0);
            }
        } else {
            spinDemoVpp->setSuffix("");
            spinDemoOffset->setSuffix("");
            spinDemoNoise->setSuffix("");
            if (QLabel *lbl = qobject_cast<QLabel*>(demoLayout->labelForField(spinDemoNoise))) {
                lbl->setText("噪声水平");
            }
            
            spinDemoFreq->setValue(1.0);
            spinDemoVpp->setValue(100.0);
        }
    });

    demoTimer = new QTimer(this);
    demoTimer->setObjectName("demoTimer");
    m_demoStatusMonitor->setTextTransport(false,false);
    connect(demoTimer, &QTimer::timeout, this, &MainWindow::onDemoTimerTick);
    demoTimeCounter = 0.0;
}

void MainWindow::onDemoTimerTick()
{
    qint64 currentMs = QDateTime::currentMSecsSinceEpoch();
    qint64 lastMs = demoTimer->property("lastTickMs").toLongLong();
    if (lastMs == 0) lastMs = currentMs - 20; // assumed 20ms for first tick
    qint64 elapsed = currentMs - lastMs;
    if (elapsed > 100) elapsed = 100; // clamp max lag to 100ms
    demoTimer->setProperty("lastTickMs", currentMs);
    
    int intervalMs = spinDemoInterval->value();
    if (intervalMs <= 0) intervalMs = 1;
    // Demo owns an exact sampling clock. Keep the shared sampling-rate setting
    // synchronized so plotting, FFT, filters and recording all use that clock.
    const double demoSampleRate = 1000.0 / intervalMs;
    if (spinFs && spinFs->value() != demoSampleRate) {
        spinFs->setValue(demoSampleRate);
    }
    
    double residual = demoTimer->property("residualMs").toDouble();
    double totalTimeToSimulate = elapsed + residual;
    
    int samplesToGenerate = static_cast<int>(totalTimeToSimulate / intervalMs);
    demoTimer->setProperty("residualMs", totalTimeToSimulate - samplesToGenerate * intervalMs);
    
    if (samplesToGenerate <= 0) return;
    
    double dt = intervalMs / 1000.0;

    DemoDataGenerator::Parameters params;
    params.frequency = spinDemoFreq->value();
    params.vpp = spinDemoVpp->value();
    params.offset = spinDemoOffset->value();
    params.dutyCycle = spinDemoDuty->value();
    params.pulseWidth = spinDemoPulseWidth->value();
    params.noiseLevel = spinDemoNoise->value();

    DemoDataGenerator::WaveType type = static_cast<DemoDataGenerator::WaveType>(comboDemoType->currentIndex());
    
    int numChannels = comboDemoChannels->currentIndex() + 1;
    bool isFireWater = (comboDemoProtocol->currentIndex() == 0);
    
    QByteArray packet;
    
    for (int s = 0; s < samplesToGenerate; ++s) {
        demoTimeCounter += dt;
        
        QVector<float> channelData;
        for (int i = 0; i < numChannels; ++i) {
            // slight phase offset for multi-channel visual separation
            double chVal = DemoDataGenerator::generate(type, demoTimeCounter + i * 0.05, params) * (1.0 - i * 0.05);
            channelData.append(static_cast<float>(chVal));
        }
        
        if (isFireWater) { // FireWater
            QStringList parts;
            for (float val : channelData) {
                parts.append(QString::number(val, 'f', 6));
            }
            packet.append(parts.join(",").toUtf8());
            packet.append("\n");
        } else { // JustFloat
            for (float val : channelData) {
                packet.append(reinterpret_cast<const char*>(&val), 4);
            }
            // Vofa+ JustFloat tail
            const unsigned char tail[4] = {0x00, 0x00, 0x80, 0x7F};
            packet.append(reinterpret_cast<const char*>(tail), 4);
        }
    }
    
    // Send data to console
    if (chkShowReceiveData->isChecked()) {
        QMetaObject::invokeMethod(this, "handleSerialDataReceived", Qt::QueuedConnection, Q_ARG(QByteArray, packet));
    }
    
    if (!parser || comboDemoProtocol->property("lastProtocol").toInt() != comboDemoProtocol->currentIndex()) {
        if (parser) {
            parser->deleteLater();
        }
        if (comboDemoProtocol->currentIndex() == 0) {
            parser = new FireWaterParser(this);
        } else {
            parser = new JustFloatParser(this);
        }
        connect(parser, &ProtocolParser::channelsUpdated, this, &MainWindow::handleChannelsUpdated);
        m_demoStatusMonitor->bindProtocol(parser, DataProtocols::ProtocolSelection::fromLegacy(comboDemoProtocol->currentIndex() == 0 ? 1 : 2), {});
        comboDemoProtocol->setProperty("lastProtocol", comboDemoProtocol->currentIndex());
    }
    
    if (parser) {
        demoTimer->setProperty("currentReceiveTimestampNs",
                               QDateTime::currentMSecsSinceEpoch() * 1000000LL);
        m_demoStatusMonitor->setReceiveTimestamp(demoTimer->property("currentReceiveTimestampNs").toLongLong());
        parser->processData(packet.constData(), packet.size());
    }
    
    // Recording is performed from the parsed channel frames in
    // handleChannelsUpdated(). Do not append the raw Demo packet here: doing
    // both would mix binary/text packets into the crash-safe CSV recording.
}
