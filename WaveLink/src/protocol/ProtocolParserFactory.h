#pragma once
#include "ProtocolProfile.h"
class QObject;
class ProtocolParser;

namespace DataProtocols {
class ProtocolParserFactory {
public:
    // Retain the old 0..3 mapping, including Raw for unknown legacy indices.
    static ProtocolParser *createLegacy(int index, bool bigEndian, QObject *parent);
    // New ID-based callers must handle a failure rather than silently using Raw.
    static ProtocolParser *create(const ProtocolSelection &selection, QObject *parent, QString &error);
    static ProtocolParser *create(const ProtocolSelection &selection, const ProtocolProfile &profile,
                                  QObject *parent, QString &error);
    // A sample can be validated without the live stream's acquisition window.
    static ProtocolParser *createCustom(const ProtocolProfile &profile, QObject *parent, QString &error);
};
} // namespace DataProtocols

