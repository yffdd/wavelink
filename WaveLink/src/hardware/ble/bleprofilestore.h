#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>

// Whitelisted portable templates, separate from per-device identity/settings.
// Legacy profiles are normalized on read and written only after user approval.
class BleProfileStore
{
public:
    explicit BleProfileStore(QSettings &settings) : m_settings(settings) {}
    static QString normalizeUuid(const QString &text);
    static bool normalize(const QJsonObject &input, QJsonObject &output, QString &error);
    static QJsonObject nus();
    QJsonObject deviceProfile(const QJsonObject &device, QString &error) const;
    bool saveDeviceProfile(const QJsonObject &device, const QJsonObject &profile, QString &error,
                           const QJsonArray &validatedEntries = {});
    QJsonArray templates(QString &error) const;
    bool saveTemplate(const QString &name, const QJsonObject &profile, bool replace, QString &error);
    static bool parseTemplate(const QByteArray &bytes, QString &name, QJsonObject &profile, QString &error);
    static QByteArray exportTemplate(const QString &name, const QJsonObject &profile, QString &error);
private:
    static QString deviceKey(const QJsonObject &device);
    static bool validName(const QString &name);
    QSettings &m_settings;
};
