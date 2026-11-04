#include "statuspanelmodel.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QLockFile>
#include <QStandardPaths>
#include <QUuid>
#include <QSet>
#include <QDateTime>
#include <cmath>
using namespace StatusMonitoring;
namespace StatusPanels {
QString newId(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
void normalizeBinding(Panel &p){
    if(p.bindingMode==BindingMode::Legacy){
        const Rule *first=nullptr;for(const auto &r:p.items)if(r.source==Source::Channel){if(!first)first=&r;else if(first->protocolId!=r.protocolId||first->protocolShape!=r.protocolShape)return;}
        p.protocolId=first?first->protocolId:QString();p.protocolShape=first?first->protocolShape:QByteArray();p.bindingMode=p.protocolId.isEmpty()?BindingMode::Follow:BindingMode::Locked;
    }
    for(auto &r:p.items)if(r.source==Source::Channel){r.protocolId=p.bindingMode==BindingMode::Locked?p.protocolId:QString();r.protocolShape=p.bindingMode==BindingMode::Locked?p.protocolShape:QByteArray();}
}
static QJsonObject bindingJson(const Panel &p){return {{"mode",p.bindingMode==BindingMode::Locked?"locked":p.bindingMode==BindingMode::Follow?"follow":"legacy"},{"protocolId",p.protocolId},{"protocolShape",QString::fromLatin1(p.protocolShape)},{"protocolName",p.protocolName}};}
static QJsonObject ruleJson(const Rule &r) {
    QJsonArray maps,ranges,rows;
    for(const auto &m:r.mappings) maps.append(QJsonObject{{"value",double(m.value)},{"state",m.state},{"color",m.color},{"severity",m.severity},{"note",m.note}});
    for(const auto &v:r.ranges) ranges.append(QJsonObject{{"lower",std::isfinite(v.lower)?QJsonValue(v.lower):QJsonValue()},{"upper",std::isfinite(v.upper)?QJsonValue(v.upper):QJsonValue()},
        {"includeLower",v.includeLower},{"includeUpper",v.includeUpper},{"state",v.state},{"retain",v.retainWithHysteresis},{"color",v.color},{"severity",v.severity},{"note",v.note}});
    for(const auto &m:r.textMatches) rows.append(QJsonObject{{"text",m.text},{"state",m.state},{"prefix",m.prefix},{"id",m.id},{"color",m.color},{"severity",m.severity},{"note",m.note},{"enabled",m.enabled}});
    const auto &t=r.textSource;
    return {{"id",r.id},{"name",r.name},{"note",r.note},{"source",r.source==Source::Channel?"channel":"text"},
        {"kind",QStringList{"mapping","bits","threshold","text"}.value(int(r.kind))},{"enabled",r.enabled},{"converted",r.converted},
        {"protocolId",r.protocolId},{"protocolShape",QString::fromLatin1(r.protocolShape)},{"channel",r.channel},{"width",r.width},{"firstBit",r.firstBit},{"bitCount",r.bitCount},
        {"timeoutMs",r.timeoutMs},{"holdMs",r.holdMs},{"hysteresis",r.hysteresis},{"mappings",maps},{"ranges",ranges},{"textMatches",rows},{"recordEveryMatch",r.recordEveryMatch},{"event",eventOptionsJson(r.event)},
        {"textSource",QJsonObject{{"id",t.id},{"delimiter",QString::fromLatin1(t.delimiter.toHex().toUpper())},{"encoding",t.encoding},{"datagram",t.datagram},{"trim",t.trim},
            {"caseSensitive",t.caseSensitive},{"strictSync",t.strictSync},{"maxBytes",t.maxBytes},{"partialTimeoutMs",t.partialTimeoutMs}}}};
}
static Rule readRule(const QJsonObject &o) {
    Rule r;r.id=o["id"].toString();r.name=o["name"].toString();r.note=o["note"].toString();
    r.source=o["source"]==QJsonValue("channel")?Source::Channel:Source::Text;
    r.kind=RuleKind(QStringList{"mapping","bits","threshold","text"}.indexOf(o["kind"].toString()));
    r.enabled=o["enabled"].toBool();r.converted=o["converted"].toBool();r.protocolId=o["protocolId"].toString();r.protocolShape=o["protocolShape"].toString().toLatin1();
    r.channel=o["channel"].toInt();r.width=o["width"].toInt();r.firstBit=o["firstBit"].toInt();r.bitCount=o["bitCount"].toInt();
    r.timeoutMs=o["timeoutMs"].toInt();r.holdMs=o["holdMs"].toInt();r.hysteresis=o["hysteresis"].toDouble();r.recordEveryMatch=o["recordEveryMatch"].toBool();
    if(o.contains("event"))r.event=readEventOptions(o["event"].toObject());else r.event.onChange=r.source==Source::Channel;
    for(const auto &v:o["mappings"].toArray()) {auto a=v.toObject();r.mappings.append({quint32(a["value"].toDouble()),a["state"].toString(),a["color"].toString(),a["severity"].toString(),a["note"].toString()});}
    for(const auto &v:o["ranges"].toArray()) {auto a=v.toObject();Range x;x.lower=a["lower"].isNull()?-std::numeric_limits<double>::infinity():a["lower"].toDouble();x.upper=a["upper"].isNull()?std::numeric_limits<double>::infinity():a["upper"].toDouble();
        x.includeLower=a["includeLower"].toBool();x.includeUpper=a["includeUpper"].toBool();x.state=a["state"].toString();x.retainWithHysteresis=a["retain"].toBool();x.color=a["color"].toString();x.severity=a["severity"].toString();x.note=a["note"].toString();r.ranges.append(x);}
    for(const auto &v:o["textMatches"].toArray()) {auto a=v.toObject();r.textMatches.append({a["text"].toString(),a["state"].toString(),a["prefix"].toBool(),a["id"].toString(),a["color"].toString(),a["severity"].toString(),a["note"].toString(),a["enabled"].toBool()});}
    auto a=o["textSource"].toObject();auto &t=r.textSource;t.id=a["id"].toString();t.delimiter=QByteArray::fromHex(a["delimiter"].toString().toLatin1());t.encoding=a["encoding"].toString();
    t.datagram=a["datagram"].toBool();t.trim=a["trim"].toBool();t.caseSensitive=a["caseSensitive"].toBool();t.strictSync=a["strictSync"].toBool();t.maxBytes=a["maxBytes"].toInt();t.partialTimeoutMs=a["partialTimeoutMs"].toInt();return r;
}
QVector<Rule> flatten(const QVector<Panel> &panels) {QVector<Rule> rules;for(auto p:panels){normalizeBinding(p);for(auto r:p.items) {r.enabled=r.enabled&&p.enabled;if(r.source==Source::Text) r.textSource.id=p.id+":"+(r.textSource.id.isEmpty()?r.id:r.textSource.id);rules.append(r);}}return rules;}
QVector<PanelLayout::Item> layoutItems(const Panel &panel)
{
    QVector<PanelLayout::Item> items;
    items.reserve(panel.items.size());
    for (const auto &rule : panel.items) items.append({rule.id, rule.name});
    return items;
}

Panel duplicate(const Panel &p)
{
    auto copy = p;
    copy.id = newId();
    copy.name += "_1";
    QHash<QString, QString> sources, newIds;
    for (auto &rule : copy.items) {
        const auto oldId = rule.id;
        rule.id = newId();
        newIds.insert(oldId, rule.id);
        if (!rule.textSource.id.isEmpty()) {
            if (!sources.contains(rule.textSource.id)) sources[rule.textSource.id] = newId();
            rule.textSource.id = sources[rule.textSource.id];
        }
        for (auto &match : rule.textMatches) match.id = newId();
    }
    PanelLayout::remap(copy.layoutRows, newIds);
    return copy;
}
Panel duplicateForAppend(const Panel &panel,const QStringList &names){auto copy=duplicate(panel);copy.name=panel.name;int suffix=1;while(names.contains(copy.name))copy.name=panel.name+"_"+QString::number(suffix++);return copy;}
bool protectedExportPath(const QString &path,const QString &storePath){
    const auto folder=QFileInfo(storePath).absoluteDir();
    QStringList protectedPaths{storePath,storePath+".bak",storePath+".ui.ini",storePath+".lock"};
    for(const auto &name:{"cmd_panels.json","cmd_panels.json.bak","cmd_panels.json.lock","custom_protocols.json","custom_protocols.json.bak","custom_protocols.json.lock","global-config-transaction.json","global-config-transaction.json.lock"})protectedPaths<<folder.filePath(name);
    auto resolved=[](const QString &value){QFileInfo file(value);return file.exists()?file.canonicalFilePath():file.absoluteFilePath();};
    for(const auto &p:protectedPaths)if(resolved(path).compare(resolved(p),Qt::CaseInsensitive)==0)return true;return false;
}
QByteArray encode(const QVector<Panel> &panels,QString &error) {
    error.clear();if(panels.size()>512) {error="面板数量超过 512。";return {};}
    QSet<QString> ids,items;QJsonArray array;
    for(auto p:panels) {
        normalizeBinding(p);
        if(p.protocolName.size()>1024||p.protocolId.size()>128||p.protocolShape.size()>1024||(p.bindingMode==BindingMode::Locked&&(p.protocolId.isEmpty()||p.protocolShape.isEmpty()||p.protocolId=="builtin.raw"))||(p.bindingMode!=BindingMode::Locked&&(!p.protocolId.isEmpty()||!p.protocolShape.isEmpty()||!p.protocolName.isEmpty()))){error="面板协议绑定无效。";return {};}
        if(p.id.isEmpty()||p.id.size()>128||ids.contains(p.id)||p.name.trimmed().isEmpty()||p.name.size()>1024||p.note.size()>32768||p.columns<1||p.columns>PanelLayout::StatusMaxColumns) {error="面板名称、ID 或布局无效。";return {};}
        ids.insert(p.id);QJsonArray rows;for(const auto &r:p.items) {
            if(items.contains(r.id)||r.note.size()>32768) {error="监视项 ID 重复或说明过长。";return {};}
            items.insert(r.id);rows.append(ruleJson(r));
        }
        if (p.layoutRows && PanelLayout::arrange(layoutItems(p), p.columns, *p.layoutRows) != *p.layoutRows) {
            error = QStringLiteral("面板“%1”的自定义分行无效。").arg(p.name);
            return {};
        }
        QJsonObject panel{{"id",p.id},{"name",p.name},{"note",p.note},{"enabled",p.enabled},{"columns",p.columns},{"binding",bindingJson(p)},{"items",rows}};
        PanelLayout::write(panel, p.layoutRows);
        array.append(panel);
    }
    if(!Engine::validate(flatten(panels),error)) return {};
    auto bytes=QJsonDocument(QJsonObject{{"fileType","WaveLink.StatusPanels"},{"schemaVersion",1},{"panels",array}}).toJson();
    if(bytes.size()>8*1024*1024) {error="状态面板配置超过 8 MiB。";return {};}
    return bytes;
}
bool decode(const QByteArray &bytes,QVector<Panel> &panels,QString &error) {
    if(bytes.size()>8*1024*1024) {error="配置文件超过 8 MiB。";return false;}
    QJsonParseError parse;auto doc=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError || !doc.isObject()) {error="JSON 无效："+parse.errorString();return false;}
    const auto o=doc.object();if(o["fileType"]!=QJsonValue("WaveLink.StatusPanels")||o["schemaVersion"]!=QJsonValue(1)||!o["panels"].isArray()) {error="不是受支持的状态面板配置。";return false;}
    int totalItems=0;if(o["panels"].toArray().size()>512){error="面板数量超过上限。";return false;}
    for(const auto &p:o["panels"].toArray()){const auto items=p.toObject()["items"].toArray();totalItems+=items.size();if(totalItems>1024){error="监视项数量超过上限。";return false;}for(const auto &r:items){const auto row=r.toObject();if(row["mappings"].toArray().size()>256||row["ranges"].toArray().size()>256||row["textMatches"].toArray().size()>256){error="规则行数超过上限。";return false;}}}
    QVector<Panel> candidate;
    for(const auto &v:o["panels"].toArray()) {const auto a=v.toObject();Panel p;p.id=a["id"].toString();p.name=a["name"].toString();p.note=a["note"].toString();p.enabled=a["enabled"].toBool();p.columns=a["columns"].toInt();
        for(const auto &r:a["items"].toArray()) {const auto obj=r.toObject();for(const auto &m:obj["mappings"].toArray()) {const auto n=m.toObject()["value"].toDouble(-1);if(n<0||n>4294967295.0||std::floor(n)!=n){error="映射整数越界。";return false;}}p.items.append(readRule(obj));}
        if(a.contains("binding")){auto b=a["binding"].toObject();const auto mode=b["mode"].toString();if(mode!="locked"&&mode!="follow"&&mode!="legacy"){error="面板协议绑定模式无效。";return false;}p.bindingMode=mode=="locked"?BindingMode::Locked:mode=="follow"?BindingMode::Follow:BindingMode::Legacy;p.protocolId=b["protocolId"].toString();p.protocolShape=b["protocolShape"].toString().toLatin1();p.protocolName=b["protocolName"].toString();}
        if (!PanelLayout::read(a.value("layoutRows"), layoutItems(p), p.columns, p.layoutRows, error)) return false;
        normalizeBinding(p);candidate.append(p);}
    const auto canonical=encode(candidate,error);if(canonical.isEmpty()) return false;
    // Event and panel binding blocks are optional only for older schema-1 files.
    // Keep strict comparison for every existing field and for all nested keys.
    auto normalized=o;auto oldPanels=o["panels"].toArray();const auto currentPanels=QJsonDocument::fromJson(canonical).object()["panels"].toArray();
    for(int p=0;p<oldPanels.size();++p){auto panel=oldPanels[p].toObject();if(!panel.contains("binding"))panel["binding"]=currentPanels[p].toObject()["binding"];if(!panel["items"].isArray())continue;auto items=panel["items"].toArray();const auto current=currentPanels[p].toObject()["items"].toArray();for(int i=0;i<items.size();++i){auto item=items[i].toObject();if(!item.contains("event"))item["event"]=current[i].toObject()["event"];items[i]=item;}panel["items"]=items;oldPanels[p]=panel;}normalized["panels"]=oldPanels;
    if(QJsonDocument::fromJson(canonical).object()!=normalized) {error="配置包含未知、缺失或类型不正确的字段；原配置未改动。";return false;}
    panels=candidate;return true;
}
static bool writeFile(const QString &path,const QByteArray &bytes,QString &error) {QSaveFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit()) {error="无法保存 "+path+"："+f.errorString();return false;}return true;}
static bool readFile(const QString &path,QByteArray &bytes,QString &error) {QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>8*1024*1024){error="无法读取配置或文件过大："+path;return false;}bytes=f.readAll();return true;}
Store::Store(QString path):m_path(path.isEmpty()?QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("status_panels.json"):path){}
bool Store::load(QVector<Panel> &panels,QString &error) {
    m_exists=QFile::exists(m_path);m_original.clear();m_writable=false;
    if(!m_exists) {if(QFile::exists(m_path+".bak")){error="主配置缺失，可恢复有效备份。";return false;}panels.clear();m_writable=true;return true;}
    if(!readFile(m_path,m_original,error)||!decode(m_original,panels,error)) return false;
    m_writable=true;return true;
}
bool Store::save(const QVector<Panel> &panels,QString &error) {
    auto bytes=encode(panels,error);if(bytes.isEmpty()) return false;
    if(!m_writable) {error="配置未能安全读取，请先恢复备份；原文件已保留。";return false;}
    QDir().mkpath(QFileInfo(m_path).absolutePath());QLockFile lock(m_path+".lock");if(!lock.tryLock(0)){error="另一个实例正在修改状态配置。";return false;}
    QByteArray current;if(QFile::exists(m_path)!=m_exists || (m_exists && (!readFile(m_path,current,error)||current!=m_original))) {error="状态配置已被其他实例修改，请重新打开软件后再试。";return false;}
    if(QFile::exists(QDir(QFileInfo(m_path).absolutePath()).filePath("global-config-transaction.json"))) {error="全局配置恢复尚未完成，暂不写入状态配置。";return false;}
    if(m_exists && !writeFile(m_path+".bak",m_original,error)) return false;
    if(!writeFile(m_path,bytes,error)) return false;m_original=bytes;m_exists=true;return true;
}
bool Store::restoreBackup(QVector<Panel> &panels,QString &error) {
    QByteArray backup;QVector<Panel> candidate;if(!readFile(m_path+".bak",backup,error)||!decode(backup,candidate,error)) return false;
    QLockFile lock(m_path+".lock");if(!lock.tryLock(0)){error="配置正在使用中。";return false;}
    if(QFile::exists(QDir(QFileInfo(m_path).absolutePath()).filePath("global-config-transaction.json"))){error="全局配置恢复尚未完成，暂不恢复状态配置。";return false;}
    QByteArray current;if(QFile::exists(m_path)!=m_exists || (m_exists && (!readFile(m_path,current,error)||current!=m_original))){error="原文件已变化，恢复已取消。";return false;}
    if(m_exists && !writeFile(m_path+".before-repair-"+newId()+".json",current,error)) return false;
    if(!writeFile(m_path,backup,error)) return false;m_original=backup;m_exists=true;m_writable=true;panels=candidate;return true;
}
}
