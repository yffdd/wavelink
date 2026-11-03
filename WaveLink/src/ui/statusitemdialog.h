#pragma once
#include "statuspanelmodel.h"
#include "StatusTextMonitor.h"
#include <QDialog>
#include <QVariant>
#include <functional>
class QLineEdit;class QComboBox;class QSpinBox;class QDoubleSpinBox;class QCheckBox;class QPlainTextEdit;class QTableWidget;class QLabel;
struct StatusProtocolOption {
    QString id,name;QByteArray shape;QStringList channelNames;
    bool custom=false,numeric=true;
    QString warning;
};
struct StatusUiContext {QString interfaceName,protocolName,protocolId;QByteArray protocolShape;QStringList channelNames;StatusMonitoring::Session *session=nullptr;bool connected=false;QHash<QString,QString> protocolNames;
    QVector<StatusProtocolOption> protocols;
    // Resolve once when editing, not on the panel's periodic refresh path.
    std::function<QVector<StatusProtocolOption>()> protocolCatalog;
    bool inheritPanelBinding=false,panelBindingPending=false;QString panelProtocolId,panelProtocolName;QByteArray panelProtocolShape;
};
QString statusBindingSummary(const StatusPanels::Panel &panel,const StatusUiContext &context);
class StatusPanelBindingEditor : public QWidget {
public:
    StatusPanelBindingEditor(const StatusPanels::Panel &panel,const StatusUiContext &context,QWidget *parent=nullptr);
    bool collect(StatusPanels::Panel &panel,QString &error) const;
private:
    StatusUiContext m_context;QComboBox *m_mode,*m_choice;QLabel *m_hint;QLineEdit *m_name;QString m_id,m_savedName;QByteArray m_shape;
    void updateUi();void selectProtocol();
};
class StatusItemDialog : public QDialog {
public:
    StatusItemDialog(const StatusMonitoring::Rule &rule,const QVector<StatusMonitoring::Rule> &siblings,const StatusUiContext &context,QWidget *parent=nullptr);
    StatusMonitoring::Rule resultRule() const {return m_result;}
    bool collect(StatusMonitoring::Rule &rule,QString &error) const;
    void setSaveHandler(std::function<bool(const StatusMonitoring::Rule &,QString &)> handler) {m_saveHandler=std::move(handler);}
private:
    StatusMonitoring::Rule m_original,m_result;StatusUiContext m_context;QVector<StatusMonitoring::Rule> m_siblings;
    QLineEdit *m_name;QComboBox *m_source,*m_kind,*m_channel,*m_value,*m_width,*m_encoding,*m_delimiter,*m_framing,*m_shared;
    QSpinBox *m_first,*m_count,*m_timeout,*m_hold,*m_max,*m_partial;QDoubleSpinBox *m_hysteresis;
    QCheckBox *m_enabled,*m_trim,*m_case,*m_sync,*m_every;QPlainTextEdit *m_note,*m_sample,*m_output;
    QTableWidget *m_table;QLabel *m_hint;QWidget *m_numberPage,*m_textPage,*m_bits,*m_threshold;
    QComboBox *m_sampleFormat,*m_chunks;
    QComboBox *m_mode,*m_eventTrigger;QSpinBox *m_eventDuration;QWidget *m_eventBox;QTableWidget *m_silence;QLabel *m_timeoutTitle;
    QHash<int,QVector<QVariantList>> m_silenceDrafts;int m_triggerDrafts[2]={0,1};
    void addSilence();void updateSilenceRow(int row);void saveSilenceDraft(int source);void loadSilenceDraft(int source);
    QHash<int,QVector<QVariantList>> m_drafts;int m_tableKey=-1,m_previousSource=0;int m_timeouts[2]={0,0};
    int m_previousFormat=0;QString m_independentSourceId;
    QLabel *m_bindingHint;
    void updateBindingUi();
    std::function<bool(const StatusMonitoring::Rule &,QString &)> m_saveHandler;
    QHash<int,QVector<QVector<QVariantList>>> m_undo;
    QVector<QVariantList> tableSnapshot() const;
    void rememberRules();void undoRules();
    void rebuildTable();void addRow();void moveRow(int direction);void runTest();void generateSample();
    void updateSource();
};
