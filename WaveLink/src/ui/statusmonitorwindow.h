#pragma once
#include "statusitemdialog.h"
#include <QDialog>
#include <functional>
class QListWidget;class QLabel;class QGridLayout;class QScrollArea;class QTableWidget;class QFrame;class QPushButton;
class StatusMonitorWindow : public QDialog {
    Q_OBJECT
public:
    using ContextProvider=std::function<StatusUiContext()>;
    using ApplyRules=std::function<bool(const QVector<StatusMonitoring::Rule>&,QString&)>;
    StatusMonitorWindow(ContextProvider context,ApplyRules apply,QWidget *parent=nullptr,const QString &path={});
    ~StatusMonitorWindow() override;
    void applyTheme(bool dark);
    QVector<StatusPanels::Panel> panels() const {return m_panels;}
    bool replacePanels(const QVector<StatusPanels::Panel> &panels,QString &error);
    bool reloadConfig(QString &error);
    void savePreferences() {saveUi();}
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    struct Card {QFrame *frame;QLabel *title,*state,*meta,*severity;QString id;};
    void applyCardTheme(Card &card);
    ContextProvider m_context;ApplyRules m_apply;StatusPanels::Store m_store;
    QVector<StatusPanels::Panel> m_panels;QVector<Card> m_cards;QVector<StatusMonitoring::Event> m_history;
    QListWidget *m_list;QLabel *m_title,*m_note,*m_banner,*m_origin,*m_empty;QScrollArea *m_scroll;QWidget *m_cardBody;QGridLayout *m_grid;QTableWidget *m_events;
    QPushButton *m_panelToggle, *m_editLayout;
    bool m_dark=false,m_rebuilding=false;int m_columns=0;QString m_selected;StatusMonitoring::Session *m_lastSession=nullptr;
    quint64 m_historyRevision=0,m_renderedRevision=~quint64(0);QString m_filter;
    int selectedIndex() const;void rebuildList(const QString &selection={});void rebuildCards();void layoutCards();void refresh();
    void editLayout();
    void addPanel();void editPanel();void deletePanel();void copyPanel();void movePanel(int direction);
    void reorderItem(const QString &source,const QString &target);
    void toggleItem(const QString &id);
    void editItem(const QString &id={});void itemMenu(const QString &id,const QPoint &pos);void listMenu(const QPoint &pos);
    void importPanels();void exportPanels(bool all);void showDiagnostics();void showHelp();void showEvents();
    void report(const QString &error);void saveUi();
};
