#include "commandpanelwindow.h"
#include "commandswitchconfig.h"
#include "ProtocolConfigTransfer.h"
#include "testregistrysandbox.h"
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QFontDatabase>
#include <QFileDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QClipboard>
#include <QKeyEvent>
#include <QGroupBox>
#include <QHelpEvent>
#include <QToolTip>
#include <QTextDocument>
#include <QScreen>
#include <iostream>
#define VERIFY(c) do { if (!(c)) { std::cerr << "Switch failure at " << __LINE__ << ": " #c "\n"; return 1; } } while (0)

class Messages : public QObject {
public: QString last;
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) if (auto box = qobject_cast<QMessageBox *>(object)) {
            last = box->text(); QTimer::singleShot(0, box, &QDialog::accept); return true;
        }
        return false;
    }
};
static void settle() { for (int i = 0; i < 10; ++i) QApplication::processEvents(); }
static PanelContentWidget *panelOf(CommandPanelWindow &window) {
    for (auto panel : window.findChildren<PanelContentWidget *>())
        if (auto stack = qobject_cast<QStackedWidget *>(panel->parentWidget())) if (stack->indexOf(panel) >= 0) return panel;
    return nullptr;
}
static QByteArray readFile(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
static qreal thumbCenterX(QWidget *indicator) {
    const auto image = indicator->grab().toImage(); const qreal ratio = image.devicePixelRatio();
    qreal sum = 0; int count = 0;
    // Sample inside the track only, away from the parent background. A white
    // round thumb must really move, not merely change a stored state property.
    for (int x = 4; x < indicator->width() - 4; ++x) {
        const auto color = image.pixelColor(qRound(x * ratio), qRound(indicator->height() * ratio / 2));
        if (color.red() > 240 && color.green() > 240 && color.blue() > 240) { sum += x; ++count; }
    }
    return count ? sum / count : -1;
}
static QJsonArray library(const QJsonObject &command) {
    return {QJsonObject{{"id", "switch-panel"}, {"name", "开关测试"}, {"commands", QJsonArray{command}}}};
}
static int descriptionTooltipTests(int argc, char **argv) {
    // Exercise a real Qt tooltip, including painted text contrast. Merely
    // checking the stylesheet string would not catch the reported black box.
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
    PanelDef definition; definition.name = "命令说明";
    const QString note = "启动采集 <mode=1> & 保留原文\n第二行说明，不参与发送";
    for (int type = 0; type < 3; ++type) {
        CommandDef command; command.widgetType = type; command.name = QString("命令 %1").arg(type); command.note = note;
        command.payload = "DO NOT SEND"; command.intervalMs = 0;
        command.switchStates = {{{"OFF", "关闭", ""}, {"ON", "开启", "#eeeecc"}}};
        definition.commands.append(command);
    }
    PanelContentWidget panel(definition); panel.resize(800, 450); panel.show(); settle();
    int sent = 0;
    QObject::connect(&panel, &PanelContentWidget::sendRequested, [&](const QByteArray &, bool) { ++sent; });
    panel.setSwitchSendHandler([&](const QByteArray &) { ++sent; return true; });
    const auto commands = panel.findChildren<CommandButton *>(); VERIFY(commands.size() == 3);
    for (bool dark : {false, true}) {
        panel.applyTheme(dark); settle();
        for (auto command : commands) {
            auto plain = [](const QString &html) { QTextDocument document; document.setHtml(html); return document.toPlainText(); };
            VERIFY(plain(command->toolTip()) == note);
            if (auto slider = qobject_cast<SliderCommandWidget *>(command)) {
                VERIFY(plain(slider->findChild<QDoubleSpinBox *>("sliderCommandValue")->toolTip()) == note);
                VERIFY(plain(slider->findChild<QLabel *>("sliderCommandPreview")->toolTip()) == note);
            }
            const auto point = command->rect().center();
            QHelpEvent hover(QEvent::ToolTip, point, command->mapToGlobal(point));
            QApplication::sendEvent(command, &hover); settle();
            VERIFY(QToolTip::isVisible() && plain(QToolTip::text()) == note);
            QWidget *tip = nullptr;
            for (auto top : QApplication::topLevelWidgets()) if (top->windowType() == Qt::ToolTip && top->isVisible()) tip = top;
            VERIFY(tip);
            const auto pixels = tip->grab().toImage(); const qreal ratio = pixels.devicePixelRatio();
            int ink = 0;
            for (int y = qRound(8 * ratio); y < pixels.height() - qRound(8 * ratio); ++y)
                for (int x = qRound(8 * ratio); x < pixels.width() - qRound(8 * ratio); ++x) {
                    const auto color = pixels.pixelColor(x, y);
                    if (dark ? color.lightness() > 200 : color.lightness() < 80) ++ink;
                }
            VERIFY(ink > 50);
            if (argc > 1) VERIFY(tip->grab().save(QString::fromLocal8Bit(argv[1]) + QString("/command-description-%1-%2.png").arg(command->getDef().widgetType).arg(dark ? "dark" : "light")));
            QToolTip::hideText();
            auto changed = command->getDef(); changed.note = "\n  \t"; command->updateDef(changed);
            VERIFY(plain(command->toolTip()) == "暂无说明");
            changed.note = "修改后的说明"; command->updateDef(changed); VERIFY(plain(command->toolTip()) == changed.note);
            changed.note = note; command->updateDef(changed);
            QStringList manyLines;
            for (int i = 0; i < 500; ++i) manyLines.append(QString("说明第%1行：这一行保留完整的中文、数字123、<标记> & 符号。").arg(i));
            const QString moderate = QString("完整说明：用途及操作步骤。\n").repeated(8) + "完整内容结束";
            const QStringList longCases{moderate, manyLines.join('\n'), QString("0123456789ABCDEF").repeated(10000), QString::fromUtf8("状态 😀 信息 ").repeated(10000)};
            for (int sample = 0; sample < longCases.size(); ++sample) {
                changed.note = longCases[sample]; command->updateDef(changed);
                QWidget *target = command;
                // Hovering the slider value box or its editor follows the same
                // bound, rather than showing an unbounded child tooltip.
                if (auto slider = qobject_cast<SliderCommandWidget *>(command))
                    target = slider->findChild<QDoubleSpinBox *>("sliderCommandValue")->findChild<QLineEdit *>();
                VERIFY(target);
                const auto center = target->rect().center(), global = target->mapToGlobal(center);
                QHelpEvent longHover(QEvent::ToolTip, center, global); QApplication::sendEvent(target, &longHover); settle();
                VERIFY(QToolTip::isVisible());
                const auto preview = plain(QToolTip::text());
                VERIFY(command->getDef().note == longCases[sample]);
                if (sample == 0) VERIFY(preview.endsWith("完整内容结束") && !preview.contains("右键编辑"));
                else VERIFY(preview.contains("右键编辑命令可查看完整说明") && preview.size() < 32768);
                QWidget *longTip = nullptr;
                for (auto top : QApplication::topLevelWidgets()) if (top->windowType() == Qt::ToolTip && top->isVisible()) longTip = top;
                auto screen = QGuiApplication::screenAt(global); if (!screen) screen = QGuiApplication::primaryScreen();
                const auto available = screen->availableGeometry().size();
                if (longTip && (longTip->width() > qMin(640, available.width() * 3 / 5) + 8 || longTip->height() > qMin(600, available.height() * 7 / 10) + 8))
                    std::cerr << "Long tooltip size " << longTip->width() << "x" << longTip->height() << " available=" << available.width() << "x" << available.height() << '\n';
                VERIFY(longTip && longTip->width() <= qMin(640, available.width() * 3 / 5) + 8 && longTip->height() <= qMin(600, available.height() * 7 / 10) + 8);
                if (argc > 1 && command->getDef().widgetType == 2 && sample == 1)
                    VERIFY(longTip->grab().save(QString::fromLocal8Bit(argv[1]) + QString("/command-description-long-%1.png").arg(dark ? "dark" : "light")));
                QToolTip::hideText();
            }
            changed.note = note; command->updateDef(changed);
        }
        VERIFY(sent == 0);
    }
    CommandEditDialog editor(definition.commands[0]);
    bool named = false;
    for (auto label : editor.findChildren<QLabel *>()) { VERIFY(label->text() != "备注:"); named |= label->text() == "说明:"; }
    VERIFY(named && editor.findChild<QTextEdit *>("commandDescription")->toPlainText() == note && editor.getDef().note == note);
    return 0;
}
int main(int argc, char **argv) {
    TestRegistrySandbox registry; VERIFY(registry.ready()); QApplication app(argc, argv);
    app.setAttribute(Qt::AA_DontUseNativeDialogs);
    Messages messages; app.installEventFilter(&messages);
    QStandardPaths::setTestModeEnabled(true); app.setOrganizationName("WaveLinkTests"); app.setApplicationName("SwitchTests");
    const auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); VERIFY(QDir().mkpath(root));
    QTemporaryDir dir(root + "/switch-XXXXXX"); VERIFY(dir.isValid());
    app.setApplicationName("SwitchTests/" + QFileInfo(dir.path()).fileName());
    VERIFY(QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)) == QDir::cleanPath(dir.path()));
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).value(0), 9));
    if (argc > 1) VERIFY(QDir().mkpath(QString::fromLocal8Bit(argv[1])));
    VERIFY(descriptionTooltipTests(argc, argv) == 0);
    CommandDef def; def.id = "switch-1"; def.name = "设备采集"; def.widgetType = 2;
    def.switchStates = {{{"AA 00", "已关闭 / 中文", ""}, {"AA 01", "已开启 / 中文", "#227755"}}};
    def.isHex = true; def.appendIndex = 1; def.note = "备注：任意 ABC @!，不发送";
    def.autoSend = true; def.intervalMs = 1; def.extraPayload = "DO NOT SEND";
    {
        SwitchCommandWidget button(def); button.resize(200, 60); button.show(); QList<QByteArray> sent;
        button.setSendHandler([&](const QByteArray &bytes) { sent.append(bytes); return true; }); settle();
        auto indicator = button.findChild<QWidget *>("commandSwitchIndicator");
        VERIFY(indicator && indicator->isVisible() && indicator->testAttribute(Qt::WA_TransparentForMouseEvents));
        VERIFY(!indicator->property("switchOn").toBool() && thumbCenterX(indicator) > 0 && thumbCenterX(indicator) < indicator->width() / 2);
        VERIFY(sent.isEmpty() && button.currentState() == 0 && button.getDef().switchDefaultState == 0);
        VERIFY(!button.getDef().autoSend && button.getDef().intervalMs == 0 && button.getDef().extraPayload.isEmpty());
        button.click(); VERIFY(button.currentState() == 1 && sent == QList<QByteArray>{QByteArray::fromHex("AA010D0A")});
        for (bool dark : {false, true, false}) {
            button.applyTheme(dark); settle(); VERIFY(button.currentState() == 1 && sent.size() == 1);
            VERIFY(indicator->property("switchOn").toBool() && thumbCenterX(indicator) > indicator->width() / 2);
            if (argc > 1) button.grab().save(QString::fromLocal8Bit(argv[1]) + (dark ? "/command-switch-on-dark.png" : "/command-switch-on-light.png"));
        }
        button.forceStopTimer(); VERIFY(button.currentState() == 1 && sent.size() == 1);
        button.click(); VERIFY(button.currentState() == 0 && sent.last() == QByteArray::fromHex("AA000D0A"));
        button.setSendHandler([](const QByteArray &) { return false; }); button.click();
        VERIFY(button.currentState() == 0 && button.accessibleDescription().contains("未提交") && sent.size() == 2);
        VERIFY(!indicator->property("switchOn").toBool() && thumbCenterX(indicator) < indicator->width() / 2);
        button.sendDataPacket(true); VERIFY(button.currentState() == 0 && sent.size() == 2);
        int admissions = 0;
        button.setSendHandler([&](const QByteArray &) { ++admissions; button.click(); return true; });
        button.click(); VERIFY(admissions == 1 && button.currentState() == 1);
        auto changed = def; changed.switchDefaultState = 1; changed.appendIndex = 5; changed.customAppendPayload = "FE EE";
        button.updateDef(changed); VERIFY(button.currentState() == 1 && admissions == 1);
        button.setSendHandler([&](const QByteArray &bytes) { sent.append(bytes); return true; });
        button.click(); VERIFY(button.currentState() == 0 && sent.last() == QByteArray::fromHex("AA00FEEE") && button.getDef().switchDefaultState == 1);
        auto invalid = changed; invalid.switchStates[0].payload = "GG"; button.updateDef(invalid); button.click();
        VERIFY(button.currentState() == 1 && sent.size() == 3 && button.accessibleDescription().contains("HEX"));
        CommandDef noId = def; noId.id.clear(); SwitchCommandWidget fresh(noId); VERIFY(!fresh.getDef().id.isEmpty());
    }
    {
        SwitchCommandWidget button(def); button.show(); int sends = 0;
        button.setSendHandler([&](const QByteArray &) { ++sends; return true; });
        auto indicator = button.findChild<QWidget *>("commandSwitchIndicator"); VERIFY(indicator);
        // The indicator is visual only. Clicking its position is a single
        // normal command click, and keyboard Space has the same behavior.
        button.resize(200, 60); settle();
        const auto center = indicator->mapTo(&button, indicator->rect().center());
        QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&button, &press); QApplication::sendEvent(&button, &release);
        VERIFY(sends == 1 && button.currentState() == 1 && indicator->property("switchOn").toBool());
        QKeyEvent keyPress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier), keyRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(&button, &keyPress); QApplication::sendEvent(&button, &keyRelease);
        VERIFY(sends == 2 && button.currentState() == 0);
        for (const auto size : {QSize(100, 40), QSize(200, 60), QSize(400, 200)}) {
            button.resize(size); settle(); VERIFY(button.size() == size);
            VERIFY(button.rect().contains(indicator->geometry()) && button.currentState() == 0 && sends == 2);
            for (const auto label : button.findChildren<QLabel *>()) VERIFY(button.rect().contains(label->geometry()));
        }
    }
    CommandDef edited;
    {
        CommandEditDialog editor(def); editor.show(); settle();
        auto type = editor.findChild<QComboBox *>("commandWidgetType"), initial = editor.findChild<QComboBox *>("commandSwitchDefault");
        auto hex = editor.findChild<QCheckBox *>("commandHex");
        auto a = editor.findChild<QTextEdit *>("commandSwitchPayload0"), b = editor.findChild<QTextEdit *>("commandSwitchPayload1");
        auto annotation = editor.findChild<QLineEdit *>("commandSwitchAnnotation1");
        VERIFY(type && initial && hex && a && b && annotation && type->count() == 3 && type->currentIndex() == 2);
        int typeLabels = 0, stateLabels = 0;
        for (const auto label : editor.findChildren<QLabel *>()) {
            VERIFY(label->text() != "命令形式:" && label->text() != "状态注释：");
            if (label->text() == "命令类型:") ++typeLabels;
            if (label->text() == "状态名称：") ++stateLabels;
        }
        VERIFY(typeLabels == 1 && stateLabels == 2);
        VERIFY(!editor.findChild<QWidget *>("commandSwitchOptions")->isHidden() && editor.findChild<QTextEdit *>("commandPayload")->parentWidget()->isHidden());
        VERIFY(editor.findChild<QPushButton *>("commandColorPicker")->isHidden() && initial->currentIndex() == 0);
        auto switchOptions = editor.findChild<QWidget *>("commandSwitchOptions");
        for (const auto label : switchOptions->findChildren<QLabel *>()) {
            VERIFY(label->font().pointSizeF() >= 10 && label->minimumHeight() >= label->fontMetrics().height());
        }
        for (int i = 0; i < 2; ++i) VERIFY(editor.findChild<QPushButton *>(QString("commandSwitchColor%1").arg(i))->text() == "选择按钮颜色");
        const int shortHeight = switchOptions->height();
        for (auto input : {a, b}) input->setPlainText(QString("AA").repeated(600));
        settle();
        VERIFY(switchOptions->height() > shortHeight && a->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded && b->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded);
        auto firstGroup = editor.findChild<QGroupBox *>("commandSwitchState0"), secondGroup = editor.findChild<QGroupBox *>("commandSwitchState1");
        VERIFY(firstGroup && secondGroup && firstGroup->geometry().bottom() < secondGroup->y());
        a->setPlainText("aa 00\n0d\t0a"); b->setPlainText("aa01"); settle();
        VERIFY(a->toPlainText() == "AA 00 0D 0A" && b->toPlainText() == "AA 01");
        VERIFY(switchOptions->height() == shortHeight && a->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && b->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
        annotation->setText("开启，中文 + @ !"); initial->setCurrentIndex(1);
        annotation->setText("启用采集，中文 + @ !"); VERIFY(initial->currentIndex() == 1 && initial->currentText().contains(annotation->text()));
        VERIFY(!editor.findChild<QLabel *>("commandPreview")->text().contains(annotation->text()));
        // HEX restrictions are on payloads only. Clipboard restoration on all exits.
        struct ClipboardRestore { QMimeData *saved = new QMimeData;
            ClipboardRestore() { if (auto data = QApplication::clipboard()->mimeData()) for (const auto &format : data->formats()) saved->setData(format, data->data(format)); }
            ~ClipboardRestore() { QApplication::clipboard()->setMimeData(saved); }
        } restore;
        a->selectAll(); QApplication::clipboard()->setText("5a5A\n00 01");
        QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier, QString(QChar(22))); QApplication::sendEvent(a, &paste);
        VERIFY(a->toPlainText() == "5A 5A 00 01");
        a->selectAll(); QApplication::clipboard()->setText("中文 NOT HEX"); QApplication::sendEvent(a, &paste);
        VERIFY(a->toPlainText() == "5A 5A 00 01" && annotation->text() == "启用采集，中文 + @ !");
        b->setPlainText("41 42");
        a->setPlainText("5A 5A 30 31");
        hex->setChecked(false); VERIFY(!hex->isChecked() && a->toPlainText() == "ZZ01");
        hex->setChecked(true); VERIFY(a->toPlainText() == "5A 5A 30 31" && annotation->text() == "启用采集，中文 + @ !");
        a->setPlainText("FF"); hex->setChecked(false); VERIFY(hex->isChecked() && a->toPlainText() == "FF");
        a->setPlainText("A"); editor.findChild<QPushButton *>("commandSave")->click(); VERIFY(messages.last.contains("HEX") && editor.isVisible());
        a->clear(); editor.findChild<QPushButton *>("commandSave")->click(); VERIFY(messages.last.contains("不能为空") && editor.isVisible());
        a->setPlainText("AA00"); b->setPlainText("AA01");
        bool picked = false; QTimer::singleShot(0, [&] {
            if (auto color = editor.findChild<QColorDialog *>("commandColorDialog")) {
                auto transparent = color->findChild<QPushButton *>("commandColorTransparent");
                picked = transparent && transparent->text() == "使用透明按钮";
                if (transparent) transparent->click(); else color->reject();
            }
        });
        editor.findChild<QPushButton *>("commandSwitchColor1")->click(); VERIFY(picked && editor.getDef().switchStates[1].color.isEmpty());
        VERIFY(editor.findChild<QPushButton *>("commandSwitchColor1")->text() == "选择按钮颜色");
        // Ordinary/button/slider editing remains available, without stale extra data in switches.
        type->setCurrentIndex(0); VERIFY(editor.findChild<QWidget *>("commandSwitchOptions")->isHidden());
        type->setCurrentIndex(1); VERIFY(editor.getDef().widgetType == 1 && editor.getDef().extraPayload.isEmpty());
        type->setCurrentIndex(2); edited = editor.getDef();
        VERIFY(edited.widgetType == 2 && edited.switchDefaultState == 1 && edited.switchStates[0].payload == "AA 00" && edited.intervalMs == 0);
        VERIFY(edited.note == def.note && edited.switchStates[1].annotation == annotation->text());
        for (int height : {600, 900, 1200}) {
            editor.resize(760, height); settle();
            const auto defaults = editor.findChild<QWidget *>("commandSwitchDefaultRow");
            const auto first = editor.findChild<QGroupBox *>("commandSwitchState0");
            const auto second = editor.findChild<QGroupBox *>("commandSwitchState1");
            VERIFY(defaults && first && second && first->y() - defaults->geometry().bottom() <= 12
                && second->y() - first->geometry().bottom() <= 12);
            const int formatGap = hex->mapTo(&editor, QPoint()).y() - switchOptions->mapTo(&editor, QPoint(0, switchOptions->height())).y();
            if (formatGap < 0 || formatGap > 16) {
                std::cerr << "Switch form gap=" << formatGap << " height=" << height << " options=" << switchOptions->height() << " hint=" << switchOptions->sizeHint().height() << "\n";
                if (argc > 1) editor.grab().save(QString::fromLocal8Bit(argv[1]) + "/command-switch-layout-failure.png");
            }
            VERIFY(formatGap >= 0 && formatGap <= 16);
        }
        if (argc > 1) { editor.resize(760, 900); settle(); editor.grab().save(QString::fromLocal8Bit(argv[1]) + "/command-switch-editor.png"); }
        editor.findChild<QPushButton *>("commandSave")->click(); VERIFY(editor.result() == QDialog::Accepted);
    }
    {
        CommandPanelWindow window; window.resize(1000, 700); window.show();
        VERIFY(window.loadConfigFromString(QJsonDocument(library(QJsonObject{{"name", "旧按钮"}, {"payload", "HELLO"}, {"intervalMs", 0}})).toJson()));
        auto panel = panelOf(window); VERIFY(panel && panel->getDef().commands[0].widgetType == 0);
        panel->addCommand(edited); auto toggle = panel->findChild<SwitchCommandWidget *>(); VERIFY(toggle && toggle->currentState() == 1);
        int sent = 0; window.setSwitchSendHandler([&](const QByteArray &) { ++sent; return true; });
        toggle->click(); VERIFY(sent == 1 && toggle->currentState() == 0);
        window.hide(); window.show(); settle(); VERIFY(sent == 1 && toggle->currentState() == 0);
        for (bool dark : {false, true}) {
            window.applyTheme(dark); settle();
            auto ordinary = panel->findChildren<CommandButton *>(); CommandButton *button = nullptr;
            for (auto candidate : ordinary) if (candidate->getDef().widgetType == 0) button = candidate;
            VERIFY(button && toggle->size() == button->size() && toggle->width() == 200 && toggle->height() == 60 && toggle->currentState() == 0 && sent == 1);
            if (argc > 1) window.grab().save(QString::fromLocal8Bit(argv[1]) + (dark ? "/command-switch-dark.png" : "/command-switch-light.png"));
        }
        VERIFY(QMetaObject::invokeMethod(panel, "contentChanged", Qt::DirectConnection));
        const auto path = dir.filePath("cmd_panels.json"); const auto bytes = readFile(path); VERIFY(!bytes.isEmpty());
        const auto saved = QJsonDocument::fromJson(bytes).array()[0].toObject()["commands"].toArray()[1].toObject();
        VERIFY(saved["switchDefaultState"].toInt() == 1 && !saved.contains("currentState") && saved["switchStates"].toArray().size() == 2);
        QString error; VERIFY(CommandSwitchConfig::validate(saved, error));
        CommandPanelWindow restored; VERIFY(panelOf(restored)->getDef().commands[1].switchStates[1].annotation == edited.switchStates[1].annotation);
        auto restoredToggle = panelOf(restored)->findChild<SwitchCommandWidget *>(); VERIFY(restoredToggle && restoredToggle->currentState() == 1);
        int unexpected = 0; restored.setSwitchSendHandler([&](const QByteArray &) { ++unexpected; return true; }); settle(); VERIFY(unexpected == 0);
        // Bad switch imports must not partly overwrite either panels or whole-app settings.
        auto broken = saved; auto states = saved["switchStates"].toArray(); auto state = states[0].toObject(); state["payload"] = "GG"; states[0] = state; broken["switchStates"] = states;
        VERIFY(!restored.loadConfigFromString(QJsonDocument(library(broken)).toJson()) && messages.last.contains("HEX"));
        VERIFY(panelOf(restored)->getDef().commands.size() == 2 && restoredToggle->currentState() == 1 && unexpected == 0);
        DataProtocols::ProtocolProfileStore store(dir.filePath("protocols.json")); VERIFY(store.load(error));
        QSettings settings("WaveLink", "WaveLink"); settings.setValue("switchSentinel", "original"); settings.sync();
        QVariantMap global{{"CommandPanel/json", QString::fromUtf8(QJsonDocument(library(broken)).toJson())}, {"WaveLink/switchSentinel", "changed"}};
        VERIFY(!DataProtocols::ProtocolConfigTransfer::apply(global, store, -1, false, 1, path, error) && error.contains("HEX"));
        settings.sync(); VERIFY(settings.value("switchSentinel").toString() == "original" && readFile(path) == bytes);
        for (auto invalid : {QJsonValue(-1), QJsonValue(2), QJsonValue(0.5), QJsonValue("1")}) {
            broken = saved; broken["switchDefaultState"] = invalid; VERIFY(!CommandSwitchConfig::validate(broken, error));
        }
        broken = saved; broken["switchStates"] = QJsonArray{states[0]}; VERIFY(!CommandSwitchConfig::validate(broken, error));
        // Valid whole-app transfer preserves switch fields, also in append mode.
        global["CommandPanel/json"] = QString::fromUtf8(QJsonDocument(library(saved)).toJson());
        VERIFY(DataProtocols::ProtocolConfigTransfer::apply(global, store, -1, false, 1, dir.filePath("whole.json"), error));
        auto whole = QJsonDocument::fromJson(readFile(dir.filePath("whole.json"))).array();
        VERIFY(whole[0].toObject()["commands"].toArray()[0].toObject()["switchStates"] == saved["switchStates"]);
        VERIFY(DataProtocols::ProtocolConfigTransfer::apply(global, store, -1, false, 2, dir.filePath("whole.json"), error));
        VERIFY(QJsonDocument::fromJson(readFile(dir.filePath("whole.json"))).array().size() == 2);
    }
    std::cout << "Switch command states, payloads, safety, editor, HEX, themes, persistence and global config passed.\n";
    return 0;
}
