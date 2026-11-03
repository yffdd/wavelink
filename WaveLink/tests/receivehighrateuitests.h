#pragma once
#include <QDataStream>

static int receiveHighRateUiTests()
{
    QTemporaryDir folder; VERIFY(folder.isValid()); QString error;
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    main.setValue("editCacheDir", folder.path()); main.setValue("chkHexReceive", true); main.setValue("chkShowReceiveData", true);
    main.setValue("Console/ReceiveMaxLines", 1000); main.setValue("Console/ReceiveMaxCharacters", 50000); main.setValue("Console/ReceiveRefreshMs", 100);
    main.setValue("comboRecordingMode", 1); main.sync(); advanced.setValue("InterfaceType", 0); advanced.setValue("Fs", 500.0); advanced.sync();
    ProtocolProfileStore store; VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(2), error));
    auto port = new ReceivePort; auto worker = new TestSerialHandler(port);
    MainWindow host(nullptr, worker); host.resize(1200, 900); host.show();
    host.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    auto open = host.findChild<QPushButton *>("btnOpenClose"); auto protocol = host.findChild<QComboBox *>("comboProtocol");
    VERIFY(until([&] { return open->isEnabled() && protocol->isEnabled(); }));
    auto ports = host.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
    open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
    auto record = host.findChild<QPushButton *>("btnToggleRecord"); auto receive = host.findChild<QPlainTextEdit *>("textReceive");
    VERIFY(record && receive);
    QByteArray frame; QDataStream bytes(&frame, QIODevice::WriteOnly); bytes.setByteOrder(QDataStream::LittleEndian); bytes.setFloatingPointPrecision(QDataStream::SinglePrecision);
    for (int i = 1; i <= 15; ++i) bytes << float(i);
    frame.append(QByteArray::fromHex("0000807F")); VERIFY(frame.size() == 64);
    // Existing JustFloat framing requires two equal-width frames to lock.
    // Warm up BEFORE recording; this is unrelated to display retention.
    VERIFY(feed(worker, port, frame.repeated(2)));
    VERIFY(until([&] { return host.findChildren<QCheckBox *>("chkChannel").size() == 15; }));
    host.findChild<QPushButton *>("btnClearReceive")->click();
    record->click(); VERIFY(until([&] { return record->text().contains("停止"); }));
    const auto batch = frame.repeated(50); QElapsedTimer elapsed; elapsed.start();
    for (int i = 0; i < 600; ++i) {
        VERIFY(feed(worker, port, batch));
        VERIFY(QMetaObject::invokeMethod(worker, [] {}, Qt::BlockingQueuedConnection));
        QCoreApplication::processEvents(); VERIFY(QMetaObject::invokeMethod(&host, "flushReceiveText", Qt::DirectConnection));
        VERIFY(receive->document()->characterCount() - 1 <= 50000 && record->text().contains("停止"));
    }
    record->click(); VERIFY(until([&] { return record->text().contains("开始"); }));
    const auto text = receive->toPlainText(); VERIFY(text.endsWith("00 00 80 7F ") && text.size() <= 50000);
    const QDir cache(folder.filePath("WaveLink_Cache")); const auto files = cache.entryList({"*.wlr"}, QDir::Files); VERIFY(files.size() == 1);
    int recorded = 0;
    bool correct = true;
    const bool visited = WlrRecordingWriter::visitFiles({cache.filePath(files[0])}, [&](qint64, const QVector<double> &values, qint64) {
        if (values.size() != 15) return false;
        for (int i = 0; i < values.size(); ++i) if (values[i] != i + 1) correct = false;
        ++recorded; return true;
    }, error);
    if (!visited || !correct || recorded != 30000) std::cerr << "High-rate WLR: visited=" << visited << " correct=" << correct << " frames=" << recorded << " error=" << error.toUtf8().constData() << "\n";
    VERIFY(visited && correct && recorded == 30000);
    VERIFY(host.findChildren<QCheckBox *>("chkChannel").size() == 15);
    open->click(); host.close();
    std::cout << "500Hz/15-channel/60-second-equivalent JustFloat pipeline: 30000 complete WLR frames, bounded HEX history and live plotting; " << elapsed.elapsed() << " ms software replay, not a radio/real-time hardware test.\n";
    return 0;
}
