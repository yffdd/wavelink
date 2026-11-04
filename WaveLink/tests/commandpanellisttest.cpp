#include "commandpanelwindow.h"
#include "testregistrysandbox.h"
#include "sidebarmenutesthelper.h"
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QPixmap>
#include <QSettings>
#include <QSet>
#include <QFileDialog>
#include <QInputDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QElapsedTimer>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSpinBox>
#include "commandpanelconfig.h"
#include <QThread>
#include <QMap>
#include <QTextBrowser>
#include <iostream>

#define VERIFY(c) do { if (!(c)) { std::cerr << "Command sidebar failure at " << __LINE__ << ": " #c << "\n"; return 1; } } while (0)

static QJsonObject panel(const QString &id, const QString &name, bool withCommands)
{
    QJsonArray commands;
    if (withCommands) {
        commands.append(QJsonObject{{"id", "button-id"}, {"name", "HEX命令"}, {"payload", "AA 55"},
            {"extraPayload", "01 02"}, {"isHex", true}, {"autoSend", true}, {"intervalMs", 60000},
            {"appendIndex", 5}, {"customAppendPayload", "0D 0A"}, {"btnColor", "#126789"}, {"note", "按钮备注"}, {"widgetType", 0}});
        commands.append(QJsonObject{{"id", "slider-id"}, {"name", "滑条"}, {"payload", "SET:"},
            {"extraPayload", "unused"}, {"isHex", false}, {"autoSend", false}, {"intervalMs", 321},
            {"appendIndex", 1}, {"customAppendPayload", "extra"}, {"btnColor", "#563412"}, {"note", "滑条备注"}, {"widgetType", 1},
            {"minValue", -12.5}, {"maxValue", 52.5}, {"stepValue", 0.25}, {"defaultValue", 1.75}, {"decimals", 2}, {"sliderSendMode", 1}});
        commands.append(QJsonObject{{"id", "list-slider-id"}, {"name", "列表滑条"}, {"payload", "AA"}, {"isHex", true}, {"widgetType", 1},
            {"sliderMode", 1}, {"sliderValues", QJsonArray{"00", "01 02", "FF"}}, {"defaultListIndex", 1}, {"appendIndex", 1}});
        commands.append(QJsonObject{{"id", "switch-id"}, {"name", "开关"}, {"widgetType", 2}, {"isHex", true}, {"appendIndex", 1}, {"note", "开关备注"},
            {"switchDefaultState", 1}, {"switchStates", QJsonArray{
                QJsonObject{{"payload", "AA00"}, {"annotation", "关闭状态名称"}, {"color", ""}},
                QJsonObject{{"payload", "AA01"}, {"annotation", "开启状态名称"}, {"color", "#227755"}}}}});
    }
    return QJsonObject{{"id", id}, {"name", name}, {"remark", "完整面板备注"}, {"columns", 2},
        {"buttonWidth", 180}, {"buttonHeight", 70}, {"commands", commands}};
}
static bool inspectCommandHelp(CommandPanelWindow &window, bool dark)
{
    QPushButton *help = nullptr;
    for (auto button : window.findChildren<QPushButton *>()) if (button->text() == "使用说明") help = button;
    if (!help) return false;
    bool checked = false;
    QTimer::singleShot(0, &window, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto body = dialog->findChild<QTextBrowser *>();
        const auto text = body ? body->toPlainText() : QString();
        checked = dialog->windowTitle() == "命令交互面板帮助" && body &&
            text.contains("左下角“导出所有”") && text.contains("左下角“导入面板”") &&
            text.contains("立即发送一次") && text.contains("AA 32 30") &&
            text.contains("不受主界面接收框") && text.contains("一键修复") &&
            text.contains("不包含设备连接参数") && !text.contains("左侧“导出”") &&
            !dialog->windowFlags().testFlag(Qt::WindowContextHelpButtonHint) &&
            dialog->windowFlags().testFlag(Qt::WindowMinimizeButtonHint) &&
            dialog->windowFlags().testFlag(Qt::WindowMaximizeButtonHint);
        dialog->resize(540, 400); qApp->processEvents();
        checked = checked && body->horizontalScrollBar()->maximum() == 0;
        const auto artifacts = QDir::current().filePath("artifacts"); QDir().mkpath(artifacts);
        dialog->grab().save(artifacts + QString("/command-help-%1-%2.png").arg(dark).arg(dialog->devicePixelRatioF()));
        dialog->reject();
    });
    help->click();
    return checked;
}
static PanelContentWidget *content(CommandPanelWindow &window, const QString &name)
{
    for (auto widget : window.findChildren<PanelContentWidget *>()) if (widget->getDef().name == name) return widget;
    return nullptr;
}
static bool sameCommand(CommandDef a, CommandDef b)
{
    if (a.switchDefaultState != b.switchDefaultState) return false;
    for (int i = 0; i < 2; ++i) if (a.switchStates[i].payload != b.switchStates[i].payload ||
        a.switchStates[i].annotation != b.switchStates[i].annotation || a.switchStates[i].color != b.switchStates[i].color) return false;
    return a.name == b.name && a.payload == b.payload && a.extraPayload == b.extraPayload && a.isHex == b.isHex &&
        a.intervalMs == b.intervalMs && a.appendIndex == b.appendIndex && a.customAppendPayload == b.customAppendPayload &&
        a.btnColor == b.btnColor && a.note == b.note && a.widgetType == b.widgetType && a.minValue == b.minValue &&
        a.maxValue == b.maxValue && a.stepValue == b.stepValue && a.defaultValue == b.defaultValue &&
        a.decimals == b.decimals && a.sliderSendMode == b.sliderSendMode && a.sliderMode == b.sliderMode &&
        a.sliderValues == b.sliderValues && a.defaultListIndex == b.defaultListIndex;
}
static bool inspectListMenu(CommandPanelWindow &window, QListWidget *list, const QPoint &pos,
                            bool hasTarget, int row, const QString &screenshot = {})
{
    bool checked = false; QTimer driver; driver.setSingleShot(true);
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        auto menu = window.findChild<QMenu *>("commandPanelListMenu"); if (!menu) return;
        const QMap<QString, bool> expected{{"commandPanelMenuAdd", true}, {"commandPanelMenuImport", true},
            {"commandPanelMenuSettings", hasTarget}, {"commandPanelMenuRename", hasTarget}, {"commandPanelMenuCopy", hasTarget}, {"commandPanelMenuDelete", hasTarget},
            {"commandPanelMenuRemark", hasTarget}, {"commandPanelMenuExportCurrent", hasTarget},
            {"commandPanelMenuExportAll", list->count() > 0}, {"commandPanelMenuUp", hasTarget && row > 0},
            {"commandPanelMenuDown", hasTarget && row + 1 < list->count()}};
        checked = true;
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            const auto action = menu->findChild<QAction *>(it.key());
            checked = checked && action && action->isEnabled() == it.value();
        }
        if (!screenshot.isEmpty()) checked = menu->grab().save(screenshot) && checked;
        menu->close();
    });
    driver.start(0);
    return QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, pos)) && checked;
}
static bool exportFromMenu(CommandPanelWindow &window, QListWidget *list, const QPoint &pos,
                           const char *action, const QString &path)
{
    bool picked = false; QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        if (auto picker = window.findChild<QFileDialog *>()) if (picker->isVisible() && !picked) {
            picked = true; picker->setDirectory(QFileInfo(path).absolutePath()); picker->selectFile(QFileInfo(path).fileName());
            QMetaObject::invokeMethod(picker, "accept");
        }
    });
    driver.start(5);
    const bool chosen = chooseSidebarActionAt(window, list, pos, "commandPanelListMenu", action);
    driver.stop(); return chosen && picked && QFile::exists(path);
}
static PanelContentWidget *selectedContent(CommandPanelWindow &window)
{
    for (auto panel : window.findChildren<PanelContentWidget *>())
        if (auto stack = qobject_cast<QStackedWidget *>(panel->parentWidget()))
            if (stack->indexOf(panel) >= 0 && stack->currentWidget() == panel) return panel;
    return nullptr;
}
static bool selected(CommandPanelWindow &window, const QString &id, int row)
{
    auto list = window.findChild<QListWidget *>("commandPanelList");
    auto panel = selectedContent(window);
    return list && list->currentRow() == row && list->currentItem() &&
        list->currentItem()->data(Qt::UserRole).toString() == id && panel && panel->getDef().id == id;
}
static bool removeSelected(CommandPanelWindow &window)
{
    // Keep the real deletion decision but skip Qt 5.14's incompatible native
    // QMessageBox Show handler on the offscreen test platform.
    class Confirmation : public QObject {
    public: bool shown = false;
    protected: bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) if (auto box = qobject_cast<QMessageBox *>(object)) {
            shown = true; QTimer::singleShot(0, box, [box] { box->button(QMessageBox::Yes)->click(); }); return true;
        }
        return false;
    }
    } confirmation;
    qApp->installEventFilter(&confirmation);
    const bool invoked = QMetaObject::invokeMethod(&window, "onRemovePanel", Qt::DirectConnection);
    qApp->removeEventFilter(&confirmation);
    return invoked && confirmation.shown;
}
int main(int argc, char **argv)
{
    TestRegistrySandbox registry; VERIFY(registry.ready()); QApplication app(argc, argv);
    app.setAttribute(Qt::AA_DontUseNativeDialogs);
    const int fontId = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (fontId >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).value(0), 9));
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName("WaveLinkTests"); app.setApplicationName("CommandPanelListTests");
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    VERIFY(QDir().mkpath(basePath));
    QTemporaryDir temporary(basePath + "/sidebar-XXXXXX"); VERIFY(temporary.isValid());
    // Nest AppData under a unique, automatically cleaned test directory. No
    // user's command configuration or application settings are read/written.
    app.setApplicationName("CommandPanelListTests/" + QFileInfo(temporary.path()).fileName());
    VERIFY(QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)) == QDir::cleanPath(temporary.path()));
    const QString defaultFile = temporary.filePath("cmd_panels.json");
    const QJsonArray fixture{panel("original-id", "测试", true), panel("collision-id", "测试_1", false), panel("last-id", "末尾", false)};
    QSettings settings("WaveLink", "WaveLink");
    const QString selectionKey = "CommandPanel/lastPanelId";
    QString copiedSelection;
    int sends = 0;
    {
        class Messages : public QObject {
        protected: bool eventFilter(QObject *object, QEvent *event) override {
            if (event->type() == QEvent::Show) if (auto box = qobject_cast<QMessageBox *>(object)) {
                QTimer::singleShot(0, box, &QDialog::accept); return true;
            }
            return false;
        }
        } messages;
        qApp->installEventFilter(&messages);
        CommandPanelWindow menus; menus.resize(900, 700); menus.show();
        VERIFY(menus.loadConfigFromString(QJsonDocument(fixture).toJson()));
        auto list = menus.findChild<QListWidget *>("commandPanelList"); VERIFY(list && list->count() == 3);
        list->setCurrentRow(2); QApplication::processEvents();
        const QPoint blank(8, list->viewport()->height() - 8); VERIFY(!list->itemAt(blank));
        for (bool dark : {false, true}) {
            menus.applyTheme(dark); QApplication::processEvents();
            VERIFY(inspectCommandHelp(menus, dark));
            QString image;
            if (argc > 1) { VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); image = QString::fromLocal8Bit(argv[1]) + (dark ? "/command-list-blank-dark.png" : "/command-list-blank-light.png"); }
            VERIFY(inspectListMenu(menus, list, blank, false, -1, image) && selected(menus, "last-id", 2));
        }
        const auto allPath = temporary.filePath("all-panels.json");
        VERIFY(exportFromMenu(menus, list, blank, "commandPanelMenuExportAll", allPath));
        QFile all(allPath); VERIFY(all.open(QIODevice::ReadOnly)); const auto allData = QJsonDocument::fromJson(all.readAll()).array(); all.close();
        VERIFY(allData.size() == 3 && allData[0].toObject()["commands"].toArray().size() == 4 && selected(menus, "last-id", 2));
        const auto switchData = allData[0].toObject()["commands"].toArray()[3].toObject();
        VERIFY(switchData["switchDefaultState"].toInt() == 1 && switchData["switchStates"].toArray()[1].toObject()["annotation"].toString() == "开启状态名称");
        // Item menus target the right-clicked panel, not the previous selection.
        const QPoint first = list->visualItemRect(list->item(0)).center();
        VERIFY(inspectListMenu(menus, list, first, true, 0) && selected(menus, "original-id", 0));
        const auto onePath = temporary.filePath("one-panel.json");
        VERIFY(exportFromMenu(menus, list, first, "commandPanelMenuExportCurrent", onePath));
        QFile one(onePath); VERIFY(one.open(QIODevice::ReadOnly)); const auto oneData = QJsonDocument::fromJson(one.readAll()).array(); one.close();
        VERIFY(oneData.size() == 1 && oneData[0].toObject()["id"].toString() == "original-id" && oneData[0].toObject()["commands"].toArray() == allData[0].toObject()["commands"].toArray());
        VERIFY(inspectListMenu(menus, list, list->visualItemRect(list->item(2)).center(), true, 2) && selected(menus, "last-id", 2));
        VERIFY(menus.loadConfigFromString("[]")); QApplication::processEvents();
        VERIFY(inspectListMenu(menus, list, blank, false, -1) && list->count() == 0 && list->currentRow() == -1);
        bool added = false; QTimer nameDriver;
        QObject::connect(&nameDriver, &QTimer::timeout, &nameDriver, [&] {
            if (auto dialog = menus.findChild<QInputDialog *>()) if (dialog->isVisible() && !added) {
                added = true; dialog->setTextValue("空白处新增"); dialog->accept();
            }
        });
        nameDriver.start(5);
        VERIFY(chooseSidebarActionAt(menus, list, blank, "commandPanelListMenu", "commandPanelMenuAdd"));
        nameDriver.stop(); VERIFY(added && list->count() == 1 && list->item(0)->text() == "空白处新增");
        qApp->removeEventFilter(&messages);
    }
    settings.remove(selectionKey); settings.sync();
    {
        // First opening must honor the application theme without a manual
        // applyTheme call, including panels added/reloaded while it is light.
        app.setStyleSheet("QPushButton {background:#333333;color:#dddddd;}");
        app.setProperty("waveLinkDarkTheme", false);
        CommandPanelWindow window;
        VERIFY(window.loadConfigFromString(QJsonDocument(fixture).toJson()));
        window.show(); QApplication::processEvents();
        auto actionsMatch = [&](bool dark) {
            for (auto panel : window.findChildren<PanelContentWidget *>()) {
                auto stack = qobject_cast<QStackedWidget *>(panel->parentWidget());
                if (!stack || stack->indexOf(panel) < 0) continue; // Removed panels await deferred deletion.
                if (panel->isDarkTheme() != dark) { std::cerr << "Action panel theme: " << panel->getDef().name.toStdString() << " actual=" << panel->isDarkTheme() << " expected=" << dark << "\n"; return false; }
                for (const auto name : {"commandPanelSettings", "commandAddCommand"}) {
                    auto button = panel->findChild<QPushButton *>(name);
                    if (!button) return false;
                    button->ensurePolished();
                    if (button->palette().color(QPalette::Button) != QColor(dark ? "#333333" : "#ffffff") ||
                        button->palette().color(QPalette::ButtonText) != QColor(dark ? "#dddddd" : "#333333")) { std::cerr << "Action palette " << name << " dark=" << dark << " background=" << button->palette().color(QPalette::Button).name().toStdString() << " ink=" << button->palette().color(QPalette::ButtonText).name().toStdString() << "\n"; return false; }
                }
            }
            return true;
        };
        VERIFY(actionsMatch(false));
        for (bool dark : {true, false}) {
            app.setProperty("waveLinkDarkTheme", dark); QApplication::processEvents();
            VERIFY(actionsMatch(dark));
        }
        window.close(); app.setProperty("waveLinkDarkTheme", true); QApplication::processEvents();
        app.setProperty("waveLinkDarkTheme", false); window.show(); QApplication::processEvents();
        VERIFY(actionsMatch(false));
        VERIFY(window.loadConfigFromString(QJsonDocument(fixture).toJson()) && actionsMatch(false));
        app.setStyleSheet(QString()); app.setProperty("waveLinkDarkTheme", QVariant());
    }
    settings.remove(selectionKey); settings.sync();
    {
        PanelDef definition; definition.name = "滑条主题测试";
        CommandDef slider; slider.name = "默认滑条"; slider.payload = "SET:{value}"; slider.intervalMs = 0;
        slider.widgetType = static_cast<int>(CommandWidgetType::Slider); slider.defaultValue = 25; definition.commands << slider;
        slider.name = "浅色自定义"; slider.btnColor = "#f9e0a0"; definition.commands << slider;
        slider.name = "深色自定义"; slider.btnColor = "#1f4870"; definition.commands << slider;
        PanelContentWidget themed(definition); themed.resize(720, 520); themed.show();
        QObject::connect(&themed, &PanelContentWidget::sendRequested, &themed, [&] { ++sends; });
        auto widgets = themed.findChildren<SliderCommandWidget *>(); VERIFY(widgets.size() == 3);
        for (bool dark : {false, true, false}) {
            themed.applyTheme(dark); QCoreApplication::processEvents();
            for (int i = 0; i < widgets.size(); ++i) {
                auto widget = widgets[i]; auto name = widget->findChild<QLabel *>("sliderCommandName"); auto preview = widget->findChild<QLabel *>("sliderCommandPreview");
                auto value = widget->findChild<QDoubleSpinBox *>("sliderCommandValue");
                VERIFY(name && preview && value && value->value() == 25 && sameCommand(widget->getDef(), definition.commands[i]));
                const bool lightText = i == 0 ? dark : i == 2;
                VERIFY(lightText ? name->palette().color(QPalette::WindowText).lightness() > 200 : name->palette().color(QPalette::WindowText).lightness() < 80);
                VERIFY(lightText ? preview->palette().color(QPalette::WindowText).lightness() > 200 : preview->palette().color(QPalette::WindowText).lightness() < 80);
                VERIFY(dark ? value->palette().color(QPalette::Base).lightness() < 80 : value->palette().color(QPalette::Base).lightness() > 220);
                if (i == 0) {
                    const auto surface = widget->grab().toImage(); const double scale = surface.devicePixelRatio();
                    const auto background = surface.pixelColor(qRound(5 * scale), qRound((widget->height() - 6) * scale));
                    VERIFY(background.alpha() == 0); // Default color is now truly transparent.
                }
            }
            VERIFY(sends == 0);
            if (argc > 1) { VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); themed.grab().save(QString::fromLocal8Bit(argv[1]) + (dark ? "/slider-command-dark.png" : "/slider-command-light.png")); }
        }
        // Commands added or edited while already light must inherit the
        // current panel theme, without waiting for another theme toggle.
        slider.name = "浅色中新建"; slider.btnColor.clear(); themed.addCommand(slider); QCoreApplication::processEvents();
        auto added = themed.findChildren<SliderCommandWidget *>().last();
        VERIFY(added->findChild<QLabel *>("sliderCommandName")->palette().color(QPalette::WindowText).lightness() < 80);
        auto edited = added->getDef(); edited.name = "编辑后"; added->updateDef(edited);
        VERIFY(added->findChild<QLabel *>("sliderCommandPreview")->palette().color(QPalette::WindowText).lightness() < 80 && sends == 0);
    }
    {
        CommandPanelWindow window; window.applyTheme(false); window.show();
        QObject::connect(&window, &CommandPanelWindow::sendDataRequested, &window, [&] { ++sends; });
        VERIFY(window.loadConfigFromString(QJsonDocument(fixture).toJson(), false));
        auto list = window.findChild<QListWidget *>("commandPanelList"); VERIFY(list && list->count() == 3);
        VERIFY(selected(window, "original-id", 0)); // No history: first panel.
        const PanelDef original = content(window, "测试")->getDef();
        VERIFY(chooseSidebarAction(window, list, 0, "commandPanelListMenu", "commandPanelMenuCopy"));
        VERIFY(list->count() == 4 && list->currentRow() == 1 && list->item(1)->text() == "测试_2");
        VERIFY(list->item(2)->text() == "测试_1" && list->item(3)->text() == "末尾");
        auto copied = content(window, "测试_2"); VERIFY(copied && !copied->isDarkTheme());
        const PanelDef copy = copied->getDef();
        VERIFY(copy.id != original.id && copy.remark == original.remark && copy.columns == original.columns);
        VERIFY(copy.buttonWidth == original.buttonWidth && copy.buttonHeight == original.buttonHeight && copy.commands.size() == original.commands.size());
        for (int i = 0; i < copy.commands.size(); ++i) {
            VERIFY(sameCommand(copy.commands[i], original.commands[i]));
            VERIFY(copy.commands[i].id != original.commands[i].id && !copy.commands[i].autoSend);
        }
        for (auto timer : copied->findChildren<QTimer *>()) VERIFY(!timer->isActive());
        VERIFY(!content(window, "测试")->getDef().commands[0].autoSend && sends == 0);
        VERIFY(chooseSidebarAction(window, list, 0, "commandPanelListMenu", "commandPanelMenuCopy"));
        VERIFY(list->count() == 5 && list->item(1)->text() == "测试_3" && list->item(2)->text() == "测试_2");
        VERIFY(chooseSidebarAction(window, list, 2, "commandPanelListMenu", "commandPanelMenuCopy"));
        VERIFY(list->count() == 6 && list->currentRow() == 3 && list->item(3)->text() == "测试_2_1");
        copiedSelection = list->currentItem()->data(Qt::UserRole).toString();
        VERIFY(!copiedSelection.isEmpty() && selected(window, copiedSelection, 3));
        settings.sync(); VERIFY(settings.value(selectionKey).toString() == copiedSelection);
        QFile saved(defaultFile); VERIFY(saved.open(QIODevice::ReadOnly));
        const auto onDisk = QJsonDocument::fromJson(saved.readAll()).array(); saved.close();
        VERIFY(onDisk.size() == 6 && onDisk[3].toObject().value("name") == QJsonValue("测试_2_1"));
        VERIFY(!onDisk[0].toObject().value("commands").toArray()[0].toObject().value("autoSend").toBool());
        VERIFY(!onDisk[1].toObject().value("commands").toArray()[0].toObject().value("autoSend").toBool());
        window.forceStopAllTimers();
        if (argc > 1) { QCoreApplication::processEvents(); VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1]))); VERIFY(window.grab().save(QString::fromLocal8Bit(argv[1]) + "/command-panel-copy.png")); }
    }
    {
        CommandPanelWindow reopened;
        auto list = reopened.findChild<QListWidget *>("commandPanelList"); VERIFY(list && list->count() == 6);
        VERIFY(list->item(1)->text() == "测试_3" && list->item(3)->text() == "测试_2_1");
        VERIFY(selected(reopened, copiedSelection, 3)); // Recreate the whole dialog.
        VERIFY(content(reopened, "测试_2")->getDef().commands.size() == 4);
        reopened.forceStopAllTimers();
        // Closing/reopening the same window must not reset its selection.
        reopened.show(); list->setCurrentRow(5); reopened.close(); reopened.show();
        VERIFY(selected(reopened, "last-id", 5)); reopened.close();
    }
    {
        CommandPanelWindow restarted;
        VERIFY(selected(restarted, "last-id", 5));
        auto list = restarted.findChild<QListWidget *>("commandPanelList");
        auto active = selectedContent(restarted); active->setPanelName("重命名后"); list->currentItem()->setText("重命名后");
        restarted.show();
        VERIFY(chooseSidebarAction(restarted, list, 5, "commandPanelListMenu", "commandPanelMenuUp"));
        VERIFY(selected(restarted, "last-id", 4));
        restarted.reloadConfig(); VERIFY(selected(restarted, "last-id", 4) && list->currentItem()->text() == "重命名后");
        VERIFY(chooseSidebarAction(restarted, list, 4, "commandPanelListMenu", "commandPanelMenuDown"));
        VERIFY(selected(restarted, "last-id", 5));
        // Appending assigns new IDs, keeps the selected existing panel.
        VERIFY(restarted.loadConfigFromString(QJsonDocument(fixture).toJson(), true));
        VERIFY(list->count() == 9 && selected(restarted, "last-id", 5));
        restarted.forceStopAllTimers();
    }
    {
        CommandPanelWindow restarted;
        VERIFY(selected(restarted, "last-id", 5));
        VERIFY(removeSelected(restarted));
        VERIFY(selected(restarted, "original-id", 0)); // Deleted history: FIRST, not a neighboring row.
        auto list = restarted.findChild<QListWidget *>("commandPanelList");
        list->setCurrentRow(2); const QString other = list->currentItem()->data(Qt::UserRole).toString();
        // A replacement preserves the selected ID even with duplicate names,
        // changed order and labels; a missing ID never falls back by row/name.
        const QJsonArray replacement{panel("replacement-first", "测试_2", false), panel(other, "同名", false), panel("replacement-last", "同名", false)};
        VERIFY(restarted.loadConfigFromString(QJsonDocument(replacement).toJson()));
        VERIFY(selected(restarted, other, 1));
        settings.setValue(selectionKey, "missing-or-corrupted-id"); settings.sync();
        restarted.reloadConfig(); VERIFY(selected(restarted, "original-id", 0));
        restarted.forceStopAllTimers();
    }
    for (const QString &invalidId : {QString(), QString("not-in-this-library")}) {
        settings.setValue(selectionKey, invalidId); settings.sync();
        CommandPanelWindow fallback; VERIFY(selected(fallback, "original-id", 0));
        settings.sync(); VERIFY(settings.value(selectionKey).toString() == "original-id");
        fallback.forceStopAllTimers();
    }
    {
        CommandPanelWindow empty;
        VERIFY(empty.loadConfigFromString("[]"));
        auto list = empty.findChild<QListWidget *>("commandPanelList");
        settings.sync(); VERIFY(list->count() == 0 && list->currentRow() == -1 && !selectedContent(empty) && !settings.contains(selectionKey));
        // Legacy missing/duplicate IDs receive distinct identities; remember
        // the generated ID rather than confusing panels with the same name.
        const QJsonArray legacy{panel("", "相同名称", false), panel("", "相同名称", false), panel("duplicate", "旧面板", false), panel("duplicate", "旧面板", false)};
        VERIFY(empty.loadConfigFromString(QJsonDocument(legacy).toJson()));
        QSet<QString> ids;
        for (int i = 0; i < list->count(); ++i) { const auto id = list->item(i)->data(Qt::UserRole).toString(); VERIFY(!id.isEmpty() && !ids.contains(id)); ids.insert(id); }
        list->setCurrentRow(1); copiedSelection = list->currentItem()->data(Qt::UserRole).toString();
        VERIFY(selected(empty, copiedSelection, 1));
    }
    {
        CommandPanelWindow legacyReopened;
        VERIFY(selected(legacyReopened, copiedSelection, 1));
        // Leave no panels: subsequent openings still show the welcome page.
        VERIFY(legacyReopened.loadConfigFromString("[]"));
    }
    {
        CommandPanelWindow emptyReopened;
        VERIFY(emptyReopened.findChild<QListWidget *>("commandPanelList")->count() == 0 && !selectedContent(emptyReopened));
    }
    {
        CommandPanelWindow window;window.show();VERIFY(window.loadConfigFromString(QJsonDocument(QJsonArray{panel("polish","面板",true)}).toJson()));
        auto current=selectedContent(window);VERIFY(current);auto settingsButton=current->findChild<QPushButton*>("commandPanelSettings");VERIFY(settingsButton&&!settingsButton->toolTip().isEmpty());VERIFY(!current->findChild<QPushButton*>("commandAddCommand")->toolTip().isEmpty());
        bool checked=false;QTimer::singleShot(0,&window,[&]{auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;checked=(dialog->windowFlags()&Qt::WindowMinMaxButtonsHint)==Qt::WindowMinMaxButtonsHint&&!(dialog->windowFlags()&Qt::WindowContextHelpButtonHint);dialog->findChild<QLineEdit*>("commandPanelNameEdit")->setText("新面板名称");auto description=dialog->findChild<QTextEdit*>("commandPanelDescriptionEdit");description->clear();checked=checked&&description->placeholderText().contains("说明")&&description->placeholderText().contains("不参与命令发送");description->setPlainText("面板说明 中文 <> &");dialog->accept();});settingsButton->click();VERIFY(checked&&current->getDef().name=="新面板名称"&&current->getDef().remark=="面板说明 中文 <> &");
        auto list=window.findChild<QListWidget*>("commandPanelList");VERIFY(list->item(0)->text()=="新面板名称");
        QTimer::singleShot(0,&window,[&]{auto dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());if(dialog){dialog->setTextValue("新面板名称");dialog->accept();}});window.findChild<QPushButton*>("commandAddPanel")->click();VERIFY(list->count()==2&&list->item(1)->text()=="新面板名称_1");
        auto button=current->findChild<CommandButton*>();VERIFY(button);button->commandMenuRequested(button,current->mapToGlobal(QPoint(5,5)));auto menu=current->findChild<QMenu*>("commandItemMenu");VERIFY(menu&&!menu->actions().isEmpty()&&menu->actions().first()->text()=="发送一次"&&menu->findChild<QAction*>("commandMenuEdit"));menu->close();
        VERIFY(window.findChild<QPushButton*>("commandExportAll")->text()=="导出所有");
        list->setCurrentRow(1);bool configured=false;QTimer settingsDriver;
        QObject::connect(&settingsDriver,&QTimer::timeout,[&]{auto dialog=window.findChild<QDialog*>("commandPanelSettingsDialog");if(!dialog||!dialog->isVisible()||configured)return;configured=dialog->findChild<QLineEdit*>("commandPanelNameEdit")->text()=="新面板名称";dialog->findChild<QLineEdit*>("commandPanelNameEdit")->setText("右键设置面板");dialog->findChild<QTextEdit*>("commandPanelDescriptionEdit")->setPlainText("右键菜单编辑说明");dialog->accept();});settingsDriver.start(5);
        VERIFY(chooseSidebarActionAt(window,list,list->visualItemRect(list->item(0)).center(),"commandPanelListMenu","commandPanelMenuSettings"));settingsDriver.stop();VERIFY(configured&&current->getDef().name=="右键设置面板"&&current->getDef().remark=="右键菜单编辑说明");VERIFY(list->item(0)->text()=="右键设置面板"&&list->item(1)->text()=="新面板名称_1");
    }
    {
        CommandPanelWindow window;VERIFY(window.width()==950);auto def=panel("default-width","默认三列",false);def["columns"]=3;def["buttonWidth"]=200;def["buttonHeight"]=60;QJsonArray commands;
        for(int i=0;i<3;++i)commands.append(QJsonObject{{"id",QString("width-command-%1").arg(i)},{"name",QString("命令%1").arg(i+1)},{"payload","TEST"}});def["commands"]=commands;VERIFY(window.loadConfigFromString(QJsonDocument(QJsonArray{def}).toJson()));window.show();
        QElapsedTimer settle;settle.start();while(settle.elapsed()<300){QApplication::processEvents();QThread::msleep(2);}
        auto current=selectedContent(window);VERIFY(current);auto buttons=current->findChildren<CommandButton*>();auto scroll=current->findChild<QScrollArea*>("commandPanelScroll");VERIFY(buttons.size()==3&&scroll);
        const int row=buttons[0]->y();for(auto button:buttons){VERIFY(button->width()==200&&button->y()==row);VERIFY(button->geometry().right()<scroll->viewport()->width());}
    }
    {
        // A custom layout can omit the remaining columns of a row and still
        // reserve a separate full-width row for a command slider.
        auto definition = panel("custom-rows", "自定义命令", true);
        auto commands = definition["commands"].toArray();
        const QJsonValue slider = commands.at(1);
        commands = QJsonArray{};
        for (int i = 0; i < 8; ++i)
            commands.append(QJsonObject{{"id", QString::number(i)}, {"name", QString("命令 %1").arg(i)}, {"payload", "TEST"}});
        commands.append(slider);
        definition["commands"] = commands;
        definition["columns"] = 3;
        const PanelLayout::Rows initial{{"0"}, {"1", "2", "3"}, {"4", "5"}, {"6", "7"}, {"slider-id"}};
        PanelLayout::write(definition, initial);

        // Exercise the styled scroll area's deferred child visibility.
        const auto originalStyle = app.styleSheet();
        app.setStyleSheet("QDialog { background-color: #1e1e1e; }"
                          "QWidget { color: #dddddd; }"
                          "QScrollArea { background-color: #1e1e1e; border: none; }"
                          "QScrollArea > QWidget > QWidget { background-color: #1e1e1e; }"
                          "QPushButton { background-color: #333333; color: #ffffff; border: 1px solid #555555; padding: 5px; }");
        CommandPanelWindow window;
        window.resize(1000, 700);
        window.show();
        int layoutSends = 0;
        QObject::connect(&window, &CommandPanelWindow::sendDataRequested, [&] { ++layoutSends; });
        VERIFY(window.loadConfigFromString(QJsonDocument(QJsonArray{definition}).toJson()));
        auto current = selectedContent(window);
        VERIFY(current && current->getDef().layoutRows == initial);
        auto edit = current->findChild<QPushButton *>("commandEditLayout");
        VERIFY(edit && edit->isVisible());
        bool editorChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->resize(1800, 1200);
            QCoreApplication::processEvents();
            const auto content = dialog->findChild<QWidget *>("panelLayoutContent");
            const auto mode = dialog->findChild<QComboBox *>("panelLayoutMode");
            const auto modeLabel = dialog->findChild<QLabel *>("panelLayoutModeLabel");
            const auto columnsLabel = dialog->findChild<QLabel *>("panelLayoutColumnsLabel");
            const auto hint = dialog->findChild<QLabel *>("panelLayoutHint");
            const auto rowLabel = dialog->findChild<QLabel *>("panelLayoutRowLabel");
            const auto columns = dialog->findChild<QSpinBox *>("panelLayoutColumns");
            const auto frames = dialog->findChildren<QFrame *>("panelLayoutRowFrame");
            const auto rows = dialog->findChildren<QListWidget *>("panelLayoutRow");
            const auto down = dialog->findChildren<QPushButton *>("panelLayoutRowDown");
            editorChecked = dialog->objectName() == "commandLayoutEditor" && rows.size() == 5 &&
                            rows[1]->count() == 3 && rows[4]->item(0)->text().contains("滑条") &&
                            rows[4]->item(0)->sizeHint().width() > rows[1]->item(0)->sizeHint().width() &&
                            !down.last()->isEnabled() &&
                            content && content->width() <= 1160 && content->x() > 16 &&
                            mode && mode->font().bold() && mode->font().pointSizeF() >= 13 &&
                            modeLabel && modeLabel->font().pointSizeF() == mode->font().pointSizeF() &&
                            columnsLabel && columnsLabel->font().pointSizeF() == mode->font().pointSizeF() &&
                            hint && hint->font().pointSizeF() >= 12 &&
                            rowLabel && rowLabel->font().pointSizeF() == hint->font().pointSizeF() &&
                            columns && columns->value() == 3 && columns->maximum() == 10 &&
                            frames.size() == 5 && frames.first()->height() < 150 &&
                            frames.last()->y() + frames.last()->height() < 720 &&
                            rows.first()->font().pointSizeF() >= 12;
            if (!editorChecked) {
                std::cerr << "Layout editor geometry: rows=" << rows.size() << " controls=" << down.size();
                if (content) std::cerr << " content=" << content->x() << "/" << content->width();
                if (mode) std::cerr << " modeFont=" << mode->font().pointSizeF() << "/" << mode->font().bold();
                if (columns) std::cerr << " columns=" << columns->value() << "/" << columns->maximum();
                if (!frames.isEmpty()) std::cerr << " firstHeight=" << frames.first()->height() << " lastBottom="
                                                << frames.last()->geometry().bottom();
                if (!rows.isEmpty()) std::cerr << " rowFont=" << rows.first()->font().pointSizeF();
                if (rows.size() == 5 && rows[1]->count() && rows[4]->count())
                    std::cerr << " widths=" << rows[1]->item(0)->sizeHint().width() << "/" << rows[4]->item(0)->sizeHint().width();
                std::cerr << "\n";
            }
            if (editorChecked) {
                const auto previewShowsAll = [dialog](int expectedRows) {
                    QCoreApplication::processEvents();
                    auto preview = dialog->findChild<QScrollArea *>();
                    if (!preview) return false;
                    int visibleRows = 0;
                    for (auto row : preview->widget()->findChildren<QFrame *>("panelLayoutRowFrame")) {
                        if (!row->isVisible()) continue;
                        ++visibleRows;
                        const int bottom = row->mapTo(preview->viewport(), QPoint(0, row->height())).y();
                        auto label = row->findChild<QLabel *>("panelLayoutRowLabel");
                        if (bottom > preview->viewport()->height() || !label || label->font().pointSizeF() < 12)
                            return false;
                    }
                    return visibleRows == expectedRows;
                };

                const int rowHeight = frames.first()->sizeHint().height();
                down.first()->click();
                columns->setValue(4);
                const bool customAfterChange = previewShowsAll(5);
                mode->setCurrentIndex(0);
                const bool autoAfterSwitch = previewShowsAll(3);
                columns->setValue(3);
                const bool autoAfterChange = previewShowsAll(4);
                dialog->resize(850, 470);
                QCoreApplication::processEvents();
                auto preview = dialog->findChild<QScrollArea *>();
                const bool canScroll = preview && preview->viewport()->height() > rowHeight &&
                                       preview->verticalScrollBar()->maximum() > 0;
                dialog->resize(1800, 1200);
                columns->setValue(4);
                mode->setCurrentIndex(1);
                const bool customAfterSwitch = previewShowsAll(5);
                editorChecked = customAfterChange && autoAfterSwitch && autoAfterChange && canScroll && customAfterSwitch;
                if (!editorChecked) std::cerr << "Layout preview clips rows after changing columns or mode\n";
            }
            if (editorChecked) dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            else dialog->reject();
        });
        edit->click();
        auto expected = initial;
        expected.swapItemsAt(0, 1);
        VERIFY(editorChecked && layoutSends == 0 && current->getDef().layoutRows == expected && current->getDef().columns == 4);
        bool settingsSynced = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto columns = dialog->findChild<QSpinBox *>("commandPanelColumns");
            settingsSynced = columns && columns->value() == 4;
            dialog->reject();
        });
        current->openPanelSettings();
        VERIFY(settingsSynced);
        window.reloadConfig();
        QCoreApplication::processEvents();
        current = selectedContent(window);
        VERIFY(current && current->getDef().layoutRows == expected && current->getDef().columns == 4);
        QHash<QString, CommandButton *> widgets;
        for (auto button : current->findChildren<CommandButton *>()) widgets.insert(button->getDef().id, button);
        VERIFY(widgets.size() == 9 && widgets["1"]->y() == widgets["3"]->y() && widgets["0"]->y() > widgets["3"]->y());
        VERIFY(widgets["slider-id"]->y() > widgets["7"]->y() && widgets["slider-id"]->width() > 3 * widgets["0"]->width());
        window.resize(680, 600);
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 200) { QCoreApplication::processEvents(); QThread::msleep(2); }
        auto scroll = current->findChild<QScrollArea *>("commandPanelScroll");
        VERIFY(scroll->horizontalScrollBar()->maximum() > 0 && widgets["1"]->y() == widgets["3"]->y());
        const auto list = window.findChild<QListWidget *>("commandPanelList");
        VERIFY(chooseSidebarActionAt(window, list, list->visualItemRect(list->item(0)).center(),
                                     "commandPanelListMenu", "commandPanelMenuCopy"));
        const auto copy = selectedContent(window)->getDef();
        VERIFY(copy.layoutRows && copy.layoutRows->first().size() == 3 && copy.layoutRows->first().first() == copy.commands[1].id);
        VERIFY(copy.layoutRows->last().first() == copy.commands.last().id && copy.commands.last().id != "slider-id");

        QJsonArray decoded;
        QString error;
        QStringList notices;
        definition["layoutRows"] = QJsonArray{QJsonArray{"0", "slider-id"}};
        VERIFY(!CommandPanelConfig::decode(QJsonDocument(QJsonArray{definition}).toJson(), decoded, error, notices));
        VERIFY(layoutSends == 0);
        app.setStyleSheet(originalStyle);
    }
    VERIFY(sends == 0);
    std::cout << "Command panel blank/item menus, all/current exports, copying, themes, full contents, safe timers, remembered IDs and first-panel fallback passed.\n";
    return 0;
}
