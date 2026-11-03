#include "commandpanelwindow.h"
#include "commandpanelconfig.h"
#include "ProtocolConfigTransfer.h"
#include "testregistrysandbox.h"
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QSettings>
#include <QFontDatabase>
#include <iostream>
#define VERIFY(c) do { if (!(c)) { std::cerr << "Recovery failure at " << __LINE__ << ": " #c "\n"; return 1; } } while (0)

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
static QByteArray read(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
static QJsonArray fixture() {
    QJsonObject button{{"name", "普通命令"}, {"payload", "HELLO"}, {"note", "备注保持"}, {"widgetType", 0}};
    QJsonObject missing{{"name", "旧开关"}, {"id", "switch-id"}, {"widgetType", 2}, {"isHex", true}, {"appendIndex", 1}};
    return {QJsonObject{{"id", "first"}, {"name", "已有设备"}, {"commands", QJsonArray{button}}},
            QJsonObject{{"id", "second"}, {"name", "新设备"}, {"commands", QJsonArray{missing}}}};
}
static QJsonObject lastCommand(const QByteArray &bytes) {
    return QJsonDocument::fromJson(bytes).array().last().toObject()["commands"].toArray().last().toObject();
}
static SwitchCommandWidget *toggleOf(CommandPanelWindow &window) {
    for (auto panel : window.findChildren<PanelContentWidget *>())
        if (auto toggle = panel->findChild<SwitchCommandWidget *>()) return toggle;
    return nullptr;
}
int main(int argc, char **argv) {
    // Explicit maintenance operation used after software validation. No registry
    // or UI settings are touched. The repair itself checks lock and exact bytes.
    if (argc == 3 && QByteArray(argv[1]) == "--repair-file") {
        QCoreApplication app(argc, argv); const auto path = QString::fromLocal8Bit(argv[2]);
        QByteArray bytes, repaired; QString error, report;
        if (!CommandPanelConfig::read(path, bytes, error) || !CommandPanelConfig::repairFile(path, bytes, true, repaired, report, error)) {
            std::cerr << error.toUtf8().constData() << '\n'; return 1;
        }
        std::cout << report.toUtf8().constData() << '\n'; return 0;
    }
    TestRegistrySandbox registry; VERIFY(registry.ready()); QApplication app(argc, argv);
    const int fontId = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (fontId >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).value(0), 9));
    app.setAttribute(Qt::AA_DontUseNativeDialogs);
    QStandardPaths::setTestModeEnabled(true); app.setOrganizationName("WaveLinkTests"); app.setApplicationName("RecoveryTests");
    const auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); VERIFY(QDir().mkpath(root));
    QTemporaryDir dir(root + "/recovery-XXXXXX"); VERIFY(dir.isValid());
    app.setApplicationName("RecoveryTests/" + QFileInfo(dir.path()).fileName());
    VERIFY(QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)) == QDir::cleanPath(dir.path()));
    Messages messages; app.installEventFilter(&messages);
    const auto path = dir.filePath("cmd_panels.json"); const auto original = QJsonDocument(fixture()).toJson();
    QString error; VERIFY(CommandPanelConfig::write(path, original, error));
    QSettings("WaveLink", "WaveLink").setValue("CommandPanel/lastPanelId", "second");
    QByteArray migrated;
    {
        // Exact reported failure: two-state fields completely stripped by an
        // older writer. Other panels and all commands must remain accessible.
        CommandPanelWindow window; window.show(); QApplication::processEvents();
        auto list = window.findChild<QListWidget *>("commandPanelList");
        VERIFY(list && list->count() == 2 && list->currentItem()->data(Qt::UserRole).toString() == "second");
        VERIFY(messages.last.isEmpty());
        auto toggle = toggleOf(window); VERIFY(toggle && toggle->getDef().switchNeedsRepair);
        int sent = 0; window.setSwitchSendHandler([&](const QByteArray &) { ++sent; return true; });
        toggle->click(); VERIFY(sent == 0 && toggle->currentState() == 0);
        auto status = window.findChild<QLabel *>("commandConfigStatus");
        VERIFY(status && status->text().contains("待补全"));
        migrated = read(path); VERIFY(lastCommand(migrated)["switchNeedsRepair"].toBool());
        const auto backups = QDir(dir.path()).entryList({"cmd_panels.json.before-repair-*.json"}, QDir::Files);
        VERIFY(backups.size() == 1 && read(dir.filePath(backups[0])) == original);
        for (auto panel : window.findChildren<PanelContentWidget *>()) if (panel->getDef().id == "first")
            VERIFY(panel->getDef().commands.size() == 1 && panel->getDef().commands[0].payload == "HELLO");
        for (bool dark : {false, true}) {
            window.applyTheme(dark); QApplication::processEvents();
            VERIFY(toggle->toolTip().contains("暂无说明") && toggle->accessibleDescription().contains("待补全") && status->text().contains("待补全"));
            if (argc > 1) { QDir().mkpath(QString::fromLocal8Bit(argv[1])); window.grab().save(QString::fromLocal8Bit(argv[1]) + (dark ? "/command-recovery-dark.png" : "/command-recovery-light.png")); }
        }
        // Pending configuration survives independent/global import unchanged.
        DataProtocols::ProtocolProfileStore store(dir.filePath("protocols.json")); VERIFY(store.load(error));
        QVariantMap values{{"CommandPanel/json", QString::fromUtf8(original)}};
        VERIFY(DataProtocols::ProtocolConfigTransfer::apply(values, store, -1, false, 1, dir.filePath("import.json"), error));
        VERIFY(lastCommand(read(dir.filePath("import.json")))["switchNeedsRepair"].toBool());
        CommandEditDialog editor(toggle->getDef());
        editor.findChild<QTextEdit *>("commandSwitchPayload0")->setPlainText("AA 00");
        editor.findChild<QTextEdit *>("commandSwitchPayload1")->setPlainText("AA 01");
        editor.findChild<QPushButton *>("commandSave")->click(); VERIFY(editor.result() == QDialog::Accepted);
        CommandDef repaired = editor.getDef(); VERIFY(!repaired.switchNeedsRepair);
        toggle->updateDef(repaired); toggle->click(); VERIFY(sent == 1 && toggle->currentState() == 1);
        VERIFY(QMetaObject::invokeMethod(&window, "onContentChanged", Qt::DirectConnection));
        if (lastCommand(read(path))["switchNeedsRepair"].toBool()) std::cerr << "Save status: " << messages.last.toUtf8().constData() << " / " << status->text().toUtf8().constData() << " toggle=" << toggle->getDef().switchNeedsRepair << '\n';
        VERIFY(!lastCommand(read(path))["switchNeedsRepair"].toBool());
        VERIFY(!read(path + ".bak").isEmpty());
    }
    const auto valid = read(path);
    {
        CommandPanelWindow restored; VERIFY(toggleOf(restored) && !toggleOf(restored)->getDef().switchNeedsRepair);
        VERIFY(toggleOf(restored)->currentState() == 0);
    }
    // Already complete state commands with only missing display metadata are
    // upgraded without disabling or changing the two actual payloads.
    auto legacy = fixture(); auto panel = legacy[1].toObject(); auto command = lastCommand(valid);
    command.remove("switchDefaultState");
    auto states = command["switchStates"].toArray(); auto state = states[0].toObject(); state.remove("color"); state.remove("annotation"); states[0] = state;
    command["switchStates"] = states; panel["commands"] = QJsonArray{command}; legacy[1] = panel;
    QJsonArray decoded; QStringList changes;
    VERIFY(CommandPanelConfig::decode(QJsonDocument(legacy).toJson(), decoded, error, changes));
    auto decodedCommand = decoded[1].toObject()["commands"].toArray()[0].toObject();
    VERIFY(!decodedCommand["switchNeedsRepair"].toBool() && decodedCommand["switchDefaultState"].toInt(-1) == 0);
    VERIFY(decodedCommand["switchStates"].toArray()[0].toObject()["payload"] == command["switchStates"].toArray()[0].toObject()["payload"]);
    // A malformed explicit default is not silently auto-corrected; one-click
    // repair retains payloads but quarantines the command until edited.
    command["switchDefaultState"] = 7; panel["commands"] = QJsonArray{command}; legacy[1] = panel;
    const auto invalid = QJsonDocument(legacy).toJson(); changes.clear();
    VERIFY(!CommandPanelConfig::decode(invalid, decoded, error, changes));
    VERIFY(CommandPanelConfig::write(path, invalid, error));
    {
        CommandPanelWindow window; window.show(); QApplication::processEvents();
        VERIFY(window.findChild<QListWidget *>("commandPanelList")->count() == 0);
        auto repair = window.findChild<QPushButton *>("commandConfigRepair"); VERIFY(repair && !repair->isHidden());
        repair->click(); VERIFY(toggleOf(window) && toggleOf(window)->getDef().switchNeedsRepair);
        VERIFY(window.findChild<QListWidget *>("commandPanelList")->count() == 2);
        VERIFY(toggleOf(window)->getDef().switchStates[0].payload == "AA 00");
    }
    // Broken JSON restores a valid backup, preserving the exact corrupt input.
    VERIFY(CommandPanelConfig::write(path + ".bak", valid, error));
    const QByteArray corrupt = "[{broken"; VERIFY(CommandPanelConfig::write(path, corrupt, error));
    {
        CommandPanelWindow window; auto repair = window.findChild<QPushButton *>("commandConfigRepair"); repair->click();
        VERIFY(toggleOf(window) && !toggleOf(window)->getDef().switchNeedsRepair);
        VERIFY(messages.last.contains("有效备份"));
    }
    bool retained = false;
    for (const auto &name : QDir(dir.path()).entryList({"cmd_panels.json.before-repair-*.json"}, QDir::Files)) retained |= read(dir.filePath(name)) == corrupt;
    VERIFY(retained);
    VERIFY(QFile::remove(path));
    {
        CommandPanelWindow window;
        auto repair = window.findChild<QPushButton *>("commandConfigRepair"); VERIFY(!repair->isHidden());
        repair->click(); VERIFY(toggleOf(window) && !toggleOf(window)->getDef().switchNeedsRepair);
    }
    QByteArray result; QString report;
    // Lock, optimistic concurrency, journal and unavailable-backup cases all
    // preserve the original bytes, rather than using an empty default library.
    VERIFY(!CommandPanelConfig::repairFile(path, QByteArray("stale"), true, result, report, error));
    {
        QLockFile lock(path + ".lock"); VERIFY(lock.tryLock(0));
        VERIFY(!CommandPanelConfig::repairFile(path, read(path), true, result, report, error));
    }
    const auto journal = dir.filePath("global-config-transaction.json"); VERIFY(CommandPanelConfig::write(journal, "{}", error));
    const auto before = read(path);
    VERIFY(!CommandPanelConfig::repairFile(path, before, true, result, report, error) && read(path) == before); VERIFY(QFile::remove(journal));
    VERIFY(CommandPanelConfig::write(path, corrupt, error) && QFile::remove(path + ".bak"));
    VERIFY(!CommandPanelConfig::repairFile(path, corrupt, true, result, report, error) && read(path) == corrupt);
    // Normal save must also notice an older instance stripping switch fields.
    VERIFY(CommandPanelConfig::write(path, valid, error));
    {
        CommandPanelWindow window; VERIFY(CommandPanelConfig::write(path, original, error));
        QMetaObject::invokeMethod(&window, "onContentChanged", Qt::DirectConnection);
        VERIFY(read(path) == original && !window.findChild<QPushButton *>("commandConfigRepair")->isHidden());
    }
    std::cout << "Command migration, disabled placeholders, original backups, one-click recovery, imports, locks and conflicts passed.\n";
    return 0;
}
