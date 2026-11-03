#pragma once
#include "ProtocolProfileStore.h"
#include <QVariantMap>

namespace DataProtocols {
// The entire INI is staged before mutation. Protocol IDs and selection
// remapping are prepared together; legacy keys cannot override a skipped block.
class ProtocolConfigTransfer {
public:
    static bool read(const QString &path, QVariantMap &values, QString &error);
    static bool exportFile(const QString &path, const ProtocolSnapshot &snapshot, const QString &commandPath, QString &error, const QString &statusPath = {});
    // policy: -1 skip, otherwise ProtocolImportMode. commandMode: 0 skip,
    // 1 replace, 2 append. statusMode uses the same skip/replace/append values.
    // Missing status blocks leave existing panels intact. Missing protocol
    // blocks use validated legacy IDs. Empty statusPath uses commandPath's dir.
    static bool apply(const QVariantMap &values, ProtocolProfileStore &store, int policy,
                      bool restoreSelections, int commandMode, const QString &commandPath, QString &error, int statusMode = 0, const QString &statusPath = {});
    // A durable rollback journal is created before any writes. Startup restores
    // interrupted imports before constructing widgets or loading their settings.
    static bool recover(const QString &storePath, const QString &commandPath, QString &error, const QString &statusPath = {});
};
}
