#include "ProtocolConfigTransfer.h"
#include "../core/commandsliderconfig.h"
#include "../core/commandswitchconfig.h"
#include "../core/commandpanelconfig.h"
#include "../ui/statuspanelmodel.h"
#include <QSettings>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDataStream>
#include <QLockFile>
#include <QUuid>

namespace DataProtocols {
namespace {
constexpr qint64 Limit = 32 * 1024 * 1024;
bool fail(QString &error, const QString &message) { error = message; return false; }
bool readBytes(const QString &path, QByteArray &bytes, QString &error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > Limit) return fail(error, "无法读取或文件超过 32 MiB：" + path);
    bytes = f.read(Limit + 1);
    return (f.error() == QFile::NoError && bytes.size() <= Limit) || fail(error, "读取失败：" + path);
}
bool writeBytes(const QString &path, const QByteArray &bytes, QString &error) {
    if (bytes.size() > Limit) return fail(error, "配置或恢复记录超过 32 MiB，未写入：" + path);
    QSaveFile f(path); f.setDirectWriteFallback(false);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) return fail(error, "无法安全写入：" + path + "\n" + f.errorString());
    return true;
}
QVariantMap capture(QSettings &settings) {
    QVariantMap map; settings.sync(); for (const auto &key : settings.allKeys()) map.insert(key, settings.value(key)); return map;
}
QString pack(const QVariantMap &values) {
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_5_12); stream << values;
    return QString::fromLatin1(bytes.toBase64());
}
bool unpack(const QJsonValue &value, QVariantMap &values) {
    const auto bytes = QByteArray::fromBase64(value.toString().toLatin1()); QDataStream stream(bytes); stream.setVersion(QDataStream::Qt_5_12); stream >> values;
    return stream.status() == QDataStream::Ok && stream.atEnd();
}
bool put(QSettings &settings, const QVariantMap &values) {
    settings.clear(); for (auto it = values.cbegin(); it != values.cend(); ++it) settings.setValue(it.key(), it.value()); settings.sync();
    return settings.status() == QSettings::NoError;
}
QString journalPath(const QString &storePath) { return QDir(QFileInfo(storePath).absolutePath()).filePath("global-config-transaction.json"); }
QString statusFile(const QString &commandPath,const QString &requested) {return requested.isEmpty()?QDir(QFileInfo(commandPath).absolutePath()).filePath("status_panels.json"):requested;}
bool protocolKey(QString key) {
    if (key.startsWith("WaveLink/")) key.remove(0, 9);
    else if (key.startsWith("AdvancedApp/")) key.remove(0, 12);
    return key == "comboProtocol" || key == "Protocol" || key == "Endianness" || key == "Ble/protocol";
}
bool localCacheKey(QString key) {
    // These names were used in different settings stores and old flat INI
    // files. Keep all variants local, including when importing older exports.
    for (const auto &prefix : {QStringLiteral("WaveLink/"), QStringLiteral("AdvancedApp/"), QStringLiteral("MyCompany/")}) {
        if (key.startsWith(prefix)) { key.remove(0, prefix.size()); break; }
    }
    return key == "editCacheDir" || key == "CacheDir";
}
bool restoreJournal(const QString &path, const QStringList &filePaths, QString &error) {
    QByteArray bytes; if (!readBytes(path, bytes, error)) return false;
    QJsonParseError parse; const auto doc = QJsonDocument::fromJson(bytes, &parse); const auto root = doc.object();
    QVariantMap mainValues, advValues, myValues;
    if (parse.error != QJsonParseError::NoError || root.value("schema").toInt() != 1 ||
        !unpack(root.value("main"), mainValues) || !unpack(root.value("advanced"), advValues) || !unpack(root.value("my"), myValues))
        return fail(error, "配置导入恢复记录损坏；已保留文件：" + path);
    const auto files = root.value("files").toArray();
    if (files.size() != filePaths.size()) return fail(error, "配置导入恢复记录的文件数量无效");
    // Validate ALL targets and concurrent changes before restoring any of them.
    for (int i = 0; i < files.size(); ++i) {
        const auto entry = files[i].toObject();
        if (entry.value("path").toString() != QFileInfo(filePaths[i]).absoluteFilePath()) return fail(error, "恢复记录的路径与当前工程配置目录不一致，未覆盖文件");
        if (QFile::exists(filePaths[i])) {
            QByteArray current; if (!readBytes(filePaths[i], current, error)) return false;
            if (current != QByteArray::fromBase64(entry.value("before").toString().toLatin1()) &&
                current != QByteArray::fromBase64(entry.value("after").toString().toLatin1()))
                return fail(error, "检测到导入后另有修改，无法自动覆盖，请保留并检查恢复记录：" + path);
        }
    }
    for (int i = 0; i < files.size(); ++i) {
        const auto entry = files[i].toObject();
        if (entry.value("existed").toBool()) {
            if (!writeBytes(filePaths[i], QByteArray::fromBase64(entry.value("before").toString().toLatin1()), error)) return false;
        } else if (QFile::exists(filePaths[i]) && !QFile::remove(filePaths[i])) return fail(error, "无法恢复导入前的文件状态：" + filePaths[i]);
    }
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"), my("MyCompany", "WaveLink");
    const bool a = put(main, mainValues), b = put(advanced, advValues), c = put(my, myValues);
    if (!a || !b || !c) return fail(error, "配置恢复写入失败，恢复记录已保留");
    if (!QFile::remove(path)) return fail(error, "配置已恢复，但无法移除恢复记录：" + path);
    return true;
}
}
bool ProtocolConfigTransfer::read(const QString &path, QVariantMap &values, QString &error) {
    QByteArray bytes; if (!readBytes(path, bytes, error)) return false;
    QSettings settings(path, QSettings::IniFormat); values = capture(settings);
    if (settings.status() != QSettings::NoError || values.isEmpty()) return fail(error, "配置文件无效或为空");
    return true;
}
bool ProtocolConfigTransfer::exportFile(const QString &path, const ProtocolSnapshot &snapshot, const QString &commandPath, QString &error,const QString &requestedStatusPath) {
    const auto statusPath=statusFile(commandPath,requestedStatusPath);
    auto resolved = [](const QString &p) { const QFileInfo f(p); return f.exists() ? f.canonicalFilePath() : f.absoluteFilePath(); };
    const QStringList protectedPaths{ProtocolProfileStore::defaultPath(), ProtocolProfileStore::defaultPath() + ".bak", ProtocolProfileStore::defaultPath()+".lock", commandPath, commandPath + ".bak", commandPath+".lock", statusPath,statusPath+".bak",statusPath+".ui.ini",statusPath+".lock", journalPath(ProtocolProfileStore::defaultPath())};
    for (const auto &p : protectedPaths) if (resolved(path).compare(resolved(p), Qt::CaseInsensitive) == 0) return fail(error, "不能将导出文件覆盖软件的内部配置文件");
    const auto protocols = ProtocolProfileValidator::encode(snapshot, true, error); if (protocols.isEmpty()) return false;
    StatusPanels::Store statusStore(statusPath);QVector<StatusPanels::Panel> statusPanels;
    if(!statusStore.load(statusPanels,error))return false;
    const auto statusBytes=StatusPanels::encode(statusPanels,error);if(statusBytes.isEmpty())return false;
    // QSettings formats only a temporary file; QSaveFile publishes it atomically.
    QTemporaryDir dir; if (!dir.isValid()) return fail(error, "无法创建配置导出临时目录");
    const auto staged = dir.filePath("settings.ini");
    {
        QSettings dest(staged, QSettings::IniFormat), main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"), my("MyCompany", "WaveLink");
        const QVector<QPair<QString, QSettings *>> sources{{"WaveLink/", &main}, {"AdvancedApp/", &advanced}, {"MyCompany/", &my}};
        // The protocol library is the sole authority for the global choice.
        // Do not export stale per-interface or duplicate legacy selections.
        for (const auto &source : sources) {
            const auto values = capture(*source.second);
            for (auto it = values.cbegin(); it != values.cend(); ++it)
                if (!protocolKey(source.first + it.key()) && !localCacheKey(source.first + it.key()))
                    dest.setValue(source.first + it.key(), it.value());
        }
        if (main.status() != QSettings::NoError || advanced.status() != QSettings::NoError || my.status() != QSettings::NoError) return fail(error, "现有配置读取失败，未导出不完整配置");
        dest.setValue("DataProtocols/json", QString::fromUtf8(protocols));
        dest.setValue("StatusPanel/json",QString::fromUtf8(statusBytes));
        QSettings statusUi(statusPath+".ui.ini",QSettings::IniFormat);statusUi.sync();
        if(statusUi.status()!=QSettings::NoError)return fail(error,"状态面板窗口偏好读取失败");
        dest.setValue("StatusPanel/selectedId",statusUi.value("selectedPanel"));dest.setValue("StatusPanel/geometry",statusUi.value("geometry"));
        if (QFile::exists(commandPath)) { QByteArray commands; if (!readBytes(commandPath, commands, error)) return false; dest.setValue("CommandPanel/json", QString::fromUtf8(commands)); }
        dest.sync(); if (dest.status() != QSettings::NoError) return fail(error, "配置导出临时写入失败");
    }
    QByteArray result; return readBytes(staged, result, error) && writeBytes(path, result, error);
}
bool ProtocolConfigTransfer::recover(const QString &storePath, const QString &commandPath, QString &error,const QString &requestedStatusPath) {
    const auto path = journalPath(storePath); if (!QFile::exists(path)) return true;
    QLockFile lock(path + ".lock"); if (!lock.tryLock(0)) return fail(error, "另一个实例正在导入或恢复配置");
    QLockFile commandLock(commandPath + ".lock"), storeLock(storePath + ".lock");
    if (!commandLock.tryLock(0) || !storeLock.tryLock(0))
        return fail(error, "命令或协议配置正在保存，未执行恢复，请稍后重试");
    const auto statusPath=statusFile(commandPath,requestedStatusPath);QLockFile statusLock(statusPath+".lock");
    if(!statusLock.tryLock(0))return fail(error,"状态配置正在保存，未执行恢复，请稍后重试");
    QByteArray bytes;if(!readBytes(path,bytes,error))return false;
    const auto count=QJsonDocument::fromJson(bytes).object()["files"].toArray().size();
    QStringList targets{storePath,storePath+".bak",commandPath};
    if(count!=3){targets<<statusPath<<statusPath+".bak"<<statusPath+".ui.ini";}
    return restoreJournal(path,targets,error);
}
bool ProtocolConfigTransfer::apply(const QVariantMap &values, ProtocolProfileStore &store, int policy,
                                   bool restoreSelections, int commandMode, const QString &commandPath, QString &error,int statusMode,const QString &requestedStatusPath) {
    if (!store.ready()) return fail(error, "协议库未就绪，无法导入全局配置");
    if (policy < -1 || policy > 2 || commandMode < 0 || commandMode > 2 || statusMode<0 || statusMode>2) return fail(error, "导入策略无效");
    // Share the normal command-library writer's lock before reading its old
    // contents, including append mode and the rollback snapshot.
    QLockFile commandLock(commandPath + ".lock");
    if (!commandLock.tryLock(0)) return fail(error, "命令配置正在保存，未修改任何配置，请稍后重试");
    const auto statusPath=statusFile(commandPath,requestedStatusPath);QLockFile statusLock(statusPath+".lock");
    if(!statusLock.tryLock(0))return fail(error,"状态配置正在保存，未修改任何配置，请稍后重试");
    ProtocolSnapshot candidate = store.snapshot();
    QHash<QString,QString> protocolIds;ProtocolSnapshot importedProtocols;
    if (values.contains("DataProtocols/json")) {
        // Explicitly skipped protocol data must not prevent importing other
        // settings/command panels, even when the embedded library is invalid.
        if (policy >= 0) {
            const auto bytes = values.value("DataProtocols/json").toString().toUtf8();
            ProtocolImportPreview preview;
            if (!ProtocolProfileStore::prepareImport(candidate, bytes, true, static_cast<ProtocolImportMode>(policy), restoreSelections, preview, error)) return false;
            candidate = preview.snapshot;
            protocolIds=preview.idMap;
            if(!ProtocolProfileValidator::decode(bytes,true,importedProtocols,error))return false;
        }
    } else if (restoreSelections && policy >= 0) {
        auto legacy = [&](const QString &modern, const QString &old, int fallback) {
            const QVariant v = values.contains(modern) ? values.value(modern) : values.value(old, fallback);
            bool ok = false; const int n = v.toInt(&ok); return ok && n >= 0 && n <= 3 ? n : fallback;
        };
        const bool hasEndian = values.contains("AdvancedApp/Endianness") || values.contains("Endianness");
        const bool endian = values.value("AdvancedApp/Endianness", values.value("Endianness", 0)).toInt() == 1;
        if (values.contains("WaveLink/comboProtocol") || values.contains("comboProtocol") || values.contains("AdvancedApp/Protocol") || values.contains("Protocol"))
            candidate.shared = ProtocolSelection::fromLegacy(legacy("WaveLink/comboProtocol", "comboProtocol", legacy("AdvancedApp/Protocol", "Protocol", 1)), endian);
        else if (hasEndian && candidate.shared.id == "builtin.justfloat") candidate.shared.bigEndian = endian;
        candidate.ble = candidate.shared;
    }
    ProtocolSnapshot normalizedCandidate;
    if (!ProtocolProfileValidator::snapshot(candidate, normalizedCandidate, error)) return false;
    candidate = normalizedCandidate;
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"), my("MyCompany", "WaveLink");
    const auto oldMain = capture(main), oldAdv = capture(advanced), oldMy = capture(my);
    if (main.status() != QSettings::NoError || advanced.status() != QSettings::NoError || my.status() != QSettings::NoError) return fail(error, "当前配置读取失败");
    auto nextMain = oldMain, nextAdv = oldAdv, nextMy = oldMy;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto key = it.key();
        if (key.startsWith("DataProtocols/") || key.startsWith("CommandPanel/") || key.startsWith("StatusPanel/") || protocolKey(key) || localCacheKey(key)) continue;
        if (key.startsWith("WaveLink/")) nextMain.insert(key.mid(9), it.value());
        else if (key.startsWith("AdvancedApp/")) nextAdv.insert(key.mid(12), it.value());
        else if (key.startsWith("MyCompany/")) nextMy.insert(key.mid(10), it.value());
        else nextMain.insert(key, it.value()); // pre-namespace INI compatibility
    }
    nextMain.remove("Ble/protocol");
    if (policy >= 0 && restoreSelections) {
        nextMain.insert("comboProtocol", candidate.shared.kind == "custom" ? 4 : candidate.shared.legacyIndex());
        nextAdv.insert("Protocol", candidate.shared.kind == "custom" ? 4 : candidate.shared.legacyIndex()); nextAdv.insert("Endianness", candidate.shared.bigEndian ? 1 : 0);
    }
    QByteArray nextCommands;
    const bool changeCommands = commandMode != 0 && values.contains("CommandPanel/json");
    if (changeCommands) {
        QJsonArray imported; QStringList notices;
        if (!CommandPanelConfig::decode(values.value("CommandPanel/json").toString().toUtf8(), imported, error, notices)) return false;
        QJsonArray combined;
        if (commandMode == 2 && QFile::exists(commandPath)) {
            QByteArray bytes; if (!readBytes(commandPath, bytes, error) || !CommandPanelConfig::decode(bytes, combined, error, notices)) return false;
        }
        for (const auto &v : imported) {
            if (!v.isObject()) return fail(error, "命令面板项目无效"); auto obj = v.toObject();
            if (commandMode == 2) {
                const auto name = obj.value("name").toString(); QString finalName = name; int suffix = 1;
                auto exists = [&] { for (const auto &p : combined) if (p.toObject().value("name").toString() == finalName) return true; return false; };
                while (exists()) finalName = name + "_" + QString::number(suffix++);
                obj.insert("name", finalName); obj.insert("id", QUuid::createUuid().toString());
            }
            combined.append(obj);
        }
        nextCommands = QJsonDocument(combined).toJson();
        for (const auto &panel : combined) for (const auto &command : panel.toObject()["commands"].toArray()) {
            QString cause;
            if (!CommandSliderConfig::validate(command.toObject(), cause) || !CommandSwitchConfig::validate(command.toObject(), cause))
                return fail(error, QString("命令“%1”的参数无效：%2；未修改配置").arg(command.toObject()["name"].toString(), cause));
        }
    }
    const bool changeStatus=statusMode!=0&&values.contains("StatusPanel/json");QByteArray nextStatus,nextStatusUi;
    if(changeStatus){
        QVector<StatusPanels::Panel> imported,combined;
        if(!StatusPanels::decode(values.value("StatusPanel/json").toString().toUtf8(),imported,error))return false;
        if(statusMode==2){StatusPanels::Store current(statusPath);if(!current.load(combined,error))return false;}
        QStringList names;for(const auto &p:combined)names<<p.name;
        for(auto panel:imported){
            auto remap=[&](QString &id,QByteArray &shape){if(protocolIds.contains(id)){
                const auto originalId=id;const auto mapped=protocolIds.value(originalId);
                // Rebind only when the saved structure matches the imported definition.
                // Stale/missing references stay visibly mismatched, never silently follow ChN.
                for(const auto &source:importedProtocols.profiles)if(source.id==originalId){
                    ProtocolSelection selection;selection.kind="custom";selection.id=originalId;
                    if(shape==StatusMonitoring::Session::structure(selection,source))for(const auto &target:candidate.profiles)if(target.id==mapped){selection.id=mapped;shape=StatusMonitoring::Session::structure(selection,target);break;}
                    break;
                }
                id=mapped;
            }};
            remap(panel.protocolId,panel.protocolShape);
            for(auto &rule:panel.items)if(rule.source==StatusMonitoring::Source::Channel)remap(rule.protocolId,rule.protocolShape);
            if(statusMode==2){panel=StatusPanels::duplicateForAppend(panel,names);}
            names<<panel.name;combined.append(panel);
        }
        nextStatus=StatusPanels::encode(combined,error);if(nextStatus.isEmpty())return false;
        QTemporaryDir temp;if(!temp.isValid())return fail(error,"无法创建状态面板偏好临时目录");
        {QSettings existing(statusPath+".ui.ini",QSettings::IniFormat),staged(temp.filePath("ui.ini"),QSettings::IniFormat);auto prefs=capture(existing);
            if(existing.status()!=QSettings::NoError)return fail(error,"状态面板窗口偏好读取失败");
            if(statusMode==1){prefs["selectedPanel"]=values.value("StatusPanel/selectedId");if(values.contains("StatusPanel/geometry"))prefs["geometry"]=values.value("StatusPanel/geometry");}
            bool found=false;for(const auto &p:combined)if(p.id==prefs.value("selectedPanel").toString())found=true;
            if(!found)prefs["selectedPanel"]=combined.isEmpty()?QString():combined.first().id;
            if(!put(staged,prefs))return fail(error,"状态面板窗口偏好暂存失败");}
        if(!readBytes(temp.filePath("ui.ini"),nextStatusUi,error))return false;
    }
    const auto path = journalPath(store.path());
    QDir dir(QFileInfo(path).absolutePath()); if (!dir.mkpath(".")) return fail(error, "无法创建配置目录");
    QLockFile lock(path + ".lock"); if (!lock.tryLock(0)) return fail(error, "另一个实例正在导入配置");
    if (QFile::exists(path)) return fail(error, "存在尚未恢复的配置导入，请先重启或处理：" + path);
    if (QFile::exists(store.path())) {
        QByteArray disk; ProtocolSnapshot diskSnapshot;
        if (!ProtocolProfileStore::readFile(store.path(), disk, error) || !ProtocolProfileValidator::decode(disk, true, diskSnapshot, error)) return false;
        if (ProtocolProfileValidator::encode(diskSnapshot, true, error) != ProtocolProfileValidator::encode(store.snapshot(), true, error)) {
            QString reloadError; store.load(reloadError);
            return fail(error, "协议库已被其他实例修改，请重新预览导入；未修改任何配置");
        }
    }
    const auto protocolBytes = ProtocolProfileValidator::encode(candidate, true, error); if (protocolBytes.isEmpty()) return false;
    const bool changeStore = protocolBytes != ProtocolProfileValidator::encode(store.snapshot(), true, error);
    QJsonArray files; const QStringList targets{store.path(), store.path() + ".bak", commandPath,statusPath,statusPath+".bak",statusPath+".ui.ini"}; QByteArray oldProtocol,oldStatus;
    for (int i = 0; i < targets.size(); ++i) {
        QByteArray before; const bool existed = QFile::exists(targets[i]); if (existed && !readBytes(targets[i], before, error)) return false;
        if (i == 0) oldProtocol = before;
        if(i==3)oldStatus=before;
        auto after=before;
        if(i==0&&changeStore)after=protocolBytes;else if(i==1&&changeStore&&!oldProtocol.isEmpty())after=oldProtocol;
        else if(i==2&&changeCommands)after=nextCommands;else if(i==3&&changeStatus)after=nextStatus;
        else if(i==4&&changeStatus&&!oldStatus.isEmpty()){QVector<StatusPanels::Panel> valid;QString ignored;if(StatusPanels::decode(oldStatus,valid,ignored))after=oldStatus;}
        else if(i==5&&changeStatus)after=nextStatusUi;
        files.append(QJsonObject{{"path", QFileInfo(targets[i]).absoluteFilePath()}, {"existed", existed}, {"before", QString::fromLatin1(before.toBase64())}, {"after", QString::fromLatin1(after.toBase64())}});
    }
    QJsonObject journal{{"schema", 1}, {"main", pack(oldMain)}, {"advanced", pack(oldAdv)}, {"my", pack(oldMy)}, {"files", files}};
    if (!writeBytes(path, QJsonDocument(journal).toJson(), error)) return false;
    // Commit the library first: its optimistic concurrency check rejects stale
    // edits before any settings or command-file writes take place.
    bool ok = !changeStore || store.commit(candidate, error);
    if (!ok && !store.ready()) {
        // Validation/concurrency failure occurs before the store writes. Do
        // not restore a stale snapshot over a different instance's new file.
        if (!QFile::remove(path)) return fail(error, error + "\n无法移除未执行的事务记录：" + path);
        QString reloadError; store.load(reloadError);
        return false;
    }
    if (ok && changeCommands) ok = writeBytes(commandPath, nextCommands, error);
    if(ok&&changeStatus){
        if(!oldStatus.isEmpty()){QVector<StatusPanels::Panel> valid;QString ignored;if(StatusPanels::decode(oldStatus,valid,ignored))ok=writeBytes(statusPath+".bak",oldStatus,error);}
        if(ok)ok=writeBytes(statusPath,nextStatus,error);
        if(ok)ok=writeBytes(statusPath+".ui.ini",nextStatusUi,error);
    }
    if (ok) { const bool a = put(main, nextMain), b = put(advanced, nextAdv), c = put(my, nextMy); ok = a && b && c; if (!ok) error = "配置写入失败"; }
    if (ok && QFile::remove(path)) return true;
    if (ok) error = "配置已写入但无法完成事务标记，正在恢复导入前状态";
    const auto cause = error; QString rollbackError;
    QLockFile rollbackStoreLock(store.path() + ".lock");
    if (!rollbackStoreLock.tryLock(0)) return fail(error, cause + "\n协议配置正在保存，恢复记录已保留，请稍后重启恢复。");
    if (!restoreJournal(path, targets, rollbackError)) return fail(error, cause + "\n自动恢复失败：" + rollbackError);
    rollbackStoreLock.unlock();
    store.load(rollbackError);
    return fail(error, cause + "\n已恢复导入前配置。");
}
}
