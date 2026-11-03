#pragma once
#include "StatusMonitor.h"
#include <QJsonDocument>
namespace StatusPanels {
enum class BindingMode { Legacy, Follow, Locked };
struct Panel {QString id,name,note;bool enabled=true;int columns=3;QVector<StatusMonitoring::Rule> items;
    BindingMode bindingMode=BindingMode::Legacy;QString protocolId,protocolName;QByteArray protocolShape;
};
// Infer a uniform old binding without discarding mixed legacy bindings. In the
// new modes item binding fields are mirrors used by the runtime engine.
void normalizeBinding(Panel &panel);
QString newId();
Panel duplicate(const Panel &panel);
Panel duplicateForAppend(const Panel &panel,const QStringList &existingNames);
bool protectedExportPath(const QString &path,const QString &storePath);
QVector<StatusMonitoring::Rule> flatten(const QVector<Panel> &panels);
QByteArray encode(const QVector<Panel> &panels,QString &error);
bool decode(const QByteArray &bytes,QVector<Panel> &panels,QString &error);
class Store {
public:
    explicit Store(QString path={});
    QString path() const {return m_path;}
    bool load(QVector<Panel> &panels,QString &error);
    bool save(const QVector<Panel> &panels,QString &error);
    bool restoreBackup(QVector<Panel> &panels,QString &error);
private:
    QString m_path;QByteArray m_original;bool m_exists=false,m_writable=false;
};
}
