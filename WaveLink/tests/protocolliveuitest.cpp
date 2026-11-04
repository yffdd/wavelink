#include "mainwindow.h"
#include "protocoleditorwidget.h"
#include "ProtocolProfileStore.h"
#include "ProtocolConfigTransfer.h"
#include "ProtocolParser.h"
#include "commandpanelwindow.h"
#include "statusmonitorwindow.h"
#include "protocolmanagerdialog.h"
#include "fakeserialbackend.h"
#include "testregistrysandbox.h"
#include "thememanager.h"
#include "rangeslider.h"
#include "waveformgraph.h"
#include "systemsettingsdialog.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QPushButton>
#include <QToolButton>
#include <QToolBar>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QSettings>
#include <QLockFile>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QNetworkProxy>
#include <QTimer>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDialogButtonBox>
#include <QFile>
#include <QDir>
#include <QScrollArea>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextStream>
#include <QTextCursor>
#include <QStackedWidget>
#include <QUuid>
#include <QTableWidget>
#include <QHeaderView>
#include <QImage>
#include <QFileDialog>
#include <QProgressDialog>
#include <QProgressBar>
#include <QMessageBox>
#include <QMenu>
#include "filter.h"
#include <limits>
#include "recordingchannelnames.h"
#include <QFontMetrics>
#include <QGroupBox>
#include <QFormLayout>
#include <QScrollBar>
#include <QAbstractTextDocumentLayout>
#include <QWheelEvent>
#include <functional>
#include <QDockWidget>
#include <QStatusBar>
#include <QRegularExpression>
#include <iostream>
using namespace DataProtocols;
static QString artifactDirectory;
#define VERIFY(c) do { if (!(c)) { std::cerr << "Protocol main UI failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)
static bool until(std::function<bool()> condition) { QElapsedTimer t; t.start(); while (!condition() && t.elapsed() < 8000) { QCoreApplication::processEvents(); QThread::msleep(2); } return condition(); }
#include "blequickuitests.h"
static bool channelDockFits(MainWindow &window) {
    auto dock = window.findChild<QDockWidget *>("DockChannel");
    auto scroll = window.findChild<QScrollArea *>("RightSharedScrollArea");
    if (!dock || !scroll || !scroll->widget()) return false;
    const QFont sourceFont = qApp->font();
    for (int points : {9, 11}) {
        QFont font = sourceFont; font.setPointSize(points); qApp->setFont(font);
        for (auto theme : {ThemeManager::DarkTheme, ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
            ThemeManager::applyTheme(qApp, theme);
            QCoreApplication::processEvents();
            window.resizeDocks({dock}, {1}, Qt::Horizontal);
            QCoreApplication::processEvents(); QCoreApplication::processEvents();
            auto content = scroll->widget();
            if (!artifactDirectory.isEmpty()) dock->grab().save(artifactDirectory + QString("/channel-narrow-%1-%2.png").arg(points).arg(theme));
            for (auto control : content->findChildren<QWidget *>()) {
                if (!control->isVisible() || (!qobject_cast<QAbstractButton *>(control) && !qobject_cast<QAbstractSpinBox *>(control) && !qobject_cast<QComboBox *>(control))) continue;
                const auto rect = QRect(control->mapTo(content, QPoint()), control->size());
                if (rect.right() >= scroll->viewport()->width()) {
                    std::cerr << "Channel control clipped: " << control->metaObject()->className() << " x=" << rect.x() << " width=" << rect.width() << "\n";
                    return false;
                }
            }
            if (content->width() > scroll->viewport()->width()) {
                std::cerr << "Channel dock clipped: font=" << points << " theme=" << theme << " dock=" << dock->width()
                    << " viewport=" << scroll->viewport()->width() << " content=" << content->width()
                    << " minimumHint=" << content->minimumSizeHint().width() << "\n";
                return false;
            }
        }
    }
    // Theme/font changes while collapsed must update the EXPANDED constraint,
    // not force the narrow strip open or restore a stale fixed width.
    auto expand = dock->findChild<QToolButton *>("DockExpandBtn");
    auto sideExpand = dock->findChild<QToolButton *>("DockCollapsedSideExpand");
    if (!expand || !sideExpand) return false;
    expand->click();
    if (!until([&] { return dock->property("dockCollapsed").toBool() && dock->width() == dock->maximumWidth(); })) return false;
    const int collapsedWidth = dock->width();
    ThemeManager::applyTheme(qApp, ThemeManager::LightTheme); QCoreApplication::processEvents(); QCoreApplication::processEvents();
    if (dock->minimumWidth() != collapsedWidth || dock->maximumWidth() != collapsedWidth) return false;
    sideExpand->click(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
    window.resizeDocks({dock}, {1}, Qt::Horizontal); QCoreApplication::processEvents(); QCoreApplication::processEvents();
    if (dock->property("dockCollapsed").toBool() || scroll->widget()->width() > scroll->viewport()->width()) return false;
    auto reset = window.findChild<QToolButton *>("btnRestoreDefaultLayout"); if (!reset) return false;
    reset->click(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
    if (scroll->widget()->width() > scroll->viewport()->width()) return false;
    qApp->setFont(sourceFont); ThemeManager::applyTheme(qApp, ThemeManager::DarkTheme); QCoreApplication::processEvents();
    return true;
}
static bool headerEmptyAreasThemed(bool dark) {
    QTableWidget table(1, 1); table.resize(560, 280); table.setColumnWidth(0, 100); table.show(); QApplication::processEvents();
    for (auto header : {table.horizontalHeader(), table.verticalHeader()}) {
        auto viewport = header->viewport(); const auto image = viewport->grab().toImage();
        const QPoint logical = header->orientation() == Qt::Horizontal ? QPoint(viewport->width() - 12, viewport->height() / 2) : QPoint(viewport->width() / 2, viewport->height() - 12);
        const int lightness = image.pixelColor(qRound(logical.x() * image.devicePixelRatio()), qRound(logical.y() * image.devicePixelRatio())).lightness();
        if (dark ? lightness > 100 : lightness < 180) return false;
    }
    return true;
}
static bool protocolAccentsThemed(MainWindow &window) {
    ProtocolManagerDialog manager(ProtocolProfileStore::defaultPath(), &window);
    manager.resize(1350, 850); manager.show();
    auto name = manager.findChild<QLineEdit *>("protocolName");
    auto editor = manager.findChild<ProtocolEditorWidget *>();
    auto generate = manager.findChild<QPushButton *>("protocolGenerateSample");
    auto parse = manager.findChild<QPushButton *>("protocolTest");
    auto diagnostics = manager.findChild<QPlainTextEdit *>("protocolDiagnostics");
    if (!name || !editor || !generate || !parse || !diagnostics || !generate->isEnabled() || !parse->isEnabled()) return false;
    diagnostics->setPlainText("【解析错误】字节偏移 10：帧尾不匹配；预期 0D 0A，实际 0D 00。");
    bool checked = true;
    for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
        ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents(); QCoreApplication::processEvents();
        const bool dark = theme == ThemeManager::DarkTheme;
        checked = checked && generate->palette().color(QPalette::Button) == QColor("#176ea6")
            && diagnostics->palette().color(QPalette::Text) == QColor(dark ? "#ff8b8b" : "#b42318")
            && diagnostics->toPlainText().contains("字节偏移 10")
            && parse->palette().color(QPalette::Button) == QColor("#187545")
            && generate->palette().color(QPalette::ButtonText) == QColor(Qt::white)
            && parse->palette().color(QPalette::ButtonText) == QColor(Qt::white)
            && name->font().pointSizeF() >= editor->font().pointSizeF() + 2
            && name->font().weight() >= QFont::DemiBold
            && name->height() >= name->fontMetrics().height() + 12
            && (dark ? name->palette().color(QPalette::Base).lightness() < 100 && name->palette().color(QPalette::Text).lightness() > 150
                : name->palette().color(QPalette::Base).lightness() > 200 && name->palette().color(QPalette::Text).lightness() < 100);
        if (!artifactDirectory.isEmpty()) manager.grab().save(artifactDirectory + (dark ? "/protocol-accents-dark.png" : "/protocol-accents-light.png"));
        for (auto button : {generate, parse}) {
            button->setEnabled(false); QCoreApplication::processEvents();
            checked = checked && button->palette().color(QPalette::Disabled, QPalette::Button) == QColor("#687580");
            button->setEnabled(true);
        }
    }
    manager.close(); return checked;
}
static bool engineProtocolHelpThemed(MainWindow &window) {
    bool checked = false;
    QTimer::singleShot(0, &window, [&] {
        auto dialog = window.findChild<QDialog *>("engineProtocolHelpDialog"); if (!dialog) return;
        auto headings = dialog->findChildren<QToolButton *>("engineProtocolSection");
        auto bodies = dialog->findChildren<QTextBrowser *>("engineProtocolBody");
        auto scroll = dialog->findChild<QScrollArea *>("engineProtocolScroll");
        if (headings.size() != 4 || bodies.size() != 4 || !scroll) { dialog->reject(); return; }
        auto bodyFits = [](QTextBrowser *body) {
            return body->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && body->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff
                && body->document()->documentLayout()->documentSize().height() <= body->viewport()->height() + 1
                && body->verticalScrollBar()->maximum() == 0 && body->horizontalScrollBar()->maximum() == 0;
        };
        headings[2]->click(); QCoreApplication::processEvents();
        const auto example = bodies[2]->toPlainText(); checked = bodies[2]->isVisible() && headings[2]->arrowType() == Qt::DownArrow && example.contains("JUSTFLOAT_MAX_LEN");
        checked = checked && until([&] { return bodyFits(bodies[2]) && bodies[2]->height() > 350 && scroll->verticalScrollBar()->maximum() > 0; });
        checked = checked && window.findChild<QComboBox *>("comboProtocol")->count() == 4 && window.findChild<QComboBox *>("comboProtocol")->itemText(3) == "自定义协议"
            && headings[3]->text().contains("自定义协议") && bodies[3]->toPlainText().contains("1 → IR（AFE4400 LED1）")
            && bodies[3]->toPlainText().contains("2 → RED（AFE4400 LED2）");
        for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
            ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents(); QCoreApplication::processEvents();
            const bool dark = theme == ThemeManager::DarkTheme;
            checked = checked && (dark ? dialog->palette().color(QPalette::Window).lightness() < 100 : dialog->palette().color(QPalette::Window).lightness() > 200);
            for (auto body : bodies) {
                checked = checked && (dark ? body->palette().color(QPalette::Base).lightness() < 100 && body->palette().color(QPalette::Text).lightness() > 150
                    : body->palette().color(QPalette::Base).lightness() > 200 && body->palette().color(QPalette::Text).lightness() < 100);
            }
            checked = checked && (dark ? headings[0]->palette().color(QPalette::Button).lightness() < 100 : headings[0]->palette().color(QPalette::Button).lightness() > 200);
            checked = checked && bodies[2]->isVisible() && bodies[2]->toPlainText() == example
                && bodies[2]->toHtml().contains(dark ? "#1a1d20" : "#edf1f5", Qt::CaseInsensitive);
            checked = checked && until([&] { return bodyFits(bodies[2]); });
            if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + (dark ? "/engine-protocol-help-dark.png" : "/engine-protocol-help-light.png"));
        }
        for (int i = 0; i < headings.size(); ++i) if (bodies[i]->isHidden()) headings[i]->click();
        for (int width : {480, 1100, 700}) {
            dialog->resize(width, 500);
            checked = checked && until([&] {
                for (auto body : bodies) if (!bodyFits(body)) return false;
                return scroll->verticalScrollBar()->maximum() > 0 && scroll->widget()->width() <= scroll->viewport()->width();
            });
        }
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        checked = checked && scroll->verticalScrollBar()->value() > 0;
        scroll->verticalScrollBar()->setValue(0);
        QWheelEvent wheel(QPointF(12, 12), QPointF(bodies[2]->viewport()->mapToGlobal(QPoint(12, 12))), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(bodies[2]->viewport(), &wheel); QCoreApplication::processEvents();
        checked = checked && scroll->verticalScrollBar()->value() > 0 && bodyFits(bodies[2]);
        headings[2]->click(); checked = checked && bodies[2]->isHidden() && headings[2]->arrowType() == Qt::RightArrow; dialog->accept();
    });
    return QMetaObject::invokeMethod(&window, "showProtocolHelpDialog", Qt::DirectConnection) && checked;
}
static bool checksumLayoutFits() {
    QScrollArea scroll; scroll.setWidgetResizable(true);
    auto editor = new ProtocolEditorWidget; scroll.setWidget(editor); scroll.show();
    auto conditions = editor->findChild<QTableWidget *>("protocolConditions");
    auto hint = editor->findChild<QLabel *>("protocolConditionsHint");
    auto note = editor->findChild<QLabel *>("protocolBinaryNote");
    auto rule = editor->findChild<QLabel *>("protocolChecksumRule");
    auto summary = editor->findChild<QLabel *>("protocolChecksumSummary");
    auto algorithmRow = editor->findChild<QWidget *>("protocol_algorithmRow");
    auto algorithm = editor->findChild<QComboBox *>("protocol_algorithm");
    auto headerAlgorithm = editor->findChild<QComboBox *>("protocol_headerAlgorithm");
    auto addCondition = editor->findChild<QPushButton *>("protocolAddCondition");
    auto choose = [](QComboBox *box, const char *value) { box->setCurrentIndex(box->findData(value)); };
    auto gap = [editor](QWidget *a, QWidget *b) { return b->mapTo(editor, QPoint()).y() - a->mapTo(editor, QPoint()).y() - a->height(); };
    auto sourceFont = editor->font();
    for (int points : {sourceFont.pointSize(), 14}) {
        QFont font = sourceFont; font.setPointSize(points); editor->setFont(font);
        for (auto fixture : {ProtocolEditorWidget::example(true), ProtocolEditorWidget::lengthExample(), ProtocolEditorWidget::dynamicArrayExample()}) {
            ProtocolProfile profile; QString error;
            if (!ProtocolProfileValidator::profile(fixture, profile, error)) return false;
            editor->setProfile(profile);
            for (int width : {1400, 500, 900, 1400}) {
                scroll.resize(width, width == 900 ? 700 : 2200);
                for (const char *head : {"none", "xor8", "crc16-modbus", "none"}) {
                    if (fixture["frame"].toObject()["mode"] == QJsonValue("fixed") && QByteArray(head) != "none") continue;
                    choose(headerAlgorithm, head);
                    for (const char *body : {"crc32-iso-hdlc", "sum8", "none", "crc16-modbus", "none"}) {
                        choose(algorithm, body); QApplication::processEvents(); QApplication::processEvents();
                        const bool labelsFit = rule->height() >= rule->heightForWidth(rule->width()) && note->height() >= note->heightForWidth(note->width()) && hint->height() >= hint->heightForWidth(hint->width())
                            && rule->height() <= rule->heightForWidth(rule->width()) + 2 && note->height() <= note->heightForWidth(note->width()) + 2;
                        const int firstGap = gap(algorithmRow, rule), noteGap = gap(summary->isHidden() ? rule : summary, note), conditionGap = gap(note, hint->parentWidget());
                        const bool fit = labelsFit && firstGap >= 6 && firstGap <= 12 && noteGap >= 6 && noteGap <= 12 && conditionGap >= 6 && conditionGap <= 16
                            && conditions->height() >= 110 && conditions->viewport()->height() >= 64
                            && gap(hint, conditions) >= 4 && gap(conditions, addCondition) >= 6
                            && addCondition->geometry().bottom() < hint->parentWidget()->height();
                        if (!fit) {
                            std::cerr << "Checksum layout: " << profile.name.toStdString() << " width=" << width << " font=" << points << " head=" << head << " data=" << body
                                << " gaps=" << firstGap << "," << noteGap << "," << conditionGap << " table=" << conditions->height() << " viewport=" << conditions->viewport()->height() << " labels=" << labelsFit
                                << " hint/table=" << gap(hint, conditions) << " group=" << hint->parentWidget()->height() << " tableBottom=" << conditions->geometry().bottom()
                                << " rule=" << rule->height() << "/" << rule->heightForWidth(rule->width()) << " note=" << note->height() << "/" << note->heightForWidth(note->width()) << "\n";
                            if (!artifactDirectory.isEmpty()) scroll.grab().save(artifactDirectory + "/protocol-checksum-layout-failure.png");
                            return false;
                        }
                        if (!artifactDirectory.isEmpty() && profile.isDynamicArray() && width == 1400 && points == sourceFont.pointSize() && QByteArray(head) == "xor8" && QByteArray(body) == "none") {
                            auto section = editor->findChild<QWidget *>("protocolBinarySection");
                            editor->grab(QRect(section->mapTo(editor, QPoint()), section->size())).save(artifactDirectory + "/protocol-checksum-array-dark.png");
                        }
                    }
                }
            }
        }
    }
    // Populated conditions keep their rows scrollable and their action buttons
    // below the viewport, regardless of the selected checksum algorithm.
    for (int count : {1, 8, 16}) {
        while (conditions->rowCount() < count) addCondition->click();
        for (const char *body : {"none", "crc32-iso-hdlc", "none"}) {
            choose(algorithm, body); QApplication::processEvents(); QApplication::processEvents();
            if (conditions->rowCount() != count || conditions->height() < 110 || gap(conditions, addCondition) < 6
                || addCondition->geometry().bottom() >= hint->parentWidget()->height()) return false;
        }
    }
    return true;
}
class ReceivePort : public FakePort {
public:
    void feed(QByteArray bytes) { incoming += bytes; emit readyRead(); }
    qint64 bytesAvailable() const override { return incoming.size() + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 count) override { const auto n = qMin<qint64>(count, incoming.size()); memcpy(data, incoming.constData(), size_t(n)); incoming.remove(0, int(n)); return n; }
private: QByteArray incoming;
};
static bool feed(TestSerialHandler *worker, ReceivePort *port, const QByteArray &bytes) { return QMetaObject::invokeMethod(worker, [port, bytes] { port->feed(bytes); }, Qt::QueuedConnection); }
static void confirmSwitch() {
    QTimer::singleShot(0, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolApplyConfirm") {
        auto dialog = qobject_cast<QDialog *>(widget); auto message = dialog->findChild<QLabel *>("protocolApplyMessage");
        if (dialog->width() < 500 || !message || message->height() < message->heightForWidth(message->width())) std::abort();
        if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + "/protocol-switch-confirm.png");
        dialog->accept();
    } });
}
// Windows Qt 5.14 QMessageBox::showEvent assumes a native platform interface;
// offscreen has none. Skip only that native Show handler in this software test,
// keeping the application's dialog decisions and imported results testable.
class OffscreenMessages : public QObject {
public:
    bool openOfflineSettings = false;
    QString commandButton = "configCommandSkip",statusButton="configStatusSkip",standaloneStatusButton="statusStandaloneCancel", lastTitle, lastText;
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) if (auto box = qobject_cast<QMessageBox *>(object)) {
            lastTitle = box->windowTitle(); lastText = box->text();
            auto choice = box->objectName() == "configCommandImportDialog" ? box->findChild<QPushButton *>(commandButton) : nullptr;
            if(box->objectName()=="configStatusImportDialog")choice=box->findChild<QPushButton*>(statusButton);
            if(box->objectName()=="statusStandaloneImportDialog")choice=box->findChild<QPushButton*>(standaloneStatusButton);
            if (box->objectName() == "offlineImportAdvice" && openOfflineSettings) choice = box->findChild<QPushButton *>("offlineAdviceSettings");
            if (choice) QTimer::singleShot(0, choice, &QPushButton::click);
            else QTimer::singleShot(0, box, &QDialog::accept);
            return true;
        }
        return false;
    }
};
static bool configFileAction(MainWindow &window, const QString &path, const char *slot, int policy = 1, bool restoreSelection = true) {
    QTimer driver; bool picked = false, previewAccepted = false; QElapsedTimer deadline; deadline.start();
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (widget->isVisible()) {
            if (auto picker = qobject_cast<QFileDialog *>(widget)) {
                if (!picked) { picked = true; picker->selectFile(path); QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection); }
                else if (deadline.elapsed() > 7000) picker->reject();
            } else if (auto dialog = qobject_cast<QDialog *>(widget)) {
                if (dialog->objectName() == "configProtocolImportDialog" && !previewAccepted) {
                    auto mode = dialog->findChild<QComboBox *>("configProtocolImportMode");
                    auto restore = dialog->findChild<QCheckBox *>("configProtocolRestoreSelection");
                    auto buttons = dialog->findChild<QDialogButtonBox *>();
                    if (!mode || !restore || !buttons) { dialog->reject(); continue; }
                    mode->setCurrentIndex(policy < 0 ? 3 : policy); restore->setChecked(restoreSelection);
                    previewAccepted = buttons->button(QDialogButtonBox::Ok)->isEnabled();
                    if (previewAccepted) QTimer::singleShot(0, buttons->button(QDialogButtonBox::Ok), &QPushButton::click);
                    else dialog->reject();
                    if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + "/global-config-protocol-import.png");
                } else if (deadline.elapsed() > 7000) dialog->reject();
            }
        }
    });
    driver.start(10);
    return QMetaObject::invokeMethod(&window, slot, Qt::DirectConnection) && picked
        && (QString::fromLatin1(slot) != "onImportConfig" || previewAccepted);
}
static bool editSystemSettings(MainWindow &window, int lines, int characters, int refresh, int fps, int bottomPercent = -1, int automaticUpdates = -1, int lineWidth = -1, int offlineMemory = -1) {
    bool applied = false;
    QTimer::singleShot(0, &window, [&] {
        auto dialog = window.findChild<QDialog *>("systemSettingsDialog"); if (!dialog) return;
        auto lineCount = dialog->findChild<QComboBox *>("systemReceiveLines"); auto charCount = dialog->findChild<QSpinBox *>("systemReceiveCharacters");
        auto interval = dialog->findChild<QComboBox *>("systemReceiveRefreshMs"); auto plot = dialog->findChild<QComboBox *>("systemPlotFps");
        auto apply = dialog->findChild<QPushButton *>("systemSettingsApply");
        if (lineCount && charCount && interval && plot && apply) {
            lineCount->setCurrentIndex(lineCount->findData(lines)); charCount->setValue(characters / 10000); interval->setCurrentIndex(interval->findData(refresh)); plot->setCurrentIndex(plot->findData(fps));
            if (bottomPercent >= 0) { if (auto bottom = dialog->findChild<QSpinBox *>("systemDefaultBottomPercent")) bottom->setValue(bottomPercent); }
            if (automaticUpdates >= 0) { if (auto automatic = dialog->findChild<QCheckBox *>("systemAutomaticUpdates")) automatic->setChecked(automaticUpdates != 0); }
            if (lineWidth >= 0) { if (auto width = dialog->findChild<QComboBox *>("systemPlotLineWidth")) width->setCurrentIndex(width->findData(lineWidth)); }
            if (offlineMemory >= 0) { if (auto memory = dialog->findChild<QSpinBox *>("systemOfflineMemoryMB")) memory->setValue(offlineMemory); }
            apply->click(); applied = dialog->isVisible();
        }
        dialog->reject();
    });
    return QMetaObject::invokeMethod(&window, "showSystemSettings", Qt::DirectConnection) && applied;
}
static int configUiRoundTrip(const QString &directory, const ProtocolSnapshot &source, OffscreenMessages &messages) {
    QString error; ProtocolProfileStore store; VERIFY(store.load(error) && store.commit(source, error));
    const auto expected = ProtocolProfileValidator::encode(store.snapshot(), true, error);
    const auto commandPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("cmd_panels.json");
    const QJsonArray commands{QJsonObject{{"id", QUuid::createUuid().toString()}, {"name", "配置回放面板"}, {"columns", 3},
        {"commands", QJsonArray{QJsonObject{{"id", QUuid::createUuid().toString()}, {"name", "读取温度"}, {"payload", "READ"}, {"appendIndex", 1}, {"btnColor", "#17688c"}},
            QJsonObject{{"id", QUuid::createUuid().toString()}, {"name", "列表参数"}, {"widgetType", 1}, {"sliderMode", 1}, {"payload", "AA"}, {"isHex", true},
                {"sliderValues", QJsonArray{"00", "01 02", "FF"}}, {"defaultListIndex", 1}, {"btnColor", ""}}}}}};
    VERIFY(ProtocolProfileStore::writeFile(commandPath, QJsonDocument(commands).toJson(), error));
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    advanced.setValue("InterfaceType", 0); advanced.sync(); main.setValue("configUiSentinel", "exported"); main.sync();
    // Global import/recovery must respect the same locks as individual saves.
    const auto journalPath = QDir(QFileInfo(store.path()).absolutePath()).filePath("global-config-transaction.json");
    {
        QLockFile busyCommands(commandPath + ".lock"); VERIFY(busyCommands.tryLock(0));
        const QVariantMap lockedValues{{"WaveLink/configUiSentinel", "must-not-write"}, {"CommandPanel/json", "[]"}};
        VERIFY(!ProtocolConfigTransfer::apply(lockedValues, store, -1, false, 1, commandPath, error));
        VERIFY(error.contains("正在保存") && main.value("configUiSentinel").toString() == "exported");
        VERIFY(!QFile::exists(journalPath) && ProtocolProfileValidator::encode(store.snapshot(), true, error) == expected);
        QFile current(commandPath); VERIFY(current.open(QIODevice::ReadOnly));
        VERIFY(QJsonDocument::fromJson(current.readAll()).array() == commands); current.close();
        QFile journal(journalPath); VERIFY(journal.open(QIODevice::WriteOnly) && journal.write("{}") == 2); journal.close();
        VERIFY(!ProtocolConfigTransfer::recover(store.path(), commandPath, error) && error.contains("正在保存") && QFile::exists(journalPath));
        busyCommands.unlock();
        QLockFile busyProtocols(store.path() + ".lock"); VERIFY(busyProtocols.tryLock(0));
        VERIFY(!ProtocolConfigTransfer::recover(store.path(), commandPath, error) && error.contains("正在保存") && QFile::exists(journalPath));
        VERIFY(QFile::remove(journalPath));
    }
    const auto configPath = directory + "/ui-global-config.ini";
    const auto sourceCache = directory + "/source-PC-cache", localCache = directory + "/target-PC-cache";
    VERIFY(QDir().mkpath(sourceCache) && QDir().mkpath(localCache));
    {
        MainWindow exported(nullptr, new TestSerialHandler(new ReceivePort)); exported.show();
        VERIFY(until([&] { return exported.findChild<QLabel *>("lblProtocolSummary")->text().contains("已应用"); }));
        exported.findChild<QTimer *>("serialHardwareCheckTimer")->stop(); messages.lastTitle.clear();
        exported.findChild<QSpinBox *>("spinConsoleSendInterval")->setValue(321);
        exported.findChild<QLineEdit *>("editCacheDir")->setText(sourceCache);
        auto receiveLines = exported.findChild<QComboBox *>("comboReceiveLines"); VERIFY(receiveLines);
        receiveLines->setCurrentIndex(receiveLines->findData(5000));
        VERIFY(editSystemSettings(exported, 5000, 30000, 200, 20, 40, 0, 2, 512));
        VERIFY(configFileAction(exported, configPath, "onExportConfig") && messages.lastTitle == "导出成功");
        VERIFY(messages.lastText.contains(QString("%1 个自定义协议").arg(source.profiles.size())) && messages.lastText.contains("命令交互面板"));
        QVariantMap values; ProtocolSnapshot decoded;
        VERIFY(ProtocolConfigTransfer::read(configPath, values, error));
        for (const auto &key : values.keys()) VERIFY(!key.endsWith("/editCacheDir") && !key.endsWith("/CacheDir"));
        VERIFY(main.value("editCacheDir").toString() == sourceCache && advanced.value("CacheDir").toString() == sourceCache);
        VERIFY(values.value("WaveLink/Console/SendIntervalMs").toInt() == 321);
        VERIFY(values.value("WaveLink/Console/ReceiveMaxLines").toInt() == 5000);
        VERIFY(values.value("WaveLink/Console/ReceiveMaxCharacters").toInt() == 30000 && values.value("WaveLink/Console/ReceiveRefreshMs").toInt() == 200 && values.value("WaveLink/Display/PlotFps").toInt() == 20);
        VERIFY(values.value("WaveLink/Layout/DefaultBottomPercent").toInt() == 40);
        VERIFY(values.value("WaveLink/Display/PlotLineWidth").toInt() == 2);
        VERIFY(values.value("WaveLink/Display/OfflineMemoryMB").toInt() == 512);
        VERIFY(values.contains("WaveLink/Update/AutoCheck") && !values.value("WaveLink/Update/AutoCheck").toBool());
        VERIFY(ProtocolProfileValidator::decode(values["DataProtocols/json"].toString().toUtf8(), true, decoded, error));
        VERIFY(ProtocolProfileValidator::encode(decoded, true, error) == expected);
        VERIFY(QJsonDocument::fromJson(values["CommandPanel/json"].toString().toUtf8()).array()[0].toObject()["name"].toString() == "配置回放面板");
        const auto savedList = QJsonDocument::fromJson(values["CommandPanel/json"].toString().toUtf8()).array()[0].toObject()["commands"].toArray()[1].toObject();
        VERIFY(savedList["sliderMode"] == QJsonValue(1) && savedList["defaultListIndex"] == QJsonValue(1) && savedList["sliderValues"].toArray() == QJsonArray({"00", "01 02", "FF"}));
        exported.close();
    }
    // Begin with a different library and commands; import through the real
    // Settings slots and nested file/protocol/command confirmation dialogs.
    VERIFY(store.load(error)); auto local = source; local.profiles = {source.profiles[0]};
    local.profiles[0].id = QUuid::createUuid().toString(QUuid::WithoutBraces); local.profiles[0].name = "现有协议";
    local.profiles[0].definition.insert("id", local.profiles[0].id); local.profiles[0].definition.insert("name", local.profiles[0].name);
    local.shared = ProtocolSelection::fromLegacy(0); local.ble = local.shared; VERIFY(store.commit(local, error));
    VERIFY(ProtocolProfileStore::writeFile(commandPath, "[]", error));
    main.setValue("configUiSentinel", "local"); main.sync();
    QString appendedSelection;
    {
        MainWindow imported(nullptr, new TestSerialHandler(new ReceivePort)); imported.show();
        auto protocol = imported.findChild<QComboBox *>("comboProtocol"), choices = imported.findChild<QComboBox *>("comboCustomProfile");
        VERIFY(until([&] { return protocol->isEnabled(); })); imported.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        // Change the local path immediately before importing, without relying
        // on closing the window or exporting to save the user's edit first.
        imported.findChild<QLineEdit *>("editCacheDir")->setText(localCache);
        imported.findChild<QSpinBox *>("spinConsoleSendInterval")->setValue(999);
        auto importedReceiveLines = imported.findChild<QComboBox *>("comboReceiveLines"); VERIFY(importedReceiveLines);
        importedReceiveLines->setCurrentIndex(importedReceiveLines->findData(1000));
        VERIFY(editSystemSettings(imported, 1000, 50000, 100, 30, 25, 1, 3, 16));
        messages.commandButton = "configCommandOverwrite"; messages.lastTitle.clear();
        VERIFY(configFileAction(imported, configPath, "onImportConfig", 2) && messages.lastTitle == "导入成功");
        VERIFY(imported.findChild<QLineEdit *>("editCacheDir")->text() == localCache && main.value("editCacheDir").toString() == localCache && advanced.value("CacheDir").toString() == localCache);
        VERIFY(imported.findChild<QSpinBox *>("spinConsoleSendInterval")->value() == 321
            && !imported.findChild<QCheckBox *>("chkConsolePeriodicSend")->isChecked());
        VERIFY(importedReceiveLines->currentData().toInt() == 5000 &&
               imported.findChild<QPlainTextEdit *>("textReceive")->document()->maximumBlockCount() == 5000);
        VERIFY(imported.findChild<QTimer *>("consoleReceiveUpdateTimer")->interval() == 200 && imported.findChild<QTimer *>("waveformRefreshTimer")->interval() == 50);
        VERIFY(SystemPreferences::read(main).receiveCharacters == 30000 && SystemPreferences::read(main).plotFps == 20);
        VERIFY(SystemPreferences::read(main).defaultBottomPercent == 40);
        VERIFY(SystemPreferences::read(main).plotLineWidth == 2);
        VERIFY(SystemPreferences::read(main).offlineMemoryMB == 512);
        VERIFY(!SystemPreferences::read(main).automaticUpdates);
        VERIFY(until([&] { return protocol->isEnabled() && choices->currentData().toString() == source.shared.id; }));
        VERIFY(store.load(error) && ProtocolProfileValidator::encode(store.snapshot(), true, error) == expected && main.value("configUiSentinel").toString() == "exported");
        auto panels = imported.findChildren<PanelContentWidget *>(); VERIFY(panels.size() == 1 && panels[0]->getDef().name == "配置回放面板");
        VERIFY(panels[0]->getDef().commands.size() == 2 && panels[0]->getDef().commands[0].payload == "READ");
        const auto listCommand = panels[0]->getDef().commands[1];
        VERIFY(listCommand.sliderMode == 1 && listCommand.defaultListIndex == 1 && listCommand.sliderValues == QStringList({"00", "01 02", "FF"}) && listCommand.isHex && listCommand.btnColor.isEmpty());
        messages.commandButton = "configCommandSkip"; messages.lastTitle.clear();
        // Old configuration files still contain paths from the source PC.
        { QSettings old(configPath, QSettings::IniFormat);
          for (const auto &key : {"WaveLink/editCacheDir", "AdvancedApp/CacheDir", "MyCompany/CacheDir", "editCacheDir", "CacheDir"})
              old.setValue(key, "Z:/missing-on-this-PC/foreign-cache");
          old.sync(); VERIFY(old.status() == QSettings::NoError); }
        VERIFY(configFileAction(imported, configPath, "onImportConfig", 1) && messages.lastTitle == "导入成功");
        VERIFY(imported.findChild<QLineEdit *>("editCacheDir")->text() == localCache && main.value("editCacheDir").toString() == localCache && advanced.value("CacheDir").toString() == localCache);
        VERIFY(until([&] { return protocol->isEnabled(); }) && store.load(error) && ProtocolProfileValidator::encode(store.snapshot(), true, error) == expected);
        messages.commandButton = "configCommandAppend"; messages.lastTitle.clear();
        VERIFY(configFileAction(imported, configPath, "onImportConfig", 0) && messages.lastTitle == "导入成功");
        VERIFY(until([&] { return protocol->isEnabled(); }) && store.load(error) && store.snapshot().profiles.size() == source.profiles.size() * 2);
        appendedSelection = store.snapshot().shared.id; VERIFY(appendedSelection != source.shared.id && choices->currentData().toString() == appendedSelection);
        QString selectedName; for (const auto &p : source.profiles) if (p.id == source.shared.id) selectedName = p.name;
        VERIFY(!selectedName.isEmpty() && choices->currentText().contains(selectedName + "_1"));
        panels = imported.findChildren<PanelContentWidget *>(); QStringList panelNames;
        for (auto panel : panels) { panelNames << panel->getDef().name; for (const auto &command : panel->getDef().commands) VERIFY(!command.autoSend); }
        // QObject child order changes when QStackedWidget raises its current
        // page; validate actual persisted ordering rather than child order.
        VERIFY(panels.size() == 2 && panelNames.contains("配置回放面板") && panelNames.contains("配置回放面板_1"));
        QFile commandData(commandPath); VERIFY(commandData.open(QIODevice::ReadOnly));
        const auto appendedCommands = QJsonDocument::fromJson(commandData.readAll()).array(); commandData.close();
        VERIFY(appendedCommands.size() == 2 && appendedCommands[1].toObject()["name"].toString() == "配置回放面板_1");
        const auto appended = ProtocolProfileValidator::encode(store.snapshot(), true, error);
        QVariantMap skipValues; VERIFY(ProtocolConfigTransfer::read(configPath, skipValues, error));
        const auto skipPath = directory + "/ui-global-config-skip.ini";
        { QSettings skip(skipPath, QSettings::IniFormat); skip.setIniCodec("UTF-8");
          for (auto it = skipValues.cbegin(); it != skipValues.cend(); ++it) skip.setValue(it.key(), it.value());
          skip.setValue("DataProtocols/json", "{broken}"); skip.setValue("WaveLink/configUiSentinel", "skip-imported"); skip.sync(); VERIFY(skip.status() == QSettings::NoError); }
        messages.commandButton = "configCommandSkip"; messages.lastTitle.clear();
        VERIFY(configFileAction(imported, skipPath, "onImportConfig", -1) && messages.lastTitle == "导入成功");
        VERIFY(until([&] { return protocol->isEnabled(); }) && store.load(error) && ProtocolProfileValidator::encode(store.snapshot(), true, error) == appended);
        VERIFY(main.value("configUiSentinel").toString() == "skip-imported" && choices->currentData().toString() == appendedSelection);
        VERIFY(imported.findChildren<PanelContentWidget *>().size() == 2); imported.close();
    }
    {
        MainWindow restarted(nullptr, new TestSerialHandler(new ReceivePort)); restarted.show();
        VERIFY(restarted.findChild<QLineEdit *>("editCacheDir")->text() == localCache);
        VERIFY(until([&] { return restarted.findChild<QComboBox *>("comboProtocol")->isEnabled(); }));
        VERIFY(restarted.findChild<QComboBox *>("comboCustomProfile")->currentData().toString() == appendedSelection);
        VERIFY(restarted.findChildren<PanelContentWidget *>().size() == 2); restarted.close();
        VERIFY(restarted.findChild<QSpinBox *>("spinConsoleSendInterval")->value() == 321
            && !restarted.findChild<QTimer *>("consoleSendTimer")->isActive());
    }
    return 0;
}
static int consoleInteractionTests(const QString &directory, OffscreenMessages &messages) {
    struct ClipboardRestore {
        QMimeData *saved = new QMimeData;
        ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
        ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
    } clipboardRestore;
    QString error; ProtocolProfileStore store; VERIFY(store.load(error)); const auto previous = store.snapshot();
    auto raw = previous; raw.shared = ProtocolSelection::fromLegacy(0); raw.ble = raw.shared; VERIFY(store.commit(raw, error));
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    main.setValue("Serial/FlowControl", 0); main.setValue("chkHexSend", false); main.setValue("comboAppendNewline", 1);
    main.setValue("Console/SendIntervalMs", 1000); advanced.setValue("InterfaceType", 0); main.sync(); advanced.sync();
    auto pump = [](int ms) { QElapsedTimer time; time.start(); while (time.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(1); } };
    {
        auto port = new ReceivePort; auto worker = new TestSerialHandler(port); MainWindow window(nullptr, worker); window.show();
        auto periodic = window.findChild<QCheckBox *>("chkConsolePeriodicSend"); auto interval = window.findChild<QSpinBox *>("spinConsoleSendInterval");
        auto timer = window.findChild<QTimer *>("consoleSendTimer"); auto send = window.findChild<QTextEdit *>("textSend");
        auto receive = window.findChild<QPlainTextEdit *>("textReceive"); auto open = window.findChild<QPushButton *>("btnOpenClose");
        auto hex = window.findChild<QCheckBox *>("chkHexSend"); auto append = window.findChild<QComboBox *>("comboAppendNewline");
        VERIFY(periodic && interval && timer && send && receive && hex && append);
        VERIFY(!send->acceptRichText());
        auto pasteSample = [&](const QString &plain, const QString &html) {
            auto mime = new QMimeData;
            mime->setText(plain); mime->setHtml(html);
            QApplication::clipboard()->setMimeData(mime); send->clear(); send->paste();
        };
        hex->setChecked(false);
        const QString plainSample = QStringLiteral("-106\n温度: 25.34\n<b>literal</b>");
        pasteSample(plainSample, QStringLiteral("<p style='background-color:white;color:red;font-size:36pt'><b>formatted</b></p>"));
        VERIFY(send->toPlainText() == plainSample);
        for (auto block = send->document()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto format = it.fragment().charFormat();
                VERIFY(format.background().style() == Qt::NoBrush && format.foreground().style() == Qt::NoBrush);
                VERIFY(!format.fontItalic() && format.fontWeight() != QFont::Bold);
            }
        hex->setChecked(true);
        pasteSample("aa 55\n00 ff", "<span style='background:white'>aa 55<br>00 ff</span>");
        VERIFY(send->toPlainText() == "AA 55 00 FF");
        hex->setChecked(false); send->clear();
        VERIFY(until([&] { return open->isEnabled(); }) && interval->value() == 1000 && !periodic->isChecked() && !periodic->isEnabled());
        auto sendButton = window.findChild<QPushButton *>("btnSend"); VERIFY(sendButton);
        const QFont consoleFont = qApp->font(); const QString consoleStyle = qApp->styleSheet();
        for (int points : {9, 11}) {
            QFont font = consoleFont; font.setPointSize(points); qApp->setFont(font);
            for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
                ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents(); QCoreApplication::processEvents();
                VERIFY(sendButton->width() >= 96 && sendButton->width() >= sendButton->fontMetrics().horizontalAdvance(sendButton->text()) + 24);
                VERIFY(sendButton->height() == send->height() && send->width() > sendButton->width());
                if (points == 9 && !artifactDirectory.isEmpty()) window.findChild<QDockWidget *>("DockConsole")->grab().save(
                    artifactDirectory + (theme == ThemeManager::DarkTheme ? "/console-send-dark.png" : "/console-send-light.png"));
            }
        }
        qApp->setFont(consoleFont); qApp->setStyleSheet(consoleStyle); QCoreApplication::processEvents();
        window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        auto rxSave = window.findChild<QPushButton *>("btnSaveReceive"), rxCopy = window.findChild<QPushButton *>("btnCopyReceive"), rxClear = window.findChild<QPushButton *>("btnClearReceive");
        auto txClear = window.findChild<QPushButton *>("btnClearSend");
        VERIFY(rxSave && rxCopy && rxClear && rxSave->text() == "保存接收" && rxCopy->text() == "复制接收");
        VERIFY(until([&] { return rxSave->mapTo(&window, QPoint()).x() < rxCopy->mapTo(&window, QPoint()).x()
            && rxSave->mapTo(&window, QPoint()).y() > rxClear->mapTo(&window, QPoint()).y() + rxClear->height() / 2
            && rxSave->mapTo(&window, QPoint()).y() == rxCopy->mapTo(&window, QPoint()).y()
            && periodic->mapTo(&window, QPoint()).x() < interval->mapTo(&window, QPoint()).x()
            && interval->mapTo(&window, QPoint()).x() < txClear->mapTo(&window, QPoint()).x(); }));
        messages.lastTitle.clear(); rxSave->click(); VERIFY(messages.lastTitle == "暂无接收数据");
        auto clipboard = QApplication::clipboard(); clipboard->setText("保留原有剪贴板内容");
        rxCopy->click(); VERIFY(clipboard->text() == "保留原有剪贴板内容" && window.statusBar()->currentMessage().contains("为空"));
        const QString content = "[12:34:56.789] 温度：25.34 ℃\nAA 55 0D 0A\n第二行\n";
        receive->setPlainText(content); const QString snapshot = receive->toPlainText();
        QTextCursor selection = receive->textCursor(); selection.setPosition(2); selection.setPosition(7, QTextCursor::KeepAnchor); receive->setTextCursor(selection);
        const int scroll = receive->verticalScrollBar()->value();
        rxCopy->click();
        VERIFY(clipboard->text() == snapshot && receive->toPlainText() == snapshot && window.statusBar()->currentMessage().contains("已复制"));
        VERIFY(receive->textCursor().anchor() == selection.anchor() && receive->textCursor().position() == selection.position()
            && receive->verticalScrollBar()->value() == scroll);
        receive->appendPlainText("新收到的内容"); rxCopy->click(); VERIFY(clipboard->text() == receive->toPlainText() && clipboard->text().contains("新收到"));
        const QString copied = clipboard->text(); rxClear->click(); rxCopy->click();
        VERIFY(receive->toPlainText().isEmpty() && clipboard->text() == copied && window.statusBar()->currentMessage().contains("为空"));
        receive->setPlainText(snapshot);
        const auto target = directory + "/receive_snapshot.txt"; QString defaultName; bool picked = false;
        QTimer pickerDriver;
        QObject::connect(&pickerDriver, &QTimer::timeout, &pickerDriver, [&] {
            auto picker = window.findChild<QFileDialog *>("receiveSaveDialog"); if (!picker || !picker->isVisible() || picked) return;
            picked = true; defaultName = QFileInfo(picker->selectedFiles().value(0)).fileName();
            receive->appendPlainText("选择文件期间新收到的内容");
            picker->setDirectory(directory); picker->selectFile("receive_snapshot"); QMetaObject::invokeMethod(picker, "accept");
        });
        pickerDriver.start(10); rxSave->click(); pickerDriver.stop();
        VERIFY(picked && QRegularExpression("^WaveLink_Receive_[0-9]{8}_[0-9]{6}_[0-9]{3}\\.txt$").match(defaultName).hasMatch());
        QFile saved(target); VERIFY(saved.open(QIODevice::ReadOnly) && saved.readAll() == QByteArray::fromHex("EFBBBF") + snapshot.toUtf8()); saved.close();
        VERIFY(receive->toPlainText().contains("新收到") && window.statusBar()->currentMessage().contains("已保存"));
        bool cancelled = false; QTimer cancelPicker;
        QObject::connect(&cancelPicker, &QTimer::timeout, &cancelPicker, [&] { auto picker = window.findChild<QFileDialog *>("receiveSaveDialog");
            if (picker && picker->isVisible()) { cancelled = true; picker->reject(); } });
        cancelPicker.start(10); rxSave->click(); cancelPicker.stop();
        VERIFY(cancelled && saved.open(QIODevice::ReadOnly) && saved.readAll() == QByteArray::fromHex("EFBBBF") + snapshot.toUtf8()); saved.close();
        auto ports = window.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
        open->click(); VERIFY(until([&] { return periodic->isEnabled(); }));
        auto written = [&] { QByteArray bytes; QMetaObject::invokeMethod(worker, [&] { bytes = port->written; }, Qt::BlockingQueuedConnection); return bytes; };
        interval->setValue(10); send->setPlainText("PING"); periodic->setChecked(true);
        VERIFY(timer->isActive() && until([&] { return written().count("PING\r\n") >= 3; }));
        periodic->setChecked(false); const auto stoppedBytes = written(); pump(40); VERIFY(written() == stoppedBytes && !timer->isActive());
        append->setCurrentIndex(3); send->setPlainText("中文"); periodic->setChecked(true);
        VERIFY(until([&] { return written().endsWith(QString("中文\r").toUtf8()); }));
        interval->setValue(1000); VERIFY(timer->interval() == 1000);
        txClear->click(); VERIFY(!periodic->isChecked() && !timer->isActive());
        hex->setChecked(true); append->setCurrentIndex(0); send->setPlainText("aa 55"); periodic->setChecked(true);
        VERIFY(timer->isActive()); QMetaObject::invokeMethod(&window, "onConsolePeriodicSendTimer");
        VERIFY(until([&] { return written().endsWith(QByteArray::fromHex("AA55")); }));
        const auto beforeOdd = written(); send->setPlainText("A"); QMetaObject::invokeMethod(&window, "onConsolePeriodicSendTimer");
        VERIFY(!timer->isActive() && !periodic->isChecked() && written() == beforeOdd && window.statusBar()->currentMessage().contains("完整"));
        send->setPlainText("AA"); periodic->setChecked(true);
        auto breakCheck = window.findChild<QCheckBox *>("chkSerialBreak"); breakCheck->setChecked(true);
        VERIFY(until([&] { return worker->breakLineState.load() && breakCheck->isChecked() && breakCheck->isEnabled(); })
            && !periodic->isChecked() && !timer->isActive());
        breakCheck->setChecked(false); VERIFY(until([&] { return !worker->breakLineState.load() && periodic->isEnabled(); }));
        const auto filePath = directory + "/send_fixture.bin"; VERIFY(ProtocolProfileStore::writeFile(filePath, "FILE", error));
        window.findChild<QLineEdit *>("txtSendFilePath")->setText(filePath); periodic->setChecked(true);
        window.findChild<QPushButton *>("btnSendFile")->click(); VERIFY(!periodic->isChecked() && !timer->isActive());
        VERIFY(until([&] { return periodic->isEnabled() && written().endsWith("FILE"); }));
        hex->setChecked(false); send->setPlainText(QString(80000, 'X')); interval->setValue(10);
        QMetaObject::invokeMethod(worker, [port] { port->holdWrites = true; }, Qt::BlockingQueuedConnection);
        periodic->setChecked(true); VERIFY(until([&] { return !timer->isActive(); }));
        VERIFY(!periodic->isChecked() && worker->pendingSendBytes() <= 256 * 1024 && window.statusBar()->currentMessage().contains("队列"));
        open->click(); VERIFY(until([&] { return window.findChild<QComboBox *>("comboInterface")->isEnabled(); }));
        VERIFY(!periodic->isChecked() && !timer->isActive());
        QMetaObject::invokeMethod(worker, [port] { port->holdWrites = false; }, Qt::BlockingQueuedConnection);
        send->setPlainText("again"); open->click(); VERIFY(until([&] { return periodic->isEnabled(); }));
        pump(40); VERIFY(!periodic->isChecked() && !timer->isActive()); periodic->setChecked(true);
        open->click(); VERIFY(until([&] { return !periodic->isChecked() && !timer->isActive(); }));
        interval->setValue(777); window.close();
    }
    {
        MainWindow restarted(nullptr, new TestSerialHandler(new ReceivePort)); restarted.show();
        VERIFY(until([&] { return restarted.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
        VERIFY(restarted.findChild<QSpinBox *>("spinConsoleSendInterval")->value() == 777
            && !restarted.findChild<QCheckBox *>("chkConsolePeriodicSend")->isChecked()); restarted.close();
    }
    {
        QTcpServer server; VERIFY(server.listen(QHostAddress::LocalHost, 0));
        advanced.setValue("InterfaceType", 1); main.setValue("chkHexSend", false); main.sync(); advanced.sync();
        MainWindow tcp(nullptr, new TestSerialHandler(new ReceivePort)); tcp.show();
        auto periodic = tcp.findChild<QCheckBox *>("chkConsolePeriodicSend"); auto open = tcp.findChild<QPushButton *>("btnOpenClose");
        VERIFY(until([&] { return open->isEnabled(); }));
        tcp.findChild<QLineEdit *>("editIP")->setText("127.0.0.1"); tcp.findChild<QSpinBox *>("spinPort")->setValue(server.serverPort());
        tcp.findChild<QSpinBox *>("spinConsoleSendInterval")->setValue(10); tcp.findChild<QTextEdit *>("textSend")->setPlainText("NET");
        open->click(); VERIFY(until([&] { return periodic->isEnabled() && server.hasPendingConnections(); }));
        auto peer = server.nextPendingConnection(); QByteArray bytes; periodic->setChecked(true);
        VERIFY(until([&] { bytes += peer->readAll(); return bytes.count("NET") >= 2; }));
        peer->abort(); VERIFY(until([&] { return !periodic->isChecked() && !tcp.findChild<QTimer *>("consoleSendTimer")->isActive(); }));
        tcp.close(); peer->deleteLater();
    }
    VERIFY(store.load(error) && store.commit(previous, error)); return 0;
}
static int fileSendUiTests(const QString &directory, OffscreenMessages &messages) {
    QString error; ProtocolProfileStore store; VERIFY(store.load(error)); const auto previous = store.snapshot();
    VERIFY(store.setSelection(ProtocolSelection::fromLegacy(0), error));
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    main.setValue("Serial/FlowControl", 0); advanced.setValue("InterfaceType", 0); main.sync(); advanced.sync();
    QByteArray payload; for (int i = 0; i < 4357; ++i) payload.append(char(i % 256));
    const auto imagePath = directory + "/文件 图片.png";
    VERIFY(ProtocolProfileStore::writeFile(imagePath, payload, error));
    {
        auto port = new ReceivePort; auto worker = new TestSerialHandler(port); MainWindow window(nullptr, worker); window.show();
        auto open = window.findChild<QPushButton *>("btnOpenClose");
        VERIFY(until([&] { return open->isEnabled(); })); window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        auto eta = window.findChild<QLabel *>("lblFileSendEta"); auto etaTimer = window.findChild<QTimer *>("fileSendEtaTimer");
        auto progress = window.findChild<QProgressBar *>("progressBarFile"); auto file = window.findChild<QPushButton *>("btnSendFile");
        auto path = window.findChild<QLineEdit *>("txtSendFilePath");
        VERIFY(eta && etaTimer && progress && file && path && eta->text() == "00:00:00" && !etaTimer->isActive());
        VERIFY(progress->toolTip().contains("双击") && progress->toolTip().contains("发送中"));
        auto doubleClickProgress = [&](Qt::MouseButton button) {
            const QPoint pos = progress->rect().center();
            QMouseEvent event(QEvent::MouseButtonDblClick, pos, progress->mapToGlobal(pos), button, button, Qt::NoModifier);
            QApplication::sendEvent(progress, &event);
        };
        path->setText(imagePath); progress->setValue(progress->maximum());
        doubleClickProgress(Qt::RightButton); VERIFY(progress->value() == progress->maximum());
        doubleClickProgress(Qt::LeftButton);
        VERIFY(progress->value() == 0 && path->text() == imagePath && eta->text() == "00:00:00");
        QTimer serialWarnings;
        QObject::connect(&serialWarnings, &QTimer::timeout, [&] {
            for (auto dialog : window.findChildren<QDialog *>())
                if (dialog->isVisible() && dialog->windowTitle() == "发送已取消，串口已关闭") dialog->accept();
        });
        serialWarnings.start(10);
        VERIFY(eta->mapTo(&window, QPoint()).x() >= progress->mapTo(&window, QPoint()).x() + progress->width());
        bool selectedAll = false, submitted = false; QTimer pickerDriver; QElapsedTimer pickerDeadline; pickerDeadline.start();
        QObject::connect(&pickerDriver, &QTimer::timeout, [&] { for (auto widget : QApplication::topLevelWidgets()) if (auto picker = qobject_cast<QFileDialog *>(widget))
            if (picker->isVisible() && picker->windowTitle() == "选择要发送的文件") {
                if (!submitted) {
                    submitted = true; selectedAll = picker->selectedNameFilter() == "所有文件 (*)";
                    picker->selectFile(imagePath); QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
                } else if (pickerDeadline.elapsed() > 6000) picker->reject();
            }
        });
        pickerDriver.start(10); VERIFY(QMetaObject::invokeMethod(&window, "onOpenSendFile")); pickerDriver.stop();
        std::cerr << "File transfer UI: all-files picker returned.\n";
        VERIFY(selectedAll && path->text() == imagePath);
        auto ports = window.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
        open->click(); VERIFY(until([&] { return file->isEnabled() && open->text().contains("关闭"); }));
        auto written = [&] { QByteArray bytes; QMetaObject::invokeMethod(worker, [&] { bytes = port->written; }, Qt::BlockingQueuedConnection); return bytes; };
        window.findChild<QCheckBox *>("chkHexSend")->setChecked(true);
        window.findChild<QComboBox *>("comboAppendNewline")->setCurrentIndex(1);
        // Every byte (NUL/high-bit included) is untouched, independent of suffix
        // and the console's HEX/newline switches. These are raw transport tests,
        // not document/image decoding tests.
        for (const auto suffix : {".png", ".docx", ".zip", ""}) {
            const auto target = directory + "/raw-transfer" + suffix;
            VERIFY(ProtocolProfileStore::writeFile(target, payload, error)); path->setText(target);
            const auto before = written(); file->click(); VERIFY(etaTimer->isActive());
            VERIFY(until([&] { return !etaTimer->isActive() && written().size() >= before.size() + payload.size(); }));
            VERIFY(written() == before + payload && eta->text() == "00:00:00" && progress->value() == progress->maximum());
            doubleClickProgress(Qt::LeftButton);
            VERIFY(progress->value() == 0 && path->text() == target && progress->toolTip().contains("双击"));
        }
        std::cerr << "File transfer UI: raw serial fixtures passed.\n";
        const auto slowPath = directory + "/slow-transfer.zip";
        VERIFY(ProtocolProfileStore::writeFile(slowPath, QByteArray(65536, 'F'), error)); path->setText(slowPath);
        QMetaObject::invokeMethod(worker, [port] { port->holdWrites = true; }, Qt::BlockingQueuedConnection);
        file->click(); VERIFY(until([&] { return worker->pendingSendBytes() > 0; }));
        QElapsedTimer warming; warming.start(); VERIFY(until([&] { return warming.elapsed() >= 600; }));
        VERIFY(eta->text() == "--:--:--" && etaTimer->isActive()); // queued bytes are not a speed measurement
        progress->setValue(250); const auto activeEta = eta->text();
        const auto pendingBefore = worker->pendingSendBytes(); doubleClickProgress(Qt::LeftButton);
        VERIFY(progress->value() == 250 && eta->text() == activeEta && etaTimer->isActive()
            && path->text() == slowPath && worker->pendingSendBytes() == pendingBefore);
        QMetaObject::invokeMethod(worker, [port] { port->drainWrites(); }, Qt::BlockingQueuedConnection);
        VERIFY(until([&] { return QRegularExpression("^[0-9]{2,}:[0-9]{2}:[0-9]{2}$").match(eta->text()).hasMatch() && eta->text() != "00:00:00"; }));
        std::cerr << "File transfer UI: measured ETA " << eta->text().toStdString() << "\n";
        if (!artifactDirectory.isEmpty()) window.grab().save(artifactDirectory + "/file-send-eta.png");
        window.findChild<QPushButton *>("btnStopSendFile")->click();
        // Cancellation can outlive the file timer: resetting must remain blocked.
        progress->setValue(375); doubleClickProgress(Qt::LeftButton); VERIFY(progress->value() == 375);
        VERIFY(eta->text() == "00:00:00" && !etaTimer->isActive());
        VERIFY(until([&] { return file->isEnabled() && worker->pendingSendBytes() == 0; }));
        // Canceling a write already submitted to a native driver may safely
        // disconnect the port. Reopen before the separate disconnect test.
        if (window.findChild<QComboBox *>("comboInterface")->isEnabled()) {
            open->click(); VERIFY(until([&] { return open->text().contains("关闭") && file->isEnabled(); }));
        }
        file->click(); VERIFY(etaTimer->isActive()); open->click();
        VERIFY(until([&] { return window.findChild<QComboBox *>("comboInterface")->isEnabled(); }));
        VERIFY(eta->text() == "00:00:00" && !etaTimer->isActive());
        open->click(); VERIFY(until([&] { return open->text().contains("关闭") && file->isEnabled(); }));
        path->setText(directory + "/missing.zip"); messages.lastTitle.clear(); file->click();
        VERIFY(messages.lastTitle == "错误" && eta->text() == "00:00:00" && !etaTimer->isActive());
        open->click(); VERIFY(until([&] { return window.findChild<QComboBox *>("comboInterface")->isEnabled(); })); window.close();
    }
    advanced.setValue("InterfaceType", 1); advanced.sync();
    {
        MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.show();
        auto open = window.findChild<QPushButton *>("btnOpenClose"); VERIFY(until([&] { return open->isEnabled(); }));
        QTcpServer server; VERIFY(server.listen(QHostAddress::LocalHost, 0));
        window.findChild<QLineEdit *>("editIP")->setText("127.0.0.1"); window.findChild<QSpinBox *>("spinPort")->setValue(server.serverPort());
        window.findChild<QComboBox *>("comboLocalIP")->setCurrentText("0.0.0.0"); window.findChild<QSpinBox *>("spinLocalPort")->setValue(0);
        open->click(); VERIFY(until([&] { return server.hasPendingConnections() && open->text().contains("断开"); }));
        auto peer = server.nextPendingConnection(); QByteArray received;
        auto file = window.findChild<QPushButton *>("btnSendFile"); auto eta = window.findChild<QLabel *>("lblFileSendEta");
        auto etaTimer = window.findChild<QTimer *>("fileSendEtaTimer");
        window.findChild<QLineEdit *>("txtSendFilePath")->setText(imagePath); file->click();
        VERIFY(until([&] { received += peer->readAll(); return received.size() == payload.size() && !etaTimer->isActive(); }));
        VERIFY(received == payload && eta->text() == "00:00:00");
        file->click(); VERIFY(etaTimer->isActive()); peer->abort();
        VERIFY(until([&] { return !etaTimer->isActive() && eta->text() == "00:00:00"; }));
        window.close(); delete peer;
    }
    VERIFY(store.load(error) && store.commit(previous, error));
    std::cout << "Raw arbitrary-file serial/TCP sending, all-files picker, ETA actual progress/stop/disconnect/failure passed.\n";
    return 0;
}
static int networkUiTests(const QString &directory, OffscreenMessages &messages) {
    QString error; ProtocolProfileStore store; VERIFY(store.load(error)); const auto previous = store.snapshot();
    auto raw = previous; raw.shared = ProtocolSelection::fromLegacy(0); raw.ble = raw.shared; VERIFY(store.commit(raw, error));
    QSettings advanced("WaveLink", "AdvancedApp");
    for (const auto key : {"NetworkIP", "NetworkPort", "NetworkLocalIP", "NetworkLocalPort", "ServerIP", "ServerPort"}) advanced.remove(key);
    advanced.setValue("InterfaceType", 2); advanced.sync();
    auto matches = [](MainWindow &window) {
        return window.findChild<QLineEdit *>("editIP")->text() == "127.0.0.1" && window.findChild<QSpinBox *>("spinPort")->value() == 32123
            && window.findChild<QComboBox *>("comboLocalIP")->currentText() == "127.0.0.1" && window.findChild<QSpinBox *>("spinLocalPort")->value() == 24680
            && window.findChild<QComboBox *>("comboServerIP")->currentText() == "127.0.0.1" && window.findChild<QSpinBox *>("spinServerPort")->value() == 35400;
    };
    {
        MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.show();
        auto role = window.findChild<QComboBox *>("comboInterface"); auto open = window.findChild<QPushButton *>("btnOpenClose");
        auto remoteIP = window.findChild<QLineEdit *>("editIP"); auto remotePort = window.findChild<QSpinBox *>("spinPort");
        auto localIP = window.findChild<QComboBox *>("comboLocalIP"); auto localPort = window.findChild<QSpinBox *>("spinLocalPort");
        auto serverIP = window.findChild<QComboBox *>("comboServerIP"); auto serverPort = window.findChild<QSpinBox *>("spinServerPort");
        auto endpoint = window.findChild<QLabel *>("lblClientEndpoint");
        VERIFY(until([&] { return open->isEnabled(); }));
        VERIFY(serverIP->isEditable() && serverIP->currentText() == "0.0.0.0" && serverIP->findText("0.0.0.0") >= 0);
        VERIFY(window.findChild<QLabel *>("lblServerListenHint")->text().contains("所有本机") && localIP->currentText() == "0.0.0.0" && localPort->value() == 0);
        serverIP->setEditText("192.0.2.123");
        VERIFY(QMetaObject::invokeMethod(serverIP, "activated", Q_ARG(int, serverIP->findText("0.0.0.0"))) && serverIP->currentText() == "0.0.0.0");
        QTcpServer peerServer; VERIFY(peerServer.listen(QHostAddress::LocalHost, 0));
        role->setCurrentIndex(role->findData(1)); VERIFY(until([&] { return open->isEnabled(); }));
        remoteIP->setText("127.0.0.1"); remotePort->setValue(peerServer.serverPort()); localIP->setCurrentText("127.0.0.1");
        open->click();
        VERIFY(!remoteIP->isEnabled() && !localIP->isEnabled() && !localPort->isEnabled());
        VERIFY(until([&] { return peerServer.hasPendingConnections() && open->text().contains("断开网络"); }));
        auto peer = peerServer.nextPendingConnection();
        VERIFY(until([&] { return endpoint->isVisible() && endpoint->text().contains(QString::number(peer->peerPort())); }));
        VERIFY(endpoint->text().contains("127.0.0.1"));
        open->click(); VERIFY(until([&] { return role->isEnabled(); }));
        VERIFY(localIP->isEnabled() && localPort->isEnabled() && endpoint->isHidden());
        role->setCurrentIndex(role->findData(2)); VERIFY(until([&] { return open->isEnabled(); }));
        serverIP->setCurrentText("not-an-ip"); messages.lastTitle.clear(); open->click();
        VERIFY(until([&] { return open->text().contains("重新") && messages.lastTitle == "打开失败"; }));
        VERIFY(serverIP->currentText() == "not-an-ip" && serverIP->isEnabled() && messages.lastText.contains("监听 IP"));
        QTcpServer reserve; VERIFY(reserve.listen(QHostAddress::LocalHost, 0)); const auto listenPort = reserve.serverPort(); reserve.close();
        serverIP->setCurrentText("127.0.0.1"); serverPort->setValue(listenPort); open->click();
        VERIFY(until([&] { return open->text().contains("关闭服务器"); }) && !serverIP->isEnabled() && !serverPort->isEnabled());
        QTcpSocket peerClient; peerClient.connectToHost(QHostAddress::LocalHost, listenPort);
        VERIFY(until([&] { return peerClient.state() == QAbstractSocket::ConnectedState && window.findChild<QCheckBox *>("chkConsolePeriodicSend")->isEnabled(); }));
        window.findChild<QCheckBox *>("chkHexSend")->setChecked(false); window.findChild<QComboBox *>("comboAppendNewline")->setCurrentIndex(0);
        window.findChild<QTextEdit *>("textSend")->setPlainText("SERVER"); window.findChild<QPushButton *>("btnSend")->click();
        VERIFY(until([&] { return peerClient.bytesAvailable() == 6; }) && peerClient.readAll() == "SERVER");
        open->click(); VERIFY(until([&] { return role->isEnabled(); }));
        remoteIP->setText("127.0.0.1"); remotePort->setValue(32123); localIP->setCurrentText("127.0.0.1"); localPort->setValue(24680);
        serverIP->setCurrentText("127.0.0.1"); serverPort->setValue(35400);
        VERIFY(advanced.value("NetworkLocalIP").toString() == "127.0.0.1" && advanced.value("NetworkLocalPort").toInt() == 24680
            && advanced.value("ServerIP").toString() == "127.0.0.1" && advanced.value("ServerPort").toInt() == 35400);
        const auto path = directory + "/tcp-settings.ini"; VERIFY(configFileAction(window, path, "onExportConfig"));
        QVariantMap values; VERIFY(ProtocolConfigTransfer::read(path, values, error));
        VERIFY(values.value("AdvancedApp/NetworkIP").toString() == "127.0.0.1" && values.value("AdvancedApp/NetworkPort").toInt() == 32123
            && values.value("AdvancedApp/NetworkLocalPort").toInt() == 24680 && values.value("AdvancedApp/ServerIP").toString() == "127.0.0.1");
        remoteIP->setText("localhost"); remotePort->setValue(12345); localIP->setCurrentText("0.0.0.0"); localPort->setValue(0);
        serverIP->setCurrentText("0.0.0.0"); serverPort->setValue(12346); role->setCurrentIndex(role->findData(0));
        VERIFY(until([&] { return open->isEnabled(); }));
        VERIFY(configFileAction(window, path, "onImportConfig") && until([&] { return open->isEnabled() && role->currentData().toInt() == 2; }));
        VERIFY(matches(window) && window.findChild<QLabel *>("lblServerListenHint")->text().contains("指定"));
        VERIFY(!window.findChild<QCheckBox *>("chkConsolePeriodicSend")->isChecked()); window.close();
    }
    {
        MainWindow restarted(nullptr, new TestSerialHandler(new ReceivePort)); restarted.show();
        auto role = restarted.findChild<QComboBox *>("comboInterface"); auto open = restarted.findChild<QPushButton *>("btnOpenClose");
        VERIFY(until([&] { return open->isEnabled(); }) && role->currentData().toInt() == 2 && matches(restarted));
        if (!artifactDirectory.isEmpty()) for (const auto mode : {1, 2}) for (auto theme : {ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
            role->setCurrentIndex(role->findData(mode)); VERIFY(until([&] { return open->isEnabled(); }));
            ThemeManager::applyTheme(qApp, theme); QApplication::processEvents(); QApplication::processEvents();
            restarted.findChild<QGroupBox *>("groupDeviceConnection")->grab().save(artifactDirectory + QString("/tcp-settings-%1-%2.png").arg(mode).arg(theme));
        }
        restarted.close();
    }
    VERIFY(store.load(error) && store.commit(previous, error)); return 0;
}
static int udpUiTests(const QString &directory, OffscreenMessages &messages) {
    QString error; ProtocolProfileStore store; VERIFY(store.load(error)); const auto previous = store.snapshot();
    auto raw = previous; raw.shared = ProtocolSelection::fromLegacy(0); raw.ble = raw.shared; VERIFY(store.commit(raw, error));
    QSettings settings("WaveLink", "AdvancedApp"); settings.remove("Udp"); settings.setValue("InterfaceType", 5); settings.sync();
    auto matches = [](MainWindow &window) {
        return window.findChild<QComboBox *>("comboUdpLocalIP")->currentText() == "127.0.0.1"
            && window.findChild<QSpinBox *>("spinUdpLocalPort")->value() == 41234
            && window.findChild<QLineEdit *>("editUdpRemoteIP")->text() == "127.0.0.1"
            && window.findChild<QSpinBox *>("spinUdpRemotePort")->value() == 42345
            && !window.findChild<QCheckBox *>("chkUdpFilterRemote")->isChecked();
    };
    {
        MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.show();
        auto role = window.findChild<QComboBox *>("comboInterface"); auto open = window.findChild<QPushButton *>("btnOpenClose");
        auto localIP = window.findChild<QComboBox *>("comboUdpLocalIP"); auto localPort = window.findChild<QSpinBox *>("spinUdpLocalPort");
        auto remoteIP = window.findChild<QLineEdit *>("editUdpRemoteIP"); auto remotePort = window.findChild<QSpinBox *>("spinUdpRemotePort");
        auto filter = window.findChild<QCheckBox *>("chkUdpFilterRemote"); auto endpoint = window.findChild<QLabel *>("lblUdpEndpoint");
        auto send = window.findChild<QPushButton *>("btnSend"); auto file = window.findChild<QPushButton *>("btnSendFile");
        auto periodic = window.findChild<QCheckBox *>("chkConsolePeriodicSend");
        VERIFY(until([&] { return open->isEnabled(); }) && role->currentData().toInt() == 5);
        VERIFY(role->findData(5) == role->findData(2) + 1 && role->findData(5) < role->findData(4) && role->itemData(role->count() - 1).toInt() == 3);
        VERIFY(window.findChild<QStackedWidget *>("stackInterface")->currentWidget()->objectName() == "udpSettingsPage");
        VERIFY(localIP && localIP->isEditable() && localIP->currentText() == "0.0.0.0" && localIP->findText("0.0.0.0") >= 0);
        VERIFY(localPort->value() == 4970 && remoteIP->text().isEmpty() && remotePort->value() == 0 && filter->isChecked());
        QUdpSocket peer, reserve; peer.setProxy(QNetworkProxy::NoProxy); reserve.setProxy(QNetworkProxy::NoProxy);
        VERIFY(peer.bind(QHostAddress(QHostAddress::LocalHost), 0) && reserve.bind(QHostAddress(QHostAddress::LocalHost), 0));
        const auto listenPort = reserve.localPort(); reserve.close(); localIP->setCurrentText("127.0.0.1"); localPort->setValue(listenPort);
        open->click(); VERIFY(!localIP->isEnabled() && !remoteIP->isEnabled());
        VERIFY(until([&] { return open->text().contains("关闭 UDP"); }));
        VERIFY(endpoint->isVisible() && endpoint->text().contains(QString::number(listenPort)));
        VERIFY(!periodic->isEnabled() && !send->isEnabled() && !file->isEnabled());
        window.findChild<QCheckBox *>("chkHexReceive")->setChecked(false);
        window.findChild<QPlainTextEdit *>("textReceive")->clear();
        VERIFY(peer.writeDatagram("UDP-RECEIVE", QHostAddress::LocalHost, listenPort) == 11);
        VERIFY(until([&] { return window.findChild<QPlainTextEdit *>("textReceive")->toPlainText().contains("UDP-RECEIVE"); }));
        open->click(); VERIFY(until([&] { return role->isEnabled(); }) && localIP->isEnabled() && endpoint->isHidden());
        localIP->setCurrentText("bad-local"); messages.lastTitle.clear(); open->click();
        VERIFY(until([&] { return open->text().contains("重新打开 UDP") && messages.lastTitle == "打开失败"; }));
        VERIFY(localIP->isEnabled() && messages.lastText.contains("UDP 本机 IP"));
        localIP->setCurrentText("127.0.0.1"); remoteIP->setText("127.0.0.1"); remotePort->setValue(peer.localPort());
        open->click(); VERIFY(until([&] { return open->text().contains("关闭 UDP") && send->isEnabled(); }));
        VERIFY(!filter->isEnabled() && periodic->isEnabled() && !file->isEnabled());
        window.findChild<QCheckBox *>("chkHexSend")->setChecked(true); window.findChild<QComboBox *>("comboAppendNewline")->setCurrentIndex(0);
        window.findChild<QTextEdit *>("textSend")->setPlainText("AA 55 00 40 FF"); send->click();
        VERIFY(until([&] { return peer.hasPendingDatagrams(); }));
        QByteArray bytes; bytes.resize(65535); quint16 sourcePort = 0; const auto n = peer.readDatagram(bytes.data(), bytes.size(), nullptr, &sourcePort); bytes.resize(int(n));
        VERIFY(bytes == QByteArray::fromHex("AA550040FF") && sourcePort == listenPort);
        window.findChild<QSpinBox *>("spinConsoleSendInterval")->setValue(25); periodic->setChecked(true);
        VERIFY(until([&] { return peer.hasPendingDatagrams(); }));
        open->click(); VERIFY(until([&] { return role->isEnabled(); }) && !periodic->isChecked());
        // Shared numeric protocol must feed the same plot delivery path.
        auto protocol = window.findChild<QComboBox *>("comboProtocol"); protocol->setCurrentIndex(1);
        VERIFY(until([&] { return open->isEnabled() && protocol->isEnabled(); })); open->click();
        VERIFY(until([&] { return open->text().contains("关闭 UDP"); }));
        VERIFY(peer.writeDatagram("1,2\n3,4\n", QHostAddress::LocalHost, listenPort) == 8);
        VERIFY(until([&] { return window.findChild<QListWidget *>("listChannels")->count() == 2; }));
        open->click(); VERIFY(until([&] { return role->isEnabled(); }));
        confirmSwitch(); protocol->setCurrentIndex(0); VERIFY(until([&] { return open->isEnabled() && protocol->isEnabled(); }));
        localIP->setCurrentText("127.0.0.1"); localPort->setValue(41234); remoteIP->setText("127.0.0.1"); remotePort->setValue(42345); filter->setChecked(false);
        VERIFY(settings.value("Udp/LocalPort").toInt() == 41234 && settings.value("Udp/RemoteIP").toString() == "127.0.0.1" && !settings.value("Udp/FilterRemote").toBool());
        const auto path = directory + "/udp-settings.ini"; VERIFY(configFileAction(window, path, "onExportConfig"));
        QVariantMap values; VERIFY(ProtocolConfigTransfer::read(path, values, error));
        VERIFY(values.value("AdvancedApp/Udp/LocalPort").toInt() == 41234 && values.value("AdvancedApp/Udp/RemotePort").toInt() == 42345
            && !values.value("AdvancedApp/Udp/FilterRemote").toBool() && values.value("AdvancedApp/InterfaceType").toInt() == 5);
        localIP->setCurrentText("0.0.0.0"); localPort->setValue(0); remoteIP->clear(); remotePort->setValue(0); filter->setChecked(true);
        role->setCurrentIndex(role->findData(1)); VERIFY(until([&] { return open->isEnabled(); }));
        VERIFY(configFileAction(window, path, "onImportConfig") && until([&] { return open->isEnabled() && role->currentData().toInt() == 5; }));
        VERIFY(matches(window) && open->text().contains("打开 UDP") && endpoint->isHidden() && !periodic->isChecked());
        window.close();
    }
    {
        MainWindow restarted(nullptr, new TestSerialHandler(new ReceivePort)); restarted.show();
        auto open = restarted.findChild<QPushButton *>("btnOpenClose");
        VERIFY(until([&] { return open->isEnabled(); }) && restarted.findChild<QComboBox *>("comboInterface")->currentData().toInt() == 5 && matches(restarted));
        VERIFY(open->text().contains("打开 UDP") && restarted.findChild<QLabel *>("lblUdpEndpoint")->isHidden());
        if (!artifactDirectory.isEmpty()) for (auto theme : {ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
            ThemeManager::applyTheme(qApp, theme); QApplication::processEvents(); QApplication::processEvents();
            auto group = restarted.findChild<QGroupBox *>("groupDeviceConnection");
            const auto page = restarted.findChild<QWidget *>("udpSettingsPage");
            for (auto field : page->findChildren<QWidget *>()) {
                if (!field->isVisible() || (!qobject_cast<QComboBox *>(field) && !qobject_cast<QLineEdit *>(field) && !qobject_cast<QSpinBox *>(field))) continue;
                const auto rect = QRect(field->mapTo(group, QPoint()), field->size());
                VERIFY(rect.right() < group->width() && rect.bottom() < group->height());
            }
            // Capture through the main window so transparent group/page
            // backgrounds include the themed ancestor instead of a light
            // default standalone widget palette.
            restarted.grab(QRect(group->mapTo(&restarted, QPoint()), group->size())).save(artifactDirectory + QString("/udp-settings-%1.png").arg(theme));
        }
        restarted.close();
    }
    VERIFY(store.load(error) && store.commit(previous, error)); return 0;
}
static int cacheClearUiTests(const QString &directory, OffscreenMessages &messages) {
    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    QCoreApplication::processEvents(); QCoreApplication::processEvents();
    const auto base = directory + "/clear-cache-test", folder = base + "/WaveLink_Cache";
    VERIFY(QDir().mkpath(folder + "/keep-subdirectory"));
    auto write = [](const QString &path, const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); };
    auto read = [](const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
    const auto csv = folder + "/first.csv", part = folder + "/pending.wlr.part", nested = folder + "/keep-subdirectory/kept.csv", outside = base + "/saved.csv";
    VERIFY(write(csv, "RAW-CSV") && write(part, "RAW-WLR-PART") && write(nested, "KEEP-NESTED") && write(outside, "KEEP-SAVED"));
    auto cache = window.findChild<QLineEdit *>("editCacheDir"); auto status = window.findChild<QLabel *>("lblRecordStatus");
    auto button = window.findChild<QPushButton *>("btnClearCache"); VERIFY(cache && status && button);
    auto plot = window.findChild<QCustomPlot *>(); VERIFY(plot); auto graph = plot->addGraph(); graph->addData(0, 123); graph->addData(1, 456);
    cache->setText(base); status->setText("00:01:23 | 1.00 MB");
    auto respond = [&](int mode, const std::function<void()> &during = {}) {
        bool seen = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("cacheClearConfirm"); if (!dialog) return;
            auto message = dialog->findChild<QLabel *>("cacheClearMessage"); auto actions = dialog->findChild<QDialogButtonBox *>();
            auto confirm = dialog->findChild<QPushButton *>("confirmClearCache");
            bool safeDefault = false; if (actions) for (auto action : actions->buttons())
                if (actions->buttonRole(action) == QDialogButtonBox::RejectRole) safeDefault = qobject_cast<QPushButton *>(action)->isDefault();
            seen = message && confirm && safeDefault && !confirm->autoDefault() && message->text().contains("释放磁盘空间")
                && message->text().contains("不会清空当前显示的波形") && message->text().contains("无法撤销")
                && message->height() >= message->heightForWidth(message->width());
            if (!artifactDirectory.isEmpty() && mode == 0) dialog->grab().save(artifactDirectory + "/cache-clear-confirm.png");
            if (during) during();
            if (mode == 1 && confirm) confirm->click();
            else if (mode == -1) dialog->close();
            else { QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QCoreApplication::sendEvent(dialog, &escape); }
        });
        button->click(); return seen;
    };
    VERIFY(respond(0)); VERIFY(read(csv) == "RAW-CSV" && read(part) == "RAW-WLR-PART" && status->text() == "00:01:23 | 1.00 MB");
    VERIFY(respond(-1)); VERIFY(read(csv) == "RAW-CSV" && read(part) == "RAW-WLR-PART" && status->text() == "00:01:23 | 1.00 MB");
    // Files appearing or being changed during the confirmation are not part
    // of the approved deletion. Subfolders and independently saved files stay.
    bool changed = false; const auto arrived = folder + "/arrived.csv";
    VERIFY(respond(1, [&] { changed = write(part, "CHANGED-WLR-CONTENT") && write(arrived, "NEW-DATA"); }));
    VERIFY(changed && !QFile::exists(csv) && read(part) == "CHANGED-WLR-CONTENT" && read(arrived) == "NEW-DATA");
    VERIFY(read(nested) == "KEEP-NESTED" && read(outside) == "KEEP-SAVED" && status->text() == "00:01:23 | 1.00 MB");
    {
        const auto active = folder + "/active.csv.part"; VERIFY(write(active, "LIVE")); QLockFile lock(active + ".lock"); lock.setStaleLockTime(0); VERIFY(lock.tryLock(0));
        messages.lastTitle.clear(); VERIFY(respond(1));
        VERIFY(messages.lastTitle == "缓存正在使用" && read(part) == "CHANGED-WLR-CONTENT" && read(active) == "LIVE");
    }
    const auto alternate = directory + "/different-cache"; VERIFY(QDir().mkpath(alternate + "/WaveLink_Cache"));
    const auto otherFile = alternate + "/WaveLink_Cache/unrelated.csv"; VERIFY(write(otherFile, "UNRELATED"));
    VERIFY(respond(1, [&] { cache->setText(alternate); }));
    VERIFY(read(otherFile) == "UNRELATED" && read(part) == "CHANGED-WLR-CONTENT"); cache->setText(base);
    VERIFY(respond(1));
    VERIFY(!QFile::exists(part) && !QFile::exists(arrived) && !QFile::exists(folder + "/active.csv.part"));
    VERIFY(read(nested) == "KEEP-NESTED" && read(outside) == "KEEP-SAVED" && status->text() == "00:00:00 | 0.00 MB");
    VERIFY(graph->data()->size() == 2 && graph->data()->constBegin()->value == 123);
    messages.lastTitle.clear(); button->click(); VERIFY(messages.lastTitle == "清空缓存" && messages.lastText.contains("没有需要清理"));
    window.close(); return 0;
}
static int plotInteractionTests() {
    // The preceding layout tests intentionally save collapsed/narrow docks.
    // Reset ONLY this process's sandboxed layout before simulating real drags.
    QSettings layout("WaveLink", "WaveLink");
    for (const auto &key : {"windowState", "innerWindowState", "windowGeometry", "normalCollapsedDocks", "dataModeWindowState", "dataTransceiveModeActive"}) layout.remove(key);
    layout.sync();
    QSettings connectionSettings("WaveLink", "AdvancedApp"); connectionSettings.setValue("InterfaceType", 0); connectionSettings.sync();
    ProtocolProfileStore store; QString error;
    VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1500, 950); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    QElapsedTimer layoutReady; layoutReady.start();
    VERIFY(until([&] { return layoutReady.elapsed() > 180; })); // includes deferred layout restore
    window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    auto paused = window.findChild<QPushButton *>("btnPausePlot");
    QCheckBox *automatic = nullptr;
    for (auto check : window.findChildren<QCheckBox *>()) {
        if (check->text() == "Y轴自适应") automatic = check;
    }
    QDoubleSpinBox *fs = nullptr;
    for (auto spin : window.findChildren<QDoubleSpinBox *>()) if (spin->suffix() == " Hz" && spin->maximum() == 100000) fs = spin;
    VERIFY(fs && automatic && paused); automatic->setChecked(true); paused->setChecked(false); fs->setValue(1);
    const QVector<double> frame(12, 0.0);
    for (int i = 0; i < 250; ++i) VERIFY(QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, frame)));
    VERIFY(QMetaObject::invokeMethod(&window, "onPlotTimer"));
    QCustomPlot *main = nullptr;
    for (auto plot : window.findChildren<QCustomPlot *>()) if (plot->yAxis->label() == "数据值" && plot->graphCount() == 12) main = plot;
    VERIFY(main && main->isVisible());
    for (auto check : window.findChildren<QCheckBox *>("chkChannel")) check->setChecked(true);
    for (int ch = 0; ch < 12; ++ch) {
        auto graph = dynamic_cast<WaveformGraph *>(main->graph(ch)); VERIFY(graph);
        QVector<QCPGraphData> data; data.reserve(30000);
        for (int i = 0; i < 30000; ++i) data.append({i * .01, ch * 10 + std::sin(i * .01)});
        if (ch == 3) { data[12000].value = 500; data[25000].value = 10000; }
        graph->data()->set(data, true);
    }
    auto slider = window.findChild<RangeSlider *>("timeRangeSlider"); VERIFY(slider);
    slider->rangeChanged(100, 200);
    VERIFY(until([&] { return main->yAxis->range().upper > 500 && main->yAxis->range().upper < 2000; }));
    VERIFY(main->yAxis->range().lower < -1 && automatic->isChecked());
    slider->rangeChanged(0, 60);
    VERIFY(until([&] { return main->yAxis->range().upper > 111 && main->yAxis->range().upper < 200; }));
    for (int ch = 0; ch < 12; ++ch) VERIFY(main->graph(ch)->dataCount() == 30000);
    // Exercise the actual QCustomPlot press/move/release route with autoscale ON.
    int renders = 0; const auto connection = QObject::connect(main, &QCustomPlot::afterReplot, [&] { ++renders; });
    const QPoint start = main->axisRect()->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(main, &press);
    QElapsedTimer elapsed; elapsed.start(); int moves = 0;
    while (elapsed.elapsed() < 200) {
        QMouseEvent move(QEvent::MouseMove, start - QPoint(20 + moves % 30, 0), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(main, &move); QCoreApplication::processEvents(); QThread::msleep(1); ++moves;
    }
    QMouseEvent release(QEvent::MouseButtonRelease, start - QPoint(30, 0), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(main, &release);
    VERIFY(until([&] { return renders > 0; })); QObject::disconnect(connection);
    VERIFY(moves > 5 && renders <= 10 && automatic->isChecked());
    if (!(main->xAxis->range().lower > 0 && main->yAxis->range().upper < 200))
        std::cerr << "Drag details: x=" << main->xAxis->range().lower << "," << main->xAxis->range().upper
            << " y=" << main->yAxis->range().lower << "," << main->yAxis->range().upper
            << " rect=" << main->axisRect()->width() << "x" << main->axisRect()->height() << " moves=" << moves << " renders=" << renders << '\n';
    VERIFY(main->xAxis->range().lower > 0 && main->yAxis->range().upper < 200);
    for (int ch = 0; ch < 12; ++ch) {
        auto graph = dynamic_cast<WaveformGraph *>(main->graph(ch));
        VERIFY(graph->dataCount() == 30000 && graph->lastRangeSamples() <= 14);
    }
    // Detached copies use the same index, and independently autoscale their Y.
    QListWidget *channels = nullptr;
    for (auto list : window.findChildren<QListWidget *>()) if (list->count() == 12 && list->itemWidget(list->item(0))) channels = list;
    VERIFY(channels); bool detached = false;
    QTimer::singleShot(0, [&] { for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
        for (auto action : menu->actions()) if (action->text() == "分离到新视图") { action->trigger(); detached = true; break; }
        menu->close();
    } });
    VERIFY(QMetaObject::invokeMethod(&window, "onChannelContextMenu", Q_ARG(QPoint, channels->visualItemRect(channels->item(0)).center())) && detached);
    QCustomPlot *separate = nullptr;
    for (auto plot : window.findChildren<QCustomPlot *>()) if (plot != main && plot->yAxis->label() == "数据值" && plot->graphCount() == 1) separate = plot;
    VERIFY(separate && dynamic_cast<WaveformGraph *>(separate->graph(0)) && separate->graph(0)->dataCount() == 30000);
    slider->rangeChanged(100, 200);
    VERIFY(until([&] { return separate->yAxis->range().upper < 2 && separate->yAxis->range().lower < -1; }));
    VERIFY(main->yAxis->range().upper > 500 && separate->xAxis->range() == main->xAxis->range());
    if (!artifactDirectory.isEmpty()) window.grab().save(artifactDirectory + "/plot-large-history.png");
    // The history limit includes display indexes, not only raw QCP samples.
    QSpinBox *limit = nullptr;
    for (auto spin : window.findChildren<QSpinBox *>()) if (spin->minimum() == 16 && spin->maximum() == 2048) limit = spin;
    VERIFY(limit); limit->setValue(64);
    QVector<WaveformGraph *> histories;
    for (auto plot : window.findChildren<QCustomPlot *>()) if (plot->yAxis->label() == "数据值") for (int ch = 0; ch < plot->graphCount(); ++ch) {
        auto graph = dynamic_cast<WaveformGraph *>(plot->graph(ch)); VERIFY(graph); histories.append(graph);
        QVector<QCPGraphData> data; data.reserve(100000);
        for (int i = 0; i < 100000; ++i) data.append({i * .01, std::sin(i * .01)});
        graph->data()->set(data, true);
        bool found; graph->getValueRange(found); VERIFY(found);
    }
    limit->setValue(16);
    qint64 historyBytes = 0;
    for (auto graph : histories) {
        VERIFY(graph->dataCount() > 0 && graph->dataCount() < 100000);
        bool found = false; graph->getValueRange(found); VERIFY(found);
        historyBytes += graph->dataCount() * qint64(sizeof(QCPGraphData)) + graph->cacheBytes();
    }
    VERIFY(historyBytes <= 16LL * 1024 * 1024 && automatic->isChecked());
    window.close();
    std::cout << "12-channel long history: slider/drag autoscale, coalesced rendering, full-resolution data and detached views passed.\n";
    return 0;
}
static int interfaceHintsUiTests() {
    QTemporaryDir directory; VERIFY(directory.isValid()); QString error;
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    main.setValue("editCacheDir", directory.path()); main.sync();
    ProtocolProfileStore store; VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    for (int saved : {5, 4}) {
        advanced.setValue("InterfaceType", saved); advanced.sync();
        MainWindow host(nullptr, new TestSerialHandler(new ReceivePort)); host.resize(1200, 1000); host.show();
        auto interfaces = host.findChild<QComboBox *>("comboInterface");
        auto stack = host.findChild<QStackedWidget *>("stackInterface");
        auto label = host.findChild<QLabel *>("interfaceTypeLabel");
        VERIFY(interfaces && stack && label && interfaces->currentData().toInt() == saved && stack->currentIndex() == saved);
        VERIFY(interfaces->count() == 6 && interfaces->itemText(3) == "UDP 用户数据报" &&
               interfaces->itemText(4) == "BLE 低功耗蓝牙" && interfaces->itemText(5) == "Demo数据");
        VERIFY(label->toolTip() == interfaces->toolTip());
        for (int row = 0; row < interfaces->count(); ++row) {
            const auto tip = interfaces->itemData(row, Qt::ToolTipRole).toString();
            VERIFY(!tip.isEmpty() && label->toolTip().contains(tip));
        }
        const QVector<int> ids{0, 1, 2, 5, 4, 3};
        for (int row = 0; row < ids.size(); ++row) VERIFY(interfaces->itemData(row).toInt() == ids[row]);
        host.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        for (int id : ids) {
            interfaces->setCurrentIndex(interfaces->findData(id));
            VERIFY(until([&] { return stack->currentIndex() == id && interfaces->currentData().toInt() == id; }));
            auto page = stack->currentWidget(); int hinted = 0;
            for (auto form : page->findChildren<QFormLayout *>()) for (int row = 0; row < form->rowCount(); ++row) {
                auto item = form->itemAt(row, QFormLayout::LabelRole); auto caption = item ? qobject_cast<QLabel *>(item->widget()) : nullptr;
                if (!caption || caption->text().isEmpty()) continue;
                if (caption->toolTip().isEmpty()) std::cerr << "Missing connection hint: id=" << id << " label=" << caption->text().toStdString() << "\n";
                VERIFY(!caption->toolTip().isEmpty() && caption->toolTip().size() < 180); ++hinted;
            }
            VERIFY(hinted >= 2 && advanced.value("InterfaceType").toInt() == id);
            if (id == 0) VERIFY(host.findChild<QCheckBox *>("chkDtr")->toolTip().contains("复位"));
            for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
                ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents();
                if (saved == 5) {
                    const auto artifacts = QDir::current().filePath("artifacts"); QDir().mkpath(artifacts);
                    auto group = host.findChild<QGroupBox *>("groupDeviceConnection"); VERIFY(group);
                    VERIFY(group->grab().save(artifacts + QString("/connection-hints-%1-%2-%3.png").arg(id).arg(int(theme)).arg(host.devicePixelRatioF())));
                }
            }
        }
        interfaces->setCurrentIndex(interfaces->findData(saved)); host.close();
    }
    std::cout << "Connection interface UI: order/names, stable legacy IDs, startup restore and short label hints on all pages passed.\n";
    return 0;
}
#include "systemsettingsuitests.h"
#include "receivehighrateuitests.h"
static int receiveCacheUiTests() {
    QTemporaryDir directory; VERIFY(directory.isValid()); QString error;
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    main.remove("Console/ReceiveMaxLines"); main.setValue("chkHexReceive", false);
    main.setValue("editCacheDir", directory.path()); main.sync();
    advanced.setValue("InterfaceType", 0); advanced.sync();
    ProtocolProfileStore store; VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(1), error));
    {
        auto port = new ReceivePort; auto worker = new TestSerialHandler(port);
        MainWindow host(nullptr, worker); host.resize(1200, 900); host.show();
        auto receive = host.findChild<QPlainTextEdit *>("textReceive"); auto lines = host.findChild<QComboBox *>("comboReceiveLines");
        auto open = host.findChild<QPushButton *>("btnOpenClose"); auto hex = host.findChild<QCheckBox *>("chkHexReceive");
        VERIFY(receive && lines && hex && until([&] { return open->isEnabled(); }));
        host.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        VERIFY(lines->count() == 3 && lines->itemData(0).toInt() == 1000 && lines->itemData(1).toInt() == 2000 && lines->itemData(2).toInt() == 5000);
        VERIFY(lines->currentData().toInt() == 1000 && receive->document()->maximumBlockCount() == 1000);
        VERIFY(editSystemSettings(host, 5000, 60000, 100, 30));
        auto ports = host.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
        open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
        host.findChild<QComboBox *>("comboRecordingMode")->setCurrentIndex(1);
        auto record = host.findChild<QPushButton *>("btnToggleRecord"); record->click(); VERIFY(record->text().contains("停止"));
        QByteArray packet; for (int i = 0; i < 5100; ++i) packet += QByteArray::number(i).rightJustified(6, '0') + ",2.5\n";
        VERIFY(packet.size() > 50000 && feed(worker, port, packet));
        VERIFY(until([&] { return receive->toPlainText().endsWith("005099,2.5\n"); }));
        VERIFY(receive->document()->blockCount() == 5000 && receive->toPlainText().startsWith("000101,2.5\n"));
        VERIFY(host.findChildren<QCheckBox *>("chkChannel").size() == 2 && record->text().contains("停止"));
        record->click(); VERIFY(until([&] { return record->text().contains("开始"); }));
        const QDir cache(directory.filePath("WaveLink_Cache")); const auto files = cache.entryList({"*.wlr"}, QDir::Files);
        VERIFY(files.size() == 1); int frames = 0;
        VERIFY(WlrRecordingWriter::visitFiles({cache.filePath(files[0])}, [&](qint64, const QVector<double> &values, qint64) {
            if (values.size() != 2 || values[0] != frames || values[1] != 2.5) return false; ++frames; return true;
        }, error) && frames == 5100); // Display eviction never evicts recorded samples.
        lines->setCurrentIndex(lines->findData(1000));
        VERIFY(receive->document()->blockCount() == 1000 && receive->toPlainText().startsWith("004101,2.5\n") && receive->toPlainText().endsWith("005099,2.5\n"));
        lines->setCurrentIndex(lines->findData(5000));
        VERIFY(feed(worker, port, "5100,2.5\n") && until([&] { return receive->toPlainText().endsWith("5100,2.5\n"); }));
        VERIFY(receive->toPlainText().startsWith("004101,2.5\n"));
        host.findChild<QPushButton *>("btnClearReceive")->click();
        VERIFY(feed(worker, port, QString("温度正常\n").toUtf8()) && until([&] { return receive->toPlainText() == "温度正常\n"; }));
        host.findChild<QPushButton *>("btnClearReceive")->click(); hex->click();
        VERIFY(feed(worker, port, QByteArray(60000, char(0xAA))) && until([&] { return receive->toPlainText().size() == 60000; }));
        VERIFY(receive->toPlainText() == QString("AA ").repeated(20000));
        for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
            ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents();
            VERIFY(lines->isVisible() && lines->height() >= lines->fontMetrics().height());
            auto save = host.findChild<QPushButton *>("btnSaveReceive"), copy = host.findChild<QPushButton *>("btnCopyReceive");
            auto clear = host.findChild<QPushButton *>("btnClearReceive"); auto cacheLabel = host.findChild<QLabel *>("receiveCacheLabel");
            VERIFY(save && copy && clear && cacheLabel && save->isVisible() && copy->isVisible());
            VERIFY(save->mapTo(&host, QPoint()).y() > clear->mapTo(&host, QPoint()).y() + clear->height() / 2 &&
                   save->mapTo(&host, QPoint()).y() == copy->mapTo(&host, QPoint()).y() &&
                   save->mapTo(&host, QPoint()).x() < copy->mapTo(&host, QPoint()).x() &&
                   copy->mapTo(&host, QPoint()).x() < cacheLabel->mapTo(&host, QPoint()).x() &&
                   cacheLabel->mapTo(&host, QPoint()).x() < lines->mapTo(&host, QPoint()).x());
            VERIFY(cacheLabel->toolTip() == lines->toolTip() && lines->toolTip().contains("不影响协议解析、波形或录制"));
            for (auto label : host.findChildren<QLabel *>()) VERIFY(label->text() != "超限移除最旧内容 · 不影响录制");
            const auto artifacts = QDir::current().filePath("artifacts"); VERIFY(QDir().mkpath(artifacts));
            auto console = host.findChild<QDockWidget *>("DockConsole"); VERIFY(console);
            VERIFY(console->grab().save(artifacts + QString("/receive-toolbar-%1-%2.png").arg(int(theme)).arg(host.devicePixelRatioF())));
        }
        open->click(); VERIFY(until([&] { return open->isEnabled() && !open->text().contains("关闭"); })); host.close();
    }
    {
        MainWindow restarted(nullptr, new TestSerialHandler(new ReceivePort));
        VERIFY(restarted.findChild<QComboBox *>("comboReceiveLines")->currentData().toInt() == 5000 &&
               restarted.findChild<QPlainTextEdit *>("textReceive")->document()->maximumBlockCount() == 5000);
        restarted.close();
    }
    main.setValue("Console/ReceiveMaxLines", -1); main.sync();
    { MainWindow invalid(nullptr, new TestSerialHandler(new ReceivePort)); VERIFY(invalid.findChild<QComboBox *>("comboReceiveLines")->currentData().toInt() == 1000); invalid.close(); }
    std::cout << "Receive cache UI: rolling history, settings/restart, 50K regression, HEX/Chinese and full 5100-frame recording passed.\n";
    return 0;
}
static int commandSwitchLiveUiTests(OffscreenMessages &messages) {
    QTemporaryDir directory; VERIFY(directory.isValid());
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    advanced.setValue("InterfaceType", 0); main.setValue("editCacheDir", directory.path()); main.setValue("comboTheme", 1); main.sync(); advanced.sync();
    const QJsonArray library{QJsonObject{{"id", "live-switch"}, {"name", "开关实际入口测试"}, {"commands", QJsonArray{}}}};
    QString fileError;
    const auto commandPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("cmd_panels.json");
    VERIFY(QDir().mkpath(QFileInfo(commandPath).absolutePath()));
    VERIFY(ProtocolProfileStore::writeFile(commandPath, QJsonDocument(library).toJson(), fileError));
    // Also exercise startup when the saved application theme already matches
    // the current theme: no selector-change signal may be relied upon.
    ThemeManager::applyTheme(qApp, ThemeManager::LightTheme);
    auto port = new ReceivePort; auto worker = new TestSerialHandler(port);
    MainWindow window(nullptr, worker); window.show();
    auto open = window.findChild<QPushButton *>("btnOpenClose");
    auto role = window.findChild<QComboBox *>("comboInterface");
    VERIFY(until([&] { return open->isEnabled(); })); window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    auto commands = window.findChild<CommandPanelWindow *>(); VERIFY(commands);
    commands->show();
    auto actionsMatch = [&](bool dark) {
        QCoreApplication::processEvents();
        for (const auto name : {"commandPanelSettings", "commandAddCommand"}) {
            auto button = commands->findChild<QPushButton *>(name); if (!button) return false;
            const QColor background(dark ? "#333333" : "#ffffff"), ink(dark ? "#dddddd" : "#333333");
            if (button->palette().color(QPalette::Button) != background || button->palette().color(QPalette::ButtonText) != ink) return false;
            const auto image = button->grab().toImage(); const auto scale = image.devicePixelRatio();
            if (image.pixelColor(qRound(8 * scale), qRound(8 * scale)) != background) return false;
        }
        return true;
    };
    VERIFY(actionsMatch(false));
    const auto themeArtifacts = QDir::current().filePath("artifacts"); VERIFY(QDir().mkpath(themeArtifacts));
    commands->grab().save(themeArtifacts + "/command-actions-first-light.png");
    for (auto theme : {ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
        ThemeManager::applyTheme(qApp, theme); VERIFY(actionsMatch(theme == ThemeManager::DarkTheme));
    }
    commands->close(); ThemeManager::applyTheme(qApp, ThemeManager::DarkTheme);
    ThemeManager::applyTheme(qApp, ThemeManager::LightTheme); commands->show(); VERIFY(actionsMatch(false));
    commands->reloadConfig(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); VERIFY(actionsMatch(false));
    PanelContentWidget *panel = nullptr;
    for (auto candidate : commands->findChildren<PanelContentWidget *>()) {
        auto stack = qobject_cast<QStackedWidget *>(candidate->parentWidget());
        if (stack && stack->currentWidget() == candidate) panel = candidate;
    }
    VERIFY(panel);
    CommandDef def; def.name = "开关"; def.widgetType = 2; def.appendIndex = 1;
    def.switchStates = {{{"OFF", "关闭", ""}, {"ON", "开启", "#227755"}}}; panel->addCommand(def);
    auto toggle = panel->findChild<SwitchCommandWidget *>(); VERIFY(toggle && toggle->currentState() == 0);
    toggle->click(); if (toggle->currentState() != 0 || !messages.lastText.contains("先打开连接")) std::cerr << "Switch initial state=" << toggle->currentState() << " enabled=" << toggle->isEnabled() << " message=" << messages.lastText.toStdString() << "\n"; VERIFY(toggle->currentState() == 0 && messages.lastText.contains("先打开连接"));
    auto written = [&] { QByteArray result; QMetaObject::invokeMethod(worker, [&] { result = port->written; }, Qt::BlockingQueuedConnection); return result; };
    auto ports = window.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
    open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
    VERIFY(toggle->currentState() == 0 && written().isEmpty());
    toggle->click(); VERIFY(toggle->currentState() == 1 && until([&] { return written() == "ON\r\n"; }));
    toggle->click(); VERIFY(toggle->currentState() == 0 && until([&] { return written() == "ON\r\nOFF\r\n"; }));
    auto breakControl = window.findChild<QCheckBox *>("chkSerialBreak"); VERIFY(breakControl);
    breakControl->setChecked(true); VERIFY(until([&] { return worker->breakLineState.load() && breakControl->isChecked() && breakControl->isEnabled(); }));
    toggle->click(); VERIFY(toggle->currentState() == 0 && written() == "ON\r\nOFF\r\n" && window.statusBar()->currentMessage().contains("Break"));
    breakControl->setChecked(false); VERIFY(until([&] { return !worker->breakLineState.load() && !breakControl->isChecked() && breakControl->isEnabled(); }));
    // Reject an oversized submission rather than falsely changing state.
    auto oversized = def; oversized.switchStates[1].payload = QString(300 * 1024, 'X'); toggle->updateDef(oversized);
    toggle->click(); VERIFY(toggle->currentState() == 0 && written() == "ON\r\nOFF\r\n"); toggle->updateDef(def);
    toggle->click(); VERIFY(toggle->currentState() == 1 && until([&] { return written() == "ON\r\nOFF\r\nON\r\n"; }));
    open->click(); VERIFY(until([&] { return role->isEnabled() && open->isEnabled(); }));
    toggle->click(); VERIFY(toggle->currentState() == 1);
    // TCP uses the same admission result, not just an optimistic UI signal.
    QTcpServer server; VERIFY(server.listen(QHostAddress::LocalHost, 0));
    role->setCurrentIndex(role->findData(1)); VERIFY(until([&] { return open->isEnabled(); }));
    window.findChild<QLineEdit *>("editIP")->setText("127.0.0.1"); window.findChild<QSpinBox *>("spinPort")->setValue(server.serverPort());
    open->click(); VERIFY(until([&] { return server.hasPendingConnections() && open->text().contains("断开网络"); }));
    auto peer = server.nextPendingConnection(); QByteArray bytes;
    VERIFY(toggle->currentState() == 1); toggle->click();
    VERIFY(toggle->currentState() == 0 && until([&] { bytes += peer->readAll(); return bytes == "OFF\r\n"; }));
    open->click(); VERIFY(until([&] { return role->isEnabled() && open->isEnabled(); })); peer->deleteLater();
    // UDP submits one complete command per datagram.
    QUdpSocket udp, reserve; udp.setProxy(QNetworkProxy::NoProxy); reserve.setProxy(QNetworkProxy::NoProxy);
    VERIFY(udp.bind(QHostAddress(QHostAddress::LocalHost), 0) && reserve.bind(QHostAddress(QHostAddress::LocalHost), 0));
    const auto localPort = reserve.localPort(); reserve.close();
    role->setCurrentIndex(role->findData(5)); VERIFY(until([&] { return open->isEnabled(); }));
    window.findChild<QComboBox *>("comboUdpLocalIP")->setCurrentText("127.0.0.1"); window.findChild<QSpinBox *>("spinUdpLocalPort")->setValue(localPort);
    window.findChild<QLineEdit *>("editUdpRemoteIP")->setText("127.0.0.1"); window.findChild<QSpinBox *>("spinUdpRemotePort")->setValue(udp.localPort());
    open->click(); VERIFY(until([&] { return open->text().contains("关闭 UDP"); }));
    VERIFY(toggle->currentState() == 0); toggle->click(); VERIFY(toggle->currentState() == 1 && until([&] { return udp.hasPendingDatagrams(); }));
    QByteArray packet(int(udp.pendingDatagramSize()), '\0'); VERIFY(udp.readDatagram(packet.data(), packet.size()) == 4 && packet == "ON\r\n");
    window.close();
    std::cout << "Switch commands: real main-window serial/TCP/UDP routing, disconnected/Break/full-queue rejection and no reconnect sends passed.\n";
    return 0;
}
static int recordingShapeUiTests(OffscreenMessages &messages) {
    QTemporaryDir directory; VERIFY(directory.isValid()); QString error;
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    advanced.setValue("InterfaceType", 0); main.setValue("editCacheDir", directory.path()); advanced.setValue("CacheDir", directory.path()); main.sync(); advanced.sync();
    ProtocolProfile profile; VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(false), profile, error));
    ProtocolProfileStore store; VERIFY(store.load(error)); auto doc = store.snapshot(); doc.profiles = {profile}; doc.shared.kind = "custom"; doc.shared.id = profile.id; VERIFY(store.commit(doc, error));
    auto port = new ReceivePort; auto worker = new TestSerialHandler(port); MainWindow window(nullptr, worker); window.resize(1400, 940); window.show();
    auto open = window.findChild<QPushButton *>("btnOpenClose"); VERIFY(until([&] { return open->isEnabled(); }));
    window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    window.findChild<QPushButton *>("btnPausePlot")->setChecked(false);
    for (auto check : window.findChildren<QCheckBox *>()) if (check->text().startsWith("陷波") || check->text().startsWith("高通") || check->text().startsWith("低通") || check->text() == "录前清空") check->setChecked(false);
    auto ports = window.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY"); open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
    auto record = window.findChild<QPushButton *>("btnToggleRecord"); auto mode = window.findChild<QComboBox *>("comboRecordingMode");
    auto warning = window.findChild<QLabel *>("lblRecordingWarning"); VERIFY(record && mode && warning && warning->isHidden());
    VERIFY(record->text() == "开始录制" && !record->icon().isNull());
    const auto recordIcon = record->icon().cacheKey();
    auto packet = [](int channels) { QStringList fields; for (int i = 0; i < channels; ++i) fields << QString::number(i + 1); return "DATA:" + fields.join(',').toUtf8() + "\r\n"; };
    for (int format : {0, 1}) {
        mode->setCurrentIndex(format); messages.lastTitle.clear(); record->click();
        VERIFY(record->text() == "停止录制" && warning->isHidden() && warning->text().isEmpty());
        VERIFY(record->icon().cacheKey() == recordIcon);
        VERIFY(feed(worker, port, packet(12)));
        VERIFY(until([&] { return worker->getLoggedBytes() > 0; }));
        VERIFY(feed(worker, port, packet(17)) && until([&] { return worker->skippedRecordingFrames() == 1; }));
        VERIFY(until([&] { return warning->text() == "录制中 · 已跳过 1 帧异常数据"; }));
        VERIFY(warning->isVisible() && warning->text() == "录制中 · 已跳过 1 帧异常数据" && record->text().contains("停止") && messages.lastTitle.isEmpty());
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 17; }));
        for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
            ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents(); QCoreApplication::processEvents();
            VERIFY(record->icon().cacheKey() == recordIcon);
            const auto color = warning->palette().color(QPalette::WindowText); VERIFY(color.red() > color.green() * 1.4 && color.red() > color.blue() * 1.4);
            window.statusBar()->showMessage("临时状态消息", 1000); VERIFY(warning->isVisible());
            if (!artifactDirectory.isEmpty()) window.grab().save(artifactDirectory + QString("/recording-skipped-%1-%2.png").arg(format).arg(theme));
        }
        VERIFY(feed(worker, port, packet(12) + packet(11) + packet(12)) && until([&] { return worker->skippedRecordingFrames() == 2; }));
        VERIFY(until([&] { return warning->text() == "录制中 · 已跳过 2 帧异常数据"; }) && record->text().contains("停止") && messages.lastTitle.isEmpty());
        record->click(); VERIFY(until([&] { return record->text().contains("开始"); }) && warning->isHidden() && warning->text().isEmpty());
        VERIFY(record->text() == "开始录制" && record->icon().cacheKey() == recordIcon);
        // Start again: use 17 as the NEW fixed width, and count from zero.
        record->click(); VERIFY(record->text().contains("停止") && warning->isHidden() && worker->skippedRecordingFrames() == 0);
        VERIFY(record->icon().cacheKey() == recordIcon);
        VERIFY(feed(worker, port, packet(17) + packet(12) + packet(17)) && until([&] { return worker->skippedRecordingFrames() == 1; }));
        VERIFY(until([&] { return warning->text() == "录制中 · 已跳过 1 帧异常数据"; }));
        record->click(); VERIFY(until([&] { return record->text().contains("开始"); }) && warning->isHidden());
    }
    const QDir cache(directory.filePath("WaveLink_Cache")); const auto csvFiles = cache.entryList({"*.csv"}, QDir::Files, QDir::Name); const auto wlrFiles = cache.entryList({"*.wlr"}, QDir::Files, QDir::Name);
    VERIFY(csvFiles.size() == 2 && wlrFiles.size() == 2);
    for (int file = 0; file < 2; ++file) {
        QFile csv(cache.filePath(csvFiles[file])); VERIFY(csv.open(QIODevice::ReadOnly)); const auto rows = csv.readAll().trimmed().split('\n');
        VERIFY(rows.size() == (file ? 3 : 4)); for (int row = 1; row < rows.size(); ++row) VERIFY(rows[row].split(',').size() == (file ? 19 : 14));
        int saved = 0; VERIFY(WlrRecordingWriter::visitFiles({cache.filePath(wlrFiles[file])}, [&](qint64, const QVector<double> &values, qint64) { ++saved; return values.size() == (file ? 17 : 12); }, error) && saved == (file ? 2 : 3));
    }
    open->click(); window.close(); std::cout << "Recording shape UI: first-frame locking, continued display/recording, red permanent warning, cumulative/reset counters and CSV/WLR contents passed.\n"; return 0;
}
static int timeRangeSpanTests() {
    QTemporaryDir directory; VERIFY(directory.isValid());
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    advanced.setValue("InterfaceType", 0); advanced.sync(); main.setValue("editCacheDir", directory.path()); main.sync();
    ProtocolProfileStore store; QString error; VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(1), error));
    auto port = new ReceivePort; auto worker = new TestSerialHandler(port);
    MainWindow window(nullptr, worker); window.resize(1200, 900); window.show();
    auto slider = window.findChild<RangeSlider *>("timeRangeSlider"); auto open = window.findChild<QPushButton *>("btnOpenClose");
    VERIFY(slider && open && until([&] { return open->isEnabled(); }));
    window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    QCustomPlot *plot = nullptr;
    for (auto candidate : window.findChildren<QCustomPlot *>()) if (candidate->yAxis->label() == "数据值") plot = candidate;
    VERIFY(plot); QElapsedTimer settle; settle.start(); VERIFY(until([&] { return settle.elapsed() > 200; }));
    auto matches = [&] {
        const auto view = plot->xAxis->range();
        return std::abs(slider->lowerValue() - view.lower) < 1e-9 && std::abs(slider->upperValue() - view.upper) < 1e-9
            && std::abs(slider->upperValue() - slider->lowerValue() - view.size()) < 1e-9;
    };
    VERIFY(std::abs(plot->xAxis->range().size() - 10) < 1e-9 && matches());
    // Empty-view rescale/resize/theme updates must not reintroduce 0..1.
    for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
        ThemeManager::applyTheme(qApp, theme); window.resize(1000, 820); QCoreApplication::processEvents();
        plot->xAxis->setRange(0, 25); VERIFY(matches()); plot->xAxis->setRange(0, 10);
        settle.restart(); VERIFY(until([&] { return settle.elapsed() > 130; }) && matches());
        VERIFY(std::abs(slider->upperValue() - slider->lowerValue() - 10) < 1e-9);
    }
    window.findChild<QPushButton *>("btnPausePlot")->setChecked(false);
    for (auto check : window.findChildren<QCheckBox *>()) if (check->text().startsWith("陷波") || check->text().startsWith("高通") || check->text().startsWith("低通")) check->setChecked(false);
    auto ports = window.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
    open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
    VERIFY(feed(worker, port, "1,2\n2,3\n3,4\n4,5\n5,6\n"));
    VERIFY(until([&] { return plot->graphCount() == 2 && plot->graph(0)->dataCount() >= 5; }));
    VERIFY(matches() && std::abs(plot->xAxis->range().size() - 10) < 1e-9 && slider->minimum() < 0);
    QPushButton *left = nullptr;
    for (auto button : window.findChildren<QPushButton *>()) if (button->text() == "<") left = button;
    VERIFY(left); left->click(); VERIFY(matches() && std::abs(slider->upperValue() - slider->lowerValue() - 10) < 1e-9);
    // A history span shorter than the view must preserve 10 s rather than
    // clamp the selection to the number of seconds received so far.
    const double latest = slider->maximum(); VERIFY(latest > 0 && latest < 1);
    slider->setValues(0, latest / 2); VERIFY(matches());
    window.findChild<QPushButton *>("btnTimeScrollToLatest")->click();
    VERIFY(matches() && std::abs(slider->upperValue() - latest) < 1e-9);
    plot->xAxis->setRange(0, 60); VERIFY(matches());
    VERIFY(feed(worker, port, "6,7\n7,8\n8,9\n"));
    VERIFY(until([&] { return plot->graph(0)->dataCount() >= 8; }));
    VERIFY(matches() && std::abs(slider->upperValue() - slider->lowerValue() - 60) < 1e-9);
    open->click(); VERIFY(until([&] { return open->text().contains("打开"); }));
    VERIFY(QMetaObject::invokeMethod(&window, "onClearPlots") && matches() && std::abs(slider->upperValue() - slider->lowerValue() - 10) < 1e-9);
    if (!artifactDirectory.isEmpty()) window.grab().save(artifactDirectory + "/time-slider-default-span.png");
    window.close(); std::cout << "Time span: startup 10 s, empty rescale, early acquisition, custom span, latest jump and clear synchronization passed.\n"; return 0;
}
static int rangeSliderThemeTests() {
    QMainWindow host; auto slider = new RangeSlider; host.setCentralWidget(slider);
    slider->setRange(0, 100); slider->setValues(20, 60); slider->setRelativeOffset(7.5);
    int changes = 0; QObject::connect(slider, &RangeSlider::rangeChanged, [&](double, double) { ++changes; });
    host.show();
    for (int width : {360, 900}) {
        host.resize(width, 80);
        for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
            slider->setValues(20, 60); const int before = changes;
            ThemeManager::applyTheme(qApp, theme); QCoreApplication::processEvents(); QCoreApplication::processEvents();
            VERIFY(changes == before && slider->minimum() == 0 && slider->maximum() == 100 && slider->lowerValue() == 20 && slider->upperValue() == 60);
            // Render in the parent: this transparent custom widget inherits
            // its background from QMainWindow, not Fusion's default Window role.
            const bool dark = theme == ThemeManager::DarkTheme; const auto pixmap = host.grab(); const auto image = pixmap.toImage();
            auto pixel = [&](int x, int y) { return image.pixelColor(qRound(x * pixmap.devicePixelRatioF()), qRound(y * pixmap.devicePixelRatioF())); };
            const int y = slider->height() / 2 + 7;
            VERIFY(dark ? pixel(2, 2).lightness() < 90 : pixel(2, 2).lightness() > 180);
            // Away from text, antialiased edges, handles and endpoint labels.
            VERIFY(pixel(slider->width() * .75, y) == QColor(dark ? "#3e3e42" : "#e1e5ea"));
            VERIFY(pixel(slider->width() * .3, y) == QColor(dark ? "#5cba62" : "#0078d7"));
            VERIFY(dark ? slider->palette().color(QPalette::Active, QPalette::WindowText).lightness() > 128
                : slider->palette().color(QPalette::Active, QPalette::WindowText).lightness() < 128);
            // Numeric labels must have foreground pixels against the themed
            // parent, rather than the old white-on-white value labels.
            const int labelX = 6 + int(.2 * (slider->width() - 12)); int ink = 0;
            const int top = (slider->height() - 24) / 2;
            for (int x = qMax(0, labelX - 20); x < qMin(slider->width(), labelX + 20); ++x) for (int yy = 0; yy < top; ++yy) {
                const auto color = pixel(x, yy);
                if (dark ? color.lightness() > 150 : color.lightness() < 100) ++ink;
            }
            VERIFY(ink > 8);
            if (!artifactDirectory.isEmpty()) pixmap.save(artifactDirectory + QString("/time-slider-%1-%2.png").arg(dark ? "dark" : "light").arg(width));
            // Test the real mouse route: individual handle and whole-range
            // drag must retain the same range calculations after recoloring.
            auto position = [&](double value) { return QPoint(6 + int(value / 100 * (slider->width() - 12)), slider->height() / 2); };
            auto drag = [&](double from, double to) {
                QMouseEvent press(QEvent::MouseButtonPress, position(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(slider, &press);
                QMouseEvent move(QEvent::MouseMove, position(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(slider, &move);
                QMouseEvent release(QEvent::MouseButtonRelease, position(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier); QCoreApplication::sendEvent(slider, &release);
            };
            drag(20, 30); VERIFY(std::abs(slider->lowerValue() - 30) < .5 && slider->upperValue() == 60);
            slider->setValues(20, 60); drag(40, 50);
            VERIFY(std::abs(slider->lowerValue() - 30) < .5 && std::abs(slider->upperValue() - 70) < .5);
            VERIFY(std::abs(slider->upperValue() - slider->lowerValue() - 40) < 1e-9);
        }
    }
    host.close(); ThemeManager::applyTheme(qApp, ThemeManager::DarkTheme);
    std::cout << "Time slider: light/dark live switching, track/selection/label contrast, resize and handle/range dragging passed.\n"; return 0;
}
static bool statusPanelFileAction(StatusMonitorWindow *window,const QString &path,bool save){
    QTimer driver;bool picked=false;QElapsedTimer deadline;deadline.start();
    QObject::connect(&driver,&QTimer::timeout,&driver,[&]{for(auto widget:QApplication::topLevelWidgets())if(auto dialog=qobject_cast<QFileDialog*>(widget))if(dialog->isVisible()){if(!picked){picked=true;dialog->selectFile(path);QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection);}else if(deadline.elapsed()>7000)dialog->reject();}});
    driver.start(10);window->findChild<QPushButton*>(save?"statusExportAll":"statusImportPanels")->click();driver.stop();return picked&&deadline.elapsed()<7000;
}
static int statusMonitorLiveUiTests(OffscreenMessages &messages) {
    {QSettings main("WaveLink","WaveLink");main.setValue("comboTheme",1);main.sync();}
    QSettings advanced("WaveLink","AdvancedApp");advanced.setValue("InterfaceType",3);advanced.sync();
    QString catalogError;ProtocolProfile catalogProfile;VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(false),catalogProfile,catalogError));ProtocolProfileStore library;VERIFY(library.load(catalogError));auto catalogSnapshot=library.snapshot();catalogSnapshot.profiles.append(catalogProfile);VERIFY(library.commit(catalogSnapshot,catalogError));
    MainWindow host(nullptr,new TestSerialHandler(new ReceivePort));host.show();
    auto button=host.findChild<QPushButton*>("openStatusMonitor");auto window=host.findChild<StatusMonitorWindow*>("statusMonitorWindow");auto open=host.findChild<QPushButton*>("btnOpenClose");
    VERIFY(button&&window&&open);VERIFY(until([&]{return open->isEnabled();}));button->click();VERIFY(window->isVisible());
    bool group=false;for(auto box:host.findChildren<QGroupBox*>())if(box->title()=="设备交互")group=true;VERIFY(group);
    StatusPanels::Panel panel;panel.id=StatusPanels::newId();panel.name="Demo 状态联测";StatusMonitoring::Rule rule;rule.id=StatusPanels::newId();rule.name="实时数值";rule.kind=StatusMonitoring::RuleKind::Threshold;StatusMonitoring::Range range;range.state="收到数值";rule.ranges={range};panel.items={rule};QString error;VERIFY(window->replacePanels({panel},error));
    auto themed=[&](bool dark){return until([&]{
        auto cards=window->findChildren<QFrame*>("statusCard");auto list=window->findChild<QListWidget*>("statusPanelList");if(cards.isEmpty()||!list)return false;
        const QColor background(dark?"#27323c":"#ffffff"),ink(dark?"#e1e8ef":"#1b3548");
        for(auto card:cards){if(card->palette().color(QPalette::Window)!=background)return false;for(auto name:{"statusCardTitle","statusCardMeta"}){auto label=card->findChild<QLabel*>(name);if(!label||label->palette().color(QPalette::WindowText)!=ink)return false;}const auto image=card->grab().toImage();const auto scale=image.devicePixelRatio();if(image.pixelColor(qRound((card->width()-9)*scale),qRound((card->height()-9)*scale))!=background)return false;}
        return list->palette().color(QPalette::Base).lightness()>(dark?0:200)&&list->palette().color(QPalette::Base).lightness()<(dark?100:256);
    });};
    const bool lightReady=themed(false);if(!lightReady){auto card=window->findChild<QFrame*>("statusCard");auto list=window->findChild<QListWidget*>("statusPanelList");std::cerr<<"Status theme: index="<<host.findChild<QComboBox*>("comboTheme")->currentIndex()<<" app="<<qApp->property("waveLinkDarkTheme").toBool()<<" card="<<card->palette().color(QPalette::Window).name().toStdString()<<" list="<<list->palette().color(QPalette::Base).name().toStdString()<<" title="<<card->findChild<QLabel*>("statusCardTitle")->palette().color(QPalette::WindowText).name().toStdString()<<" style="<<window->styleSheet().toStdString()<<"\n";}VERIFY(host.findChild<QComboBox*>("comboTheme")->currentIndex()==1&&lightReady);
    auto three=panel;auto eventRule=rule;eventRule.id=StatusPanels::newId();eventRule.name="事件提示";eventRule.event.enabled=true;auto disabledRule=rule;disabledRule.id=StatusPanels::newId();disabledRule.name="停用监视";disabledRule.enabled=false;three.items={eventRule,rule,disabledRule};VERIFY(window->replacePanels({three},error)&&themed(false));VERIFY(window->findChildren<QFrame*>("statusCard").size()==3);
    auto themeChoice=host.findChild<QComboBox*>("comboTheme");
    for(bool dark:{true,false}){themeChoice->setCurrentIndex(dark?0:1);VERIFY(themed(dark));three.note=dark?"深色重建":"浅白重建";VERIFY(window->replacePanels({three},error)&&themed(dark));}
    window->close();ThemeManager::applyTheme(qApp,ThemeManager::DarkTheme);VERIFY(!window->isVisible());button->click();VERIFY(themed(true));
    ThemeManager::applyTheme(qApp,ThemeManager::LightTheme);VERIFY(themed(false));
    const auto themeArtifacts=QDir::current().filePath("artifacts");QDir().mkpath(themeArtifacts);window->grab().save(themeArtifacts+"/status-live-light.png");ThemeManager::applyTheme(qApp,ThemeManager::DarkTheme);VERIFY(themed(true));window->grab().save(themeArtifacts+"/status-live-dark.png");ThemeManager::applyTheme(qApp,ThemeManager::LightTheme);VERIFY(themed(false));
    VERIFY(window->replacePanels({panel},error)&&themed(false));
    {
        const int protocolBefore=host.findChild<QComboBox*>("comboProtocol")->currentIndex();bool catalogShown=false,customChannels=false,metadataCorrect=false;
        QTimer::singleShot(30,window,[&]{auto settings=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!settings)return;auto choices=settings->findChild<QComboBox*>("statusProtocolChoice");if(!choices){settings->reject();return;}const int custom=choices->findText("    "+catalogProfile.name);catalogShown=choices->findText("系统协议")>=0&&choices->findText("自定义协议")>=0&&custom>=0;if(custom>=0){choices->setCurrentIndex(custom);settings->findChild<QComboBox*>("statusProtocolBinding")->setCurrentIndex(0);settings->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();}else settings->reject();});
        window->findChild<QPushButton*>("statusPanelSettings")->click();ProtocolSelection chosen;chosen.kind="custom";chosen.id=catalogProfile.id;metadataCorrect=window->panels()[0].protocolId==catalogProfile.id&&window->panels()[0].protocolShape==StatusMonitoring::Session::structure(chosen,catalogProfile);
        QTimer::singleShot(30,window,[&]{auto editor=dynamic_cast<StatusItemDialog*>(QApplication::activeModalWidget());if(!editor)return;const auto names=catalogProfile.outputChannelNames(256);customChannels=!names.isEmpty()&&editor->findChild<QComboBox*>("statusChannel")->itemText(0).contains(names[0])&&!editor->findChild<QComboBox*>("statusProtocolChoice");editor->reject();});
        window->findChild<QPushButton*>("statusAddItem")->click();VERIFY(catalogShown&&customChannels&&metadataCorrect);VERIFY(host.findChild<QComboBox*>("comboProtocol")->currentIndex()==protocolBefore&&window->panels()[0].items.size()==1);VERIFY(window->replacePanels({panel},error));
    }
    {
        StatusItemDialog editor(rule,{rule},{},window);editor.show();editor.activateWindow();editor.raise();auto table=editor.findChild<QTableWidget*>("statusRuleTable");VERIFY(table);QApplication::processEvents();table->editItem(table->item(0,4));QApplication::processEvents();QLineEdit *cell=nullptr;for(auto edit:table->findChildren<QLineEdit*>())if(edit->isVisible()){cell=edit;break;}VERIFY(cell);cell->setText("主程序内快捷键保存");cell->setFocus();VERIFY(until([&]{return cell->hasFocus();}));
        QKeyEvent press(QEvent::KeyPress,Qt::Key_S,Qt::ControlModifier,"s");QApplication::sendEvent(cell,&press);QKeyEvent release(QEvent::KeyRelease,Qt::Key_S,Qt::ControlModifier,"s");QApplication::sendEvent(&editor,&release);VERIFY(editor.isVisible()&&editor.resultRule().ranges[0].state=="主程序内快捷键保存");editor.findChild<QPushButton*>("statusSaveItem")->click();VERIFY(editor.result()==QDialog::Accepted);
    }
    open->click();VERIFY(until([&]{for(auto label:window->findChildren<QLabel*>())if(label->text().contains("收到数值"))return true;return false;}));
    window->close();VERIFY(!window->isVisible());VERIFY(QMetaObject::invokeMethod(&host,"showStatusMonitor"));VERIFY(window->isVisible());
    auto action=host.findChild<QAction*>("actionStatusMonitor");VERIFY(action);action->trigger();VERIFY(window->isVisible());
    open->click();VERIFY(until([&]{return host.findChild<QComboBox*>("comboInterface")->isEnabled();}));
    auto interfaceBox=host.findChild<QComboBox*>("comboInterface");interfaceBox->setCurrentIndex(interfaceBox->findData(0));VERIFY(until([&]{return host.findChild<QComboBox*>("comboProtocol")->isEnabled();}));
    QTemporaryDir temp;VERIFY(temp.isValid());
    const auto standalone=temp.filePath("status-panels.json");VERIFY(statusPanelFileAction(window,standalone,true));QFile panelFile(standalone);VERIFY(panelFile.open(QIODevice::ReadOnly));QVector<StatusPanels::Panel> exportedPanels;VERIFY(StatusPanels::decode(panelFile.readAll(),exportedPanels,error)&&exportedPanels.size()==1);panelFile.close();const auto originalPanels=StatusPanels::encode(window->panels(),error);
    messages.standaloneStatusButton="statusStandaloneCancel";VERIFY(statusPanelFileAction(window,standalone,false)&&StatusPanels::encode(window->panels(),error)==originalPanels);
    messages.standaloneStatusButton="statusStandaloneAppend";VERIFY(statusPanelFileAction(window,standalone,false));VERIFY(window->panels().size()==2&&window->panels()[1].name=="Demo 状态联测_1"&&window->panels()[1].items[0].id!=window->panels()[0].items[0].id);
    VERIFY(statusPanelFileAction(window,standalone,false)&&window->panels().size()==3&&window->panels()[2].name=="Demo 状态联测_2");
    messages.standaloneStatusButton="statusStandaloneReplace";VERIFY(statusPanelFileAction(window,standalone,false)&&StatusPanels::encode(window->panels(),error)==originalPanels);
    const auto broken=temp.filePath("broken-panels.json");VERIFY(ProtocolProfileStore::writeFile(broken,"{broken",error));VERIFY(statusPanelFileAction(window,broken,false)&&StatusPanels::encode(window->panels(),error)==originalPanels);VERIFY(messages.lastText.contains("JSON"));
    const auto config=temp.filePath("status.ini");messages.lastTitle.clear();VERIFY(configFileAction(host,config,"onExportConfig")&&messages.lastTitle=="导出成功");
    panel.name="仅本机的新名称";VERIFY(window->replacePanels({panel},error));window->applyTheme(true);messages.statusButton="configStatusOverwrite";VERIFY(configFileAction(host,config,"onImportConfig")&&messages.lastTitle=="导入成功");VERIFY(window->panels().size()==1&&window->panels()[0].name=="Demo 状态联测"&&themed(false));VERIFY(until([&]{return host.findChild<QComboBox*>("comboProtocol")->isEnabled();}));
    messages.statusButton="configStatusAppend";VERIFY(configFileAction(host,config,"onImportConfig")&&messages.lastTitle=="导入成功");VERIFY(window->panels().size()==2&&window->panels()[1].name=="Demo 状态联测_1");VERIFY(until([&]{return host.findChild<QComboBox*>("comboProtocol")->isEnabled();}));
    // Reloaded in-memory Store must still allow subsequent ordinary edits.
    auto edited=window->panels();edited[0].note="全局导入后仍能保存";VERIFY(window->replacePanels(edited,error));host.close();
    std::cout<<"Status entry, tools action, real Demo pipeline, local configuration and hidden-window monitoring passed.\n";return 0;
}
static int highFrequencyDemoUiTests() {
    QTemporaryDir folder; VERIFY(folder.isValid());
    QSettings mainSettings("WaveLink","WaveLink"), advanced("WaveLink","AdvancedApp");
    mainSettings.setValue("editCacheDir",folder.path()); advanced.setValue("InterfaceType",3);
    MainWindow window(nullptr,new TestSerialHandler(new ReceivePort)); window.resize(1500,950); window.show();
    auto open=window.findChild<QPushButton *>("btnOpenClose");
    VERIFY(until([&]{return open->isEnabled();}));
    window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    auto channels=window.findChild<QComboBox *>("comboDemoChannels");
    auto interval=window.findChild<QSpinBox *>("spinDemoInterval");
    auto frequency=window.findChild<QDoubleSpinBox *>("spinDemoFreq");
    auto timer=window.findChild<QTimer *>("demoTimer");
    VERIFY(channels && interval && frequency && timer);
    channels->setCurrentIndex(1); interval->setValue(4); frequency->setValue(100);
    QCheckBox *automatic=nullptr;
    for(auto check:window.findChildren<QCheckBox *>()) {
        if(check->text()=="Y轴自适应") { automatic=check; check->setChecked(true); }
        if(check->text()=="显示接收数据") check->setChecked(false);
        if(check->text().startsWith("高通") || check->text().startsWith("低通") || check->text().startsWith("陷波")) check->setChecked(false);
    }
    window.findChild<QPushButton *>("btnPausePlot")->setChecked(false);
    open->click(); VERIFY(until([&]{return timer->isActive();}));
    // Replay 100 seconds through the real Demo generator/parser, without
    // waiting 100 wall-clock seconds or touching any hardware interface.
    for(int batch=0;batch<1000;++batch) {
        timer->setProperty("lastTickMs",QDateTime::currentMSecsSinceEpoch()-100);
        VERIFY(QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection));
        VERIFY(QMetaObject::invokeMethod(&window,"onPlotTimer"));
        if(batch%50==0) QCoreApplication::processEvents();
    }
    open->click(); VERIFY(!timer->isActive());
    QCustomPlot *plot=nullptr;
    for(auto candidate:window.findChildren<QCustomPlot *>()) if(candidate->yAxis->label()=="数据值" && candidate->graphCount()==2) plot=candidate;
    VERIFY(plot && automatic && automatic->isChecked());
    const int count=plot->graph(0)->dataCount(); VERIFY(count>=25000);
    for(int ch=0;ch<2;++ch) {
        auto graph=dynamic_cast<WaveformGraph *>(plot->graph(ch));
        VERIFY(graph && graph->pen().isCosmetic() && graph->pen().widthF()==1.0);
        VERIFY(graph->dataCount()==count);
    }
    const QColor originalColor=plot->graph(0)->pen().color();
    VERIFY(editSystemSettings(window,1000,50000,100,30,-1,-1,2));
    VERIFY(plot->graph(0)->pen().widthF()==2 && plot->graph(0)->pen().isCosmetic());
    QListWidget *channelList=nullptr;
    for(auto list:window.findChildren<QListWidget *>()) if(list->count()==2 && list->itemWidget(list->item(0))) channelList=list;
    VERIFY(channelList); bool detached=false;
    QTimer::singleShot(0,[&] {
        for(auto widget:QApplication::topLevelWidgets()) if(auto menu=qobject_cast<QMenu *>(widget)) if(menu->isVisible()) {
            for(auto action:menu->actions()) if(action->text()=="分离到新视图") {action->trigger(); detached=true; break;}
            menu->close();
        }
    });
    VERIFY(QMetaObject::invokeMethod(&window,"onChannelContextMenu",Q_ARG(QPoint,channelList->visualItemRect(channelList->item(0)).center())) && detached);
    for(int width:{3,1}) {
        VERIFY(editSystemSettings(window,1000,50000,100,30,-1,-1,width));
        int checked=0;
        for(auto view:window.findChildren<QCustomPlot *>()) for(int ch=0;ch<view->graphCount();++ch) if(auto graph=dynamic_cast<WaveformGraph *>(view->graph(ch))) {
            VERIFY(graph->pen().widthF()==width && graph->pen().isCosmetic() && graph->dataCount()==count); ++checked;
        }
        VERIFY(checked==3 && plot->graph(0)->pen().color()==originalColor);
    }
    window.findChild<QPushButton *>("btnMergeAllViews")->click();
    for(int seconds:{5,60,100}) {
        plot->xAxis->setRange(0,seconds);
        VERIFY(until([&]{return plot->yAxis->range().upper>40 && plot->yAxis->range().lower<-40;}));
        for(int frame=0;frame<5;++frame) {
            plot->xAxis->setRange(frame*.004,seconds+frame*.004);
            plot->replot(); // Measure actual paints, not only queued requests.
            QCoreApplication::processEvents();
        }
        VERIFY(std::abs(plot->xAxis->range().size()-seconds)<1e-6);
        VERIFY(plot->graph(0)->dataCount()==count && automatic->isChecked());
    }
    if(!artifactDirectory.isEmpty()) window.grab().save(artifactDirectory+"/demo-100hz.png");
    window.close();
    std::cout << "Real Demo 2ch/4ms/100Hz, 100 seconds replay, raw history, cosmetic lines, autoscale and range changes passed; OpenGL=" << plot->openGl() << ".\n";
    return 0;
}
static int auditReliabilityUiTests(OffscreenMessages &messages) {
    // Exercise the actual import dialog and live filtering after clearing.
    QTemporaryDir dir; VERIFY(dir.isValid());
    QSettings settings("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
    settings.setValue("editCacheDir", dir.path()); advanced.setValue("InterfaceType", 0);
    QString error; ProtocolProfileStore store;
    VERIFY(store.load(error) && store.setSelection(ProtocolSelection::fromLegacy(0), error));
    MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.show();
    VERIFY(until([&] { return window.findChild<QPushButton *>("btnOpenClose")->isEnabled(); }));
    window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
    QCheckBox *hpf = nullptr;
    for (auto check : window.findChildren<QCheckBox *>()) {
        if (check->text().startsWith("高通")) hpf = check;
        if (check->text().startsWith("陷波") || check->text().startsWith("低通")) check->setChecked(false);
    }
    window.findChild<QPushButton *>("btnPausePlot")->setChecked(false);
    VERIFY(hpf); hpf->setChecked(false); hpf->setChecked(true);
    auto feed = [&] {
        for (int i = 0; i < 2000; ++i) QMetaObject::invokeMethod(&window, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({42,42})));
        QMetaObject::invokeMethod(&window, "onPlotTimer");
        QMetaObject::invokeMethod(&window, "onPlotTimer"); // Drain both budgeted UI batches before comparing history.
    };
    feed();
    QCustomPlot *plot = nullptr;
    for (auto candidate : window.findChildren<QCustomPlot *>()) if (candidate->yAxis->label() == "数据值") plot = candidate;
    VERIFY(plot && plot->graphCount() == 2);
    auto lastValue = [&] { auto it = plot->graph(0)->data()->constEnd(); --it; return it->value; };
    VERIFY(std::abs(lastValue()) < 0.001);
    VERIFY(QMetaObject::invokeMethod(&window, "onClearPlots")); feed();
    VERIFY(hpf->isChecked() && std::abs(lastValue()) < 0.001);
    const auto size = plot->graph(0)->data()->size();
    const auto path = dir.filePath("missing-column.csv");
    QFile file(path); VERIFY(file.open(QIODevice::WriteOnly)); file.write("Time,Ch1,Ch2\n0.5,42\n0.6,43,53\n"); file.close();
    bool picked = false; QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, [&] {
        for (auto widget : QApplication::topLevelWidgets()) if (auto dialog = qobject_cast<QFileDialog *>(widget)) {
            if (dialog->isVisible() && !picked) { picked = true; dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); }
        }
    }); driver.start(10); messages.lastText.clear();
    QPushButton *import = nullptr;
    for (auto button : window.findChildren<QPushButton *>()) if (button->text().contains("导入数据")) import = button;
    VERIFY(import); import->click(); driver.stop();
    if (!picked || !messages.lastText.contains("不完整") || plot->graph(0)->data()->size() != size)
        std::cerr << "Audit CSV picked=" << picked << " message=" << messages.lastText.toStdString() << " size=" << plot->graph(0)->data()->size() << " before=" << size << "\n";
    VERIFY(picked && messages.lastText.contains("不完整") && plot->graph(0)->data()->size() == size);
    window.close(); return 0;
}
static int layoutQuickButtonUiTests() {
    auto contrast = [](const QColor &a, const QColor &b) {
        auto luminance = [](const QColor &c) {
            auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
            return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
        };
        const auto x = luminance(a), y = luminance(b); return (qMax(x,y)+0.05)/(qMin(x,y)+0.05);
    };
    for (int initialTheme : {1, 0}) {
        QSettings settings("WaveLink", "WaveLink"); settings.setValue("comboTheme", initialTheme); settings.sync();
        MainWindow window(nullptr, new TestSerialHandler(new ReceivePort)); window.resize(1600, 950); window.show();
        auto mode = window.findChild<QToolButton *>("btnDataTransceiveMode");
        auto reset = window.findChild<QToolButton *>("btnRestoreDefaultLayout");
        auto toolbar = window.findChild<QToolBar *>("mainToolBar");
        VERIFY(mode && reset && toolbar && !mode->icon().isNull() && !reset->icon().isNull());
        VERIFY(mode->isCheckable() && !reset->isCheckable() && mode->styleSheet().isEmpty() && reset->styleSheet().isEmpty());
        for (auto theme : {initialTheme == 1 ? ThemeManager::LightTheme : ThemeManager::DarkTheme,
                           ThemeManager::DarkTheme, ThemeManager::LightTheme}) {
            ThemeManager::applyTheme(qApp, theme); QApplication::processEvents();
            for (auto button : {mode, reset}) {
                button->ensurePolished();
                VERIFY(button->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !button->toolTip().isEmpty());
                VERIFY(button->palette().color(QPalette::ButtonText) == QColor(Qt::white));
                VERIFY(contrast(button->palette().color(QPalette::ButtonText), button->palette().color(QPalette::Button)) >= 4.5);
            }
            const QString tag = QString("/%1-start-%2").arg(theme == ThemeManager::DarkTheme ? "dark" : "light").arg(initialTheme);
            if (!artifactDirectory.isEmpty()) toolbar->grab().save(artifactDirectory + tag + "-idle.png");
            QEvent hover(QEvent::Enter); QApplication::sendEvent(mode, &hover); QApplication::processEvents();
            mode->setDown(true);
            if (!artifactDirectory.isEmpty()) toolbar->grab().save(artifactDirectory + tag + "-pressed.png");
            mode->setDown(false); QEvent leave(QEvent::Leave); QApplication::sendEvent(mode, &leave);
            if (mode->isChecked()) mode->click();
            mode->click(); QApplication::processEvents(); VERIFY(mode->isChecked());
            VERIFY(window.findChild<QDockWidget *>("DockConsole")->isVisible());
            if (!artifactDirectory.isEmpty()) toolbar->grab().save(artifactDirectory + tag + "-checked.png");
            mode->click(); QApplication::processEvents(); VERIFY(!mode->isChecked());
            mode->click(); reset->click(); QApplication::processEvents(); VERIFY(!mode->isChecked());
        }
        window.close();
    }
    std::cout << "Toolbar layout buttons: theme startup/live switch, icons, contrast and layout actions passed.\n";
    return 0;
}
#include "plotreadoutuitests.h"
#include "plotbackgrounduitests.h"
#include "scopemeasurementuitests.h"
#include "offlinehistoryuitests.h"
int main(int argc, char **argv) {
    std::cout.setf(std::ios::unitbuf);
    TestRegistrySandbox registry; VERIFY(registry.ready()); QApplication app(argc, argv);
    app.setAttribute(Qt::AA_DontUseNativeDialogs);
    OffscreenMessages offscreenMessages;
    if (app.platformName() == "offscreen") app.installEventFilter(&offscreenMessages);
    if (argc > 1) { artifactDirectory = QString::fromLocal8Bit(argv[1]); QDir().mkpath(artifactDirectory); }
    for (int order : {2, 4}) for (auto type : {BiquadFilter::LOWPASS, BiquadFilter::HIGHPASS, BiquadFilter::NOTCH}) {
        BiquadCascade filter; filter.configure(type, 250, 20, order);
        VERIFY(std::isfinite(filter.process(1)) && std::isnan(filter.process(std::numeric_limits<double>::quiet_NaN())));
        VERIFY(std::isfinite(filter.process(2)) && std::isfinite(filter.process(3)));
    }
    QStandardPaths::setTestModeEnabled(true); app.setApplicationName("WaveLinkProtocolLiveUi-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc"); if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).value(0), 9));
    if (qEnvironmentVariableIsSet("WAVELINK_RANGE_SLIDER_UI_ONLY")) return rangeSliderThemeTests();
    if (qEnvironmentVariableIsSet("WAVELINK_PLOT_READOUT_UI_ONLY")) return plotReadoutUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_PLOT_BACKGROUND_UI_ONLY")) return plotBackgroundUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_SCOPE_MEASUREMENT_UI_ONLY")) return scopeMeasurementUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_OFFLINE_HISTORY_UI_ONLY")) return offlineHistoryUiTests(offscreenMessages);
    if (qEnvironmentVariableIsSet("WAVELINK_PAGED_OFFLINE_UI_ONLY")) return offlineHistoryUiTests(offscreenMessages, true);
    if (qEnvironmentVariableIsSet("WAVELINK_DETACHED_REMOVAL_UI_ONLY")) return detachedChannelRemovalUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_LAYOUT_BUTTONS_UI_ONLY")) return layoutQuickButtonUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_HIGH_FREQUENCY_UI_ONLY")) {
        const int result=highFrequencyDemoUiTests();
        return result ? result : plotInteractionTests();
    }
    if (qEnvironmentVariableIsSet("WAVELINK_AUDIT_RELIABILITY_UI_ONLY")) return auditReliabilityUiTests(offscreenMessages);
    if (qEnvironmentVariableIsSet("WAVELINK_TIME_RANGE_UI_ONLY")) return timeRangeSpanTests();
    if (qEnvironmentVariableIsSet("WAVELINK_INTERFACE_HINTS_UI_ONLY")) return interfaceHintsUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_BLE_QUICK_UI_ONLY")) return bleQuickUiTests(app.arguments().value(2));
    if (qEnvironmentVariableIsSet("WAVELINK_RECEIVE_CACHE_UI_ONLY")) return receiveCacheUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_SYSTEM_SETTINGS_UI_ONLY")) return systemSettingsUiTests(offscreenMessages);
    if (qEnvironmentVariableIsSet("WAVELINK_RECEIVE_HIGH_RATE_UI_ONLY")) return receiveHighRateUiTests();
    if (qEnvironmentVariableIsSet("WAVELINK_CONSOLE_UI_ONLY")) { QTemporaryDir folder; return consoleInteractionTests(folder.path(), offscreenMessages); }
    if (qEnvironmentVariableIsSet("WAVELINK_RECORDING_SHAPE_UI_ONLY")) return recordingShapeUiTests(offscreenMessages);
    if (qEnvironmentVariableIsSet("WAVELINK_COMMAND_SWITCH_UI_ONLY")) return commandSwitchLiveUiTests(offscreenMessages);
    if(qEnvironmentVariableIsSet("WAVELINK_STATUS_UI_ONLY"))return statusMonitorLiveUiTests(offscreenMessages);
    VERIFY(rangeSliderThemeTests() == 0);
    QTemporaryDir dir; VERIFY(dir.isValid()); QString error;
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"); advanced.setValue("InterfaceType", 0); main.setValue("editCacheDir", dir.path()); main.setValue("comboProtocol", 0); main.sync();
    if (qEnvironmentVariableIsSet("WAVELINK_PLOT_UI_ONLY")) return plotInteractionTests();
    if (qEnvironmentVariableIsSet("WAVELINK_FILE_SEND_UI_ONLY")) return fileSendUiTests(dir.path(), offscreenMessages);
    ProtocolProfile text, binary, variable; VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(false), text, error)); VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::example(true), binary, error));
    VERIFY(ProtocolProfileValidator::profile(ProtocolEditorWidget::lengthExample(), variable, error));
    ProtocolProfileStore store; VERIFY(store.load(error)); auto snapshot = store.snapshot(); snapshot.profiles = {text, binary, variable}; snapshot.shared.kind = "custom"; snapshot.shared.id = text.id; VERIFY(store.commit(snapshot, error));
    auto port = new ReceivePort; auto worker = new TestSerialHandler(port);
    {
        MainWindow window(nullptr, worker); window.resize(1200, 900); window.show();
        auto protocol = window.findChild<QComboBox *>("comboProtocol"), profiles = window.findChild<QComboBox *>("comboCustomProfile"), interfaces = window.findChild<QComboBox *>("comboInterface");
        auto open = window.findChild<QPushButton *>("btnOpenClose"); auto summary = window.findChild<QLabel *>("lblProtocolSummary");
        VERIFY(protocol && profiles && interfaces && open && summary && !window.findChild<QPushButton *>("btnApplyProtocol"));
        VERIFY(interfaces->count() == 6 && interfaces->itemText(interfaces->count() - 1) == "Demo数据" && interfaces->itemData(interfaces->count() - 1).toInt() == 3);
        VERIFY(interfaces->findData(5) == 3 && interfaces->findData(4) == 4 && interfaces->findData(3) == 5);
        VERIFY(until([&] { return open->isEnabled() && summary->text().contains("已应用"); })); VERIFY(protocol->currentData().toString() == "custom" && profiles->currentData().toString() == text.id);
        VERIFY(summary->text().startsWith("文本帧 · 自动读取全部列\n") && !summary->text().contains("项自定义配置"));
        auto manage = window.findChild<QPushButton *>("btnCustomProtocols");
        VERIFY(manage && manage->isVisible() && manage->text() == "⚙ 管理自定义协议" && manage->styleSheet().contains("background-color"));
        window.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        VERIFY(engineProtocolHelpThemed(window)); VERIFY(protocolAccentsThemed(window)); ThemeManager::applyTheme(&app, ThemeManager::DarkTheme);
        auto ports = window.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
        open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
        VERIFY(feed(worker, port, "DATA:25.34,3.30\r\n"));
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 2; }));
        const auto checks = window.findChildren<QCheckBox *>("chkChannel"); VERIFY(checks[0]->text().contains("温度") && checks[0]->text().contains("°C") && checks[1]->text().contains("电压"));
        checks[0]->setChecked(false); VERIFY(advanced.value("CustomChannels/" + text.id + "/temperature/Visible", true).toBool() == false);
        QPushButton *record = nullptr; for (auto b : window.findChildren<QPushButton *>()) if (b->text().contains("开始录制")) record = b; VERIFY(record);
        record->click(); VERIFY(until([&] { return record->text().contains("停止录制") && !protocol->isEnabled(); }));
        auto changed = text; auto fields = changed.definition.value("channels").toArray(); auto firstField = fields[0].toObject(); firstField.insert("scale", 2); fields[0] = firstField; changed.definition.insert("channels", fields);
        ProtocolSnapshot changedSnapshot; changedSnapshot.profiles = {changed}; const auto changeBytes = ProtocolProfileValidator::encode(changedSnapshot, false, error); VERIFY(!changeBytes.isEmpty());
        auto recordingChanged = text; recordingChanged.definition.insert("recordingValueMode", "converted");
        ProtocolSnapshot recordingChange; recordingChange.profiles = {recordingChanged}; const auto recordingChangeBytes = ProtocolProfileValidator::encode(recordingChange, false, error); VERIFY(!recordingChangeBytes.isEmpty());
        bool guardedImportRejected = false;
        QTimer::singleShot(0, [&] {
            for (auto widget : QApplication::topLevelWidgets()) if (auto manager = qobject_cast<ProtocolManagerDialog *>(widget)) {
                QString refusal; guardedImportRejected = !manager->importBytes(changeBytes, ProtocolImportMode::MergeById, true, refusal) && !refusal.isEmpty();
                refusal.clear(); guardedImportRejected = guardedImportRejected && !manager->importBytes(recordingChangeBytes, ProtocolImportMode::MergeById, true, refusal) && !refusal.isEmpty(); manager->close();
            }
        });
        VERIFY(QMetaObject::invokeMethod(&window, "showCustomProtocols")); VERIFY(guardedImportRejected);
        VERIFY(!profiles->isEnabled());
        profiles->setCurrentIndex(profiles->findData(binary.id)); VERIFY(profiles->currentData().toString() == text.id);
        VERIFY(feed(worker, port, "DATA:25.34,3.30,2.5,2.8\r\nDATA:1,2,3,4\r\n"));
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 4; }));
        auto fourChecks = window.findChildren<QCheckBox *>("chkChannel");
        QCheckBox *automaticCheck = nullptr;
        for (auto check : fourChecks) if (check->text() == "Ch3") automaticCheck = check;
        VERIFY(automaticCheck); automaticCheck->setChecked(false);
        VERIFY(!advanced.value("CustomChannels/" + text.id + "/auto-column-2/Visible", true).toBool());
        VERIFY(until([&] { return worker->getLoggedBytes() > 0; })); record->click(); VERIFY(until([&] { return protocol->isEnabled(); }));
        VERIFY(channelDockFits(window));
        // Vary frame width outside CSV recording; implicit identities and
        // default metadata survive removal/recreation of trailing channels.
        VERIFY(feed(worker, port, "DATA:1,2\r\n"));
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 2; }));
        VERIFY(feed(worker, port, "DATA:25.34,3.30,2.5,2.8\r\n"));
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 4; }));
        automaticCheck = nullptr;
        for (auto check : window.findChildren<QCheckBox *>("chkChannel")) if (check->text() == "Ch3") automaticCheck = check;
        VERIFY(automaticCheck && !automaticCheck->isChecked());
        bool fourthValueSeen = false;
        for (auto plot : window.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) {
            auto graph = plot->graph(i); if (graph->name() == "Ch4" && !graph->data()->isEmpty())
                fourthValueSeen = std::abs((graph->data()->constEnd() - 1)->value - 2.8) < 1e-12;
        }
        VERIFY(fourthValueSeen);
        auto findTemperature = [&]() -> QCPGraph * { for (auto p : window.findChildren<QCustomPlot *>()) for (int i = 0; i < p->graphCount(); ++i) if (p->graph(i)->name().contains("温度")) return p->graph(i); return nullptr; };
        VERIFY(until([&] { return findTemperature() && findTemperature()->data()->size() >= 2; }));
        auto graphBeforeRename = findTemperature(); const int pointsBeforeRename = graphBeforeRename->data()->size();
        VERIFY(store.load(error)); auto renamed = text.definition; renamed.insert("name", "Renamed only"); QString savedId; VERIFY(store.saveProfile(renamed, savedId, error));
        QTimer::singleShot(0, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolManagerDialog") qobject_cast<QDialog *>(widget)->reject(); });
        VERIFY(QMetaObject::invokeMethod(&window, "showCustomProtocols")); app.processEvents();
        VERIFY(summary->text().contains("已应用") && !summary->text().contains("修订")); VERIFY(findTemperature() == graphBeforeRename && findTemperature()->data()->size() == pointsBeforeRename);
        // Numeric recording choice must reach the worker, but must not clear
        // plot history or ask the user to discard identical display data.
        for (const QString mode : {QString("converted"), QString("raw")}) {
            auto recordingDefinition = renamed; recordingDefinition.insert("recordingValueMode", mode);
            VERIFY(store.load(error) && store.saveProfile(recordingDefinition, savedId, error));
            int applied = 0; bool unexpectedConfirmation = false;
            const auto ack = QObject::connect(worker, &SerialHandler::protocolApplyFinished, &window, [&](quint64, bool success, const QString &) { if (success) ++applied; });
            QTimer dismiss; QObject::connect(&dismiss, &QTimer::timeout, [&] {
                for (auto widget : QApplication::topLevelWidgets()) if (widget->isVisible()) {
                    if (widget->objectName() == "protocolManagerDialog") qobject_cast<QDialog *>(widget)->reject();
                    if (widget->objectName() == "protocolApplyConfirm") { unexpectedConfirmation = true; qobject_cast<QDialog *>(widget)->reject(); }
                }
            }); dismiss.start(10);
            VERIFY(QMetaObject::invokeMethod(&window, "showCustomProtocols"));
            VERIFY(until([&] { return applied == 1 && summary->text().contains("已应用") && profiles->isEnabled(); }));
            QObject::disconnect(ack);
            VERIFY(!unexpectedConfirmation && findTemperature() == graphBeforeRename && findTemperature()->data()->size() == pointsBeforeRename);
        }
        // Auto-application still honors the user's cancellation of destructive
        // history clearing and keeps the original parser/UI choice together.
        profiles->setCurrentIndex(profiles->findData(binary.id));
        QTimer::singleShot(0, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "protocolApplyConfirm") qobject_cast<QDialog *>(widget)->reject(); });
        VERIFY(until([&] { return profiles->currentData().toString() == text.id && summary->text().contains("已应用"); }));
        profiles->setCurrentIndex(profiles->findData(binary.id)); confirmSwitch();
        VERIFY(until([&] { return summary->text().contains("已应用") && profiles->isEnabled() && profiles->currentData().toString() == binary.id; }));
        VERIFY(feed(worker, port, QByteArray::fromHex("aa55e60933335340e70d0a")));
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 2; }));
        open->click(); VERIFY(until([&] { return interfaces->isEnabled(); }));
        // BLE follows the global protocol choice even with no native helper/radio.
        // Interface changes apply synchronously and can ask about existing
        // history. Arm the simulated user response BEFORE changing interface,
        // rather than relying on a race in the plot frame's arrival.
        confirmSwitch(); interfaces->setCurrentIndex(interfaces->findData(4)); VERIFY(until([&] { return open->isEnabled() && protocol->currentData().toString() == "custom" && profiles->currentData().toString() == binary.id; }));
        VERIFY(window.findChild<QStackedWidget *>("stackInterface")->currentIndex() == 4);
        VERIFY(manage->isVisible());
        protocol->setCurrentIndex(protocol->findData("custom")); profiles->setCurrentIndex(profiles->findData(text.id)); confirmSwitch(); VERIFY(until([&] { return summary->text().contains("已应用") && profiles->currentData().toString() == text.id; }));
        VERIFY(manage->isVisible());
        VERIFY(store.load(error) && store.snapshot().shared.id == text.id && store.snapshot().ble.id == text.id);
        confirmSwitch(); interfaces->setCurrentIndex(interfaces->findData(3)); VERIFY(until([&] { return open->isEnabled(); })); VERIFY(!protocol->isEnabled());
        VERIFY(window.findChild<QStackedWidget *>("stackInterface")->currentIndex() == 3 && advanced.value("InterfaceType").toInt() == 3);
        open->click(); VERIFY(until([&] { return open->text().contains("停止"); }));
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() > 0; })); VERIFY(window.findChildren<QCheckBox *>("chkChannel")[0]->text().startsWith("通道"));
        open->click(); VERIFY(until([&] { return interfaces->isEnabled(); }));
        confirmSwitch(); interfaces->setCurrentIndex(1); VERIFY(until([&] { return open->isEnabled() && summary->text().contains("已应用"); })); VERIFY(profiles->currentData().toString() == text.id);
        QTcpServer peerServer; VERIFY(peerServer.listen(QHostAddress::LocalHost, 0));
        for (auto spin : window.findChildren<QSpinBox *>()) if (spin->objectName() == "spinPort") spin->setValue(peerServer.serverPort());
        auto ip = window.findChild<QLineEdit *>("editIP"); VERIFY(ip); ip->setText("127.0.0.1"); open->click(); VERIFY(until([&] { return peerServer.hasPendingConnections(); }));
        auto peer = peerServer.nextPendingConnection(); peer->write("DATA:25.34,3.30\r\n"); peer->flush();
        VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 2; }));
        open->click(); VERIFY(until([&] { return interfaces->isEnabled(); }));
        // Apply the length-field snapshot through the actual MainWindow
        // queued connection and verify its global selection survives restart.
        confirmSwitch(); interfaces->setCurrentIndex(0); VERIFY(until([&] { return open->isEnabled() && summary->text().contains("已应用"); }));
        profiles->setCurrentIndex(profiles->findData(variable.id)); confirmSwitch();
        VERIFY(until([&] { return summary->text().contains("已应用") && profiles->isEnabled() && profiles->currentData().toString() == variable.id; })); VERIFY(summary->text().contains("长度字段帧"));
        ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY"); open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
        const auto variablePacket = QByteArray::fromHex("AA5506E60933335340650D0A");
        VERIFY(feed(worker, port, QByteArray::fromHex("AA5500") + variablePacket.left(7))); app.processEvents();
        VERIFY(feed(worker, port, variablePacket.mid(7))); VERIFY(until([&] { return window.findChildren<QCheckBox *>("chkChannel").size() == 2; }));
        open->click(); VERIFY(until([&] { return interfaces->isEnabled(); }));
        ThemeManager::applyTheme(&app, ThemeManager::LightTheme); window.resize(720, 1300); app.processEvents();
        if (argc > 1) { QDir().mkpath(QString::fromLocal8Bit(argv[1])); window.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-live-light.png"); }
        // Verify the three-column manager with the application's REAL styles,
        // rather than only a standalone dialog's synthetic test palette.
        bool managerLayoutOk = false, managerLightOk = false, managerDarkOk = false, managerOpenedCurrent = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto widget : QApplication::topLevelWidgets()) if (auto manager = qobject_cast<ProtocolManagerDialog *>(widget)) {
                manager->resize(1920, 1500);
                QTimer::singleShot(50, manager, [&, manager] {
                    auto columns = manager->findChild<QSplitter *>("protocolColumns");
                    auto protocolList = manager->findChild<QListWidget *>("protocolList");
                    managerOpenedCurrent = protocolList->currentRow() == 2 && protocolList->currentItem()->data(Qt::UserRole).toString() == variable.id
                        && manager->findChild<QLineEdit *>("protocolName")->text() == variable.name
                        && manager->findChild<QComboBox *>("protocolPreviewProtocol")->currentData().toString() == variable.id;
                    auto middle = manager->findChild<QScrollArea *>("protocolEditorScroll");
                    auto right = manager->findChild<QScrollArea *>("protocolPreviewScroll");
                    managerLayoutOk = columns && columns->count() == 3 && middle->width() > 300 && right->width() > 200;
                    auto editor = manager->findChild<ProtocolEditorWidget *>();
                    // Check the tall/maximized geometry with both real theme
                    // styles and every protocol page, not only the first row.
                    for (int row = 0; row < 3; ++row) {
                        protocolList->setCurrentRow(row); app.processEvents();
                        auto name = editor->findChild<QLineEdit *>("protocolName"); auto type = editor->findChild<QComboBox *>("protocolType"); auto desc = editor->findChild<QPlainTextEdit *>("protocolDescription");
                        const int nameGap = type->mapTo(editor, QPoint()).y() - name->mapTo(editor, QPoint()).y() - name->height();
                        const int typeGap = desc->mapTo(editor, QPoint()).y() - type->mapTo(editor, QPoint()).y() - type->height();
                        managerLayoutOk = managerLayoutOk && name->mapTo(editor, QPoint()).y() <= 12 && nameGap >= 6 && nameGap <= 12 && typeGap >= 6 && typeGap <= 12;
                        if (argc > 1) manager->grab().save(QString::fromLocal8Bit(argv[1]) + QString("/protocol-maximized-%1-light.png").arg(row));
                    }
                    protocolList->setCurrentRow(0); app.processEvents();
                    auto description = editor->findChild<QPlainTextEdit *>("protocolDescription");
                    auto firstField = editor->findChild<QComboBox *>("protocol_encoding");
                    const int sectionGap = firstField->mapTo(editor, QPoint()).y() - description->mapTo(editor, QPoint()).y() - description->height();
                    managerLayoutOk = managerLayoutOk && sectionGap >= 8 && sectionGap <= 30;
                    managerLightOk = protocolList->palette().color(QPalette::Base).lightness() > 200 && headerEmptyAreasThemed(false);
                    if (argc > 1) manager->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-three-columns-light.png");
                    protocolList->setCurrentRow(1); app.processEvents();
                    auto frameSummary = editor->findChild<QLabel *>("protocolFrameSummary");
                    managerLightOk = managerLightOk && frameSummary && frameSummary->palette().color(QPalette::WindowText).lightness() < 100;
                    auto sizeField = editor->findChild<QSpinBox *>("protocol_sizeBytes");
                    auto payloadField = editor->findChild<QSpinBox *>("protocol_payloadOffset");
                    const int frameGap = payloadField->mapTo(editor, QPoint()).y() - sizeField->mapTo(editor, QPoint()).y() - sizeField->height();
                    managerLayoutOk = managerLayoutOk && frameGap >= 6 && frameGap <= 12;
                    if (argc > 1) manager->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-fixed-checksum-light.png");
                    ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); app.processEvents();
                    managerDarkOk = protocolList->palette().color(QPalette::Base).lightness() < 100 && protocolList->palette().color(QPalette::Text).lightness() > 150 && headerEmptyAreasThemed(true);
                    managerDarkOk = managerDarkOk && frameSummary->palette().color(QPalette::WindowText).lightness() > 150;
                    auto fieldsTable = editor->findChild<QTableWidget *>("protocolFields"); QFontMetrics metrics(fieldsTable->horizontalHeader()->font());
                    for (int col = 1; col < fieldsTable->columnCount(); ++col) if (!fieldsTable->isColumnHidden(col))
                        managerLayoutOk = managerLayoutOk && fieldsTable->columnWidth(col) >= metrics.horizontalAdvance(fieldsTable->horizontalHeaderItem(col)->text()) + 16;
                    if (argc > 1) manager->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-three-columns-dark.png");
                    auto algorithm = editor->findChild<QComboBox *>("protocol_algorithm");
                    algorithm->setCurrentIndex(algorithm->findData("none")); app.processEvents();
                    auto note = editor->findChild<QLabel *>("protocolBinaryNote");
                    auto checksumRule = editor->findChild<QLabel *>("protocolChecksumRule");
                    const int checksumGap = note->mapTo(editor, QPoint()).y() - checksumRule->mapTo(editor, QPoint()).y() - checksumRule->height();
                    managerLayoutOk = managerLayoutOk && checksumGap >= 6 && checksumGap <= 12;
                    auto conditionHint = editor->findChild<QLabel *>("protocolConditionsHint"); auto conditions = editor->findChild<QTableWidget *>("protocolConditions");
                    managerLayoutOk = managerLayoutOk && conditionHint->parentWidget()->height() <= conditions->height() + conditionHint->height() + 110;
                    if (argc > 1) manager->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-fixed-no-checksum-dark.png");
                    QTimer::singleShot(0, manager, [&, manager] {
                        if (auto help = manager->findChild<QDialog *>("protocolChecksumHelpDialog")) {
                            auto body = help->findChild<QTextBrowser *>("protocolChecksumHelpText");
                            managerDarkOk = managerDarkOk && body && body->palette().color(QPalette::Base).lightness() < 100 && body->palette().color(QPalette::Text).lightness() > 150;
                            if (body) { const auto link = body->document()->find("CRC RevEng 参数目录"); managerDarkOk = managerDarkOk && !link.isNull() && link.charFormat().foreground().color().lightness() > 150; }
                            if (argc > 1) help->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-checksum-help-dark.png"); help->accept();
                        }
                    });
                    editor->findChild<QPushButton *>("protocolChecksumHelp")->click();
                    QTimer::singleShot(0, manager, [&, manager] {
                        auto guide = manager->findChild<QDialog *>("protocolUsageDialog");
                        if (!guide) { managerLayoutOk = false; return; }
                        auto navigation = guide->findChild<QListWidget *>("protocolUsageList");
                        auto body = guide->findChild<QTextBrowser *>("protocolUsageText");
                        auto columns = guide->findChild<QSplitter *>("protocolUsageColumns");
                        managerLayoutOk = managerLayoutOk && navigation && navigation->count() == 10 && body && columns && columns->count() == 2
                            && guide->windowFlags().testFlag(Qt::WindowMinMaxButtonsHint);
                        if (navigation && body) {
                            for (int section = 0; section < 10; ++section) {
                                navigation->setCurrentRow(section); app.processEvents();
                                managerLayoutOk = managerLayoutOk && body->toPlainText().size() > 200 && body->toPlainText().contains(navigation->currentItem()->text().mid(4));
                            }
                            navigation->setCurrentRow(6); app.processEvents();
                            managerDarkOk = managerDarkOk && body->palette().color(QPalette::Base).lightness() < 100
                                && body->palette().color(QPalette::Text).lightness() > 150
                                && body->toPlainText().contains("从帧起点 0") && body->toPlainText().contains("系统计算");
                            if (argc > 1) guide->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-usage-checksum-dark.png");
                            ThemeManager::applyTheme(&app, ThemeManager::LightTheme); app.processEvents(); app.processEvents();
                            navigation->setCurrentRow(0); app.processEvents();
                            managerLightOk = managerLightOk && body->palette().color(QPalette::Base).lightness() > 200 && body->palette().color(QPalette::Text).lightness() < 100;
                            if (argc > 1) guide->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-usage-start-light.png");
                            guide->resize(720, 480); navigation->setCurrentRow(7); app.processEvents();
                            managerLayoutOk = managerLayoutOk && body->width() > 250 && body->toPlainText().contains("自动用 HEX") && body->toPlainText().contains("保留相同字节");
                            ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); app.processEvents(); app.processEvents();
                            if (argc > 1) guide->grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-usage-testing-dark.png");
                        }
                        guide->accept();
                    });
                    manager->findChild<QPushButton *>("protocolHelp")->click();
                    managerLayoutOk = checksumLayoutFits() && managerLayoutOk;
                    QTimer::singleShot(0, manager, [manager] {
                        if (auto confirm = manager->findChild<QDialog *>("protocolConfirm")) confirm->findChild<QPushButton *>("protocolDecision2")->click();
                    });
                    static_cast<QDialog *>(manager)->reject();
                });
            }
        });
        VERIFY(QMetaObject::invokeMethod(&window, "showCustomProtocols"));
        VERIFY(managerOpenedCurrent && managerLayoutOk && managerLightOk && managerDarkOk);
        ThemeManager::applyTheme(&app, ThemeManager::DarkTheme); window.resize(1200, 900); app.processEvents();
        if (argc > 1) window.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-live-dark.png");
        window.close();
    }
    {
        MainWindow restored; restored.show(); auto summary = restored.findChild<QLabel *>("lblProtocolSummary");
        VERIFY(until([&] { return summary->text().contains("已应用"); })); VERIFY(restored.findChild<QComboBox *>("comboCustomProfile")->currentData().toString() == variable.id && summary->text().contains("长度字段帧")); restored.close();
        VERIFY(!restored.findChild<QPushButton *>("btnCustomProtocols")->isHidden());
    }
    {
        // Legacy protocol ID/index stay unchanged; only the labels are fixed.
        VERIFY(store.load(error)); const auto previous = store.snapshot(); auto cbf = previous;
        cbf.shared = ProtocolSelection::fromLegacy(3); cbf.ble = cbf.shared; VERIFY(store.commit(cbf, error));
        advanced.setValue("InterfaceType", 0); advanced.sync();
        auto port = new ReceivePort; auto worker = new TestSerialHandler(port);
        MainWindow labels(nullptr, worker); labels.show();
        auto protocol = labels.findChild<QComboBox *>("comboProtocol");
        VERIFY(until([&] { return protocol->isEnabled(); }) && protocol->currentData().toString() == "custom");
        VERIFY(labels.findChild<QComboBox *>("comboCustomProfile")->currentText() == "CBF/IR/RED");
        VERIFY(until([&] { return labels.findChild<QLabel *>("lblProtocolSummary")->text().contains("已应用"); }));
        labels.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        labels.findChild<QPushButton *>("btnPausePlot")->setChecked(false);
        for (auto check : labels.findChildren<QCheckBox *>()) if (check->text().startsWith("陷波") || check->text().startsWith("高通") || check->text().startsWith("低通")) check->setChecked(false);
        // This project-specific protocol normally selects a TCP server. Test
        // its decoder and display without binding the user's default port.
        AMU4Parser parser; QVector<double> decoded;
        QObject::connect(&parser, &ProtocolParser::channelsUpdated, [&](const QVector<double> &values) {
            decoded = values;
            QMetaObject::invokeMethod(&labels, "handleChannelsUpdated", Q_ARG(QVector<double>, values));
        });
        const QByteArray input("2:33\r\n0:11\r\n1:22\r\n"); parser.processData(input.constData(), input.size());
        VERIFY(decoded == QVector<double>({11, 22, 33}));
        const QStringList names{"CBF (MHz)", "IR", "RED"};
        const bool labelsMatch = until([&] {
            for (int channel = 0; channel < 3; ++channel) {
                bool found = false;
                for (auto plot : labels.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) {
                    auto graph = plot->graph(i);
                    if (graph->name() == names[channel] && !graph->data()->isEmpty() && (graph->data()->constEnd() - 1)->value == (channel + 1) * 11) found = true;
                }
                if (!found) return false;
            }
            return true;
        });
        if (!labelsMatch) {
            for (auto plot : labels.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) {
                auto graph = plot->graph(i);
                std::cerr << "CBF label diagnostic: " << graph->name().toStdString() << " points=" << graph->data()->size();
                if (!graph->data()->isEmpty()) std::cerr << " latest=" << (graph->data()->constEnd() - 1)->value;
                std::cerr << "\n";
            }
        }
        VERIFY(labelsMatch);
        const auto checks = labels.findChildren<QCheckBox *>("chkChannel"); VERIFY(checks.size() == 3);
        for (int i = 0; i < 3; ++i) VERIFY(checks[i]->text() == names[i]);
        auto plotDockCount = [&] { int count = 0; for (auto dock : labels.findChildren<QDockWidget *>()) if (dock->objectName().startsWith("PlotDock_")) ++count; return count; };
        VERIFY(plotDockCount() == 4);
        labels.findChild<QPushButton *>("btnMergeAllViews")->click(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        VERIFY(plotDockCount() == 1);
        parser.processData(input.constData(), input.size()); QCoreApplication::processEvents();
        VERIFY(plotDockCount() == 1); // The preset is not re-applied every frame.
        labels.close();
        VERIFY(store.load(error) && store.commit(previous, error));
    }
    {
        // A new CBF template defaults to TCP, without automatically listening.
        VERIFY(store.load(error)); const auto previous = store.snapshot();
        auto d = ProtocolProfileValidator::keyValueTemplate(true); d.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
        ProtocolProfile p; VERIFY(ProtocolProfileValidator::profile(d, p, error));
        auto chosen = previous; chosen.profiles.append(p); chosen.shared.kind = "custom"; chosen.shared.id = p.id; chosen.shared.bigEndian = false; chosen.ble = chosen.shared;
        VERIFY(store.commit(chosen, error)); advanced.setValue("InterfaceType", 0); advanced.sync();
        {
            auto worker = new TestSerialHandler(new ReceivePort); MainWindow preset(nullptr, worker); preset.show();
            VERIFY(until([&] { return preset.findChild<QComboBox *>("comboInterface")->currentData().toInt() == 2
                && preset.findChild<QLabel *>("lblProtocolSummary")->text().contains("已应用"); }));
            bool portMatches = false; for (auto portSpin : preset.findChildren<QSpinBox *>())
                if (portSpin->maximum() == 65535 && portSpin->value() == 4970) portMatches = true;
            VERIFY(portMatches); preset.close();
        }
        // Opting out still allows other interfaces and keeps the main view.
        auto devicePreset = d.value("devicePreset").toObject(); devicePreset.insert("enabled", false); d.insert("devicePreset", devicePreset);
        VERIFY(ProtocolProfileValidator::profile(d, p, error)); chosen.profiles.last() = p;
        VERIFY(store.commit(chosen, error)); advanced.setValue("InterfaceType", 0); advanced.sync();
        auto port = new ReceivePort; auto worker = new TestSerialHandler(port); MainWindow plain(nullptr, worker); plain.show();
        auto open = plain.findChild<QPushButton *>("btnOpenClose"); auto summary = plain.findChild<QLabel *>("lblProtocolSummary");
        VERIFY(until([&] { return open->isEnabled() && summary->text().contains("已应用"); }));
        VERIFY(plain.findChild<QComboBox *>("comboInterface")->currentData().toInt() == 0 && summary->text().contains("多通道键值"));
        plain.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        plain.findChild<QPushButton *>("btnPausePlot")->setChecked(false);
        for (auto check : plain.findChildren<QCheckBox *>()) if (check->text().startsWith("陷波") || check->text().startsWith("高通") || check->text().startsWith("低通")) check->setChecked(false);
        auto ports = plain.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
        open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
        VERIFY(feed(worker, port, "2:33\r\n0:11\r\n1:22\r\n"));
        VERIFY(until([&] { return plain.findChildren<QCheckBox *>("chkChannel").size() == 3; }));
        const auto checks = plain.findChildren<QCheckBox *>("chkChannel"); VERIFY(checks[0]->text() == "CBF (MHz)" && checks[1]->text() == "IR" && checks[2]->text() == "RED");
        int plotDocks = 0; for (auto dock : plain.findChildren<QDockWidget *>()) if (dock->objectName().startsWith("PlotDock_")) ++plotDocks;
        VERIFY(plotDocks == 1);
        open->click(); VERIFY(until([&] { return open->text().contains("打开"); })); plain.close();
        VERIFY(store.load(error) && store.commit(previous, error));
    }
    {
        const QStringList savedNames{"温度,环境", "电压\"A\"", "Ch3"};
        CsvRecordingWriter csv; WlrRecordingWriter wlr;
        const auto csvPath = dir.filePath("import_named.csv"), wlrPath = dir.filePath("import_named.wlr");
        VERIFY(csv.start(csvPath, error, 250, savedNames) && wlr.start(wlrPath, error, 250, savedNames));
        for (int i = 0; i < 5; ++i) {
            const QVector<double> frame{i == 2 ? std::numeric_limits<double>::quiet_NaN() : double(i), 2, 3};
            const qint64 receiveTime = 1700000000000000000LL + i * 4000000LL;
            VERIFY(csv.append(frame, error, receiveTime) && wlr.append(frame, error, receiveTime));
        }
        VERIFY(csv.finish(error) && wlr.finish(error));
        // Older UTF-8 files without a BOM must continue to import correctly.
        const auto legacyNamedPath = dir.filePath("import_named_without_bom.csv");
        QFile originalNamed(csvPath); VERIFY(originalNamed.open(QIODevice::ReadOnly));
        const auto namedBytes = originalNamed.readAll(); originalNamed.close();
        VERIFY(namedBytes.startsWith(RecordingChannels::csvUtf8Bom()) && namedBytes.count(RecordingChannels::csvUtf8Bom()) == 1);
        VERIFY(namedBytes.startsWith(RecordingChannels::csvUtf8Bom() + "UnixTime(s;FS=250),SampleIndex,"));
        QFile legacyNamed(legacyNamedPath); VERIFY(legacyNamed.open(QIODevice::WriteOnly));
        VERIFY(legacyNamed.write(namedBytes.mid(3)) == namedBytes.size() - 3); legacyNamed.close();
        // Historical Hint headers must import through the real GUI path, both
        // with and without a BOM, preserving names, NaN gaps and time spacing.
        const auto hintPath = dir.filePath("import_named_hint.csv"), hintNoBomPath = dir.filePath("import_named_hint_without_bom.csv");
        QByteArray hintBytes = namedBytes; hintBytes.replace("UnixTime(s;FS=250)", "HostReceiveTime(s;Hint=250)");
        QFile hintFile(hintPath); VERIFY(hintFile.open(QIODevice::WriteOnly) && hintFile.write(hintBytes) == hintBytes.size()); hintFile.close();
        QFile hintNoBom(hintNoBomPath); VERIFY(hintNoBom.open(QIODevice::WriteOnly) && hintNoBom.write(hintBytes.mid(3)) == hintBytes.size() - 3); hintNoBom.close();
        advanced.setValue("InterfaceType", 0); advanced.sync();
        auto importPort = new ReceivePort; auto importWorker = new TestSerialHandler(importPort);
        MainWindow imported(nullptr, importWorker); imported.show();
        VERIFY(until([&] { return imported.findChild<QLabel *>("lblProtocolSummary")->text().contains("已应用"); }));
        bool cancelNextImport = false, importCancelled = false;
        auto fileAction = [&](const QString &path, const char *slot) {
            QTimer automate; bool chosen = false; QElapsedTimer deadline; deadline.start();
            QObject::connect(&automate, &QTimer::timeout, [&] {
                if (cancelNextImport) if (auto progress = imported.findChild<QProgressDialog *>()) {
                    if (progress->windowTitle() == "导入数据") { progress->cancel(); cancelNextImport = false; importCancelled = true; }
                }
                for (auto widget : QApplication::topLevelWidgets()) if (widget->isVisible()) {
                    if (auto dialog = qobject_cast<QFileDialog *>(widget)) {
                        if (!chosen) { chosen = true; dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); }
                        else if (deadline.elapsed() > 7000) dialog->reject();
                    } else if (auto message = qobject_cast<QMessageBox *>(widget)) message->accept();
                }
            }); automate.start(10);
            QPushButton *button = nullptr;
            const QString caption = QString::fromLatin1(slot) == "onImportData" ? "导入数据" : "导出当前波形";
            for (auto candidate : imported.findChildren<QPushButton *>()) if (candidate->text().contains(caption)) button = candidate;
            if (!button) return false;
            button->click(); return chosen;
        };
        for (const auto &path : {csvPath, wlrPath, legacyNamedPath, hintPath, hintNoBomPath}) {
            VERIFY(fileAction(path, "onImportData"));
            auto range = imported.findChild<RangeSlider *>("timeRangeSlider"); auto quick = imported.findChild<QPushButton *>("btnTimeScrollToLatest");
            auto original = imported.findChild<QPushButton *>("btnScrollToLatest");
            VERIFY(range && quick && original && quick->text() == ">>" && quick->toolTip().contains("跳转至最新时刻") && quick->isVisible());
            VERIFY(quick->mapTo(&imported, QPoint()).x() > range->mapTo(&imported, QPoint()).x() + range->width());
            QCustomPlot *wavePlot = nullptr;
            for (auto plot : imported.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) if (savedNames.contains(plot->graph(i)->name())) wavePlot = plot;
            VERIFY(wavePlot); const auto fullWindow = wavePlot->xAxis->range();
            const double latest = range->maximum(), span = latest / 4; VERIFY(latest > 0);
            range->setValues(0, span); original->click(); const double lower = range->lowerValue(), upper = range->upperValue();
            range->setValues(0, span); quick->click();
            VERIFY(std::abs(range->lowerValue() - lower) < 1e-9 && std::abs(range->upperValue() - upper) < 1e-9 && std::abs(upper - latest) < 1e-9);
            for (auto plot : imported.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) if (savedNames.contains(plot->graph(i)->name()))
                VERIFY(std::abs(plot->xAxis->range().upper - upper) < 1e-9);
            // Analysis intentionally follows the visible time window. Restore
            // the original full window for the pre-existing Mean/RMS tests.
            wavePlot->xAxis->setRange(fullWindow);
            const auto checks = imported.findChildren<QCheckBox *>("chkChannel"); VERIFY(checks.size() == 3);
            for (int i = 0; i < 3; ++i) VERIFY(checks[i]->text() == savedNames[i]);
            bool namedAnalysis = false;
            for (auto combo : imported.findChildren<QComboBox *>()) if (combo->count() == 3 && combo->itemText(0) == savedNames[0]) namedAnalysis = true;
            VERIFY(namedAnalysis);
            bool namedGraph = false;
            for (auto plot : imported.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i)
                if (plot->graph(i)->name() == savedNames[1] && plot->graph(i)->data()->size() == 5) namedGraph = true;
            VERIFY(namedGraph);
            QCPGraph *gapGraph = nullptr;
            for (auto plot : imported.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) if (plot->graph(i)->name() == savedNames[0]) gapGraph = plot->graph(i);
            VERIFY(gapGraph && gapGraph->data()->size() == 5 && std::isnan((gapGraph->data()->constBegin() + 2)->value));
            for (int i = 0; i < 5; ++i) VERIFY(std::abs((gapGraph->data()->constBegin() + i)->key - i * .004) < 1e-7);
            VERIFY(until([&] { for (auto label : imported.findChildren<QLabel *>()) if (label->text().startsWith("Mean: ")) return label->text().mid(6).toDouble() == 2; return false; }));
            QCheckBox *lowpass = nullptr; for (auto check : imported.findChildren<QCheckBox *>()) if (check->text().startsWith("低通")) lowpass = check;
            VERIFY(lowpass); lowpass->setChecked(true); app.processEvents();
            VERIFY(std::isnan((gapGraph->data()->constBegin() + 2)->value) && std::isfinite((gapGraph->data()->constBegin() + 3)->value));
            lowpass->setChecked(false); app.processEvents();
            // Newly detached views must use the active theme, even when the
            // stale AdvancedApp namespace contains the opposite preference.
            QComboBox *theme = nullptr;
            for (auto combo : imported.findChildren<QComboBox *>()) if (combo->count() == 2 && combo->itemText(0).contains("深色科技")) theme = combo;
            VERIFY(theme); theme->setCurrentIndex(1); advanced.setValue("comboTheme", 0);
            auto channels = imported.findChild<QListWidget *>();
            for (auto list : imported.findChildren<QListWidget *>()) if (list->count() == 3 && list->itemWidget(list->item(0)) && list->itemWidget(list->item(0))->findChild<QCheckBox *>("chkChannel")) channels = list;
            VERIFY(channels);
            bool separated = false;
            QTimer::singleShot(0, [&] { for (auto widget : QApplication::topLevelWidgets()) if (auto menu = qobject_cast<QMenu *>(widget)) if (menu->isVisible()) {
                for (auto action : menu->actions()) if (action->text() == "分离到新视图") { action->trigger(); separated = true; menu->close(); }
            } });
            VERIFY(QMetaObject::invokeMethod(&imported, "onChannelContextMenu", Q_ARG(QPoint, channels->visualItemRect(channels->item(0)).center())) && separated);
            VERIFY(until([&] { int visible = 0; for (auto plot : imported.findChildren<QCustomPlot *>()) if (plot->isVisible() && plot->yAxis->label() == "数据值" && plot->height() > 60) ++visible; return visible >= 2; }));
            auto plots = imported.findChildren<QCustomPlot *>(); int lightPlots = 0;
            for (auto plot : plots) if (plot->yAxis->label() == "数据值") { ++lightPlots; VERIFY(plot->axisRect()->backgroundBrush().color() == QColor("#ffffff") && plot->yAxis->tickLabelColor() == QColor("#333333")); }
            VERIFY(lightPlots >= 2);
            if (argc > 1) imported.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-detached-light.png");
            theme->setCurrentIndex(0); for (auto plot : plots) if (plot->yAxis->label() == "数据值") VERIFY(plot->axisRect()->backgroundBrush().color() == QColor("#1e1e1e"));
            if (argc > 1) imported.grab().save(QString::fromLocal8Bit(argv[1]) + "/protocol-detached-dark.png");
            const auto exportPath = path + ".export.csv"; VERIFY(fileAction(exportPath, "onExportCache"));
            QFile exported(exportPath); VERIFY(exported.open(QIODevice::ReadOnly)); QStringList header;
            const auto exportBytes = exported.readAll();
            VERIFY(exportBytes.startsWith(RecordingChannels::csvUtf8Bom()) && exportBytes.count(RecordingChannels::csvUtf8Bom()) == 1);
            exported.seek(0); QTextStream headerStream(&exported); headerStream.setCodec("UTF-8");
            VERIFY(RecordingChannels::parseCsvRow(headerStream.readLine(), header) && header.first() == "Time(s)" && header.mid(1) == savedNames);
            VERIFY(fileAction(exportPath, "onImportData"));
            for (int i = 0; i < 3; ++i) VERIFY(imported.findChildren<QCheckBox *>("chkChannel")[i]->text() == savedNames[i]);
        }
        VERIFY(QMetaObject::invokeMethod(&imported, "onClearPlots"));
        VERIFY(imported.findChildren<QCheckBox *>("chkChannel")[0]->text().contains("温度"));
        // Legacy header with no metadata still restores its channel labels.
        const auto oldPath = dir.filePath("legacy.csv"); QFile legacy(oldPath); VERIFY(legacy.open(QIODevice::WriteOnly));
        legacy.write("Time(s),CH1,Ch2\n0,1,2\n0.004,3,4\n"); legacy.close();
        VERIFY(fileAction(oldPath, "onImportData"));
        VERIFY(imported.findChildren<QCheckBox *>("chkChannel")[0]->text() == "CH1" && imported.findChildren<QCheckBox *>("chkChannel")[1]->text() == "Ch2");
        const auto legacyWlrPath = dir.filePath("legacy.wlr"); WlrRecordingWriter legacyWlr;
        VERIFY(legacyWlr.start(legacyWlrPath, error, 250) && legacyWlr.append({1, 2}, error) && legacyWlr.append({3, 4}, error) && legacyWlr.finish(error));
        VERIFY(fileAction(legacyWlrPath, "onImportData"));
        VERIFY(imported.findChildren<QCheckBox *>("chkChannel")[0]->text() == "Ch1" && imported.findChildren<QCheckBox *>("chkChannel")[1]->text() == "Ch2");
        // When a single receive batch has identical timestamps, the stored FS
        // or Hint must supply the fallback interval, including fractional or
        // scientific-notation rates. It is not a data channel.
        for (const auto &label : {QString("UnixTime"), QString("HostReceiveTime")})
        for (const auto &metadata : {QString("FS=250"), QString("Hint=250"), QString("FS=2.5e2"), QString("Hint=2.5e2"), QString("FS=125.5"), QString("Hint=125.5")}) {
            const auto timePath = dir.filePath("time_metadata.csv"); QFile timeFile(timePath);
            QByteArray bytes = RecordingChannels::csvUtf8Bom() + label.toUtf8() + "(s;" + metadata.toUtf8() + "),SampleIndex,温度,Ch2\n";
            for (int i = 0; i < 4; ++i) bytes += "1700000000.000000000," + QByteArray::number(i) + ',' + QByteArray::number(i + 1) + ",2\n";
            VERIFY(timeFile.open(QIODevice::WriteOnly) && timeFile.write(bytes) == bytes.size()); timeFile.close();
            VERIFY(fileAction(timePath, "onImportData"));
            const auto checks = imported.findChildren<QCheckBox *>("chkChannel"); VERIFY(checks.size() == 2 && checks[0]->text() == "温度" && checks[1]->text() == "Ch2");
            QCPGraph *timeGraph = nullptr;
            for (auto plot : imported.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) if (plot->graph(i)->name() == "温度") timeGraph = plot->graph(i);
            VERIFY(timeGraph && timeGraph->data()->size() == 4);
            const double rate = metadata.mid(metadata.indexOf('=') + 1).toDouble();
            for (int i = 0; i < 4; ++i) VERIFY(std::abs((timeGraph->data()->constBegin() + i)->key - i / rate) < 1e-9 && (timeGraph->data()->constBegin() + i)->value == i + 1);
            bool fsRestored = false;
            for (auto spin : imported.findChildren<QDoubleSpinBox *>()) if (spin->suffix() == " Hz" && spin->maximum() == 100000) fsRestored = std::abs(spin->value() - rate) < 1e-9;
            VERIFY(fsRestored);
        }
        const auto dynamicCsvPath = dir.filePath("dynamic_import.csv"), dynamicWlrPath = dir.filePath("dynamic_import.wlr");
        CsvRecordingWriter dynamicCsv; WlrRecordingWriter dynamicWlr;
        VERIFY(dynamicCsv.start(dynamicCsvPath, error, 250, savedNames, true) && dynamicWlr.start(dynamicWlrPath, error, 250, savedNames, true));
        for (const auto &v : {QVector<double>{1, 2}, QVector<double>{3, 4, 5}, QVector<double>{6}}) VERIFY(dynamicCsv.append(v, error) && dynamicWlr.append(v, error));
        VERIFY(dynamicCsv.finish(error) && dynamicWlr.finish(error));
        for (const auto &path : {dynamicCsvPath, dynamicWlrPath}) {
            VERIFY(fileAction(path, "onImportData"));
            auto checks = imported.findChildren<QCheckBox *>("chkChannel"); VERIFY(checks.size() == 3);
            for (int i = 0; i < 3; ++i) VERIFY(checks[i]->text() == savedNames[i]);
            QCPGraph *graph = nullptr;
            for (auto plot : imported.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) if (plot->graph(i)->name() == savedNames[2]) graph = plot->graph(i);
            VERIFY(graph && graph->data()->size() == 3 && std::isnan(graph->data()->constBegin()->value)
                && (graph->data()->constBegin() + 1)->value == 5 && std::isnan((graph->data()->constBegin() + 2)->value));
        }
        const auto precisionPath = dir.filePath("precision_ui.csv"); QFile precisionFile(precisionPath);
        VERIFY(precisionFile.open(QIODevice::WriteOnly));
        precisionFile.write(RecordingChannels::csvUtf8Bom() + "HostReceiveTime(s;FS=250),SampleIndex,原始精度,Ch2\n");
        for (int i = 0; i < 4; ++i) precisionFile.write("1700000000.0," + QByteArray::number(i) + ",12345678901234,0.000000000123456789\n");
        precisionFile.close(); VERIFY(fileAction(precisionPath, "onImportData"));
        QCPGraph *preciseGraph = nullptr;
        for (auto plot : imported.findChildren<QCustomPlot *>()) for (int i = 0; i < plot->graphCount(); ++i) if (plot->graph(i)->name() == "原始精度") preciseGraph = plot->graph(i);
        VERIFY(preciseGraph && preciseGraph->data()->size() == 4 && preciseGraph->data()->constBegin()->value == 12345678901234.0);
        const auto brokenPath = dir.filePath("broken_ui.csv"); QFile broken(brokenPath);
        VERIFY(broken.open(QIODevice::WriteOnly)); broken.write("HostReceiveTime(s;FS=250),SampleIndex,A,B\n1700000000,0,1,2\n1700000000\n"); broken.close();
        VERIFY(fileAction(brokenPath, "onImportData") && preciseGraph->data()->size() == 4 && preciseGraph->name() == "原始精度");
        const auto cancelPath = dir.filePath("cancel_ui.csv"); QFile cancelFile(cancelPath); VERIFY(cancelFile.open(QIODevice::WriteOnly));
        QByteArray many("HostReceiveTime(s;FS=250),SampleIndex,A,B\n");
        for (int i = 0; i < 200000; ++i) many += "1700000000,0,1,2\n";
        VERIFY(cancelFile.write(many) == many.size()); cancelFile.close();
        cancelNextImport = true; VERIFY(fileAction(cancelPath, "onImportData") && importCancelled);
        VERIFY(preciseGraph->data()->size() == 4 && preciseGraph->name() == "原始精度");
        // Re-entering LIVE mode must release imported raw vectors and labels.
        // Later filter changes must not restore the old file over live data.
        imported.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        auto ports = imported.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
        auto open = imported.findChild<QPushButton *>("btnOpenClose"); open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
        VERIFY(preciseGraph->name() != "原始精度");
        VERIFY(QMetaObject::invokeMethod(&imported, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({20, 30}))));
        VERIFY(QMetaObject::invokeMethod(&imported, "onPlotTimer"));
        VERIFY((preciseGraph->data()->constEnd() - 1)->value == 20);
        const double previousTime = (preciseGraph->data()->constEnd() - 1)->key;
        QDoubleSpinBox *liveFs = nullptr;
        for (auto spin : imported.findChildren<QDoubleSpinBox *>()) if (spin->suffix() == " Hz" && spin->maximum() == 100000) liveFs = spin;
        VERIFY(liveFs); liveFs->setValue(100);
        VERIFY(QMetaObject::invokeMethod(&imported, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({21, 31}))));
        VERIFY(QMetaObject::invokeMethod(&imported, "onPlotTimer"));
        VERIFY(std::abs((preciseGraph->data()->constEnd() - 1)->key - previousTime - .004) < 1e-9);
        VERIFY(QMetaObject::invokeMethod(&imported, "handleChannelsUpdated", Q_ARG(QVector<double>, QVector<double>({22, 32}))));
        VERIFY(QMetaObject::invokeMethod(&imported, "onPlotTimer"));
        VERIFY(std::abs((preciseGraph->data()->constEnd() - 1)->key - previousTime - .014) < 1e-9);
        QCheckBox *lowpass = nullptr; for (auto check : imported.findChildren<QCheckBox *>()) if (check->text().startsWith("低通")) lowpass = check;
        VERIFY(lowpass); lowpass->setChecked(true); app.processEvents();
        VERIFY((preciseGraph->data()->constEnd() - 1)->value == 22); lowpass->setChecked(false);
        // UTF-8 console characters may span any serial/TCP read boundary.
        QCheckBox *showRx = nullptr, *hexRx = nullptr;
        for (auto check : imported.findChildren<QCheckBox *>()) { if (check->text() == "显示接收数据") showRx = check; if (check->text() == "HEX 接收") hexRx = check; }
        VERIFY(showRx && hexRx); showRx->setChecked(true); hexRx->setChecked(false); QMetaObject::invokeMethod(&imported, "onClearReceive");
        const auto chinese = QString("温度正常\n").toUtf8();
        for (char byte : chinese) VERIFY(QMetaObject::invokeMethod(&imported, "handleSerialDataReceived", Q_ARG(QByteArray, QByteArray(1, byte))));
        VERIFY(until([&] { return imported.findChild<QPlainTextEdit *>("textReceive")->toPlainText().contains("温度正常"); }));
        VERIFY(!imported.findChild<QPlainTextEdit *>("textReceive")->toPlainText().contains(QChar::ReplacementCharacter));
        open->click(); VERIFY(until([&] { return imported.findChild<QComboBox *>("comboInterface")->isEnabled(); }));
        imported.close();
    }
    VERIFY(store.load(error) && store.snapshot().shared.id == variable.id && store.snapshot().ble.id == variable.id);
    // Old saved ID 3 must still restore Demo, and ID 4 must still restore BLE,
    // even though their visible rows have exchanged places.
    for (int savedId : {3, 4}) {
        advanced.setValue("InterfaceType", savedId); advanced.sync();
        MainWindow restored(nullptr, new TestSerialHandler(new ReceivePort)); restored.show();
        auto interfaces = restored.findChild<QComboBox *>("comboInterface"); auto open = restored.findChild<QPushButton *>("btnOpenClose");
        VERIFY(until([&] { return open->isEnabled(); }));
        VERIFY(interfaces->currentData().toInt() == savedId && restored.findChild<QStackedWidget *>("stackInterface")->currentIndex() == savedId);
        VERIFY(open->text().contains(savedId == 3 ? "Demo" : "BLE") && advanced.value("InterfaceType").toInt() == savedId);
        restored.close();
    }
    ProtocolSnapshot empty; VERIFY(store.commit(empty, error)); advanced.setValue("InterfaceType", 0); advanced.sync();
    auto emptyPort = new ReceivePort; auto emptyWorker = new TestSerialHandler(emptyPort);
    {
        MainWindow noProfiles(nullptr, emptyWorker); noProfiles.show();
        auto protocol = noProfiles.findChild<QComboBox *>("comboProtocol"), profiles = noProfiles.findChild<QComboBox *>("comboCustomProfile");
        auto open = noProfiles.findChild<QPushButton *>("btnOpenClose"); auto summary = noProfiles.findChild<QLabel *>("lblProtocolSummary");
        VERIFY(until([&] { return protocol->isEnabled() && open->isEnabled(); }));
        auto manage = noProfiles.findChild<QPushButton *>("btnCustomProtocols"); VERIFY(manage && manage->isHidden());
        noProfiles.findChild<QTimer *>("serialHardwareCheckTimer")->stop();
        protocol->setCurrentIndex(protocol->findData("custom")); app.processEvents(); VERIFY(profiles->count() == 0 && protocol->currentData().toString() == "custom" && summary->text().contains("没有可用"));
        VERIFY(manage->isVisible());
        bool warned = false;
        QTimer::singleShot(0, &noProfiles, [&] { if (auto dialog = noProfiles.findChild<QDialog *>("protocolMissingDialog")) { warned = dialog->windowTitle().contains("没有可用"); dialog->accept(); } });
        open->click(); VERIFY(warned && !emptyPort->isOpen());
        bool welcomeShown = false;
        QTimer::singleShot(0, &noProfiles, [&] {
            if (auto manager = noProfiles.findChild<ProtocolManagerDialog *>("protocolManagerDialog")) {
                auto page = manager->findChild<QWidget *>("protocolWelcome"); auto title = manager->findChild<QLabel *>("protocolWelcomeTitle");
                welcomeShown = page && page->isVisible() && title->text().contains("欢迎") && manager->findChild<ProtocolEditorWidget *>()->isHidden()
                    && !manager->findChild<QPushButton *>("protocolSave")->isEnabled() && manager->findChild<QPushButton *>("protocolWelcomeNew")->isEnabled();
                manager->resize(1250, 850);
                for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
                    ThemeManager::applyTheme(&app, theme); app.processEvents();
                    welcomeShown = welcomeShown && (theme == ThemeManager::DarkTheme ? title->palette().color(QPalette::WindowText).lightness() > 150 : title->palette().color(QPalette::WindowText).lightness() < 100);
                    auto description = manager->findChild<QLabel *>("protocolWelcomeText");
                    welcomeShown = welcomeShown && description->height() >= description->heightForWidth(description->width());
                    if (!artifactDirectory.isEmpty()) manager->grab().save(artifactDirectory + (theme == ThemeManager::DarkTheme ? "/protocol-welcome-dark.png" : "/protocol-welcome-light.png"));
                }
                static_cast<QDialog *>(manager)->reject();
            }
        });
        VERIFY(QMetaObject::invokeMethod(&noProfiles, "showCustomProtocols") && welcomeShown);
        protocol->setCurrentIndex(0); VERIFY(until([&] { return protocol->isEnabled(); }));
        VERIFY(manage->isHidden());
        auto ports = noProfiles.findChild<QComboBox *>("comboPorts"); ports->clear(); ports->addItem("SOFTWARE_ONLY", "SOFTWARE_ONLY");
        open->click(); VERIFY(until([&] { return open->text().contains("关闭"); }));
        // Invalid custom selection during a live Raw session retains Raw;
        // it must never leave the screen claiming an empty protocol is active.
        protocol->setCurrentIndex(protocol->findData("custom"));
        QTimer::singleShot(0, &noProfiles, [&] { if (auto dialog = noProfiles.findChild<QDialog *>("protocolMessageDialog")) dialog->accept(); });
        VERIFY(until([&] { return protocol->currentIndex() == 0; }));
        open->click(); VERIFY(until([&] { return noProfiles.findChild<QComboBox *>("comboInterface")->isEnabled(); })); noProfiles.close();
    }
    ProtocolProfile configKeyValue; VERIFY(ProtocolProfileValidator::profile(ProtocolProfileValidator::keyValueTemplate(true), configKeyValue, error));
    auto convertedConfiguration = configKeyValue.definition; convertedConfiguration.insert("recordingValueMode", "converted");
    VERIFY(ProtocolProfileValidator::profile(convertedConfiguration, configKeyValue, error));
    ProtocolSnapshot configSource; configSource.profiles = {text, binary, variable, configKeyValue}; configSource.shared.kind = "custom"; configSource.shared.id = configKeyValue.id;
    VERIFY(configUiRoundTrip(dir.path(), configSource, offscreenMessages) == 0);
    VERIFY(consoleInteractionTests(dir.path(), offscreenMessages) == 0);
    VERIFY(fileSendUiTests(dir.path(), offscreenMessages) == 0);
    VERIFY(networkUiTests(dir.path(), offscreenMessages) == 0);
    VERIFY(udpUiTests(dir.path(), offscreenMessages) == 0);
    VERIFY(cacheClearUiTests(dir.path(), offscreenMessages) == 0);
    VERIFY(plotInteractionTests() == 0);
    std::cout << "Main window custom selection, worker ACK, field labels/units, recording locks, BLE isolation, Demo, TCP, scoped visibility, restart and full config UI round-trip passed.\n"; return 0;
}
