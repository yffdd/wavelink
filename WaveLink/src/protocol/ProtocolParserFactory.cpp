#include "ProtocolParserFactory.h"
#include "ProtocolParser.h"
#include "CustomProtocolParser.h"

namespace DataProtocols {
ProtocolParser *ProtocolParserFactory::createLegacy(int index, bool bigEndian, QObject *parent) {
    switch (index) {
    case 1: return new FireWaterParser(parent);
    case 2: {
        auto parser = new JustFloatParser(parent); parser->setBigEndian(bigEndian); return parser;
    }
    case 3: return new AMU4Parser(parent);
    default: return new RawParser(parent);
    }
}
ProtocolParser *ProtocolParserFactory::create(const ProtocolSelection &selection, QObject *parent, QString &error) {
    ProtocolSelection checked;
    if (!ProtocolProfileValidator::selection(selection.toJson(), checked, error)) return nullptr;
    if (checked.kind == "custom") {
        error = QStringLiteral("创建自定义解析器需要提供已保存方案的完整快照"); return nullptr;
    }
    return createLegacy(checked.legacyIndex(), checked.bigEndian, parent);
}
ProtocolParser *ProtocolParserFactory::createCustom(const ProtocolProfile &profile, QObject *parent, QString &error) {
    ProtocolProfile checked;
    if (!ProtocolProfileValidator::profile(profile.definition, checked, error)) return nullptr;
    return new CustomProtocolParser(checked, parent, /*confirmLiveSync=*/false);
}
ProtocolParser *ProtocolParserFactory::create(const ProtocolSelection &selection, const ProtocolProfile &profile,
                                            QObject *parent, QString &error) {
    ProtocolSelection checked;
    if (!ProtocolProfileValidator::selection(selection.toJson(), checked, error)) return nullptr;
    if (checked.kind != "custom") return create(checked, parent, error);
    ProtocolProfile validated;
    if (!ProtocolProfileValidator::profile(profile.definition, validated, error)) return nullptr;
    if (validated.id != checked.id) { error = QStringLiteral("协议选择与方案快照 ID 不一致"); return nullptr; }
    return new CustomProtocolParser(validated, parent, /*confirmLiveSync=*/true);
}
} // namespace DataProtocols
