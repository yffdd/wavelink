#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DataProtocols {
constexpr int SchemaVersion = 1;
constexpr int MaxProfiles = 128;
constexpr int MaxChannels = 256;
constexpr int MaxDocumentBytes = 2 * 1024 * 1024;
constexpr int MaxFrameBytes = 64 * 1024;

// Value snapshots only: no widgets, connection handles or mutable parser state.
struct ProtocolProfile {
    QString id;
    int revision = 1;
    QString name;
    QString description;
    QJsonObject definition; // Validated, canonical schema-1 definition.

    bool isDynamicArray() const { return definition.contains("array"); }
    bool isDelimiterFramed() const { return definition.value("type") == QJsonValue("binary") && definition.value("frame").toObject().value("mode") == QJsonValue("delimiter"); }
    bool isKeyValue() const { return definition.value("type") == QJsonValue("keyvalue"); }
    bool recordsConvertedValues() const { return definition.value("recordingValueMode") == QJsonValue("converted"); }

    // Configured fields retain their order. Text protocols additionally emit
    // unconfigured columns in source order, with stable default identities.
    // Dynamic binary arrays always retain wire-index order; configured rows
    // only override metadata/conversion and never limit the received count.
    // The same layout is used by preview, plots and saved channel preferences.
    QJsonArray outputChannels(int count = MaxChannels) const;
    QStringList outputChannelNames(int count = MaxChannels) const;
};

struct ProtocolSelection {
    QString kind = QStringLiteral("builtin");
    QString id = QStringLiteral("builtin.firewater");
    bool bigEndian = false; // Only meaningful for builtin.justfloat.
    static ProtocolSelection fromLegacy(int index, bool bigEndian = false);
    int legacyIndex() const;
    QJsonObject toJson() const;
};

struct ProtocolSnapshot {
    QVector<ProtocolProfile> profiles;
    ProtocolSelection shared;
    // Historical alias for schema-1 readers/tests. Normalization mirrors
    // shared; persisted selections contain only the one global choice.
    ProtocolSelection ble = ProtocolSelection::fromLegacy(0);
};

// Strict decoding also rejects unknown rule keys: silently ignoring a rule
// from a newer file could produce plausible but incorrect measurements.
class ProtocolProfileValidator {
public:
    static bool profile(const QJsonObject &input, ProtocolProfile &output, QString &error);
    static bool selection(const QJsonObject &input, ProtocolSelection &output, QString &error);
    static bool snapshot(const ProtocolSnapshot &input, ProtocolSnapshot &output, QString &error);
    static bool decode(const QByteArray &bytes, bool storeDocument,
                       ProtocolSnapshot &output, QString &error);
    static QByteArray encode(const ProtocolSnapshot &snapshot, bool storeDocument, QString &error);
    static bool parseHex(const QString &text, QByteArray &output);
    static int typeBytes(const QString &type);
    static QJsonObject keyValueTemplate(bool cbf = false);
    // Standard editor/store rules. Repair only during migration of a valid
    // stored profile; editing keeps conflicting limits visible to the user.
    static QJsonObject standardizeBinary(QJsonObject definition, bool repairLimits = false);
};
} // namespace DataProtocols

Q_DECLARE_METATYPE(DataProtocols::ProtocolSelection)
Q_DECLARE_METATYPE(DataProtocols::ProtocolProfile)

