#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
class QWidget;
class BleHostClient;
namespace BleDialogs {
QJsonObject selectDevice(BleHostClient *client, QWidget *parent);
bool configure(BleHostClient *client, const QJsonArray &characteristics,
               const QJsonObject &savedProfile, QJsonObject &profile, QWidget *parent,
               const QString &deviceName = {});
void showError(const QString &message, const QString &device, QWidget *parent);
void showFileError(const QString &message, QWidget *parent);
void showSendStopped(const QString &message, QWidget *parent);
struct ProfileAction { bool saveNext = false, reconnect = false, applyCurrent = false; QJsonObject profile; };
ProfileAction manageProfiles(const QJsonObject &active, const QJsonObject &next,
                             const QJsonArray &entries, bool ready, bool editable, QWidget *parent, BleHostClient *client = nullptr);
void showDiagnostics(const std::function<QString(bool)> &snapshot, QWidget *parent);
}
