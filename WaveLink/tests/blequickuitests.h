#pragma once
#include "bledialogs.h"
#include "ble/blechannelpolicy.h"
#include <QHeaderView>
#include <QTreeWidget>

static int bleQuickUiTests(const QString &fakeHost)
{
    VERIFY(QFile::exists(fakeHost));
    auto characteristic = [](const QString &s, const QString &u, int handle, int props) {
        return QJsonObject{{"service", BleProfileStore::normalizeUuid(s)}, {"uuid", BleProfileStore::normalizeUuid(u)},
                           {"handle", handle}, {"serviceHandle", s == "FE59" ? 1 : 2}, {"properties", props}};
    };
    const QString customService = "e9a01000-4588-4bdd-9940-18090e247c83";
    const QString customRx = "e9a01001-4588-4bdd-9940-18090e247c83", customTx = "e9a01002-4588-4bdd-9940-18090e247c83";
    const QJsonArray entries{characteristic("FE59", "8ec90001-f315-4f60-9fb8-838830daea50", 2, 24),
        characteristic("FE59", "8ec90002-f315-4f60-9fb8-838830daea50", 5, 4),
        characteristic(customService, customRx, 11, 26), characteristic(customService, customTx, 14, 4)};
    const auto actionFits = [](QTreeWidget *tree, QToolButton *button) {
        if (!tree || !button || !button->isVisible()) return false;
        const QRect bounds(button->mapTo(tree->viewport(), QPoint()), button->size());
        const int left = tree->header()->sectionViewportPosition(1);
        const auto item = tree->itemAt(bounds.center());
        const auto font = button->fontMetrics();
        return item && tree->visualItemRect(item).contains(bounds) &&
            left <= bounds.left() && bounds.right() < left + tree->columnWidth(1) &&
            bounds.width() >= qMax(72, font.horizontalAdvance(button->text()) + 30) &&
            bounds.height() >= qMax(32, font.height() + 14);
    };
    for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
        ThemeManager::applyTheme(qApp, theme);
        BleHostClient client(nullptr, fakeHost);
        client.connectDevice("1", 0);
        VERIFY(until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; }));
        bool checked = false, ok = true;
        QTimer automation; automation.setInterval(10);
        QObject::connect(&automation, &QTimer::timeout, [&] {
            if (checked) return;
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            auto rx = dialog->findChild<QComboBox *>("bleReceiveCharacteristic"), tx = dialog->findChild<QComboBox *>("bleTransmitCharacteristic");
            auto mode = dialog->findChild<QComboBox *>("bleWriteMode"); auto stream = dialog->findChild<QCheckBox *>("bleStreamConfirmation");
            auto choice = dialog->findChild<QComboBox *>("bleCandidateChoice"); auto advanced = dialog->findChild<QToolButton *>("bleGattAdvancedToggle");
            auto apply = dialog->findChild<QPushButton *>("bleApplyConfiguration"), candidate = dialog->findChild<QPushButton *>("bleUseCandidate");
            auto receiveOnly = dialog->findChild<QPushButton *>("bleReceiveOnlyCandidate"); auto reason = dialog->findChild<QLabel *>("bleApplyReason");
            checked = true;
            if (!rx || !tx || !mode || !stream || !choice || !advanced || !apply || !candidate || !receiveOnly || !reason) { ok = false; dialog->reject(); return; }
            ok = dialog->objectName() == "bleConfigurationDialog" && dialog->windowTitle() == QStringLiteral("BLE 收发配置") &&
                dialog->findChild<QPushButton *>("bleSaveNextProfile") && !apply->isEnabled() &&
                reason->text().contains(QStringLiteral("请选择")) && !advanced->isChecked() && !rx->isVisible() && !mode->isVisible() && !stream->isVisible();
            ok = ok && choice->count() == 1 && dialog->findChild<QLabel *>("bleDeviceHeading")->text().contains("MX-02P");
            auto tree = dialog->findChild<QTreeWidget *>("bleGattTree");
            const auto action = [tree](const char *name, int handle) -> QToolButton * {
                if (tree) for (auto button : tree->findChildren<QToolButton *>(QString::fromLatin1(name)))
                    if (button->property("gattHandle").toInt() == handle) return button;
                return nullptr;
            };
            auto pickRx = action("bleGattRx", 11), pickTx = action("bleGattTx", 14), read = action("bleGattRead", 11);
            if (!tree || !tree->isVisible() || !pickRx || !pickTx || !read || action("bleGattRead", 14)) { ok = false; dialog->reject(); return; }
            for (auto button : {read, pickRx, action("bleGattTx", 11), pickTx})
                ok = ok && actionFits(tree, button);
            pickRx->click();
            ok = ok && pickRx->isChecked() && rx->currentData().toInt() == 11 && apply->isEnabled();
            read->click();
            auto readResult = dialog->findChild<QPlainTextEdit *>("bleGattReadResult");
            ok = ok && readResult && !apply->isEnabled() && until([&] { return readResult->toPlainText().contains("68 69") && apply->isEnabled(); });
            pickTx->click();
            ok = ok && pickTx->isChecked() && tx->currentData().toInt() == 14 && !apply->isEnabled();
            pickTx->click();
            ok = ok && !pickTx->isChecked() && tx->currentData().toInt() == 0 && apply->isEnabled();
            candidate->click();
            ok = ok && rx->currentData().toInt() == 11 && tx->currentData().toInt() == 14 && mode->currentData().toBool() && stream->isVisible() && !apply->isEnabled();
            ok = ok && reason->text().contains(QStringLiteral("勾选"));
            ok = ok && rx->currentText().contains("E9A01001…7C83") && tx->currentText().contains("E9A01002…7C83") &&
                rx->toolTip().contains(customRx) && tx->toolTip().contains(customTx) && tx->toolTip().contains(customService);
            stream->setChecked(true); ok = ok && apply->isEnabled();
            rx->setCurrentIndex(0);
            ok = ok && !stream->isChecked() && !apply->isEnabled();
            tx->setCurrentIndex(tx->findData(5));
            ok = ok && !stream->isChecked() && reason->text().contains(QStringLiteral("固件升级"));
            candidate->click(); stream->setChecked(true);
            advanced->setChecked(true); mode->setCurrentIndex(0);
            ok = ok && !apply->isEnabled() && reason->text().contains(QStringLiteral("不支持"));
            mode->setCurrentIndex(1); ok = ok && apply->isEnabled();
            dialog->resize(920, 560); qApp->processEvents();
            ok = ok && apply->isVisible() && dialog->rect().contains(QRect(apply->mapTo(dialog, QPoint()), apply->size()));
            for (auto combo : {rx, tx})
                ok = ok && combo->fontMetrics().horizontalAdvance(combo->currentText()) + 32 < combo->width();
            advanced->setChecked(false); qApp->processEvents();
            if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + QString("/ble-quick-%1-%2.png").arg(theme).arg(qApp->devicePixelRatio()));
            advanced->setChecked(true); qApp->processEvents();
            if (!artifactDirectory.isEmpty()) dialog->grab().save(artifactDirectory + QString("/ble-detail-%1-%2.png").arg(theme).arg(qApp->devicePixelRatio()));
            advanced->setChecked(false);
            if (theme == ThemeManager::LightTheme) {
                receiveOnly->click(); ok = ok && tx->currentIndex() == 0 && !stream->isVisible() && apply->isEnabled();
            }
            if (ok) apply->click(); else dialog->reject();
        });
        automation.start();
        QTimer watchdog; watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, [&] { if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        watchdog.start(5000);
        const auto action = BleDialogs::configure(&client, entries, {}, {}, true, nullptr, "MX-02P · C326C21BC80B");
        automation.stop(); watchdog.stop();
        VERIFY(checked && ok && action.kind == BleDialogs::ProfileAction::Applied && client.state() == BleHostClient::State::Ready && client.txCount() == 0);
        VERIFY(theme == ThemeManager::LightTheme ? action.profile["txUuid"].toString().isEmpty() : action.profile["withoutResponse"].toBool());
        client.disconnectDevice(); VERIFY(until([&] { return client.state() == BleHostClient::State::Idle; }));
    }

    const auto profile = BleProfileStore::nus();
    const auto nusRx = characteristic(profile.value("rxService").toString(), profile.value("rxUuid").toString(), 21, 16);
    const auto nusTx = characteristic(profile.value("txService").toString(), profile.value("txUuid").toString(), 22, 12);
    auto combined = entries;
    combined.append(nusRx); combined.append(nusTx);
    auto ambiguous = combined;
    auto duplicate = nusTx; duplicate["handle"] = 23; ambiguous.append(duplicate);
    VERIFY(!BleChannelPolicy::nordicUart(ambiguous).tx);
    auto noNotify = nusRx; noNotify["properties"] = 32;
    VERIFY(!BleChannelPolicy::nordicUart(QJsonArray{noNotify, nusTx}).tx);

    BleHostClient client(nullptr, fakeHost);
    client.connectDevice("1", 0);
    VERIFY(until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; }));
    bool checked = false, ok = false;
    QTimer::singleShot(20, &client, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        checked = true;
        auto rx = dialog->findChild<QComboBox *>("bleReceiveCharacteristic"), tx = dialog->findChild<QComboBox *>("bleTransmitCharacteristic");
        auto stream = dialog->findChild<QCheckBox *>("bleStreamConfirmation");
        auto apply = dialog->findChild<QPushButton *>("bleApplyConfiguration");
        if (!rx || !tx || !stream || !apply) { dialog->reject(); return; }
        ok = rx->currentData().toInt() == 21 && tx->currentData().toInt() == 22 &&
            rx->currentText().contains("Nordic UART") && tx->currentText().contains("Nordic UART") &&
            !stream->isVisible() && apply->isEnabled();
        rx->setCurrentIndex(rx->findData(11));
        ok = ok && stream->isVisible() && !stream->isChecked() && !apply->isEnabled();
        rx->setCurrentIndex(rx->findData(21));
        ok = ok && !stream->isVisible() && apply->isEnabled();
        if (ok) apply->click(); else dialog->reject();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&] { if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
    watchdog.start(5000);
    const auto applied = BleDialogs::configure(&client, combined, {}, {}, true, nullptr, "Nordic UART");
    VERIFY(applied.kind == BleDialogs::ProfileAction::Applied);
    const auto result = applied.profile;
    VERIFY(checked && ok && result.value("stream").toBool() && result.value("txUuid") == profile.value("txUuid") && client.txCount() == 0);
    bool managed = false;
    const auto appFont = qApp->font();
    auto largeFont = appFont;
    largeFont.setPointSize(14);
    qApp->setFont(largeFont);
    QTimer::singleShot(20, &client, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto advanced = dialog->findChild<QToolButton *>("bleGattAdvancedToggle");
        auto save = dialog->findChild<QPushButton *>("bleSaveNextProfile");
        auto read = dialog->findChild<QToolButton *>("bleGattRead");
        QToolButton *customReceive = nullptr;
        for (auto button : dialog->findChildren<QToolButton *>("bleGattRx")) {
            if (button->property("gattHandle").toInt() == 11) customReceive = button;
        }
        if (dialog->objectName() != "bleConfigurationDialog" || dialog->windowTitle() != QStringLiteral("BLE 收发配置") ||
            dialog->findChild<QPushButton *>("bleApplyConfiguration") || !dialog->findChild<QPushButton *>("bleSaveAndReconnect") ||
            !advanced || advanced->isChecked() || !save || !read || !customReceive || !customReceive->isVisible()) {
            dialog->reject();
            return;
        }
        read->click();
        auto output = dialog->findChild<QPlainTextEdit *>("bleGattReadResult");
        managed = output && !save->isEnabled() &&
            until([&] { return output->toPlainText().contains("68 69") && save->isEnabled(); });
        customReceive->click();
        managed = managed && customReceive->isChecked() && actionFits(dialog->findChild<QTreeWidget *>("bleGattTree"), customReceive);
        if (managed) save->click(); else dialog->reject();
    });
    watchdog.start(5000);
    const auto next = BleDialogs::configure(&client, combined, result, result, true, nullptr, "Nordic UART");
    watchdog.stop();
    qApp->setFont(appFont);
    VERIFY(managed && next.kind == BleDialogs::ProfileAction::SaveNext && next.profile.value("rxUuid").toString() == customRx);
    VERIFY(client.state() == BleHostClient::State::Ready && client.txCount() == 0);
    client.disconnectDevice();
    bool offline = false;
    QTimer::singleShot(20, &client, [&] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto tree = dialog->findChild<QTreeWidget *>("bleGattTree");
        auto advanced = dialog->findChild<QToolButton *>("bleGattAdvancedToggle");
        auto field = dialog->findChild<QLineEdit *>("bleNextRxUuid");
        const auto save = dialog->findChild<QPushButton *>("bleSaveNextProfile");
        offline = dialog->objectName() == "bleConfigurationDialog" && dialog->windowTitle() == QStringLiteral("BLE 收发配置") &&
            tree && !tree->isVisible() && advanced && !advanced->isChecked() && field && save && save->isEnabled() &&
            !dialog->findChild<QPushButton *>("bleApplyConfiguration") && !dialog->findChild<QPushButton *>("bleSaveAndReconnect");
        if (offline) {
            advanced->setChecked(true);
            offline = field->isVisible() && field->text() == customRx;
            field->setText("FFF1");
        }
        if (offline) save->click(); else dialog->reject();
    });
    watchdog.start(5000);
    const auto preset = BleDialogs::configure(&client, combined, {}, next.profile, true, nullptr, "Nordic UART");
    watchdog.stop();
    VERIFY(offline && preset.kind == BleDialogs::ProfileAction::SaveNext && preset.profile.value("rxUuid").toString() == BleProfileStore::normalizeUuid("FFF1"));
    std::cout << "BLE quick configuration: known UART matching, custom confirmation, compact choices, DFU filtering, receive-only, both themes and no unsolicited writes passed.\n";
    return 0;
}
