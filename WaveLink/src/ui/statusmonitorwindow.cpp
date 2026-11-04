#include "statusmonitorwindow.h"
#include "paneluistyle.h"
#include "panellayouteditor.h"
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QMenu>
#include <QInputDialog>
#include <QFileDialog>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QDateTime>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QDir>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QDynamicPropertyChangeEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <cmath>
using namespace StatusPanels;
using namespace StatusMonitoring;
namespace {
class StatusCard final:public QFrame {
    QPoint pressed;QTimer hold;bool rightPressed=false,longPress=false,leftPressed=false,dragged=false;
public:
    QString id;std::function<void()> edit,toggle;std::function<void(QPoint)> menu;std::function<void(const QString&)> drop;
    explicit StatusCard(QWidget *parent):QFrame(parent){setAcceptDrops(true);setCursor(Qt::PointingHandCursor);hold.setSingleShot(true);hold.setInterval(700);connect(&hold,&QTimer::timeout,this,[this]{if(!rightPressed)return;longPress=true;auto callback=edit;if(callback)QTimer::singleShot(0,window(),callback);});}
protected:
    void mousePressEvent(QMouseEvent *e)override{pressed=e->pos();if(e->button()==Qt::LeftButton){leftPressed=true;dragged=false;e->accept();return;}if(e->button()==Qt::RightButton){rightPressed=true;longPress=false;hold.start();e->accept();return;}QFrame::mousePressEvent(e);}
    void mouseDoubleClickEvent(QMouseEvent *e)override{mousePressEvent(e);}
    void mouseReleaseEvent(QMouseEvent *e)override{if(e->button()==Qt::RightButton){hold.stop();const bool open=rightPressed&&!longPress&&rect().contains(e->pos());rightPressed=false;e->accept();if(open&&menu){auto callback=menu;const auto pos=e->globalPos();QTimer::singleShot(0,window(),[callback,pos]{callback(pos);});}return;}if(e->button()==Qt::LeftButton){const bool click=leftPressed&&!dragged&&rect().contains(e->pos());leftPressed=false;e->accept();if(click&&toggle){auto callback=toggle;QTimer::singleShot(0,window(),callback);}return;}QFrame::mouseReleaseEvent(e);}
    void contextMenuEvent(QContextMenuEvent *e)override{e->accept();if(e->reason()==QContextMenuEvent::Keyboard&&menu){auto callback=menu;const auto pos=mapToGlobal(rect().center());QTimer::singleShot(0,window(),[callback,pos]{callback(pos);});}}
    void mouseMoveEvent(QMouseEvent *e)override{if(rightPressed&&(e->pos()-pressed).manhattanLength()>=QApplication::startDragDistance()){hold.stop();rightPressed=false;}if(leftPressed&&!dragged&&(e->buttons()&Qt::LeftButton)&&(e->pos()-pressed).manhattanLength()>=QApplication::startDragDistance()){dragged=true;if(!acceptDrops())return;auto drag=new QDrag(window());auto data=new QMimeData;data->setData("application/x-wavelink-status-item",id.toUtf8());drag->setMimeData(data);drag->exec(Qt::MoveAction);drag->deleteLater();return;}QFrame::mouseMoveEvent(e);}
    void dragEnterEvent(QDragEnterEvent *e)override{if(e->mimeData()->hasFormat("application/x-wavelink-status-item"))e->acceptProposedAction();}
    void dropEvent(QDropEvent *e)override{if(e->mimeData()->hasFormat("application/x-wavelink-status-item")){const auto source=QString::fromUtf8(e->mimeData()->data("application/x-wavelink-status-item"));e->acceptProposedAction();if(drop){auto callback=drop;QTimer::singleShot(0,window(),[callback,source]{callback(source);});}}}
};
QString statusName(const State &s){switch(s.quality){case Quality::Valid:return s.state;case Quality::Waiting:return s.eventMode?"等待事件":"等待数据 / 反馈";case Quality::Unknown:return "未知 / 未匹配";case Quality::Invalid:return "数值无效";case Quality::Missing:return "缺少通道";case Quality::Stale:return "数据 / 反馈超时";case Quality::Disconnected:return "已断开";case Quality::SourceMismatch:return "来源不匹配";case Quality::Disabled:return "已停用";case Quality::Unavailable:return "来源不可用";}return "未知";}
QString timestamp(qint64 ns){return ns>0?QDateTime::fromMSecsSinceEpoch(ns/1000000).toString("yyyy-MM-dd HH:mm:ss.zzz"):QString();}
QString messageText(Session *session,const State &state){if(session)for(const auto &d:session->textDiagnostics())if(d.sourceId==state.textSourceId&&d.sequence==state.sequence&&d.receiveUnixNs==state.receiveUnixNs)return d.text;return state.text+(state.text.size()>=512?"\n[全文已淘汰，以上为摘要]":"");}
void textDetails(QWidget *owner,const QString &title,const QString &body){QDialog dialog(owner);PanelUi::windowControls(&dialog);dialog.setWindowTitle(title);dialog.resize(700,500);auto layout=new QVBoxLayout(&dialog);auto text=new QPlainTextEdit(body);text->setReadOnly(true);layout->addWidget(text);auto close=new QDialogButtonBox(QDialogButtonBox::Close);close->button(QDialogButtonBox::Close)->setText("关闭");layout->addWidget(close);QObject::connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.exec();}
}
StatusMonitorWindow::StatusMonitorWindow(ContextProvider context,ApplyRules apply,QWidget *parent,const QString &path)
    :QDialog(parent,Qt::Window),m_context(context),m_apply(apply),m_store(path){
    setObjectName("statusMonitorWindow");setWindowTitle("状态监视面板");resize(1080,720);setMinimumSize(640,420);auto f=font();if(f.pointSizeF()<10)f.setPointSizeF(10);setFont(f);
    auto root=new QVBoxLayout(this);m_banner=new QLabel;m_banner->setObjectName("statusConfigMessage");m_banner->setWordWrap(true);m_banner->hide();root->addWidget(m_banner);
    auto split=new QSplitter(this);root->addWidget(split,1);auto left=new QWidget;left->setMinimumWidth(170);left->setMaximumWidth(360);split->addWidget(left);auto ll=new QVBoxLayout(left);ll->setContentsMargins(0,0,0,0);ll->addWidget(new QLabel("面板列表"));m_list=new QListWidget;m_list->setObjectName("statusPanelList");m_list->setSelectionMode(QAbstractItemView::SingleSelection);m_list->setContextMenuPolicy(Qt::CustomContextMenu);ll->addWidget(m_list,1);
    auto management=new QGridLayout;ll->addLayout(management);auto add=new QPushButton("＋ 新增面板");add->setObjectName("statusAddPanel");auto del=new QPushButton("删除面板");auto imp=new QPushButton("导入面板");imp->setObjectName("statusImportPanels");auto exp=new QPushButton("导出当前");auto help=new QPushButton("使用说明");auto repair=new QPushButton("恢复有效备份");repair->setObjectName("statusRestoreBackup");management->addWidget(add,0,0);management->addWidget(del,0,1);management->addWidget(imp,1,0);management->addWidget(exp,1,1);management->addWidget(help,2,0,1,2);management->addWidget(repair,3,0,1,2);
    add->setText("新增面板");exp->setText("导出所有");exp->setObjectName("statusExportAll");
    add->setToolTip("新增状态面板；同名时自动添加 _1、_2 等后缀。");del->setToolTip("删除当前选中面板及其监视项，需要确认，不向设备发送命令。");imp->setToolTip("从 JSON 文件追加或覆盖状态面板。");exp->setToolTip("将全部状态面板导出为一个 JSON 文件。");help->setToolTip("查看数值与文本监视的配置方法和适用边界。");repair->setToolTip("恢复上一份有效配置，恢复前另存当前原文件；可能撤销最近一次修改。");
    connect(add,&QPushButton::clicked,this,&StatusMonitorWindow::addPanel);connect(del,&QPushButton::clicked,this,&StatusMonitorWindow::deletePanel);connect(imp,&QPushButton::clicked,this,&StatusMonitorWindow::importPanels);connect(exp,&QPushButton::clicked,this,[this]{exportPanels(true);});connect(help,&QPushButton::clicked,this,&StatusMonitorWindow::showHelp);
    connect(repair,&QPushButton::clicked,this,[this]{QString error;QVector<Panel> recovered;if(!m_store.restoreBackup(recovered,error)){report(error);return;}m_panels=recovered;m_banner->setText("已恢复有效备份；恢复前的原文件已单独保留。");m_banner->show();m_apply(flatten(m_panels),error);rebuildList();});
    auto right=new QWidget;split->addWidget(right);auto rl=new QVBoxLayout(right);rl->setContentsMargins(0,0,0,0);auto top=new QHBoxLayout;rl->addLayout(top);m_title=new QLabel("状态监视");m_title->setObjectName("statusPanelTitle");auto titleFont=font();titleFont.setPointSizeF(font().pointSizeF()+3);titleFont.setBold(true);m_title->setFont(titleFont);top->addWidget(m_title,1);m_panelToggle=new QPushButton;m_panelToggle->setObjectName("statusPanelToggle");m_panelToggle->setCheckable(true);m_panelToggle->setToolTip("启用或停用当前面板全部监视；保留各监视项原本的启停设置，不影响设备连接与录制。与面板设置同步。");top->addWidget(m_panelToggle);connect(m_panelToggle,&QPushButton::clicked,this,[this](bool checked){int p=selectedIndex();if(p<0)return;auto candidate=m_panels;candidate[p].enabled=checked;QString error;if(!replacePanels(candidate,error)){rebuildCards();report(error);}});auto newItem=new QPushButton("＋ 新增监视项");newItem->setObjectName("statusAddItem");newItem->setStyleSheet("QPushButton {background:#147caf;color:white;padding:7px;border-radius:3px;}");auto settings=new QPushButton("面板设置");top->addWidget(newItem);
    m_editLayout = new QPushButton(QStringLiteral("编辑布局"));
    m_editLayout->setObjectName("statusEditLayout");
    m_editLayout->setToolTip(QStringLiteral("设置当前面板的自动排列或自定义分行。"));
    top->addWidget(m_editLayout);
    connect(m_editLayout, &QPushButton::clicked, this, &StatusMonitorWindow::editLayout);
    top->addWidget(settings);connect(newItem,&QPushButton::clicked,this,[this]{editItem();});connect(settings,&QPushButton::clicked,this,&StatusMonitorWindow::editPanel);
    m_origin=new QLabel;m_origin->setObjectName("statusSourceSummary");m_origin->setWordWrap(true);rl->addWidget(m_origin);
    newItem->setText("新增监视项");newItem->setToolTip("添加通道数值或接收文本监视规则，可先生成样例进行解析测试。");settings->setObjectName("statusPanelSettings");settings->setToolTip("修改当前面板名称、说明、启用状态和卡片布局。");
    m_title->setWordWrap(true);m_title->setTextFormat(Qt::PlainText);m_origin->setTextFormat(Qt::PlainText);
    m_scroll=new QScrollArea;m_scroll->setWidgetResizable(true);m_scroll->setObjectName("statusCardScroll");m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);rl->addWidget(m_scroll,1);m_cardBody=new QWidget;m_scroll->setWidget(m_cardBody);auto content=new QVBoxLayout(m_cardBody);content->setAlignment(Qt::AlignTop);m_note=new QLabel;m_note->setWordWrap(true);m_note->setTextFormat(Qt::PlainText);m_note->setTextInteractionFlags(Qt::TextSelectableByMouse);content->addWidget(m_note);m_empty=new QLabel;m_empty->setObjectName("statusWelcome");m_empty->setWordWrap(true);m_empty->setAlignment(Qt::AlignCenter);m_empty->setMinimumHeight(160);content->addWidget(m_empty);m_grid=new QGridLayout;content->addLayout(m_grid);content->addStretch();m_cardBody->setContextMenuPolicy(Qt::CustomContextMenu);connect(m_cardBody,&QWidget::customContextMenuRequested,this,[this](const QPoint &p){QMenu menu;auto a=menu.addAction("新增监视项");if(menu.exec(m_cardBody->mapToGlobal(p))==a)editItem();});
    auto log=new QGroupBox("最近状态变化（只记录变化，默认最多 2000 条）");log->setCheckable(true);log->setChecked(false);rl->addWidget(log);auto logLayout=new QVBoxLayout(log);auto logButtons=new QHBoxLayout;logLayout->addLayout(logButtons);auto exportLog=new QPushButton("导出记录");auto clearLog=new QPushButton("清空记录");auto diagnostics=new QPushButton("来源诊断");logButtons->addWidget(exportLog);logButtons->addWidget(clearLog);logButtons->addWidget(diagnostics);logButtons->addStretch();m_events=new QTableWidget(0,4);m_events->setObjectName("statusEventTable");m_events->setHorizontalHeaderLabels({"接收 / 变化时间","监视项","原状态 → 新状态","原因"});m_events->setEditTriggers(QAbstractItemView::NoEditTriggers);m_events->horizontalHeader()->setStretchLastSection(true);m_events->setMaximumHeight(210);logLayout->addWidget(m_events);m_events->hide();connect(log,&QGroupBox::toggled,this,[this](bool checked){m_events->setVisible(checked);if(checked)showEvents();});connect(clearLog,&QPushButton::clicked,this,[this]{m_history.clear();m_events->setRowCount(0);});connect(diagnostics,&QPushButton::clicked,this,&StatusMonitorWindow::showDiagnostics);
    auto filter=new QLineEdit;filter->setObjectName("statusEventFilter");filter->setPlaceholderText("筛选监视项、状态或原因");filter->setClearButtonEnabled(true);logButtons->insertWidget(0,filter);connect(filter,&QLineEdit::textChanged,this,[this](const QString &text){m_filter=text;showEvents();});
    top->addWidget(diagnostics);diagnostics->setToolTip("查看当前接收来源的统计及有限诊断记录。");
    exportLog->setToolTip("导出内存中保留的全部记录，不受筛选条件影响。");
    connect(exportLog,&QPushButton::clicked,this,[this]{auto path=QFileDialog::getSaveFileName(this,"导出状态变化",QString("WaveLink_StatusEvents_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),"CSV (*.csv)");if(path.isEmpty())return;auto quote=[](QString s){if(s.startsWith('=')||s.startsWith('+')||s.startsWith('-')||s.startsWith('@'))s.prepend('\'');s.replace('"',"\"\"");return '"'+s+'"';};QByteArray bytes=QByteArray::fromHex("EFBBBF")+"Time,ItemId,PreviousState,CurrentState,Reason\r\n";for(const auto &e:m_history)bytes+=(QStringList{quote(timestamp(e.unixNs)),quote(e.current.id),quote(statusName(e.previous)),quote(statusName(e.current)),quote(e.current.reason)}.join(',')+"\r\n").toUtf8();QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())report(file.errorString());});
    connect(m_list,&QListWidget::currentRowChanged,this,[this](int row){if(m_rebuilding)return;m_selected=row>=0?m_list->item(row)->data(Qt::UserRole).toString():QString();saveUi();rebuildCards();});connect(m_list,&QListWidget::customContextMenuRequested,this,&StatusMonitorWindow::listMenu);connect(m_list,&QListWidget::itemDoubleClicked,this,[this]{editPanel();});
    m_list->setDragDropMode(QAbstractItemView::InternalMove);m_list->setDefaultDropAction(Qt::MoveAction);
    connect(m_list->model(),&QAbstractItemModel::rowsMoved,this,[this]{if(m_rebuilding)return;QStringList ids;for(int i=0;i<m_list->count();++i)ids<<m_list->item(i)->data(Qt::UserRole).toString();QTimer::singleShot(0,this,[this,ids]{QVector<Panel> sorted;for(const auto &id:ids)for(const auto &p:m_panels)if(p.id==id)sorted.append(p);if(sorted.size()!=m_panels.size())return;QString error;if(!replacePanels(sorted,error)){rebuildList(m_selected);report(error);}});});
    QSettings ui(m_store.path()+".ui.ini",QSettings::IniFormat);m_selected=ui.value("selectedPanel").toString();restoreGeometry(ui.value("geometry").toByteArray());
    QString error;if(!m_store.load(m_panels,error)){m_banner->setText("状态配置未能安全读取，原文件已保留。\n"+error);m_banner->show();}else if(!m_apply(flatten(m_panels),error)){m_banner->setText(error);m_banner->show();}
    split->setSizes({210,850});rebuildList(m_selected);auto timer=new QTimer(this);timer->setInterval(100);connect(timer,&QTimer::timeout,this,&StatusMonitorWindow::refresh);timer->start();qApp->installEventFilter(this);const auto theme=qApp->property("waveLinkDarkTheme");if(theme.isValid())applyTheme(theme.toBool());
}
StatusMonitorWindow::~StatusMonitorWindow(){saveUi();}
bool StatusMonitorWindow::eventFilter(QObject *object,QEvent *event){
    if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="waveLinkDarkTheme")applyTheme(qApp->property("waveLinkDarkTheme").toBool());
    return QDialog::eventFilter(object,event);
}
bool StatusMonitorWindow::reloadConfig(QString &error){QVector<Panel> panels;if(!m_store.load(panels,error)||!m_apply(flatten(panels),error)){m_banner->setText("状态配置未能应用，原文件已保留。\n"+error);m_banner->show();return false;}QSettings ui(m_store.path()+".ui.ini",QSettings::IniFormat);ui.sync();m_selected=ui.value("selectedPanel",m_selected).toString();restoreGeometry(ui.value("geometry").toByteArray());m_panels=panels;m_history.clear();++m_historyRevision;m_banner->hide();rebuildList(m_selected);return true;}
int StatusMonitorWindow::selectedIndex()const {for(int i=0;i<m_panels.size();++i)if(m_panels[i].id==m_selected)return i;return -1;}
void StatusMonitorWindow::saveUi(){QSettings ui(m_store.path()+".ui.ini",QSettings::IniFormat);ui.setValue("selectedPanel",m_selected);ui.setValue("geometry",saveGeometry());}
bool StatusMonitorWindow::replacePanels(const QVector<Panel> &panels,QString &error){
    auto normalized = panels;
    for (auto &panel : normalized) {
        normalizeBinding(panel);
        if (panel.layoutRows) panel.layoutRows = PanelLayout::arrange(layoutItems(panel), panel.columns, *panel.layoutRows);
    }
    const auto rules=flatten(normalized);if(!Engine::validate(rules,error)||encode(normalized,error).isEmpty())return false;
    if(!m_store.save(normalized,error))return false;m_panels=normalized;
    if(!m_apply(rules,error)){m_banner->setText("配置已保存，但应用失败："+error);m_banner->show();return false;}
    m_banner->hide();rebuildList(m_selected);return true;
}
void StatusMonitorWindow::report(const QString &error){QMessageBox::warning(this,"状态监视",error);}
void StatusMonitorWindow::rebuildList(const QString &selection){m_rebuilding=true;m_list->clear();int row=-1;for(int i=0;i<m_panels.size();++i){auto item=new QListWidgetItem((m_panels[i].enabled?"":"[停用] ")+m_panels[i].name,m_list);item->setSizeHint(QSize(0,44));item->setData(Qt::UserRole,m_panels[i].id);item->setToolTip(m_panels[i].note.toHtmlEscaped());if(m_panels[i].id==selection)row=i;}if(row<0&&!m_panels.isEmpty())row=0;m_list->setCurrentRow(row);m_selected=row<0?QString():m_panels[row].id;m_rebuilding=false;rebuildCards();saveUi();}
void StatusMonitorWindow::rebuildCards(){for(auto &c:m_cards)delete c.frame;m_cards.clear();while(auto item=m_grid->takeAt(0))delete item;int p=selectedIndex();{QSignalBlocker block(m_panelToggle);m_panelToggle->setEnabled(p>=0);m_panelToggle->setChecked(p>=0&&m_panels[p].enabled);m_panelToggle->setText(p<0?"○ 面板未选择":m_panels[p].enabled?"● 面板已启用":"○ 面板已停用");m_panelToggle->setStyleSheet(m_panelToggle->isChecked()?"QPushButton {background:#237b52;color:white;padding:7px;border-radius:3px;}":"QPushButton {background:#596572;color:white;padding:7px;border-radius:3px;}");}m_note->setText(p>=0?m_panels[p].note:"");m_title->setText(p>=0?m_panels[p].name:"状态监视");m_empty->setVisible(p<0||m_panels[p].items.isEmpty());m_empty->setText(p<0?"欢迎使用状态监视\n\n先新增或导入面板，再添加通道数值或接收文本监视。\n关闭此窗口后，已启用监视仍会继续运行。":"此面板尚无监视项。\n点击“新增监视项”开始配置。");
    if(p>=0)for(const auto &r:m_panels[p].items){auto frame=new StatusCard(m_cardBody);frame->id=r.id;frame->edit=[this,id=r.id]{editItem(id);};frame->drop=[this,id=r.id](const QString &source){reorderItem(source,id);};frame->setObjectName("statusCard");frame->setProperty("ruleId",r.id);frame->setFrameShape(QFrame::StyledPanel);frame->setMinimumHeight(118);frame->setMinimumWidth(0);frame->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);auto layout=new QVBoxLayout(frame);auto title=new QLabel(r.name);title->setObjectName("statusCardTitle");title->setTextFormat(Qt::PlainText);title->setWordWrap(true);auto font=title->font();font.setBold(true);title->setFont(font);auto state=new QLabel("等待数据");state->setTextFormat(Qt::PlainText);state->setWordWrap(true);font.setPointSizeF(font.pointSizeF()+2);state->setFont(font);auto severity=new QLabel(frame);severity->setObjectName("statusSeverity");severity->setTextFormat(Qt::PlainText);severity->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);severity->setAttribute(Qt::WA_TransparentForMouseEvents);severity->hide();auto meta=new QLabel;meta->setObjectName("statusCardMeta");meta->setTextFormat(Qt::PlainText);meta->setWordWrap(true);for(auto label:{title,severity,state,meta}){label->setAttribute(Qt::WA_TransparentForMouseEvents);layout->addWidget(label);}auto tip=r.note.isEmpty()?QString("暂无说明"):r.note.left(400);if(r.note.size()>400)tip+="\n…右键可查看完整说明";tip=tip.toHtmlEscaped();tip.replace('\n',"<br>");frame->setToolTip("<qt>"+tip+"</qt>");frame->setContextMenuPolicy(Qt::CustomContextMenu);connect(frame,&QWidget::customContextMenuRequested,this,[this,id=r.id,frame](const QPoint &pos){itemMenu(id,frame->mapToGlobal(pos));});m_cards.append({frame,title,state,meta,severity,r.id});}
    for(auto &card:m_cards){auto frame=static_cast<StatusCard*>(card.frame);frame->setContextMenuPolicy(Qt::DefaultContextMenu);disconnect(frame,&QWidget::customContextMenuRequested,this,nullptr);frame->toggle=[this,id=card.id]{toggleItem(id);};frame->menu=[this,id=card.id](QPoint pos){itemMenu(id,pos);};frame->setToolTip(frame->toolTip()+"<p>左键：启用 / 停用此项；右键：菜单；右键长按：编辑。<br>不影响设备连接及其他监视项。</p>");applyCardTheme(card);}
    m_editLayout->setEnabled(p >= 0);
    m_columns=0;layoutCards();refresh();}
void StatusMonitorWindow::applyCardTheme(Card &card){
    const QColor background(m_dark?"#27323c":"#ffffff"),border(m_dark?"#4a6070":"#becbd6"),ink(m_dark?"#e1e8ef":"#1b3548");
    // Apply to every actual card, including cards recreated after edits/imports.
    // Do not rely on an ancestor's cached stylesheet/palette in Qt 5.
    card.frame->setStyleSheet(QString("QFrame#statusCard {background:%1;border:1px solid %2;border-radius:6px;}").arg(background.name(),border.name()));
    auto palette=card.frame->palette();palette.setColor(QPalette::Window,background);palette.setColor(QPalette::Base,background);palette.setColor(QPalette::WindowText,ink);card.frame->setPalette(palette);
    for(auto label:{card.title,card.meta})label->setStyleSheet("QLabel {background:transparent;color:"+ink.name()+";}");
    card.frame->update();
}
void StatusMonitorWindow::layoutCards()
{
    const int p = selectedIndex();
    const auto rows = p >= 0 && m_panels[p].layoutRows ? &*m_panels[p].layoutRows : nullptr;
    const int columns = rows ? qMax(1, PanelLayout::widestRow(*rows))
                             : qMax(1, qMin(p >= 0 ? m_panels[p].columns : 3, m_scroll->viewport()->width() / 240));
    if (columns == m_columns) return;
    m_columns = columns;
    while (auto item = m_grid->takeAt(0)) delete item;
    m_scroll->setHorizontalScrollBarPolicy(rows ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    for (int i = 0; i < PanelLayout::StatusMaxColumns; ++i) {
        m_grid->setColumnStretch(i, i < columns ? 1 : 0);
        m_grid->setColumnMinimumWidth(i, rows && !rows->isEmpty() && i < columns ? 240 : 0);
    }
    if (rows) {
        QHash<QString, QFrame *> frames;
        for (const auto &card : m_cards) {
            card.frame->setAcceptDrops(false);
            frames.insert(card.id, card.frame);
        }
        for (int row = 0; row < rows->size(); ++row) {
            const auto &ids = rows->at(row);
            for (int col = 0; col < ids.size(); ++col) m_grid->addWidget(frames.value(ids[col]), row, col);
        }
    } else {
        for (int i = 0; i < m_cards.size(); ++i) {
            m_cards[i].frame->setAcceptDrops(true);
            m_grid->addWidget(m_cards[i].frame, i / columns, i % columns);
        }
    }
}

void StatusMonitorWindow::editLayout()
{
    const int index = selectedIndex();
    if (index < 0) return;
    const auto panel = m_panels[index];
    PanelLayout::edit(this, PanelLayout::EditorKind::Status, layoutItems(panel), panel.columns, panel.layoutRows,
        [this, panelId = panel.id](int columns, const PanelLayout::Layout &layout, QString &error) {
            const int index = selectedIndex();
            if (index < 0 || m_panels[index].id != panelId) {
                error = QStringLiteral("当前面板已变更，请重新打开布局编辑器。");
                return false;
            }
            if (m_panels[index].columns == columns && m_panels[index].layoutRows == layout) return true;
            auto candidate = m_panels;
            candidate[index].columns = columns;
            candidate[index].layoutRows = layout;
            if (!m_store.save(candidate, error)) return false;
            m_panels = std::move(candidate);
            m_columns = 0;
            layoutCards();
            return true;
        });
}
void StatusMonitorWindow::resizeEvent(QResizeEvent *event){QDialog::resizeEvent(event);if(m_scroll)layoutCards();}
void StatusMonitorWindow::closeEvent(QCloseEvent *event){saveUi();hide();event->ignore();}
void StatusMonitorWindow::refresh(){const auto ctx=m_context();if(ctx.session!=m_lastSession){if(m_lastSession){auto events=m_lastSession->takeEvents();m_history+=events;m_historyRevision+=events.size();}m_lastSession=ctx.session;}
    if(ctx.session){auto events=ctx.session->takeEvents();for(const auto &e:events)m_history.append(e);m_historyRevision+=events.size();if(m_history.size()>2000)m_history.remove(0,m_history.size()-2000);}
    if(!isVisible())return;
    m_origin->setText(QString("接口：%1 · 协议：%2 · %3").arg(ctx.interfaceName,ctx.protocolName,ctx.connected?"已连接":"未连接")+(ctx.session?QString(" · 引擎累计淘汰 %1 条记录").arg(ctx.session->droppedEvents()):""));
    QHash<QString,State> states;if(ctx.session)for(const auto &s:ctx.session->snapshot())states.insert(s.id,s);
    int p=selectedIndex();if(p<0)return;m_origin->setText(m_origin->text()+"\n面板协议："+statusBindingSummary(m_panels[p],ctx));
    for(auto &card:m_cards){const Rule *rule=nullptr;for(const auto &r:m_panels[p].items)if(r.id==card.id){rule=&r;break;}if(!rule)continue;State state=states.value(card.id);if(!states.contains(card.id)){state.quality=Quality::Waiting;}if(!rule->enabled||!m_panels[p].enabled)state.quality=Quality::Disabled;else if(!ctx.connected)state.quality=Quality::Disconnected;
        QString label=(state.quality==Quality::Valid?"● ":state.quality==Quality::Waiting||state.quality==Quality::Disabled||state.quality==Quality::Disconnected?"○ ":"! ")+statusName(state);if(state.feedbackMayBeLost)label+=" · 可能遗漏反馈";if(state.processingDelayed)label+=" · 处理延迟";
        QString severity=state.severity;QString color=state.color;if(rule->source==Source::Channel&&state.quality==Quality::Valid){double value=state.value;if(rule->kind==RuleKind::Bits&&std::isfinite(value))value=double((quint64(value)>>rule->firstBit)&((quint64(1)<<rule->bitCount)-1));for(const auto &m:rule->mappings)if(m.value==value){color=m.color;severity=m.severity;break;}}
        if(rule->source==Source::Channel&&rule->kind==RuleKind::Threshold&&state.quality==Quality::Valid&&state.matchedRow>=0&&state.matchedRow<rule->ranges.size()){color=rule->ranges[state.matchedRow].color;severity=rule->ranges[state.matchedRow].severity;}
        const bool alarm=state.quality==Quality::Valid&&(severity=="提示"||severity=="警告"||severity=="错误");card.severity->setVisible(alarm);if(alarm){card.severity->setText("严重程度："+severity);const auto ink=severity=="错误"?"#ffffff":severity=="警告"?"#452800":"#ffffff";const auto fill=severity=="错误"?"#b3261e":severity=="警告"?"#ffbf47":"#176fa6";card.severity->setStyleSheet(QString("QLabel {color:%1;background:%2;border-radius:3px;padding:3px 8px;font-weight:bold;}").arg(ink,fill));}if(state.quality!=Quality::Valid||state.feedbackMayBeLost)color=state.quality==Quality::Waiting||state.quality==Quality::Disabled||state.quality==Quality::Disconnected?(m_dark?"#aaaaaa":"#666666"):(m_dark?"#ff8888":"#b3261e");if(!QColor(color).isValid())color=m_dark?"#e1e8ef":"#1b3548";
        if(card.state->text()!=label)card.state->setText(label);const auto style="QLabel {color:"+color+";background:transparent;}";if(card.state->styleSheet()!=style)card.state->setStyleSheet(style);QString meta=rule->source==Source::Text?"文本反馈":"通道 Ch"+QString::number(rule->channel+1)+(rule->converted?" · 换算值":" · 原始值");if(state.quality!=Quality::Valid&&state.lastValidMs>=0)meta+=" · 历史样本";
        if(std::isfinite(state.value))meta+=" · "+QString::number(state.value,'g',12);if(rule->source==Source::Text&&state.matches&&!rule->event.enabled)meta+=QString(" · %1 次匹配").arg(state.matches);if(rule->event.enabled)meta=QString("事件提示 · 已触发 %1 次 · 已屏蔽 %2 条\n").arg(state.matches).arg(state.suppressed)+meta;if(state.receiveUnixNs)meta+="\n"+timestamp(state.receiveUnixNs);if(!state.reason.isEmpty())meta+="\n"+state.reason;card.meta->setText(meta);
    }
    layoutCards();if(m_events->isVisible()&&m_historyRevision!=m_renderedRevision)showEvents();
}
void StatusMonitorWindow::addPanel(){bool ok;const auto name=QInputDialog::getText(this,"新增面板","面板名称",QLineEdit::Normal,"新状态面板",&ok).trimmed();if(!ok||name.isEmpty())return;auto candidate=m_panels;QStringList names;for(const auto &p:candidate)names<<p.name;Panel p;p.id=newId();p.name=PanelUi::uniqueName(name,names);const auto ctx=m_context();p.bindingMode=ctx.protocolId.isEmpty()||ctx.protocolShape.isEmpty()||ctx.protocolId=="builtin.raw"?BindingMode::Follow:BindingMode::Locked;if(p.bindingMode==BindingMode::Locked){p.protocolId=ctx.protocolId;p.protocolShape=ctx.protocolShape;p.protocolName=ctx.protocolName;}candidate.append(p);QString error;if(!replacePanels(candidate,error)){report(error);return;}rebuildList(p.id);}
void StatusMonitorWindow::editPanel(){
    int i=selectedIndex();if(i<0)return;
    QDialog dialog(this);PanelUi::windowControls(&dialog);dialog.setObjectName("statusPanelSettingsDialog");dialog.setWindowTitle("面板设置");dialog.resize(640,680);
    auto root=new QVBoxLayout(&dialog);auto scroll=new QScrollArea;scroll->setWidgetResizable(true);root->addWidget(scroll,1);auto body=new QWidget;scroll->setWidget(body);auto form=new QFormLayout(body);form->setFormAlignment(Qt::AlignTop);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto name=new QLineEdit(m_panels[i].name);name->setMaxLength(1024);auto note=new QPlainTextEdit(m_panels[i].note);note->setObjectName("statusPanelDescriptionEdit");note->setPlaceholderText("填写面板用途、设备说明或监视约定；仅用于说明，不参与解析。");note->setMinimumHeight(90);
    auto enabled=new QCheckBox("启用此面板");enabled->setObjectName("statusPanelEnabled");enabled->setChecked(m_panels[i].enabled);auto columns=new QSpinBox;columns->setObjectName("statusPanelColumns");columns->setRange(1,PanelLayout::StatusMaxColumns);columns->setValue(m_panels[i].columns);
    form->addRow("面板名称",name);form->addRow("说明",note);form->addRow("启用",enabled);form->addRow("每行最多列数",columns);
    auto binding=new StatusPanelBindingEditor(m_panels[i],m_context(),body);form->addRow(binding);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Save)->setText("保存");buttons->button(QDialogButtonBox::Cancel)->setText("取消");root->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{auto candidate=m_panels;auto &p=candidate[i];p.name=name->text().trimmed();p.note=note->toPlainText();p.enabled=enabled->isChecked();p.columns=columns->value();QString error;if(!binding->collect(p,error)||!replacePanels(candidate,error)){QMessageBox::warning(&dialog,"面板设置",error);return;}dialog.accept();});dialog.exec();
}
void StatusMonitorWindow::deletePanel(){int i=selectedIndex();if(i<0)return;QMessageBox box(QMessageBox::Question,"删除状态面板","删除面板 <b>“"+m_panels[i].name.toHtmlEscaped()+"”</b> 及其监视项？\n不会向设备发送命令。",QMessageBox::Yes|QMessageBox::Cancel,this);box.setTextFormat(Qt::RichText);box.button(QMessageBox::Yes)->setText("确认删除");box.button(QMessageBox::Cancel)->setText("取消");if(box.exec()!=QMessageBox::Yes)return;auto candidate=m_panels;candidate.removeAt(i);QString error;if(!replacePanels(candidate,error))report(error);}
void StatusMonitorWindow::copyPanel(){int i=selectedIndex();if(i<0)return;auto candidate=m_panels;auto copy=duplicate(candidate[i]);const auto base=candidate[i].name;int suffix=1;for(;;){bool found=false;for(const auto &p:candidate)if(p.name==copy.name)found=true;if(!found)break;copy.name=base+"_"+QString::number(++suffix);}candidate.insert(i+1,copy);QString error;if(!replacePanels(candidate,error)){report(error);return;}rebuildList(copy.id);}
void StatusMonitorWindow::movePanel(int direction){int i=selectedIndex(),j=i+direction;if(i<0||j<0||j>=m_panels.size())return;auto candidate=m_panels;candidate.swapItemsAt(i,j);QString error;if(!replacePanels(candidate,error))report(error);}
void StatusMonitorWindow::reorderItem(const QString &source,const QString &target){int p=selectedIndex();if(p<0||m_panels[p].layoutRows||source==target)return;int a=-1,b=-1;for(int i=0;i<m_panels[p].items.size();++i){if(m_panels[p].items[i].id==source)a=i;if(m_panels[p].items[i].id==target)b=i;}if(a<0||b<0)return;auto candidate=m_panels;candidate[p].items.move(a,b);QString error;if(!replacePanels(candidate,error))report(error);}
void StatusMonitorWindow::listMenu(const QPoint &pos){auto item=m_list->itemAt(pos);if(item)m_list->setCurrentItem(item);QMenu menu;auto add=menu.addAction("新增面板");auto edit=menu.addAction("面板设置 / 重命名");auto copy=menu.addAction("复制面板");auto up=menu.addAction("面板上移");auto down=menu.addAction("面板下移");auto del=menu.addAction("删除面板");menu.addSeparator();auto imp=menu.addAction("导入面板");auto current=menu.addAction("导出当前面板");auto all=menu.addAction("导出所有面板");for(auto a:{edit,copy,up,down,del,current})a->setEnabled(item);all->setEnabled(!m_panels.isEmpty());auto a=menu.exec(m_list->mapToGlobal(pos));if(a==add)addPanel();else if(a==edit)editPanel();else if(a==copy)copyPanel();else if(a==up)movePanel(-1);else if(a==down)movePanel(1);else if(a==del)deletePanel();else if(a==imp)importPanels();else if(a==current)exportPanels(false);else if(a==all)exportPanels(true);}
void StatusMonitorWindow::toggleItem(const QString &id){
    bool enablePanel=false;
    QString panelName,itemName;
    for(const auto &p:m_panels)for(const auto &r:p.items)if(r.id==id&&!p.enabled){enablePanel=true;panelName=p.name;itemName=r.name;}
    if(enablePanel){
        QMessageBox box(QMessageBox::Question,"启用状态面板",
            "当前面板 <b>“"+panelName.toHtmlEscaped()+"”</b> 已停用。<br><br>是否启用面板并打开监视项 <b>“"+itemName.toHtmlEscaped()+"”</b>？<br>其他监视项将恢复各自原来的启用 / 停用状态。",
            QMessageBox::Yes|QMessageBox::Cancel,this);
        box.setObjectName("statusEnablePanelConfirmation");box.setTextFormat(Qt::RichText);
        box.button(QMessageBox::Yes)->setText("启用面板并打开此项");box.button(QMessageBox::Cancel)->setText("取消");
        box.setDefaultButton(QMessageBox::Cancel);box.setEscapeButton(QMessageBox::Cancel);
        if(box.exec()!=QMessageBox::Yes)return;
    }
    // Re-read configuration after the modal dialog; never overwrite other edits.
    auto candidate=m_panels;
    for(auto &p:candidate)for(auto &r:p.items)if(r.id==id){
        if(enablePanel){p.enabled=true;r.enabled=true;}else r.enabled=!r.enabled;
        QString error;if(!replacePanels(candidate,error))report(error);return;
    }
}
void StatusMonitorWindow::editItem(const QString &id){int p=selectedIndex();if(p<0){report("请先新增或选择面板。");return;}Rule rule;int index=-1;for(int i=0;i<m_panels[p].items.size();++i)if(m_panels[p].items[i].id==id){rule=m_panels[p].items[i];index=i;break;}if(index<0&&!id.isEmpty())return;if(index<0){rule.id=newId();rule.name="新监视项";rule.protocolId=m_panels[p].protocolId;rule.protocolShape=m_panels[p].protocolShape;rule.mappings={{0,"状态0"},{1,"状态1"}};}auto context=m_context();const auto &panel=m_panels[p];context.inheritPanelBinding=panel.bindingMode!=BindingMode::Legacy;context.panelBindingPending=!context.inheritPanelBinding;context.panelProtocolId=panel.protocolId;context.panelProtocolShape=panel.protocolShape;context.panelProtocolName=panel.protocolName;StatusItemDialog editor(rule,panel.items,context,this);const auto panelId=m_panels[p].id;editor.setSaveHandler([this,panelId](const Rule &edited,QString &error){auto candidate=m_panels;for(auto &panel:candidate)if(panel.id==panelId){for(auto &item:panel.items)if(item.id==edited.id){item=edited;return replacePanels(candidate,error);}panel.items.append(edited);return replacePanels(candidate,error);}error="原面板已不存在，无法保存。";return false;});editor.exec();}
void StatusMonitorWindow::itemMenu(const QString &id,const QPoint &pos){
    int p=selectedIndex();if(p<0)return;int i=-1;for(int k=0;k<m_panels[p].items.size();++k)if(m_panels[p].items[k].id==id)i=k;if(i<0)return;
    auto original=m_panels[p].items[i];QMenu menu;menu.setObjectName("statusItemMenu");auto enabled=menu.addAction(m_panels[p].enabled&&original.enabled?"停用此监视项":"启用此监视项");enabled->setObjectName("statusToggleItem");enabled->setToolTip("启停此监视项；面板已停用时先确认启用面板，保留其他项原有设置。不断开设备连接。");auto edit=menu.addAction("编辑监视项");QAction *clear=nullptr;if(original.event.enabled){clear=menu.addAction("清除事件提示");clear->setObjectName("statusClearEvent");bool latched=false;auto ctx=m_context();if(ctx.session)for(const auto &s:ctx.session->snapshot())if(s.id==id)latched=s.eventLatched;clear->setEnabled(latched);clear->setToolTip("仅清除本项提示，不清零触发计数，不删除历史，不断开设备。进入匹配状态模式下，持续相同输入不会立即重新触发。");}menu.addSeparator();auto detail=menu.addAction("查看详情 / 说明");auto copy=menu.addAction("复制监视项");auto up=menu.addAction("前移");auto down=menu.addAction("后移");up->setEnabled(!m_panels[p].layoutRows&&i>0);down->setEnabled(!m_panels[p].layoutRows&&i+1<m_panels[p].items.size());auto value=menu.addAction("复制当前值 / 消息");auto state=menu.addAction("复制当前状态");auto del=menu.addAction("删除监视项");auto a=menu.exec(pos);if(!a)return;
    if(a==enabled){toggleItem(id);return;}
    if(a==clear){auto ctx=m_context();if(ctx.session)ctx.session->clearEvent(id);refresh();return;}
    if(a==edit){editItem(id);return;}
    if(a==detail){QString text=original.name+"\n\n说明：\n"+original.note;const auto ctx=m_context();if(ctx.session)for(const auto &s:ctx.session->snapshot())if(s.id==id)text+="\n\n当前："+statusName(s)+"\n"+s.reason+"\n"+timestamp(s.receiveUnixNs)+"\n"+messageText(ctx.session,s)+"\n"+s.detail;textDetails(this,"监视项详情",text);return;}
    if(a==value||a==state){auto ctx=m_context();if(ctx.session)for(const auto &s:ctx.session->snapshot())if(s.id==id)qApp->clipboard()->setText(a==state?statusName(s):original.source==Source::Text?messageText(ctx.session,s):QString::number(s.value,'g',15));return;}
    auto candidate=m_panels;
    if(a==copy){auto r=original;r.id=newId();int suffix=1;for(;;){r.name=original.name+"_"+QString::number(suffix++);bool exists=false;for(const auto &x:candidate[p].items)if(x.name==r.name)exists=true;if(!exists)break;}for(auto &row:r.textMatches)row.id=newId();candidate[p].items.insert(i+1,r);}
    else if(a==up&&i>0)candidate[p].items.swapItemsAt(i,i-1);
    else if(a==down&&i+1<candidate[p].items.size())candidate[p].items.swapItemsAt(i,i+1);
    else if(a==del){if(QMessageBox::question(this,"删除监视项","确认删除“"+original.name+"”？",QMessageBox::Yes|QMessageBox::Cancel)!=QMessageBox::Yes)return;candidate[p].items.removeAt(i);}else return;
    QString error;if(!replacePanels(candidate,error))report(error);
}
void StatusMonitorWindow::exportPanels(bool all){int i=selectedIndex();if(!all&&i<0)return;auto selected=all?m_panels:QVector<Panel>{m_panels[i]};QString suffix=all?"":"_"+m_panels[i].name.left(120);suffix.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1F]"),"_");auto path=QFileDialog::getSaveFileName(this,"导出状态面板","WaveLink_StatusPanels"+suffix+".json","JSON (*.json)");if(path.isEmpty())return;if(protectedExportPath(path,m_store.path())){report("请选择其他文件，不能覆盖软件内部的配置、备份或窗口偏好文件。");return;}QString error;auto bytes=encode(selected,error);if(bytes.isEmpty()){report(error);return;}QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())report(file.errorString());}
void StatusMonitorWindow::importPanels(){auto path=QFileDialog::getOpenFileName(this,"导入状态面板",{},"JSON (*.json)");if(path.isEmpty())return;QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>8*1024*1024){report("文件无法读取或超过 8 MiB。");return;}QVector<Panel> imported;QString error;if(!decode(file.readAll(),imported,error)){report(error);return;}QMessageBox choice(this);choice.setObjectName("statusStandaloneImportDialog");choice.setWindowTitle("导入状态面板");choice.setText(QString("文件含 %1 个面板。请选择导入方式。\n追加将保留现有面板并自动避免重名；覆盖将替换全部状态面板。\n独立面板文件不包含自定义协议库，缺失或不匹配的数值绑定不会自动改绑；跨电脑迁移建议使用系统配置导出。").arg(imported.size()));auto append=choice.addButton("追加",QMessageBox::AcceptRole);auto replace=choice.addButton("覆盖全部状态面板",QMessageBox::DestructiveRole);append->setObjectName("statusStandaloneAppend");replace->setObjectName("statusStandaloneReplace");auto cancel=choice.addButton("取消",QMessageBox::RejectRole);cancel->setObjectName("statusStandaloneCancel");choice.setDefaultButton(cancel);choice.setEscapeButton(cancel);choice.exec();QVector<Panel> candidate;if(choice.clickedButton()==append){candidate=m_panels;QStringList names;for(const auto &p:candidate)names<<p.name;for(const auto &p:imported){auto c=duplicateForAppend(p,names);names<<c.name;candidate.append(c);}}else if(choice.clickedButton()==replace)candidate=imported;else return;if(!replacePanels(candidate,error))report(error);}
void StatusMonitorWindow::showDiagnostics(){const auto ctx=m_context();QString text="当前接口："+ctx.interfaceName+"\n";if(ctx.session){const auto stats=ctx.session->textStatistics();for(auto it=stats.begin();it!=stats.end();++it){text+="\n来源："+it.key()+"\n";for(auto s=it.value().begin();s!=it.value().end();++s)text+=s.key()+"："+QString::number(s.value())+"\n";}for(const auto &d:ctx.session->textDiagnostics())text+=QString("\n[%1, %2) %3 · %4\n%5\n").arg(d.byteStart).arg(d.byteEnd).arg(d.code,d.reason,d.text);}textDetails(this,"接收来源诊断",text);}
void StatusMonitorWindow::showEvents(){m_renderedRevision=m_historyRevision;QHash<QString,QString> names;for(const auto &p:m_panels)for(const auto &r:p.items)names[r.id]=r.name;QVector<Event> filtered;for(const auto &e:m_history){const auto text=names.value(e.current.id,e.current.id)+statusName(e.previous)+statusName(e.current)+e.current.reason;if(m_filter.isEmpty()||text.contains(m_filter,Qt::CaseInsensitive))filtered.append(e);}m_events->setRowCount(filtered.size());for(int i=0;i<filtered.size();++i){const auto &e=filtered[i];QStringList cells{timestamp(e.unixNs),names.value(e.current.id,e.current.id),statusName(e.previous)+" → "+statusName(e.current),e.current.reason};for(int c=0;c<4;++c){auto item=m_events->item(i,c);if(!item){item=new QTableWidgetItem;m_events->setItem(i,c,item);}if(item->text()!=cells[c])item->setText(cells[c]);}}}
void StatusMonitorWindow::showHelp(){textDetails(this,"状态监视使用说明",
    "1. 新增面板，再添加监视项。一个面板可同时放置数值和文本状态。同名新面板自动添加 _1、_2 等后缀。\n\n"
    "2. 数值来源默认使用原始值，可选择换算后数值；与录制方式独立。映射/位段要求所选 8/16/32 位范围内的无符号整数，不接受负数或小数；例如监视 Ch1 的 bit0，0 显示停止、1 显示运行。阈值可处理浮点数，默认左闭右开，包含上下限可分别设置；区间不能重叠，空边界表示无界。生效延迟用于连续满足条件后的防抖；恢复回差仅用于勾选回差的阈值区间，事件模式不使用这两项。锁定模式的“选择协议”按系统协议、自定义协议分组，JustFloat 大小端分别列出；Raw 无数值通道，不可选。协议绑定统一放在面板设置；新面板默认锁定当前有效数值协议，所有通道监视项继承面板绑定。旧绑定一致时自动迁移，混合绑定保留原行为并提示统一；旧结构需明确选择后才更新。通道名随所选协议显示，序号不变；列表最多展开 10 项。选择统一更新本面板的通道数值监视，不切换采集协议；文本监视不受数值绑定影响。锁定模式核对标识及结构，跟随 ChN 需自行保证通道含义一致。\n\n"
    "3. 文本反馈不限于 ACK。完整匹配比较整条正文，前缀匹配保留其后详情；正文不要重复填写结束符。示例：UTF-8、结束符 0D0A，完整匹配 DEVICE READY，显示设备就绪；ERROR: 使用前缀匹配。文本监视直接读取接口 RX 字节，不受主界面“显示接收数据”、HEX 接收或接收显示缓存影响，匹配规则不是正则表达式，也不是在任意位置搜索关键词；多条命中时按表格顺序选择第一条。忽略大小写仅作用于 ASCII 英文字母；空白处理只去除消息首尾空格和 TAB。UDP 可选整报文，但不会自动删除 CR/LF；如果报文末尾带回车换行，建议使用结束符分帧。Raw 模式没有数值通道，但可监视文本；Demo 无设备文本来源。\n\n"
    "4. 新建监视项的状态有效期默认为“不自动过期”（0 ms），已有项保留已保存期限；事件模式同一配置显示为“数据静默超时”，新建也默认 0（关闭超时）。默认不过期不代表一直有效，断线、来源不匹配及非法数据仍按异常显示；周期状态如每秒收到 ALIVE，可设置状态有效期 3000 ms，未匹配消息不会续期。事件模式的数据静默超时与提示保持时间独立，合法完整消息（含屏蔽或未匹配消息）表示来源仍活跃，但不延长提示。同面板监视项可复用文本来源；复用参数只读，改为独立来源后不会影响其他项。\n\n"
    "5. 先生成样例/解析测试再保存。解析测试不向设备发送、不改变真实状态。关闭此面板窗口只隐藏界面，软件运行期间仍继续监视；停用面板或监视项才停止对应判断，退出软件后不会继续监视。顶部面板总开关与面板设置同步，重新启用时保留各监视项原来的启停设置。面板停用时点击监视项（或从右键菜单启用），需先确认启用面板并打开此项；其他项恢复原来的启停状态，取消则不作更改。断线立即失效，重连等待新数据。监视不关联发送命令、不自动重试、不改变命令开关。\n\n"
    "6. 支持本地保存、有效备份恢复和独立 JSON 导入导出。左下角导出所有保存全部面板，右键列表可导出当前。全局配置同时保存协议库与状态面板，导入可单独选择覆盖、追加或跳过；旧配置没有状态块时保留现有面板。独立追加会重新分配面板/监视项 ID、避免重名并保留协议引用；独立 JSON 不包含自定义协议库和窗口位置，跨电脑建议使用全局配置。只有全局配置在协议库一并追加导入时才同步映射协议 ID；缺失或过期的结构绑定不会自动改为跟随通道。\n\n"
    "7. 任意二进制与文本混传需设备有明确外层边界，不能仅扫描关键词证明反馈含义。超时、缺少通道、无效数值、可能遗漏反馈单独标记，不用正常状态掩盖异常。\n\n"
    "8. 卡片左键启停，短按右键打开菜单，右键长按编辑，可拖动排序；不会断开设备。Ctrl+S 只保存，左侧保存按钮保存并关闭；规则增删/排序支持 Ctrl+Z。规则颜色以色块展示（棋盘格为默认/透明），双击选择，颜色窗口为中文；拾取屏幕颜色下方的“使用透明颜色”清除自定义颜色，恢复主题默认显示，文字不会隐藏，保存监视项后生效。有效状态按严重程度显示独立标识：提示为蓝色、警告为橙色、错误为红色；“无”不显示标识。无效、停用或断线时不显示旧等级，等级不触发命令或弹窗。最近状态记录在内存中最多保留 2000 条，默认记录变化；文本实时项可选择记录每次匹配，事件模式可选择每次匹配触发。筛选仅影响显示，“导出记录”导出全部保留记录；“清空记录”不清零触发/屏蔽次数，也不清除卡片事件提示。退出软件不保留这些记录，不能替代长期日志。\n\n"
    "9. 监视模式在解析来源之后：实时状态（旧配置默认）按最新数据更新；生效延迟用于防抖，不是提示时长。事件提示逐帧捕获，新建默认保持至手动清除或下一事件（0 ms），也可选择定时清除；新事件覆盖当前提示，触发历史保留。右键“清除事件提示”不清零次数、不删除历史；重连、改变影响解析的配置或重启后次数重新累计；仅修改名称、说明、颜色等展示信息不会因此重置当前监视状态。\n\n"
    "10. 事件屏蔽最多 64 条，任一启用条件命中即静默。支持固定数值、闭区间（包含上下限）；文本来源还支持固定文本，遵循编码/空白/大小写设置，数值条件需整条正文可解析为有限十进制数字。数值筛选在位段提取前，浮点数建议用区间。默认数值背景 0 可修改、删除或取消勾选。屏蔽/未匹配内容不覆盖、不延长提示，不影响绘图录制和其他项。非法数据、缺少通道及断线优先显示异常。\n\n"
    "11. 重复触发：进入匹配状态时触发，连续命中同一规则只触发一次，遇到背景/未匹配内容后重新等待进入；每次匹配均触发则每帧/消息计数并刷新提示时间。500 Hz 标志通道可配置 10→状态A、20→状态B，屏蔽 0，提示默认保留至手动清除或下一事件；如只需短暂显示，可将“提示保持时间”改为 2000 ms；事件模式不使用防抖或恢复回差。");}
void StatusMonitorWindow::applyTheme(bool dark){m_dark=dark;m_list->setStyleSheet(PanelUi::listStyle(dark));auto listPalette=m_list->palette();for(auto role:{QPalette::Base,QPalette::Window})listPalette.setColor(role,QColor(dark?"#1e1e1e":"#ffffff"));listPalette.setColor(QPalette::Text,QColor(dark?"#dddddd":"#333333"));listPalette.setColor(QPalette::Highlight,QColor("#00a85a"));listPalette.setColor(QPalette::HighlightedText,Qt::white);m_list->setPalette(listPalette);m_list->viewport()->setPalette(listPalette);m_list->viewport()->update();setStyleSheet((dark?QString("QToolTip {background:#303030;color:white;border:1px solid #777;padding:6px;}"):QString("QToolTip {background:white;color:#202020;border:1px solid #aaa;padding:6px;}"))+PanelUi::listStyle(dark));for(auto &card:m_cards)applyCardTheme(card);refresh();}
