#include "ProtocolProfileStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QSet>
#include <limits>
#include <QJsonDocument>

namespace DataProtocols {
namespace {
bool fail(QString &error, const QString &message) { error = message; return false; }
QString newUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
int find(const QVector<ProtocolProfile> &profiles, const QString &id) {
    for (int i = 0; i < profiles.size(); ++i) if (profiles[i].id == id) return i;
    return -1;
}
QString availableName(const QString &name, const QSet<QString> &names, bool forceSuffix = false) {
    if (!forceSuffix && !names.contains(name)) return name;
    for (int i = 1; ; ++i) {
        const QString suffix = QStringLiteral("_%1").arg(i);
        const QString candidate = name.left(64 - suffix.size()) + suffix;
        if (!names.contains(candidate)) return candidate;
    }
}
QSet<QString> namesOf(const QVector<ProtocolProfile> &profiles, const QString &except = {}) {
    QSet<QString> names;
    for (const auto &profile : profiles) if (profile.id != except) names.insert(profile.name);
    return names;
}
bool remap(ProtocolSelection &selection, const QHash<QString, QString> &mapping, QString &error) {
    if (selection.kind != "custom") return true;
    if (!mapping.contains(selection.id)) return fail(error, QStringLiteral("导入选择缺少目标方案"));
    selection.id = mapping.value(selection.id); return true;
}
} // namespace

ProtocolProfileStore::ProtocolProfileStore(const QString &path) : m_path(path) {}
QString ProtocolProfileStore::defaultPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("custom_protocols.json");
}
ProtocolSnapshot ProtocolProfileStore::legacySelections(QSettings &main, QSettings &advanced) {
    auto readIndex = [](const QVariant &value, int fallback) {
        bool ok = false; const int index = value.toInt(&ok);
        return ok && index >= 0 && index <= 3 ? index : fallback;
    };
    const int fallback = readIndex(advanced.value("Protocol", 1), 1);
    const int shared = main.contains("comboProtocol") ? readIndex(main.value("comboProtocol"), fallback) : fallback;
    const bool endian = advanced.value("Endianness", 0).toInt() == 1;
    ProtocolSnapshot result;
    result.shared = ProtocolSelection::fromLegacy(shared, endian);
    result.ble = result.shared;
    return result; // No writes; old settings remain usable until UI migration.
}
bool ProtocolProfileStore::readFile(const QString &path, QByteArray &bytes, QString &error) {
    error.clear(); QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
    if (file.size() > MaxDocumentBytes) return fail(error, QStringLiteral("文件超过 2 MiB：%1").arg(path));
    const auto result = file.read(MaxDocumentBytes + 1LL);
    if (file.error() != QFile::NoError || result.size() > MaxDocumentBytes)
        return fail(error, QStringLiteral("文件读取失败或超过限额：%1").arg(path));
    bytes = result; return true;
}
bool ProtocolProfileStore::writeFile(const QString &path, const QByteArray &bytes, QString &error) {
    error.clear();
    if (bytes.isEmpty() || bytes.size() > MaxDocumentBytes) return fail(error, QStringLiteral("保存数据为空或超过 2 MiB"));
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("无法安全保存 %1：%2").arg(path, file.errorString()));
    return true;
}
bool ProtocolProfileStore::load(QString &error, const ProtocolSnapshot &defaults) {
    error.clear();
    ProtocolSnapshot candidate;
    QByteArray original;
    if (!QFileInfo::exists(m_path)) {
        if (!ProtocolProfileValidator::snapshot(defaults, candidate, error)) return false;
    } else {
        QByteArray bytes;
        if (!readFile(m_path, bytes, error) || !ProtocolProfileValidator::decode(bytes, true, candidate, error)) {
            // Keep the last in-memory snapshot. Block saving over an invalid
            // file until explicit restore, rather than treating it as empty.
            m_ready = false; return false;
        }
        original = bytes;
    }
    m_snapshot = candidate; m_ready = true;
    if (original.isEmpty() && defaults.shared.kind == "builtin" && defaults.shared.id == "builtin.amu4") return commit(candidate, error);
    if (!original.isEmpty()) {
        const auto canonical = ProtocolProfileValidator::encode(candidate, true, error);
        if (QJsonDocument::fromJson(original) != QJsonDocument::fromJson(canonical)) return commit(candidate, error);
    }
    return true;
}
bool ProtocolProfileStore::commit(const ProtocolSnapshot &input, QString &error) {
    error.clear();
    if (!m_ready) return fail(error, QStringLiteral("协议库尚未加载或文件损坏，请先加载或恢复备份"));
    ProtocolSnapshot candidate;
    if (!ProtocolProfileValidator::snapshot(input, candidate, error)) return false;
    const auto bytes = ProtocolProfileValidator::encode(candidate, true, error);
    if (bytes.isEmpty()) return false;
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) return fail(error, QStringLiteral("无法创建协议库目录"));
    QLockFile lock(m_path + ".lock");
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("协议库正被其他实例保存，请稍后重试"));
    // A last-valid backup is committed before replacing the live file. An
    // externally damaged file is never copied over the good backup.
    if (QFileInfo::exists(m_path)) {
        QByteArray previous; ProtocolSnapshot checked;
        if (!readFile(m_path, previous, error) || !ProtocolProfileValidator::decode(previous, true, checked, error)) {
            m_ready = false; return false;
        }
        const auto diskSnapshot = ProtocolProfileValidator::encode(checked, true, error);
        const auto loadedSnapshot = ProtocolProfileValidator::encode(m_snapshot, true, error);
        if (diskSnapshot != loadedSnapshot) {
            m_ready = false;
            return fail(error, QStringLiteral("协议库已被其他实例修改，请重新加载后保存"));
        }
        if (!writeFile(m_path + ".bak", previous, error)) return false;
    }
    if (!writeFile(m_path, bytes, error)) return false;
    m_snapshot = candidate; return true;
}
bool ProtocolProfileStore::restoreBackup(QString &error) {
    QLockFile lock(m_path + ".lock");
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("协议库正被其他实例保存，请稍后重试"));
    QByteArray backup; ProtocolSnapshot candidate;
    if (!readFile(m_path + ".bak", backup, error) || !ProtocolProfileValidator::decode(backup, true, candidate, error)) return false;
    if (QFileInfo::exists(m_path)) {
        const QString preserved = m_path + ".before-restore-" + newUuid();
        if (!QFile::copy(m_path, preserved)) return fail(error, QStringLiteral("无法保留恢复前的文件：%1").arg(preserved));
    }
    if (!writeFile(m_path, backup, error)) return false;
    m_snapshot = candidate; m_ready = true; return true;
}
bool ProtocolProfileStore::saveProfile(const QJsonObject &definition, QString &id, QString &error) {
    auto value = definition;
    if (!value.contains("id")) value.insert("id", newUuid());
    if (!value.contains("revision")) value.insert("revision", 1);
    ProtocolProfile profile;
    ProtocolProfile checked;
    if (!ProtocolProfileValidator::profile(value, checked, error)
        || !ProtocolProfileValidator::profile(ProtocolProfileValidator::standardizeBinary(checked.definition, true), profile, error)) return false;
    auto candidate = m_snapshot; const int index = find(candidate.profiles, profile.id);
    if (index >= 0) {
        auto comparable = profile.definition; comparable.insert("revision", candidate.profiles[index].revision);
        if (comparable != candidate.profiles[index].definition) {
            if (candidate.profiles[index].revision == std::numeric_limits<int>::max()) return fail(error, QStringLiteral("方案修订号已达到上限"));
            profile.revision = candidate.profiles[index].revision + 1;
        } else profile.revision = candidate.profiles[index].revision;
        profile.definition.insert("revision", profile.revision); candidate.profiles[index] = profile;
    } else {
        profile.revision = 1; profile.definition.insert("revision", 1); candidate.profiles.append(profile);
    }
    if (!commit(candidate, error)) return false;
    id = profile.id; return true;
}
bool ProtocolProfileStore::copyProfile(const QString &id, QString &newId, QString &error) {
    const int index = find(m_snapshot.profiles, id);
    if (index < 0) return fail(error, QStringLiteral("找不到要复制的方案"));
    auto value = m_snapshot.profiles[index].definition;
    value.insert("id", newUuid()); value.insert("revision", 1);
    value.insert("name", availableName(m_snapshot.profiles[index].name, namesOf(m_snapshot.profiles), true));
    ProtocolProfile copy;
    if (!ProtocolProfileValidator::profile(value, copy, error)) return false;
    auto candidate = m_snapshot; candidate.profiles.insert(index + 1, copy);
    // Insert and persist in one atomic commit; never append then reorder via
    // a second write that could leave the copy in the wrong position.
    if (!commit(candidate, error)) return false;
    newId = copy.id; return true;
}
bool ProtocolProfileStore::removeProfile(const QString &id, QString &error) {
    if ((m_snapshot.shared.kind == "custom" && m_snapshot.shared.id == id)
        || (m_snapshot.ble.kind == "custom" && m_snapshot.ble.id == id))
        return fail(error, QStringLiteral("协议被全局当前选择引用，请先切换到其他协议"));
    auto candidate = m_snapshot; const int index = find(candidate.profiles, id);
    if (index < 0) return fail(error, QStringLiteral("找不到要删除的方案"));
    candidate.profiles.removeAt(index); return commit(candidate, error);
}
bool ProtocolProfileStore::moveProfile(const QString &id, int delta, QString &error) {
    auto candidate = m_snapshot; const int index = find(candidate.profiles, id);
    if (index < 0 || (delta != -1 && delta != 1) || index + delta < 0 || index + delta >= candidate.profiles.size())
        return fail(error, QStringLiteral("方案不能移动到指定位置"));
    candidate.profiles.swapItemsAt(index, index + delta); return commit(candidate, error);
}
bool ProtocolProfileStore::setSelection(const ProtocolSelection &selection, QString &error) {
    auto candidate = m_snapshot; candidate.shared = selection; candidate.ble = selection;
    return commit(candidate, error);
}
QString ProtocolProfileStore::defaultExportFileName(const QStringList &ids) const {
    if (ids.isEmpty()) return QStringLiteral("WaveLink_Protocols.json");
    QStringList names; QSet<QString> seen;
    for (const auto &id : ids) {
        const int index = find(m_snapshot.profiles, id);
        if (index < 0 || seen.contains(id)) continue;
        seen.insert(id);
        QString name = m_snapshot.profiles[index].name.trimmed();
        // Keep Chinese and other Unicode names, but never allow a name to
        // introduce a path or characters forbidden in Windows file names.
        for (int i = 0; i < name.size(); ++i)
            if (name[i].unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(name[i])) name[i] = '_';
        while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
        names << (name.isEmpty() ? QStringLiteral("未命名协议") : name);
    }
    QString suffix = names.isEmpty() ? QStringLiteral("所选协议") : names.join('_');
    // Multi-selection may contain hundreds of profiles. Bound the component
    // length without splitting a UTF-16 surrogate pair or losing the suffix.
    if (suffix.size() > 160) {
        const auto count = QStringLiteral("_等%1个协议").arg(names.size());
        int end = 160 - count.size();
        if (suffix.at(end - 1).isHighSurrogate()) --end;
        suffix = suffix.left(end) + count;
    }
    return QStringLiteral("WaveLink_Protocols_%1.json").arg(suffix);
}
bool ProtocolProfileStore::exportProfiles(const QString &path, const QStringList &ids, QString &error) const {
    if (!m_ready) return fail(error, QStringLiteral("协议库尚未加载或损坏，请先加载或恢复备份"));
    auto resolved = [](const QString &path) {
        const QFileInfo file(path);
        return file.exists() ? file.canonicalFilePath() : QDir::cleanPath(file.absoluteFilePath());
    };
    // Exporting a share document over the live store would change its kind
    // and make the next startup fail. Protect the store and its backup.
#ifdef Q_OS_WIN
    const auto sensitivity = Qt::CaseInsensitive;
#else
    const auto sensitivity = Qt::CaseSensitive;
#endif
    if (resolved(path).compare(resolved(m_path), sensitivity) == 0
        || resolved(path).compare(resolved(m_path + ".bak"), sensitivity) == 0
        || resolved(path).compare(resolved(m_path + ".lock"), sensitivity) == 0)
        return fail(error, QStringLiteral("请选择其他导出位置，不能覆盖本地协议库及其备份/锁文件"));
    ProtocolSnapshot selected; QSet<QString> seen;
    if (ids.isEmpty()) selected.profiles = m_snapshot.profiles;
    else for (const auto &id : ids) {
        const int index = find(m_snapshot.profiles, id);
        if (index < 0 || seen.contains(id)) return fail(error, QStringLiteral("导出方案不存在或被重复指定"));
        seen.insert(id); selected.profiles.append(m_snapshot.profiles[index]);
    }
    const auto bytes = ProtocolProfileValidator::encode(selected, false, error);
    return !bytes.isEmpty() && writeFile(path, bytes, error);
}
bool ProtocolProfileStore::prepareImport(const ProtocolSnapshot &current, const QByteArray &bytes,
    bool storeDocument, ProtocolImportMode mode, bool restoreSelections, ProtocolImportPreview &preview, QString &error) {
    ProtocolSnapshot target, source;
    if (!ProtocolProfileValidator::snapshot(current, target, error)
        || !ProtocolProfileValidator::decode(bytes, storeDocument, source, error)) return false;
    if (restoreSelections && !storeDocument) return fail(error, QStringLiteral("独立协议文件不能恢复接口选择"));
    if (mode != ProtocolImportMode::Append && mode != ProtocolImportMode::MergeById && mode != ProtocolImportMode::Replace)
        return fail(error, QStringLiteral("未知协议导入策略"));
    ProtocolImportPreview result;
    if (mode == ProtocolImportMode::Replace) {
        result.removed = target.profiles.size(); target.profiles.clear();
    }
    for (auto profile : source.profiles) {
        const QString originalId = profile.id;
        if (mode == ProtocolImportMode::Append) {
            profile.id = newUuid(); profile.revision = 1;
            profile.definition.insert("id", profile.id); profile.definition.insert("revision", profile.revision);
        }
        const int index = find(target.profiles, profile.id);
        profile.name = availableName(profile.name, namesOf(target.profiles, profile.id));
        profile.definition.insert("name", profile.name);
        if (index < 0) { target.profiles.append(profile); ++result.added; }
        else {
            auto comparable = profile.definition; comparable.insert("revision", target.profiles[index].revision);
            if (comparable != target.profiles[index].definition) {
                if (target.profiles[index].revision == std::numeric_limits<int>::max()) return fail(error, QStringLiteral("方案修订号已达到上限"));
                profile.revision = target.profiles[index].revision + 1;
                ++result.updated;
            } else profile.revision = target.profiles[index].revision;
            profile.definition.insert("revision", profile.revision); target.profiles[index] = profile;
        }
        result.idMap.insert(originalId, profile.id);
    }
    if (restoreSelections) {
        target.shared = source.shared; target.ble = source.ble;
        if (!remap(target.shared, result.idMap, error) || !remap(target.ble, result.idMap, error)) return false;
    }
    if (!ProtocolProfileValidator::snapshot(target, result.snapshot, error)) return false;
    preview = result; return true;
}
} // namespace DataProtocols

