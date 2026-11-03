#pragma once
#include "bledialogs.h"
#include "ble/blechannelpolicy.h"
#include <QTreeWidget>

static int bleQuickUiTests(const QString &fakeHost)
{
    VERIFY(QFile::exists(fakeHost));
    auto characteristic = [](const QString &s, const QString &u, int handle, int props) {
        return QJsonObject{{"service", BleProfileStore::normalizeUuid(s)}, {"uuid", BleProfileStore::normalizeUuid(u)},
                           {"handle", handle}, {"serviceHandle", s == "FE59" ? 1 : 2}, {"properties", props}};
    };
    const QJsonArray entries{characteristic("FE59", "8ec90001-f315-4f60-9fb8-838830daea50", 2, 24),
        characteristic("FE59", "8ec90002-f315-4f60-9fb8-838830daea50", 5, 4),
        characteristic("FFF0", "FFF1", 11, 18), characteristic("FFF0", "FFF2", 14, 4)};
    for (auto theme : {ThemeManager::LightTheme, ThemeManager::DarkTheme}) {
        ThemeManager::applyTheme(qApp, theme);
        BleHostClient client(nullptr, fakeHost);
        client.connectDevice("1", 0);
        VERIFY(until([&] { return client.state() == BleHostClient::State::AwaitingConfiguration; }));
        bool checked = false, ok = true; QJsonObject result;
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
            ok = !apply->isEnabled() && reason->text().contains(QStringLiteral("请选择")) && !advanced->isChecked() && !mode->isVisible() && !stream->isVisible();
            ok = ok && choice->count() == 1 && dialog->findChild<QLabel *>("bleDeviceHeading")->text().contains("MX-02P");
            candidate->click();
            ok = ok && rx->currentData().toInt() == 11 && tx->currentData().toInt() == 14 && mode->currentData().toBool() && stream->isVisible() && !apply->isEnabled();
            ok = ok && reason->text().contains(QStringLiteral("勾选"));
            stream->setChecked(true); ok = ok && apply->isEnabled();
            tx->setCurrentIndex(tx->findData(5));
            ok = ok && !stream->isChecked() && reason->text().contains(QStringLiteral("固件升级"));
            candidate->click(); stream->setChecked(true);
            advanced->setChecked(true); mode->setCurrentIndex(0);
            ok = ok && !apply->isEnabled() && reason->text().contains(QStringLiteral("不支持"));
            mode->setCurrentIndex(1); ok = ok && apply->isEnabled();
            advanced->setChecked(false); dialog->resize(920, 560); qApp->processEvents();
            ok = ok && apply->isVisible() && dialog->rect().contains(QRect(apply->mapTo(dialog, QPoint()), apply->size()));
            ok = ok && rx->width() > 150 && tx->width() > 150;
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
        const bool accepted = BleDialogs::configure(&client, entries, {}, result, nullptr, "MX-02P · C326C21BC80B");
        automation.stop(); watchdog.stop();
        VERIFY(checked && ok && accepted && client.state() == BleHostClient::State::Ready && client.txCount() == 0);
        VERIFY(theme == ThemeManager::LightTheme ? result["txUuid"].toString().isEmpty() : result["withoutResponse"].toBool());
        client.disconnectDevice(); VERIFY(until([&] { return client.state() == BleHostClient::State::Idle; }));
    }
    std::cout << "BLE quick configuration: candidate/DFU filtering, receive-only, write-mode adaptation, actionable reasons, advanced layout, both themes and no unsolicited writes passed.\n";
    return 0;
}
