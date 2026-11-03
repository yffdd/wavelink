#include "updatemanager.h"
#include "versiondialog.h"
#include "updatebackup.h"
#include "testregistrysandbox.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QCryptographicHash>
#include <QThread>
#include <QProcess>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QDir>
#include <QSettings>
#include <QFontDatabase>
#include <QTextDocument>
#include <QStandardPaths>
#include <QUuid>
#include <iostream>
#define VERIFY(x) do { if (!(x)) { std::cerr << "Update failure at " << __LINE__ << ": " #x "\n"; return 1; } } while (0)
static bool until(std::function<bool()> condition) {
    QElapsedTimer timer; timer.start(); while (!condition() && timer.elapsed() < 15000) { qApp->processEvents(); QThread::msleep(2); } return condition();
}
static bool write(const QString &path, const QByteArray &bytes) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(); }
static QByteArray read(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
static bool script(const QString &name, const QStringList &args) {
    QProcess process; process.start("powershell.exe", QStringList{"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", QStringLiteral(WAVELINK_UPDATE_TOOLS) + "/" + name} + args);
    if (!process.waitForFinished(15000) || process.exitCode() != 0) { std::cerr << process.readAllStandardError().constData(); return false; }
    return true;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv); app.setApplicationVersion("0.13");
    if (app.arguments().contains("--parent-stub")) { QTimer::singleShot(10000, &app, &QApplication::quit); return app.exec(); }
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).value(0), 9));
    TestRegistrySandbox registry; VERIFY(registry.ready());
    QStandardPaths::setTestModeEnabled(true); app.setApplicationName("WaveLinkUpdateTest-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QTemporaryDir folder; VERIFY(folder.isValid());
    const auto key = folder.filePath("update-signing.key"), pub = folder.filePath("update-public.txt"), package = folder.filePath("fixture.exe"), notes = folder.filePath("notes.txt"), feed = folder.filePath("latest.json");
    const QByteArray bytes = QByteArray("MZ") + QByteArray(10000, 'X');
    VERIFY(write(package, bytes) && write(notes, QStringLiteral("测试更新\n保留配置，不中断录制。").toUtf8()));
    VERIFY(script("New-UpdateSigningKey.ps1", {"-KeyPath", key, "-PublicKeyPath", pub}));
    VERIFY(!script("New-UpdateSigningKey.ps1", {"-KeyPath", key, "-PublicKeyPath", pub})); // Never replace a release key.
    VERIFY(script("New-UpdateManifest.ps1", {"-Installer", package, "-Version", "999.1", "-DownloadUrl", "https://updates.example.com/fixture.exe", "-SigningKey", key, "-NotesFile", notes, "-OutputPath", feed, "-ExpectedPublicKey", QString::fromLatin1(read(pub))}));
    VERIFY(!script("New-UpdateManifest.ps1", {"-Installer", package, "-Version", "999.1", "-DownloadUrl", "https://updates.example.com/fixture.exe", "-SigningKey", key, "-NotesFile", notes, "-OutputPath", feed, "-ExpectedPublicKey", "wrong-key", "-Force"}));
    const auto publicKey = QByteArray::fromBase64(read(pub)); const auto manifest = read(feed);
    {
        const auto directory = folder.filePath("publishing"), config = QDir(directory).filePath("publish.local.json"), channel = QDir(directory).filePath("channel.cmake");
        const auto keyDigest = QCryptographicHash::hash(read(key), QCryptographicHash::Sha256);
        const QStringList arguments{"-KeyDirectory", folder.path(), "-UpdateDirectory", directory};
        VERIFY(script("Initialize-UpdatePublishing.ps1", arguments));
        const auto originalConfig = read(config), originalChannel = read(channel);
        VERIFY(originalConfig.contains("v{version}") && originalConfig.contains("currentChangelog") && originalChannel.contains("https://github.com/yffdd/wavelink/releases/latest/download/latest.json"));
        VERIFY(script("Initialize-UpdatePublishing.ps1", arguments) && read(config) == originalConfig && read(channel) == originalChannel);
        VERIFY(QCryptographicHash::hash(read(key), QCryptographicHash::Sha256) == keyDigest);
        const auto changelog = folder.filePath("help-fixture.cpp");
        VERIFY(write(changelog, QStringLiteral(R"notes(
            txtCurrentRelease->setHtml(QString::fromUtf8(R"(
              <ol><li><b>自动生成</b> 来自当前更新日志。</li><li>中文与 &amp; 符号保留。</li></ol>
            )"));
            <li>HISTORICAL_NOTES_MUST_NOT_BE_USED</li>
        )notes").toUtf8()));
        for (const auto &version : {QString("0.13"), QString("0.14.1")}) {
            const auto installer = QDir(directory).filePath("WaveLink_v" + version + "_Windows_x64_Setup.exe"); VERIFY(QFile::copy(package, installer));
            VERIFY(script("Publish-Update.ps1", {"-Config", config, "-Installer", installer, "-Version", version, "-ChangelogFile", changelog}));
            UpdateRelease generated; QString parseError;
            VERIFY(UpdateManager::parseManifest(read(QDir(directory).filePath("latest.json")), publicKey, generated, parseError));
            VERIFY(generated.version == version && generated.downloadUrl.toString() == "https://github.com/yffdd/wavelink/releases/download/v" + version + "/" + QFileInfo(installer).fileName());
            VERIFY(generated.notes.contains(QStringLiteral("来自当前更新日志")) && generated.notes.contains(" & ") && !generated.notes.contains("HISTORICAL_NOTES"));
        }
        // Also exercise the default extractor against the real current UI log.
        VERIFY(script("Publish-Update.ps1", {"-Config", config, "-Installer", package, "-Version", "0.14.2"}));
        UpdateRelease actualNotes; QString actualError;
        VERIFY(UpdateManager::parseManifest(read(feed), publicKey, actualNotes, actualError) && actualNotes.notes.contains(QStringLiteral("[在线更新]")));
        // Existing explicit notes/settings remain usable, not silently replaced.
        auto custom = QJsonDocument::fromJson(originalConfig).object(); custom.remove("notesSource"); custom["notesFile"] = notes;
        VERIFY(write(config, QJsonDocument(custom).toJson())); const auto customBytes = read(config);
        VERIFY(script("Initialize-UpdatePublishing.ps1", arguments) && read(config) == customBytes);
        VERIFY(script("Publish-Update.ps1", {"-Config", config, "-Installer", package, "-Version", "0.15"}));
        UpdateRelease customRelease; QString parseError;
        VERIFY(UpdateManager::parseManifest(read(QDir(folder.path()).filePath("latest.json")), publicKey, customRelease, parseError) && customRelease.notes == QString::fromUtf8(read(notes)));
        const auto publicBytes = read(pub); VERIFY(write(pub, "mismatched-public-key"));
        const auto rejected = folder.filePath("must-not-initialize");
        VERIFY(!script("Initialize-UpdatePublishing.ps1", {"-KeyDirectory", folder.path(), "-UpdateDirectory", rejected}));
        VERIFY(!QFile::exists(QDir(rejected).filePath("channel.cmake")) && QCryptographicHash::hash(read(key), QCryptographicHash::Sha256) == keyDigest);
        VERIFY(write(pub, publicBytes));
        // Leave the original manifest fixture intact for the network tests.
        VERIFY(write(feed, manifest));
    }
    UpdateRelease release; QString error;
    VERIFY(UpdateManager::parseManifest(manifest, publicKey, release, error));
    VERIFY(release.version == "999.1" && release.notes.contains(QStringLiteral("保留配置")) && release.size == bytes.size());
    VERIFY(UpdateManager::newer("0.14", "0.13") && !UpdateManager::newer("0.13.0", "0.13") && !UpdateManager::newer("0.9", "0.13") && !UpdateManager::newer("0.14beta", "0.13"));
    VERIFY(!UpdateManager::validHttps(QUrl("http://example.com")) && !UpdateManager::validHttps(QUrl("https://user:pass@example.com/a")));
    VERIFY(!UpdateManager::parseManifest(manifest, publicKey, release, error, QDateTime::currentDateTimeUtc().addDays(91)));
    auto tampered = QJsonDocument::fromJson(manifest).object(); tampered["payload"] = QString::fromLatin1(QByteArray("{}").toBase64());
    VERIFY(!UpdateManager::parseManifest(QJsonDocument(tampered).toJson(), publicKey, release, error) && error.contains(QStringLiteral("签名")));
    VERIFY(!UpdateManager::parseManifest(QByteArray(130 * 1024, 'X'), publicKey, release, error));
    // A fake transport runs the actual asynchronous checker/downloader, without
    // contacting a server or executing the deliberately nonexecutable fixture.
    auto payload = std::make_shared<QByteArray>(bytes);
    UpdateSource source{QUrl("https://updates.example.com/latest.json"), publicKey};
    auto transport = [manifest, payload](const QUrl &url, const UpdateManager::Sink &sink, const std::atomic_bool &cancelled, QString &) {
        const auto data = url.path().endsWith(".json") ? manifest : *payload;
        for (int offset = 0; offset < data.size(); offset += 128) { if (cancelled || !sink(data.mid(offset, 128))) return false; }
        return true;
    };
    UpdateManager manager(source, transport); int found = 0; QObject::connect(&manager, &UpdateManager::updateFound, [&] { ++found; });
    manager.check(); VERIFY(until([&] { return manager.state() == UpdateManager::State::Available; }) && found == 1);
    manager.ignoreVersion(); VERIFY(QSettings("WaveLink", "WaveLink").value("Update/IgnoredVersion").toString() == "999.1");
    manager.check(true); VERIFY(until([&] { return manager.state() == UpdateManager::State::Available; }) && found == 1);
    *payload = bytes.left(bytes.size() - 1); manager.download(); VERIFY(until([&] { return manager.state() == UpdateManager::State::Available; }));
    VERIFY(manager.packagePath().isEmpty() && manager.message().contains("SHA-256"));
    *payload = bytes; (*payload)[100] = 'Y'; manager.download(); VERIFY(until([&] { return manager.state() == UpdateManager::State::Available; }) && manager.packagePath().isEmpty());
    *payload = bytes + "extra"; manager.download(); VERIFY(until([&] { return manager.state() == UpdateManager::State::Available; }) && manager.packagePath().isEmpty());
    *payload = bytes; manager.download(); VERIFY(until([&] { return manager.state() == UpdateManager::State::Ready; }));
    VERIFY(read(manager.packagePath()) == bytes);
    const auto downloadFolder = QFileInfo(manager.packagePath()).absolutePath();
    // The native helper rejects malformed requests, never runs this fixture.
    if (argc > 1) { QProcess helper; helper.start(QString::fromLocal8Bit(argv[1]), {"invalid"}); VERIFY(helper.waitForFinished(5000) && helper.exitCode() != 0); }
    if (argc > 1) for (int scenario : {0, 1, 2}) {
        const bool correctHash = scenario != 1, commitInstall = scenario != 2;
        QTemporaryDir handoff; VERIFY(handoff.isValid());
        const auto launcher = handoff.filePath("launcher.exe"), fixture = handoff.filePath("fixture.exe"), marker = handoff.filePath("test-install-verified.marker");
        VERIFY(QFile::copy(QString::fromLocal8Bit(argv[1]), launcher) && write(fixture, bytes));
        QProcess parent; parent.start(QCoreApplication::applicationFilePath(), {"--parent-stub"}); VERIFY(parent.waitForStarted());
        const auto name = "Local\\WaveLinkUpdateTest-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(name.utf16())); VERIFY(ready);
        const auto commitName = name + "-commit";
        HANDLE commit = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(commitName.utf16())); VERIFY(commit);
        const auto digest = correctHash ? QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) : QString(64, '0');
        QProcess helper; helper.start(launcher, {QString::number(parent.processId()), name, fixture, digest, QCoreApplication::applicationFilePath(), handoff.path()});
        VERIFY(helper.waitForStarted() && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0); CloseHandle(ready);
        VERIFY(!QFile::exists(marker) && helper.state() == QProcess::Running); // Must wait for the exact parent.
        QFile replacement(fixture); VERIFY(!replacement.open(QIODevice::WriteOnly)); // Locked against replacement.
        if (commitInstall) SetEvent(commit); CloseHandle(commit);
        parent.kill(); VERIFY(parent.waitForFinished(5000)); // Only the test-created stub, never WaveLink.
        VERIFY(helper.waitForFinished(5000));
        VERIFY(correctHash && commitInstall ? helper.exitCode() == 0 && QFile::exists(marker) : helper.exitCode() != 0 && !QFile::exists(marker));
    }
    bool installClicked = false;
    for (bool dark : {false, true}) {
        app.setStyleSheet(dark ? "QWidget { background:#1e1e1e; color:#dddddd; }" : "QWidget { background:#f0f0f0; color:#333333; }");
        VersionDialog dialog(&manager, [&] { installClicked = true; }); dialog.show(); app.processEvents();
        auto summary = dialog.findChild<QLabel *>("versionSummary"), heading = dialog.findChild<QLabel *>("versionUpdateHeading");
        VERIFY(summary && heading && summary->openExternalLinks() && (summary->textInteractionFlags() & Qt::TextSelectableByMouse));
        QTextDocument about; about.setHtml(summary->text());
        VERIFY(about.toPlainText().contains("v0.13") && about.toPlainText().contains("https://xiales.lanzouu.com/b007unma2b") && about.toPlainText().contains("0000"));
        VERIFY(about.toPlainText().contains(QStringLiteral("https://xiales.lanzouu.com/b007unma2b（提取码：0000）")));
        VERIFY(summary->geometry().bottom() < heading->geometry().top() && heading->geometry().bottom() < dialog.findChild<QPushButton *>("checkForUpdates")->mapTo(&dialog, QPoint()).y());
        VERIFY(dialog.findChild<QPushButton *>("installUpdate")->isVisible());
        VERIFY(dialog.findChild<QProgressBar *>("updateProgress")->value() == 100);
        VERIFY(!dialog.findChild<QPushButton *>("checkForUpdates")->isEnabled());
        if (argc > 2) { QDir().mkpath(QString::fromLocal8Bit(argv[2])); dialog.grab().save(QString::fromLocal8Bit(argv[2]) + (dark ? "/version-dark.png" : "/version-light.png")); }
        dialog.reject(); VERIFY(!installClicked);
    }
    {
        UpdateManager unconfigured({}, transport); VersionDialog dialog(&unconfigured, [] {}); dialog.show(); app.processEvents();
        dialog.findChild<QPushButton *>("checkForUpdates")->click();
        VERIFY(unconfigured.state() == UpdateManager::State::Error && dialog.findChild<QLabel *>("updateStatus")->text().contains(QStringLiteral("尚未配置")));
        VERIFY(!dialog.findChild<QPushButton *>("installUpdate")->isVisible()); dialog.reject();
    }
    {
        auto slow = [](const QUrl &, const UpdateManager::Sink &, const std::atomic_bool &cancelled, QString &) {
            while (!cancelled) QThread::msleep(2); return false;
        };
        UpdateManager cancelled(source, slow); cancelled.check(); cancelled.cancel();
        VERIFY(until([&] { return cancelled.state() == UpdateManager::State::Idle; }) && cancelled.packagePath().isEmpty());
    }
    {
        const auto data = folder.filePath("config"); VERIFY(QDir().mkpath(data));
        const QByteArray original("original protocol bytes");
        VERIFY(write(QDir(data).filePath("cmd_panels.json"), "[]") && write(QDir(data).filePath("custom_protocols.json.bak"), original));
        VERIFY(write(QDir(data).filePath("recording.wlr"), "DO NOT CHANGE"));
        QSettings settings("WaveLink", "WaveLink"); settings.setValue("Update/AutoCheck", false); settings.sync();
        DataProtocols::ProtocolSnapshot snapshot; QString backup;
        VERIFY(UpdateBackup::create(snapshot, data, backup, error));
        QVariantMap values; VERIFY(DataProtocols::ProtocolConfigTransfer::read(QDir(backup).filePath("WaveLink_Config.ini"), values, error));
        VERIFY(values.contains("DataProtocols/json") && values.contains("CommandPanel/json") && values.contains("StatusPanel/json") && !values.value("WaveLink/Update/AutoCheck").toBool());
        VERIFY(read(QDir(backup).filePath("custom_protocols.json.bak")) == original && read(QDir(data).filePath("recording.wlr")) == "DO NOT CHANGE");
        VERIFY(!QFile::exists(QDir(backup).filePath("recording.wlr")));
        const auto notDir = folder.filePath("not-directory"); VERIFY(write(notDir, "x"));
        VERIFY(!UpdateBackup::create(snapshot, notDir, backup, error));
        VERIFY(write(QDir(data).filePath("status_panels.json"), "broken"));
        VERIFY(!UpdateBackup::create(snapshot, data, backup, error) && read(QDir(data).filePath("status_panels.json")) == "broken");
    }
    std::cout << "Signed publisher round trip, tampering/expiry rejection, version comparison, size/hash errors, retries, cancellation, ignored versions and both dialog themes passed. No installer executed.\n";
    return 0;
}
