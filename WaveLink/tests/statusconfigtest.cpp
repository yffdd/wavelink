#include "ProtocolConfigTransfer.h"
#include "statuspanelmodel.h"
#include "testregistrysandbox.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QLockFile>
#include <QFile>
#include <QFileInfo>
#include <QDataStream>
#include <iostream>
using namespace DataProtocols;
using namespace StatusPanels;
#define CHECK(c) do{if(!(c)){std::cerr<<"Status config failure "<<__LINE__<<": " #c " : "<<error.toStdString()<<"\n";return 1;}}while(0)
static QByteArray read(const QString &path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
static QString settingsBytes(const char *company,const char *app){QSettings s(company,app);s.sync();QVariantMap values;for(const auto &k:s.allKeys())values[k]=s.value(k);QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_5_12);out<<values;return QString::fromLatin1(bytes.toBase64());}
int main(int argc,char **argv){
    QString error;TestRegistrySandbox registry;CHECK(registry.ready());QCoreApplication app(argc,argv);QTemporaryDir dir;CHECK(dir.isValid());
    const auto command=dir.filePath("commands.json"),status=dir.filePath("status_panels.json"),ini=dir.filePath("config.ini");
    ProtocolProfileStore protocols(dir.filePath("protocols.json"));CHECK(protocols.load(error));
    QFile fixture(QStringLiteral(WAVELINK_PROTOCOL_EXAMPLE_DIR)+"/text_temperature_voltage.json");CHECK(fixture.open(QIODevice::ReadOnly));ProtocolSnapshot snapshot;CHECK(ProtocolProfileValidator::decode(fixture.readAll(),false,snapshot,error));snapshot.shared.kind="custom";snapshot.shared.id=snapshot.profiles[0].id;snapshot.ble=snapshot.shared;CHECK(protocols.commit(snapshot,error));
    Panel panel;panel.id=newId();panel.name="设备状态";panel.note="中文说明 <> &";
    StatusMonitoring::Rule numeric;numeric.id=newId();numeric.name="采集模式";numeric.timeoutMs=3000;numeric.protocolId=snapshot.shared.id;numeric.protocolShape=StatusMonitoring::Session::structure(snapshot.shared,snapshot.profiles[0]);numeric.mappings={{0,"停止"},{1,"采集"}};
    auto text=StatusMonitoring::Rule::textRule(newId(),"设备反馈");text.textSource.id=newId();text.textMatches={{"READY","就绪",false,newId()}};auto shared=text;shared.id=newId();shared.name="共享来源";shared.textMatches[0].id=newId();panel.items={numeric,text,shared};
    panel.items[0].event.enabled=true;panel.items[0].event.displayMs=0;panel.items[0].event.silence.append(StatusMonitoring::SilenceCondition{});panel.items[1].event.enabled=true;StatusMonitoring::SilenceCondition quiet;quiet.kind=StatusMonitoring::SilenceKind::Text;quiet.text="PING";panel.items[1].event.silence.append(quiet);
    {
        auto old=QJsonDocument::fromJson(encode({panel},error)).object();auto panels=old["panels"].toArray();auto saved=panels[0].toObject();saved.remove("binding");panels[0]=saved;old["panels"]=panels;
        QVector<Panel> migrated;CHECK(decode(QJsonDocument(old).toJson(),migrated,error)&&migrated[0].bindingMode==BindingMode::Locked&&migrated[0].protocolId==numeric.protocolId);
        auto mixed=panel;auto other=numeric;other.id=newId();other.protocolId="builtin.firewater";other.protocolShape="other-shape";mixed.items.append(other);
        old=QJsonDocument::fromJson(encode({mixed},error)).object();panels=old["panels"].toArray();saved=panels[0].toObject();saved.remove("binding");panels[0]=saved;old["panels"]=panels;
        CHECK(decode(QJsonDocument(old).toJson(),migrated,error)&&migrated[0].bindingMode==BindingMode::Legacy&&migrated[0].items.first().protocolId==numeric.protocolId&&migrated[0].items.last().protocolId==other.protocolId);
        CHECK(decode(encode(migrated,error),migrated,error)&&migrated[0].bindingMode==BindingMode::Legacy);auto rules=flatten(migrated);CHECK(rules.first().protocolId!=rules.last().protocolId);
        migrated[0].bindingMode=BindingMode::Follow;normalizeBinding(migrated[0]);for(const auto &r:flatten(migrated))if(r.source==StatusMonitoring::Source::Channel)CHECK(r.protocolId.isEmpty());
        old=QJsonDocument::fromJson(encode({panel},error)).object();panels=old["panels"].toArray();saved=panels[0].toObject();auto binding=saved["binding"].toObject();binding["unknown"]=true;saved["binding"]=binding;panels[0]=saved;old["panels"]=panels;CHECK(!decode(QJsonDocument(old).toJson(),migrated,error));
    }
    {
        CHECK(protectedExportPath(status,status)&&protectedExportPath(status+".bak",status)&&protectedExportPath(status+".ui.ini",status)&&protectedExportPath(status+".lock",status));
        CHECK(protectedExportPath(dir.filePath("custom_protocols.json"),status)&&protectedExportPath(dir.filePath("cmd_panels.json"),status)&&protectedExportPath(dir.filePath("global-config-transaction.json"),status));CHECK(!protectedExportPath(dir.filePath("WaveLink_StatusPanels.json"),status));
        const auto first=duplicateForAppend(panel,{panel.name});const auto second=duplicateForAppend(panel,{panel.name,first.name});CHECK(first.name==panel.name+"_1"&&second.name==panel.name+"_2"&&first.id!=panel.id&&first.items[0].id!=numeric.id&&first.items[1].textSource.id==first.items[2].textSource.id&&first.items[1].textSource.id!=text.textSource.id);
        auto rich=panel;rich.protocolName="设备专用协议";rich.items[0].converted=true;rich.items[0].note="监视说明";rich.items[0].mappings[1].color="#2c739f";rich.items[0].mappings[1].severity="警告";rich.items[1].textMatches[0].severity="错误";rich.items[1].textMatches[0].note="规则说明";rich.items[1].textSource.caseSensitive=false;rich.items[2].textSource.caseSensitive=false;
        QVector<Panel> roundTrip;auto bytes=encode({rich},error);CHECK(!bytes.isEmpty());CHECK(decode(bytes,roundTrip,error)&&encode(roundTrip,error)==bytes);const auto flattened=flatten(roundTrip);CHECK(roundTrip[0].protocolName==rich.protocolName&&flattened[0].converted&&flattened[0].event.silence[0].value==0&&flattened[1].textMatches[0].severity=="错误");
    }
    panel.layoutRows = PanelLayout::Rows{{text.id}, {numeric.id, shared.id}};
    const QJsonArray commandRows{QJsonArray{"start"}, QJsonArray{"stop", "reset"}};
    const QJsonArray commands{QJsonObject{{"id","start"}}, QJsonObject{{"id","stop"}}, QJsonObject{{"id","reset"}}};
    const QJsonArray commandPanels{QJsonObject{{"id","commands"},{"name","命令"},{"columns",3},{"commands",commands},{"layoutRows",commandRows}}};
    CHECK(ProtocolProfileStore::writeFile(command, QJsonDocument(commandPanels).toJson(), error));
    Store statusStore(status);QVector<Panel> loaded;CHECK(statusStore.load(loaded,error));CHECK(statusStore.save({panel},error));
    {QSettings ui(status+".ui.ini",QSettings::IniFormat);ui.setValue("selectedPanel",panel.id);ui.setValue("geometry",QByteArray("geometry"));ui.sync();}
    CHECK(ProtocolConfigTransfer::exportFile(ini,protocols.snapshot(),command,error));QVariantMap values;CHECK(ProtocolConfigTransfer::read(ini,values,error));CHECK(values.contains("StatusPanel/json")&&values["StatusPanel/selectedId"].toString()==panel.id);
    const auto original=read(status);CHECK(!ProtocolConfigTransfer::exportFile(status,protocols.snapshot(),command,error));CHECK(read(status)==original);CHECK(!ProtocolConfigTransfer::exportFile(command+".lock",protocols.snapshot(),command,error));CHECK(!ProtocolConfigTransfer::exportFile(status+".ui.ini",protocols.snapshot(),command,error));
    // Append remaps the protocol, panel, item, shared-source and matching-row IDs.
    CHECK(ProtocolConfigTransfer::apply(values,protocols,0,true,0,command,error,2));Store appended(status);CHECK(appended.load(loaded,error));CHECK(loaded.size()==2&&loaded[1].name=="设备状态_1"&&loaded[1].id!=panel.id);
    CHECK(loaded[0].layoutRows == panel.layoutRows);
    CHECK(ProtocolConfigTransfer::apply(values,protocols,-1,false,2,command,error,0));
    const auto importedCommands = QJsonDocument::fromJson(read(command)).array();
    CHECK(importedCommands.size()==2 && importedCommands[1].toObject()["layoutRows"].toArray()==commandRows);
    const auto &copy=loaded[1];
    CHECK(copy.layoutRows && copy.layoutRows->first().first()==copy.items[1].id && copy.layoutRows->last().first()==copy.items[0].id);
    CHECK(copy.items[0].id!=numeric.id&&copy.items[1].textMatches[0].id!=text.textMatches[0].id);CHECK(copy.items[1].textSource.id==copy.items[2].textSource.id&&copy.items[1].textSource.id!=text.textSource.id);
    CHECK(copy.items[0].timeoutMs==3000);CHECK(copy.items[0].event.enabled&&copy.items[0].event.displayMs==0&&copy.items[0].event.silence.size()==1&&copy.items[1].event.enabled&&copy.items[1].event.silence[0].text=="PING");
    CHECK(copy.bindingMode==BindingMode::Locked&&copy.protocolId==copy.items[0].protocolId&&copy.protocolShape==copy.items[0].protocolShape);CHECK(copy.items[0].protocolId==protocols.snapshot().shared.id&&copy.items[0].protocolId!=numeric.protocolId);
    for(const auto &p:protocols.snapshot().profiles)if(p.id==copy.items[0].protocolId)CHECK(copy.items[0].protocolShape==StatusMonitoring::Session::structure(protocols.snapshot().shared,p));
    // Old files and skipped malformed blocks preserve the library byte-for-byte.
    auto before=read(status);QVariantMap legacy{{"WaveLink/statusSentinel","legacy"}};CHECK(ProtocolConfigTransfer::apply(legacy,protocols,-1,false,0,command,error,1));CHECK(read(status)==before);
    auto invalid=values;invalid["StatusPanel/json"]="{broken";invalid["WaveLink/statusSentinel"]="invalid";CHECK(!ProtocolConfigTransfer::apply(invalid,protocols,0,true,0,command,error,1));CHECK(read(status)==before&&QSettings("WaveLink","WaveLink").value("statusSentinel").toString()=="legacy");
    CHECK(ProtocolConfigTransfer::apply(invalid,protocols,-1,false,0,command,error,0));CHECK(read(status)==before);
    {QLockFile lock(status+".lock");CHECK(lock.tryLock(0));CHECK(!ProtocolConfigTransfer::apply(values,protocols,0,true,0,command,error,1));CHECK(read(status)==before);}
    // Replace retains a valid backup and UI preferences. The Store must reload
    // after a global import so later local edits cannot overwrite a new file.
    CHECK(ProtocolConfigTransfer::apply(values,protocols,1,true,0,command,error,1));CHECK(read(status+".bak")==before);CHECK(!appended.save({},error));Store replaced(status);CHECK(replaced.load(loaded,error));CHECK(loaded.size()==1&&loaded[0].note==panel.note);CHECK(QSettings(status+".ui.ini",QSettings::IniFormat).value("selectedPanel").toString()==panel.id);
    // A stale structural binding is never silently approved during ID remapping.
    auto stalePanels=loaded;stalePanels[0].protocolShape="stale";stalePanels[0].items[0].protocolShape="stale";auto stale=values;stale["StatusPanel/json"]=QString::fromUtf8(encode(stalePanels,error));CHECK(ProtocolConfigTransfer::apply(stale,protocols,0,true,0,command,error,1));Store staleStore(status);CHECK(staleStore.load(loaded,error));CHECK(loaded[0].items[0].protocolShape=="stale");
    // Simulate interruption after all six file writes, then use startup recovery.
    const QStringList paths{protocols.path(),protocols.path()+".bak",command,status,status+".bak",status+".ui.ini"};QJsonArray files;
    for(const auto &p:paths){auto bytes=read(p);files.append(QJsonObject{{"path",QFileInfo(p).absoluteFilePath()},{"existed",QFile::exists(p)},{"before",QString::fromLatin1(bytes.toBase64())},{"after",QString::fromLatin1(QByteArray("interrupted").toBase64())}});}
    QJsonObject journal{{"schema",1},{"main",settingsBytes("WaveLink","WaveLink")},{"advanced",settingsBytes("WaveLink","AdvancedApp")},{"my",settingsBytes("MyCompany","WaveLink")},{"files",files}};const auto journalPath=dir.filePath("global-config-transaction.json");CHECK(ProtocolProfileStore::writeFile(journalPath,QJsonDocument(journal).toJson(),error));
    for(const auto &p:paths)CHECK(ProtocolProfileStore::writeFile(p,"interrupted",error));
    CHECK(ProtocolConfigTransfer::recover(protocols.path(),command,error));
    for(int i=0;i<paths.size();++i){const auto entry=files[i].toObject();CHECK(QFile::exists(paths[i])==entry["existed"].toBool());if(QFile::exists(paths[i]))CHECK(read(paths[i])==QByteArray::fromBase64(entry["before"].toString().toLatin1()));}
    CHECK(!QFile::exists(journalPath));
    // Invalid or future status configuration must not yield a partial export.
    CHECK(ProtocolProfileStore::writeFile(status,"{broken",error));const auto exported=read(ini);CHECK(!ProtocolConfigTransfer::exportFile(ini,snapshot,command,error));CHECK(read(ini)==exported);
    std::cout<<"Status global export/import, remapping, append/replace/skip, locks, stale bindings, backups and six-file startup recovery passed.\n";return 0;
}
