#pragma once
#include "ProtocolProfile.h"
#include <QHash>

class QSettings;
namespace DataProtocols {
enum class ProtocolImportMode { Append, MergeById, Replace };
struct ProtocolImportPreview {
    ProtocolSnapshot snapshot;
    QHash<QString, QString> idMap;
    int added = 0;
    int updated = 0;
    int removed = 0;
};

// This repository never connects devices or activates parsers. The UI must
// check acquisition state before applying a saved revision in a later phase.
class ProtocolProfileStore {
public:
    explicit ProtocolProfileStore(const QString &path = defaultPath());
    static QString defaultPath();
    static ProtocolSnapshot legacySelections(QSettings &mainSettings, QSettings &advancedSettings);
    bool load(QString &error, const ProtocolSnapshot &missingFileDefaults = ProtocolSnapshot{});
    bool commit(const ProtocolSnapshot &snapshot, QString &error);
    bool restoreBackup(QString &error);
    const ProtocolSnapshot &snapshot() const { return m_snapshot; }
    QString path() const { return m_path; }
    bool ready() const { return m_ready; }

    bool saveProfile(const QJsonObject &definition, QString &id, QString &error);
    bool copyProfile(const QString &id, QString &newId, QString &error);
    bool removeProfile(const QString &id, QString &error);
    bool moveProfile(const QString &id, int delta, QString &error);
    bool setSelection(const ProtocolSelection &selection, QString &error);
    // Empty IDs mean the entire library; selected exports include saved names.
    QString defaultExportFileName(const QStringList &ids = {}) const;
    bool exportProfiles(const QString &path, const QStringList &ids, QString &error) const;

    // Prepare is read-only and returns the exact candidate to show in a UI
    // preview; commit must be called explicitly after the user accepts it.
    static bool prepareImport(const ProtocolSnapshot &current, const QByteArray &bytes,
        bool storeDocument, ProtocolImportMode mode, bool restoreSelections,
        ProtocolImportPreview &preview, QString &error);
    static bool readFile(const QString &path, QByteArray &bytes, QString &error);
    static bool writeFile(const QString &path, const QByteArray &bytes, QString &error);
private:
    QString m_path;
    ProtocolSnapshot m_snapshot;
    bool m_ready = false;
};
} // namespace DataProtocols

