#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QWidget>
#include <functional>
class BleHostClient;
class QToolButton;

class GattChannels : public QWidget
{
public:
    GattChannels(const QJsonArray &entries, BleHostClient *client, QWidget *parent,
                 const std::function<void(int)> &selectRx, const std::function<void(int)> &selectTx,
                 bool compact = false);
    void updateSelection(int rx, int tx, bool edit, bool read);
    std::function<void()> refresh;

private:
    struct Row { int handle; QToolButton *rx = nullptr, *tx = nullptr, *read = nullptr; };
    QList<Row> rows;
    bool reading = false;
};

namespace BleDialogs {
QJsonObject selectDevice(BleHostClient *client, QWidget *parent);
bool confirmSendParameters(QWidget *parent, int packet, int interval);
bool confirmByteStream(QWidget *parent);
struct ProfileAction {
    enum Kind { None, Applied, SaveNext, Reconnect, SendSettingsApplied };
    Kind kind = None;
    QJsonObject profile;
};
ProfileAction configure(BleHostClient *client, const QJsonArray &entries, QJsonObject active,
                        const QJsonObject &saved, bool editable, QWidget *parent, const QString &deviceName = {});
void showError(const QString &message, const QString &device, QWidget *parent);
void showFileError(const QString &message, QWidget *parent);
void showSendStopped(const QString &message, QWidget *parent);
void showDiagnostics(const std::function<QString(bool)> &snapshot, QWidget *parent);
}
