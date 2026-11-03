#include "statusmonitorwindow.h"
#include "ProtocolParser.h"
#include <QLabel>
#include <QLayout>
#include <QFrame>
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QSaveFile>
#include <QFile>
#include <QListWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QFontDatabase>
#include <QThread>
#include <QJsonDocument>
#include <QTimer>
#include <QInputDialog>
#include <QSpinBox>
#include <QKeyEvent>
#include <QShortcut>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QAbstractItemView>
#include <QAbstractItemDelegate>
#include <QStyleOptionViewItem>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QMessageBox>
#include <QStyledItemDelegate>
#include <QPainter>
#include <iostream>
using namespace StatusMonitoring;using namespace StatusPanels;
#define CHECK(c) do {if(!(c)){std::cerr<<"Status UI failure at "<<__LINE__<<": " #c "\n";return 1;}}while(0)
static void settle(){QElapsedTimer t;t.start();while(t.elapsed()<160){QCoreApplication::processEvents();QThread::msleep(2);}}
// Qt 5.14's Windows message-box system menu requires a native HWND, absent in
// offscreen tests. Keep the real confirmation/buttons, skip native system-menu setup.
class OffscreenMessageBoxGuard : public QObject {
    bool eventFilter(QObject *object,QEvent *event) override {
        return event->type()==QEvent::Show&&qobject_cast<QMessageBox*>(object);
    }
};
class Publisher:public ProtocolParser {
public:int processData(const char*,int size)override{return size;}void send(double n){publishChannels({n},{n*2});}
};
static bool write(const QString &path,const QByteArray &bytes){QSaveFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size()&&f.commit();}
static Panel example(){Panel p;p.id=newId();p.name="采集设备状态";p.note="通道数值与文本反馈可以同时监视；所有状态均来自实际接收。";
    Rule n;n.id=newId();n.name="设备工作模式";n.note="0 为待机，1 为采集中。";n.mappings={{0,"待机","#777777"},{1,"采集中","#16854b"}};
    auto t=Rule::textRule(newId(),"最近设备反馈");t.textSource.id=newId();t.note="自定义反馈；不会向设备发送任何命令。";t.textMatches={{"DEVICE READY","已就绪",false,newId(),"#16854b"},{"ERROR:","设备故障",true,newId(),"#c23434"}};
    Rule b=n;b.id=newId();b.name="故障标志 bit0";b.kind=RuleKind::Bits;b.mappings={{0,"无故障","#16854b"},{1,"存在故障","#c23434"}};
    Rule r=n;r.id=newId();r.name="温度状态";r.kind=RuleKind::Threshold;r.mappings.clear();Range low,high;low.upper=80;low.state="温度正常";low.color="#16854b";high.lower=80;high.state="温度过高";high.color="#c23434";high.retainWithHysteresis=true;r.ranges={low,high};r.hysteresis=2;
    p.items={n,t,b,r};return p;}
int main(int argc,char **argv){QApplication app(argc,argv);QCoreApplication::setApplicationName("WaveLinkStatusUiTest");QTemporaryDir dir;CHECK(dir.isValid());
    OffscreenMessageBoxGuard modalGuard;if(QApplication::platformName()=="offscreen")app.installEventFilter(&modalGuard);
    const int fontId=QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");if(fontId>=0)app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first(),10));
    auto p=example();QString error;auto bytes=encode({p},error);CHECK(!bytes.isEmpty());QVector<Panel> decoded;CHECK(decode(bytes,decoded,error));CHECK(decoded[0].items[1].textMatches[0].text=="DEVICE READY"&&decoded[0].items[3].ranges[1].color=="#c23434");
    auto bad=QJsonDocument::fromJson(bytes).object();bad["schemaVersion"]=2;CHECK(!decode(QJsonDocument(bad).toJson(),decoded,error));CHECK(decoded[0].id==p.id);
    auto copy=duplicate(p);CHECK(copy.id!=p.id&&copy.items[0].id!=p.items[0].id&&copy.items[1].textSource.id!=p.items[1].textSource.id);
    const auto storePath=dir.filePath("store.json");Store store(storePath);CHECK(store.load(decoded,error));CHECK(store.save({p},error));Store other(storePath);CHECK(other.load(decoded,error));CHECK(store.save({p,copy},error));CHECK(!other.save({p},error));
    CHECK(write(storePath,"{broken"));Store damaged(storePath);CHECK(!damaged.load(decoded,error));CHECK(!damaged.save({},error));
    const auto journal=dir.filePath("global-config-transaction.json");CHECK(write(journal,"{}"));CHECK(!damaged.restoreBackup(decoded,error));CHECK(QFile::remove(journal));
    CHECK(damaged.restoreBackup(decoded,error));CHECK(decoded.size()==1&&decoded[0].id==p.id);CHECK(!QDir(dir.path()).entryList({"*.before-repair-*.json"}).isEmpty());
    Session session;Publisher publisher;session.bind(&publisher,"builtin.firewater");session.setConnected(true);
    StatusUiContext ctx;ctx.session=&session;ctx.connected=true;ctx.interfaceName="串口 · 软件验证";ctx.protocolName="FireWater";ctx.protocolId="builtin.firewater";ctx.protocolShape="test-shape";
    const auto windowPath=dir.filePath("window.json");const auto artifacts=QDir::current().filePath("artifacts");QDir().mkpath(artifacts);
    {
        StatusMonitorWindow window([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){return session.configure(r,e);},nullptr,windowPath);window.show();settle();auto list=window.findChild<QListWidget*>("statusPanelList");CHECK(list&&list->count()==0);CHECK(window.findChild<QLabel*>("statusWelcome")->isVisible());
        CHECK(window.replacePanels({p,copy},error));settle();CHECK(list->count()==2);CHECK(window.findChildren<QFrame*>("statusCard").size()==4);
        for (bool dark : {false, true}) {
            window.applyTheme(dark);
            QPushButton *help = nullptr; for (auto button : window.findChildren<QPushButton *>()) if (button->text() == "使用说明") help = button;
            CHECK(help); bool checked = false;
            QTimer::singleShot(0, &window, [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
                auto body = dialog->findChild<QPlainTextEdit *>(); const auto text = body ? body->toPlainText() : QString();
                checked = dialog->windowTitle() == "状态监视使用说明" && body &&
                    text.contains("不自动过期") && text.contains("默认 0（关闭超时）") &&
                    text.contains("无符号整数") && text.contains("ASCII 英文字母") &&
                    text.contains("导出全部保留记录") && text.contains("改变影响解析的配置") &&
                    text.contains("手动清除或下一事件") && text.contains("独立 JSON 不包含自定义协议库") &&
                    !dialog->windowFlags().testFlag(Qt::WindowContextHelpButtonHint) &&
                    dialog->windowFlags().testFlag(Qt::WindowMinimizeButtonHint) &&
                    dialog->windowFlags().testFlag(Qt::WindowMaximizeButtonHint);
                dialog->resize(560, 420); qApp->processEvents();
                checked = checked && body->horizontalScrollBar()->maximum() == 0;
                dialog->grab().save(artifacts + QString("/status-help-%1-%2.png").arg(dark).arg(dialog->devicePixelRatioF()));
                dialog->reject();
            });
            help->click(); CHECK(checked);
        }
        publisher.send(1);session.feedReceived("DEVICE READY\r\n",1792000000000000000LL);settle();CHECK(session.snapshot().size()==8);bool textReady=false;for(const auto &s:session.snapshot())if(s.id==p.items[1].id)textReady=s.state=="已就绪";CHECK(textReady);
        auto filter=window.findChild<QLineEdit*>("statusEventFilter");auto events=window.findChild<QTableWidget*>("statusEventTable");CHECK(filter&&events);filter->setText("不存在的状态");CHECK(events->rowCount()==0);filter->clear();CHECK(events->rowCount()>0);
        // Saving display-only changes must preserve numeric and text samples.
        auto renamed=p;renamed.items[0].name="工作模式（已重命名）";CHECK(window.replacePanels({renamed,copy},error));CHECK(session.snapshot()[0].quality==Quality::Valid);
        for(bool dark:{false,true}){app.setStyleSheet(dark?"QWidget {background:#1e1e1e;color:#eeeeee;} QLineEdit,QPlainTextEdit,QTableWidget,QListWidget {background:#262626;}":"QWidget {background:#f0f0f0;color:#202020;} QLineEdit,QPlainTextEdit,QTableWidget,QListWidget {background:white;}");window.applyTheme(dark);window.resize(1100,730);settle();window.grab().save(artifacts+(dark?"/status-panel-dark.png":"/status-panel-light.png"));
            window.resize(660,480);settle();auto scroll=window.findChild<QScrollArea*>("statusCardScroll");CHECK(scroll->horizontalScrollBar()->maximum()==0);for(auto frame:window.findChildren<QFrame*>("statusCard"))CHECK(frame->geometry().right()<=frame->parentWidget()->width());}
        auto cards=window.findChildren<QFrame*>("statusCard");QMimeData dragData;dragData.setData("application/x-wavelink-status-item",renamed.items[0].id.toUtf8());QDragEnterEvent enter(QPoint(10,10),Qt::MoveAction,&dragData,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(cards[2],&enter);CHECK(enter.isAccepted());QDropEvent drop(QPointF(10,10),Qt::MoveAction,&dragData,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(cards[2],&drop);settle();CHECK(drop.isAccepted());CHECK(window.panels()[0].items[2].id==renamed.items[0].id);
        list->setCurrentRow(1);window.close();CHECK(!window.isVisible());publisher.send(0);window.show();settle();CHECK(list->currentRow()==1);
    }
    {StatusMonitorWindow reopened([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){return session.configure(r,e);},nullptr,windowPath);CHECK(reopened.panels().size()==2);CHECK(reopened.findChild<QListWidget*>("statusPanelList")->currentRow()==1);}
    app.setStyleSheet("QWidget {background:#f0f0f0;color:#202020;}");
    {
        StatusItemDialog dialog(p.items[0],p.items,ctx);dialog.show();dialog.activateWindow();settle();
        auto channel=dialog.findChild<QComboBox*>("statusChannel");CHECK(channel->count()==256&&channel->maxVisibleItems()==10);channel->showPopup();settle();CHECK(channel->view()->height()<=channel->view()->sizeHintForRow(0)*10+10);CHECK(channel->view()->verticalScrollBar()->maximum()>0);channel->hidePopup();channel->setCurrentIndex(255);Rule collected;CHECK(dialog.collect(collected,error)&&collected.channel==255);
        CHECK(!dialog.findChild<QComboBox*>("statusProtocolBinding")&&dialog.collect(collected,error)&&collected.protocolId.isEmpty());
        auto first=dialog.findChild<QSpinBox*>("statusFirstBit");auto count=dialog.findChild<QSpinBox*>("statusBitCount");CHECK(!first->isVisible()&&!count->isVisible());auto kind=dialog.findChild<QComboBox*>("statusNumericKind");kind->setCurrentIndex(1);CHECK(first->isVisible()&&count->isVisible());kind->setCurrentIndex(2);CHECK(!first->isVisible()&&!count->isVisible());kind->setCurrentIndex(0);
        auto hold=dialog.findChild<QComboBox*>("statusHoldPreset");auto timeout=dialog.findChild<QComboBox*>("statusTimeoutPreset");auto choose=[](QComboBox *c,int value){int i=c->findData(value);c->setCurrentIndex(i);QMetaObject::invokeMethod(c,"activated",Q_ARG(int,i));};choose(hold,500);choose(timeout,0);CHECK(dialog.collect(collected,error)&&collected.holdMs==500&&collected.timeoutMs==0);auto timeoutSpin=dialog.findChild<QSpinBox*>("statusTimeout");timeoutSpin->setValue(4567);CHECK(timeout->currentData().toInt()==-1);choose(hold,0);CHECK(dialog.collect(collected,error)&&collected.holdMs==0&&collected.timeoutMs==4567);
        auto table=dialog.findChild<QTableWidget*>("statusRuleTable");auto add=dialog.findChild<QPushButton*>("statusAddRule");auto remove=dialog.findChild<QPushButton*>("statusDeleteRule");auto undo=[&]{add->setFocus();settle();QKeyEvent key(QEvent::KeyPress,Qt::Key_Z,Qt::ControlModifier,"z");QApplication::sendEvent(add,&key);settle();};
        const int rows=table->rowCount();add->click();CHECK(table->rowCount()==rows+1&&table->item(rows,1)->text()=="状态"+QString::number(rows));undo();CHECK(table->rowCount()==rows);table->item(0,1)->setText("应完整恢复的名称");table->selectRow(0);remove->click();CHECK(table->rowCount()==rows-1);undo();CHECK(table->rowCount()==rows&&table->item(0,1)->text()=="应完整恢复的名称");
        auto source=dialog.findChild<QComboBox*>("statusSourceKind");source->setCurrentIndex(1);table->item(0,1)->setText("READY");auto rowId=table->item(0,0)->data(Qt::UserRole);table->selectRow(0);remove->click();CHECK(table->rowCount()==0);undo();CHECK(table->item(0,0)->data(Qt::UserRole)==rowId&&table->item(0,1)->text()=="READY");add->click();CHECK(!table->item(1,2)->text().isEmpty());source->setCurrentIndex(0);undo();CHECK(table->rowCount()==rows);source->setCurrentIndex(1);CHECK(table->rowCount()==2);undo();CHECK(table->rowCount()==1);source->setCurrentIndex(0);kind->setCurrentIndex(2);add->click();CHECK(!table->item(table->rowCount()-1,4)->text().isEmpty());
    }
    {
        StatusItemDialog dialog(p.items[0],p.items,ctx);dialog.show();settle();auto source=dialog.findChild<QComboBox*>("statusSourceKind");auto table=dialog.findChild<QTableWidget*>("statusRuleTable");CHECK(source&&table);table->item(0,1)->setText("未保存的数值状态");source->setCurrentIndex(1);table->item(0,1)->setText("设备就绪");table->item(0,2)->setText("就绪");source->setCurrentIndex(0);CHECK(table->item(0,1)->text()=="未保存的数值状态");source->setCurrentIndex(1);CHECK(table->item(0,1)->text()=="设备就绪");Rule r;CHECK(dialog.collect(r,error));CHECK(r.timeoutMs==0&&r.source==Source::Text);dialog.findChild<QPushButton*>("statusGenerateSample")->click();dialog.findChild<QPushButton*>("statusRunTest")->click();CHECK(dialog.findChild<QPlainTextEdit*>("statusTestOutput")->toPlainText().contains("就绪"));dialog.grab().save(artifacts+"/status-editor-text-light.png");
        source->setCurrentIndex(0);table->item(0,0)->setText("中文");CHECK(!dialog.collect(r,error));
    }
    for(int i:{1,2,3}){StatusItemDialog dialog(p.items[i],p.items,ctx);dialog.show();settle();Rule result;CHECK(dialog.collect(result,error));dialog.grab().save(artifacts+QString("/status-editor-%1.png").arg(i));}
    for(bool dark:{false,true}){
        auto panel=p;panel.note.clear();panel.items[0].mappings[1].severity="提示";panel.items[2].mappings[1].severity="警告";panel.items[3].ranges[0].severity="错误";panel.items[1].textMatches[0].severity="错误";
        const auto path=dir.filePath(dark?"panel-toggle-dark.json":"panel-toggle-light.json");StatusMonitorWindow window([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){return session.configure(r,e);},nullptr,path);auto toggle=window.findChild<QPushButton*>("statusPanelToggle");CHECK(toggle&&!toggle->isEnabled());CHECK(window.replacePanels({panel},error));window.applyTheme(dark);window.show();publisher.send(1);session.feedReceived("DEVICE READY\r\n",1792000000000000000LL);settle();CHECK(toggle->isChecked()&&toggle->isEnabled());
        const QStringList severities{"提示","错误","警告","错误"};auto cards=window.findChildren<QFrame*>("statusCard");for(int i=0;i<cards.size();++i){auto badge=cards[i]->findChild<QLabel*>("statusSeverity");CHECK(badge&&badge->isVisible()&&badge->text()=="严重程度："+severities[i]&&!badge->styleSheet().isEmpty());auto title=cards[i]->findChild<QLabel*>("statusCardTitle");CHECK(title&&title->isVisible()&&cards[i]->layout()->itemAt(0)->widget()==title&&title->geometry().bottom()<badge->geometry().top());}
        window.resize(1100,730);settle();window.grab().save(artifacts+(dark?"/status-severity-dark.png":"/status-severity-light.png"));
        panel.items[2].enabled=false;CHECK(window.replacePanels({panel},error));toggle->click();settle();CHECK(!window.panels()[0].enabled&&!toggle->isChecked()&&!window.panels()[0].items[2].enabled&&ctx.connected);for(auto badge:window.findChildren<QLabel*>("statusSeverity"))CHECK(!badge->isVisible());for(const auto &s:session.snapshot())CHECK(s.quality==Quality::Disabled);
        toggle->click();settle();CHECK(window.panels()[0].enabled&&toggle->isChecked()&&!window.panels()[0].items[2].enabled&&window.panels()[0].items[0].enabled);
        bool placeholder=false,synced=false;QTimer::singleShot(0,&window,[&]{auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;auto note=dialog->findChild<QPlainTextEdit*>("statusPanelDescriptionEdit");auto enabled=dialog->findChild<QCheckBox*>("statusPanelEnabled");placeholder=note&&note->toPlainText().isEmpty()&&!note->placeholderText().isEmpty();synced=enabled&&enabled->isChecked();enabled->setChecked(false);dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});window.findChild<QPushButton*>("statusPanelSettings")->click();CHECK(placeholder&&synced&&!toggle->isChecked()&&!window.panels()[0].enabled);
        Store persisted(path);QVector<Panel> loaded;CHECK(persisted.load(loaded,error)&&!loaded[0].enabled&&!loaded[0].items[2].enabled&&loaded[0].items[0].enabled);
    }
    for(bool dark:{false,true}){
        app.setStyleSheet((dark?QString("QWidget {background:#1e1e1e;color:#eeeeee;}"):QString("QWidget {background:#f0f0f0;color:#202020;}"))+"QComboBox,QSpinBox,QDoubleSpinBox {padding:2px;border:1px solid #888888;}");
        StatusItemDialog dialog(p.items[0],p.items,ctx);dialog.show();
        for(const auto &size:{QSize(1080,760),QSize(1920,1080)})for(int mode:{0,1,0,2,0,3,0}){
            auto source=dialog.findChild<QComboBox*>("statusSourceKind");auto kind=dialog.findChild<QComboBox*>("statusNumericKind");source->setCurrentIndex(mode==3?1:0);if(mode<3)kind->setCurrentIndex(mode);dialog.resize(size);settle();
            auto contents=dialog.findChild<QWidget*>("statusEditorForm");auto timeout=dialog.findChild<QSpinBox*>("statusTimeout");auto hold=dialog.findChild<QSpinBox*>("statusHoldTime");
            CHECK(contents&&contents->height()<=contents->sizeHint().height()+2);
            auto width=dialog.findChild<QComboBox*>("statusWordWidth");if(mode==0||mode==1){if(!width->isVisible()||width->height()<width->sizeHint().height()){std::cerr<<"width geometry mode="<<mode<<" dark="<<dark<<" window="<<dialog.width()<<"x"<<dialog.height()<<" height="<<width->height()<<" hint="<<width->sizeHint().height()<<" min="<<width->minimumSizeHint().height()<<" source="<<source->height()<<" numericPage="<<width->parentWidget()->height()<<" pageHint="<<width->parentWidget()->sizeHint().height()<<" visible="<<width->isVisible()<<"\n";dialog.grab().save(artifacts+"/status-width-failure.png");}CHECK(width->isVisible()&&width->height()>=width->sizeHint().height());CHECK(std::abs(width->height()-source->height())<=2);}else CHECK(!width->isVisible());
            if(mode<3){const int gap=timeout->mapTo(contents,QPoint()).y()-hold->mapTo(contents,QPoint(0,hold->height())).y();CHECK(gap>=0&&gap<=20);}
            auto note=dialog.findChild<QPlainTextEdit*>("statusItemDescription");auto add=dialog.findChild<QPushButton*>("statusAddRule");const int gap=note->mapTo(contents,QPoint()).y()-add->mapTo(contents,QPoint(0,add->height())).y();CHECK(gap>=0&&gap<=20);
            auto save=dialog.findChild<QPushButton*>("statusSaveItem");CHECK(save->height()>=42&&save->width()>=210&&save->text()=="保存监视项"&&!save->styleSheet().isEmpty());CHECK(save->parentWidget()==dialog.findChild<QWidget*>("statusEditorSide"));
        }
        dialog.grab().save(artifacts+(dark?"/status-editor-compact-dark.png":"/status-editor-compact-light.png"));
        auto table=dialog.findChild<QTableWidget*>("statusRuleTable");for(int i=0;i<55;++i)dialog.findChild<QPushButton*>("statusAddRule")->click();settle();auto scroll=dialog.findChild<QScrollArea*>("statusEditorScroll");CHECK(table->verticalScrollBarPolicy()==Qt::ScrollBarAlwaysOff&&scroll->verticalScrollBar()->maximum()>0);
    }
    app.setStyleSheet("QWidget {background:#f0f0f0;color:#202020;}");
    {
        auto locked=p.items[0];locked.protocolId="custom-eeg";locked.protocolShape="original-structure";auto context=ctx;context.protocolNames.insert(locked.protocolId,"EEG_Fatigue_V1.2");
        context.inheritPanelBinding=true;context.panelProtocolId=locked.protocolId;context.panelProtocolShape=locked.protocolShape;
        StatusItemDialog dialog(locked,p.items,context);dialog.show();settle();auto hint=dialog.findChild<QLabel*>("statusBindingHint");CHECK(hint->isVisible()&&hint->text().contains("EEG_Fatigue_V1.2")&&hint->text().contains("当前使用其他协议"));CHECK(!dialog.findChild<QComboBox*>("statusProtocolBinding")&&!dialog.findChild<QComboBox*>("statusProtocolChoice"));Rule result;CHECK(dialog.collect(result,error)&&result.protocolId==locked.protocolId&&result.protocolShape==locked.protocolShape);
        context.protocolId=locked.protocolId;context.protocolShape="changed-structure";StatusItemDialog changed(locked,p.items,context);CHECK(changed.findChild<QLabel*>("statusBindingHint")->text().contains("结构已变更"));CHECK(changed.collect(result,error)&&result.protocolShape=="original-structure");
        auto text=p.items[1];StatusItemDialog fromText(text,p.items,context);fromText.findChild<QComboBox*>("statusSourceKind")->setCurrentIndex(0);CHECK(fromText.collect(result,error)&&result.protocolId==locked.protocolId);
        auto close=dialog.findChild<QPushButton*>("statusCloseEditor");auto save=dialog.findChild<QPushButton*>("statusSaveItem");CHECK(close&&close->parentWidget()==save->parentWidget()&&save->geometry().bottom()<close->geometry().top());CHECK(dialog.findChild<QPushButton*>("statusRunTest")->text()=="解析测试");
    }
    {
        QVector<StatusProtocolOption> options;StatusProtocolOption csv;csv.id=ctx.protocolId;csv.name="FireWater（CSV 文本）";csv.shape=ctx.protocolShape;options.append(csv);
        StatusProtocolOption raw;raw.id="builtin.raw";raw.name="Raw";raw.shape="raw-shape";raw.numeric=false;options.append(raw);
        for(bool big:{false,true}){StatusProtocolOption f;f.id="builtin.justfloat";f.name=big?"JustFloat · 大端":"JustFloat · 小端";f.shape=big?"float-big":"float-little";options.append(f);}
        StatusProtocolOption custom;custom.id="custom-temperature";custom.name="温度与状态";custom.shape="temperature-v2";custom.channelNames=QStringList{"温度","设备状态"};custom.custom=true;options.append(custom);
        auto context=ctx;context.channelNames=QStringList{"当前接口通道"};context.protocolNames.insert(custom.id,custom.name);int reads=0;context.protocolCatalog=[&]{++reads;return options;};
        auto panel=p;panel.bindingMode=BindingMode::Locked;panel.protocolId=ctx.protocolId;panel.protocolShape=ctx.protocolShape;
        StatusPanelBindingEditor picker(panel,context);picker.show();settle();CHECK(reads==1);auto choices=picker.findChild<QComboBox*>("statusProtocolChoice");auto mode=picker.findChild<QComboBox*>("statusProtocolBinding");CHECK(choices&&choices->maxVisibleItems()==10);for(auto text:{"系统协议","自定义协议"})CHECK(!(choices->model()->flags(choices->model()->index(choices->findText(text),0))&Qt::ItemIsSelectable));CHECK(!(choices->model()->flags(choices->model()->index(choices->findData(1),0))&Qt::ItemIsEnabled));
        CHECK(picker.collect(panel,error)&&panel.protocolId==ctx.protocolId);choices->setCurrentIndex(choices->findData(4));CHECK(picker.collect(panel,error)&&panel.protocolId==custom.id&&panel.protocolShape==custom.shape&&panel.protocolName==custom.name);for(const auto &item:panel.items)if(item.source==Source::Channel)CHECK(item.protocolId==custom.id&&item.protocolShape==custom.shape);
        context.inheritPanelBinding=true;context.panelProtocolId=panel.protocolId;context.panelProtocolShape=panel.protocolShape;context.panelProtocolName=panel.protocolName;StatusItemDialog item(panel.items[0],panel.items,context);CHECK(item.findChild<QComboBox*>("statusChannel")->itemText(0)=="Ch1 · 温度");auto channel=item.findChild<QComboBox*>("statusChannel");channel->setCurrentIndex(7);CHECK(item.findChild<QLabel*>("statusBindingHint")->text().contains("超出"));Rule selected;CHECK(item.collect(selected,error)&&selected.channel==7);
        choices->setCurrentIndex(choices->findData(2));CHECK(picker.collect(panel,error)&&panel.protocolShape=="float-little");choices->setCurrentIndex(choices->findData(3));CHECK(picker.collect(panel,error)&&panel.protocolShape=="float-big");
        mode->setCurrentIndex(1);CHECK(picker.collect(panel,error)&&panel.bindingMode==BindingMode::Follow&&panel.protocolId.isEmpty());for(const auto &item:panel.items)if(item.source==Source::Channel)CHECK(item.protocolId.isEmpty());picker.findChild<QPushButton*>("statusRebindCurrentProtocol")->click();CHECK(picker.collect(panel,error)&&panel.protocolId==ctx.protocolId&&panel.protocolShape==ctx.protocolShape&&reads==2);
        panel.protocolId=custom.id;panel.protocolShape="temperature-v1";StatusPanelBindingEditor stale(panel,context);CHECK(stale.findChild<QComboBox*>("statusProtocolChoice")->currentText().contains("保留结构"));CHECK(stale.collect(panel,error)&&panel.protocolShape=="temperature-v1");stale.findChild<QComboBox*>("statusProtocolChoice")->setCurrentIndex(stale.findChild<QComboBox*>("statusProtocolChoice")->findData(4));CHECK(stale.collect(panel,error)&&panel.protocolShape==custom.shape);
        picker.resize(620,400);settle();picker.grab().save(artifacts+"/status-panel-protocol-selector.png");
    }
    for(int focusKind=0;focusKind<6;++focusKind){
        StatusItemDialog dialog(p.items[0],p.items,ctx);int saves=0;dialog.setSaveHandler([&](const Rule&,QString&){++saves;return true;});dialog.show();dialog.activateWindow();dialog.raise();settle();
        auto name=dialog.findChild<QLineEdit*>("statusItemName");name->setText("快捷键保存");auto table=dialog.findChild<QTableWidget*>("statusRuleTable");QWidget *target=name;
        if(focusKind==1)target=dialog.findChild<QPlainTextEdit*>("statusItemDescription");
        if(focusKind==2)target=dialog.findChild<QPlainTextEdit*>("statusTestInput");
        if(focusKind==3){table->editItem(table->item(0,1));settle();for(auto edit:table->findChildren<QLineEdit*>())if(edit->isVisible()){edit->setText("未失焦的单元格");target=edit;break;}CHECK(target!=name);}
        if(focusKind==4)target=dialog.findChild<QComboBox*>("statusSourceKind");
        if(focusKind==5){auto spin=dialog.findChild<QSpinBox*>("statusTimeout");spin->setKeyboardTracking(false);target=spin->findChild<QLineEdit*>();static_cast<QLineEdit*>(target)->setText("4567 ms");}
        target->setFocus();settle();CHECK(target->hasFocus());auto shortcut=dialog.findChild<QShortcut*>("statusSaveShortcut");CHECK(shortcut&&shortcut->key()==QKeySequence(Qt::CTRL|Qt::Key_S));
        QKeyEvent press(QEvent::KeyPress,Qt::Key_S,Qt::ControlModifier,"s");QApplication::sendEvent(target,&press);QKeyEvent release(QEvent::KeyRelease,Qt::Key_S,Qt::ControlModifier,"s");QApplication::sendEvent(&dialog,&release);settle();CHECK(dialog.isVisible()&&saves==1);CHECK(dialog.resultRule().name=="快捷键保存");if(focusKind==3)CHECK(dialog.resultRule().mappings[0].state=="未失焦的单元格");if(focusKind==5)CHECK(dialog.resultRule().timeoutMs==4567);dialog.findChild<QPushButton*>("statusSaveItem")->click();CHECK(dialog.result()==QDialog::Accepted&&!dialog.isVisible()&&saves==2);
    }
    {
        auto text=p.items[1];auto shared=text;shared.id=newId();shared.name="共享反馈";auto siblings=p.items;siblings.append(shared);
        StatusItemDialog dialog(text,siblings,ctx);auto source=dialog.findChild<QComboBox*>("statusTextSource");CHECK(source->currentIndex()>0);source->setCurrentIndex(0);Rule independent;CHECK(dialog.collect(independent,error));CHECK(independent.textSource.id!=text.textSource.id);
    }
    for(bool bits:{true,false}){
        auto rule=bits?p.items[2]:p.items[3];if(bits){rule.firstBit=4;rule.mappings={{1,"生成成功"}};}else {Range range;range.lower=100;range.upper=200;range.state="生成成功";rule.ranges={range};}
        StatusItemDialog dialog(rule,p.items,ctx);dialog.findChild<QPushButton*>("statusGenerateSample")->click();dialog.findChild<QPushButton*>("statusRunTest")->click();CHECK(dialog.findChild<QPlainTextEdit*>("statusTestOutput")->toPlainText().contains("生成成功"));
    }
    {
        StatusMonitorWindow window([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){return session.configure(r,e);},nullptr,dir.filePath("polish.json"));CHECK(window.replacePanels({p},error));window.show();settle();
        auto list=window.findChild<QListWidget*>("statusPanelList");CHECK(list->item(0)->sizeHint().height()==44);CHECK(window.findChild<QPushButton*>("statusExportAll")->text()=="导出所有");
        for(const auto &name:{"statusAddPanel","statusAddItem","statusPanelSettings","statusRestoreBackup","statusExportAll"})CHECK(!window.findChild<QPushButton*>(name)->toolTip().isEmpty());
        QTimer::singleShot(0,&window,[&]{auto dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());if(dialog){dialog->setTextValue(p.name);dialog->accept();}});window.findChild<QPushButton*>("statusAddPanel")->click();CHECK(window.panels().size()==2&&window.panels()[1].name==p.name+"_1");
        bool flags=false;QTimer::singleShot(0,&window,[&]{auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(dialog){flags=(dialog->windowFlags()&Qt::WindowMinMaxButtonsHint)==Qt::WindowMinMaxButtonsHint&&!(dialog->windowFlags()&Qt::WindowContextHelpButtonHint);dialog->reject();}});window.findChild<QPushButton*>("statusPanelSettings")->click();CHECK(flags);
        StatusItemDialog editor(p.items[0],p.items,ctx);CHECK((editor.windowFlags()&Qt::WindowMinMaxButtonsHint)==Qt::WindowMinMaxButtonsHint&&!(editor.windowFlags()&Qt::WindowContextHelpButtonHint));
    }
    {
        // Panel enable confirmation must preserve item-level choices and persist once.
        auto panel=p;panel.enabled=false;panel.items[0].enabled=true;panel.items[1].enabled=false;panel.items[2].enabled=true;panel.items[3].enabled=false;
        int applies=0;const auto path=dir.filePath("disabled-panel.json");
        StatusMonitorWindow window([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){++applies;return session.configure(r,e);},nullptr,path);
        window.show();
        auto click=[&](QWidget *w,Qt::MouseButton button){const QPoint pos(20,20);QMouseEvent press(QEvent::MouseButtonPress,pos,w->mapToGlobal(pos),button,button,Qt::NoModifier);QApplication::sendEvent(w,&press);QMouseEvent release(QEvent::MouseButtonRelease,pos,w->mapToGlobal(pos),button,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(w,&release);};
        for(int target:{0,1})for(bool confirm:{false,true}){
            CHECK(window.replacePanels({panel},error));settle();const int before=applies;
            bool prompted=false;
            QTimer::singleShot(30,&window,[&]{auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());if(!box)return;prompted=box->objectName()=="statusEnablePanelConfirmation"&&box->text().contains(panel.name)&&box->text().contains(panel.items[target].name)&&box->defaultButton()==box->button(QMessageBox::Cancel);box->button(confirm?QMessageBox::Yes:QMessageBox::Cancel)->click();});
            auto cards=window.findChildren<QFrame*>("statusCard");click(cards[target],Qt::LeftButton);settle();CHECK(prompted);
            CHECK(window.panels()[0].enabled==confirm&&applies==before+(confirm?1:0));
            for(int i=0;i<panel.items.size();++i)CHECK(window.panels()[0].items[i].enabled==(confirm&&i==target?true:panel.items[i].enabled));
            Store saved(path);QVector<Panel> loaded;CHECK(saved.load(loaded,error)&&loaded[0].enabled==confirm);
            for(int i=0;i<panel.items.size();++i)CHECK(loaded[0].items[i].enabled==window.panels()[0].items[i].enabled);
            CHECK(ctx.connected);const auto effective=flatten(window.panels());
            for(int i=0;i<effective.size();++i)CHECK(effective[i].enabled==(confirm&&window.panels()[0].items[i].enabled));
            for(const auto &state:session.snapshot())if(!confirm||state.id==panel.items[3].id)CHECK(state.quality==Quality::Disabled);
        }
        CHECK(window.replacePanels({panel},error));settle();bool menuSeen=false,prompted=false;
        QTimer::singleShot(30,&window,[&]{auto menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;auto action=menu->findChild<QAction*>("statusToggleItem");menuSeen=action&&action->text()=="启用此监视项";if(!action){menu->close();return;}QTimer::singleShot(30,&window,[&]{auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());if(box){prompted=true;box->button(QMessageBox::Yes)->click();}});menu->setActiveAction(action);QKeyEvent key(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&key);});
        click(window.findChildren<QFrame*>("statusCard").first(),Qt::RightButton);settle();CHECK(menuSeen&&prompted&&window.panels()[0].enabled&&window.panels()[0].items[0].enabled&&!window.panels()[0].items[1].enabled);
    }
    {
        int applies=0;StatusMonitorWindow window([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){++applies;return session.configure(r,e);},nullptr,dir.filePath("interaction.json"));CHECK(window.replacePanels({p},error));const int initialApplies=applies;window.show();window.activateWindow();settle();
        auto card=window.findChildren<QFrame*>("statusCard").first();const auto id=card->property("ruleId").toString();auto click=[&](QWidget *w,Qt::MouseButton button){const QPoint pos(20,20);QMouseEvent press(QEvent::MouseButtonPress,pos,w->mapToGlobal(pos),button,button,Qt::NoModifier);QApplication::sendEvent(w,&press);QMouseEvent release(QEvent::MouseButtonRelease,pos,w->mapToGlobal(pos),button,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(w,&release);};
        click(card,Qt::LeftButton);settle();CHECK(!window.panels()[0].items[0].enabled&&ctx.connected);card=window.findChildren<QFrame*>("statusCard").first();click(card,Qt::LeftButton);settle();CHECK(window.panels()[0].items[0].enabled&&applies==initialApplies+2);
        bool menuSeen=false;QTimer::singleShot(20,&window,[&]{auto menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;menuSeen=menu->objectName()=="statusItemMenu"&&menu->actions().first()->objectName()=="statusToggleItem";menu->setActiveAction(menu->actions().first());QKeyEvent key(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&key);});card=window.findChildren<QFrame*>("statusCard").first();click(card,Qt::RightButton);settle();CHECK(menuSeen&&!window.panels()[0].items[0].enabled);
        bool edited=false;QTimer driver;QObject::connect(&driver,&QTimer::timeout,&window,[&]{auto editor=dynamic_cast<StatusItemDialog*>(QApplication::activeModalWidget());if(editor){edited=true;editor->reject();}});driver.start(10);card=window.findChildren<QFrame*>("statusCard").first();const QPoint pos(20,20);QMouseEvent press(QEvent::MouseButtonPress,pos,card->mapToGlobal(pos),Qt::RightButton,Qt::RightButton,Qt::NoModifier);QApplication::sendEvent(card,&press);QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<850)settle();QMouseEvent release(QEvent::MouseButtonRelease,pos,card->mapToGlobal(pos),Qt::RightButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(card,&release);settle();driver.stop();CHECK(edited&&!QApplication::activePopupWidget()&&!window.panels()[0].items[0].enabled);
        bool savedOpen=false,updatedOpen=false;
        QTimer::singleShot(30,&window,[&]{auto editor=dynamic_cast<StatusItemDialog*>(QApplication::activeModalWidget());if(!editor)return;editor->activateWindow();editor->raise();auto name=editor->findChild<QLineEdit*>("statusItemName");name->setText("快捷键实际持久化");name->setFocus();settle();QKeyEvent key(QEvent::KeyPress,Qt::Key_S,Qt::ControlModifier,"s");QApplication::sendEvent(name,&key);savedOpen=editor->isVisible()&&window.panels()[0].items.size()==p.items.size()+1;name->setText("再次保存不重复");name->setFocus();settle();QKeyEvent again(QEvent::KeyPress,Qt::Key_S,Qt::ControlModifier,"s");QApplication::sendEvent(name,&again);updatedOpen=editor->isVisible()&&window.panels()[0].items.size()==p.items.size()+1&&window.panels()[0].items.last().name=="再次保存不重复";if(!savedOpen||!updatedOpen)std::cerr<<"save callback focus="<<name->hasFocus()<<" items="<<window.panels()[0].items.size()<<" saved="<<savedOpen<<" updated="<<updatedOpen<<"\n";editor->reject();});
        window.findChild<QPushButton*>("statusAddItem")->click();CHECK(savedOpen&&updatedOpen);Store persisted(dir.filePath("interaction.json"));QVector<Panel> loaded;CHECK(persisted.load(loaded,error)&&loaded[0].items.size()==p.items.size()+1&&loaded[0].items.last().name=="再次保存不重复");CHECK(window.panels()[0].items[0].id==id);
    }
    for(bool dark:{false,true}){
        app.setStyleSheet(dark?"QWidget {background:#1e1e1e;color:#eeeeee;}":"QWidget {background:#f0f0f0;color:#202020;}");StatusItemDialog dialog(p.items[0],p.items,ctx);dialog.show();dialog.activateWindow();settle();auto mode=dialog.findChild<QComboBox*>("statusMonitorMode");auto source=dialog.findChild<QComboBox*>("statusSourceKind");auto hold=dialog.findChild<QSpinBox*>("statusHoldTime");hold->setValue(500);mode->setCurrentIndex(1);settle();auto filters=dialog.findChild<QTableWidget*>("statusEventSilence");auto add=dialog.findChild<QPushButton*>("statusAddSilence");CHECK(dialog.findChild<QWidget*>("statusEventOptions")->isVisible()&&!hold->isVisible());Rule event;CHECK(dialog.collect(event,error)&&event.event.enabled&&event.holdMs==0&&event.event.silence.size()==1&&event.event.silence[0].value==0&&event.event.displayMs==0);dialog.findChild<QPushButton*>("statusGenerateSample")->click();CHECK(dialog.findChild<QPlainTextEdit*>("statusTestInput")->toPlainText()=="1");dialog.findChild<QPushButton*>("statusRunTest")->click();CHECK(dialog.findChild<QPlainTextEdit*>("statusTestOutput")->toPlainText().contains("事件触发 1 次"));dialog.findChild<QPlainTextEdit*>("statusTestInput")->setPlainText("0");dialog.findChild<QPushButton*>("statusRunTest")->click();CHECK(dialog.findChild<QPlainTextEdit*>("statusTestOutput")->toPlainText().contains("已屏蔽 1 条"));
        add->click();filters->item(1,1)->setText("数值区间");filters->item(1,2)->setText("5");filters->item(1,3)->setText("10");CHECK(dialog.collect(event,error)&&event.event.silence[1].lower==5&&event.event.silence[1].upper==10);source->setCurrentIndex(1);auto rules=dialog.findChild<QTableWidget*>("statusRuleTable");rules->item(0,1)->setText("READY");rules->item(0,2)->setText("就绪");CHECK(filters->rowCount()==0);add->click();filters->item(0,2)->setText("PING");CHECK(dialog.collect(event,error)&&!event.event.onChange&&event.event.silence[0].kind==SilenceKind::Text&&event.event.silence[0].text=="PING");dialog.findChild<QPlainTextEdit*>("statusTestInput")->setPlainText("PING");dialog.findChild<QPushButton*>("statusRunTest")->click();CHECK(dialog.findChild<QPlainTextEdit*>("statusTestOutput")->toPlainText().contains("已屏蔽 1 条"));dialog.findChild<QPlainTextEdit*>("statusTestInput")->setPlainText("READY");dialog.findChild<QPushButton*>("statusRunTest")->click();CHECK(dialog.findChild<QPlainTextEdit*>("statusTestOutput")->toPlainText().contains("就绪"));
        source->setCurrentIndex(0);CHECK(filters->rowCount()==2&&filters->item(1,3)->text()=="10");dialog.findChild<QSpinBox*>("statusEventDuration")->setValue(0);CHECK(dialog.collect(event,error)&&event.event.displayMs==0);dialog.resize(1080,760);settle();dialog.grab().save(artifacts+(dark?"/status-event-editor-dark.png":"/status-event-editor-light.png"));mode->setCurrentIndex(0);CHECK(hold->isVisible()&&dialog.collect(event,error)&&!event.event.enabled&&event.holdMs==500);mode->setCurrentIndex(1);CHECK(dialog.collect(event,error)&&event.event.silence.size()==2);
    }
    {
        auto panel=p;auto rule=p.items[0];rule.event.enabled=true;rule.event.displayMs=0;rule.event.silence.append(SilenceCondition{});rule.mappings={{0,"空闲"},{10,"单帧触发"},{20,"第二事件"}};panel.items={rule};StatusMonitorWindow window([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){return session.configure(r,e);},nullptr,dir.filePath("events.json"));CHECK(window.replacePanels({panel},error));window.show();publisher.send(10);publisher.send(0);settle();bool visible=false;for(auto label:window.findChildren<QLabel*>())if(label->text().contains("单帧触发"))visible=true;CHECK(visible&&session.snapshot()[0].matches==1);bool cleared=false;QTimer::singleShot(20,&window,[&]{auto menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;auto action=menu->findChild<QAction*>("statusClearEvent");if(!action)return;cleared=action->isEnabled();menu->setActiveAction(action);QKeyEvent key(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&key);});auto card=window.findChild<QFrame*>("statusCard");QPoint pos(20,20);QMouseEvent press(QEvent::MouseButtonPress,pos,card->mapToGlobal(pos),Qt::RightButton,Qt::RightButton,Qt::NoModifier);QApplication::sendEvent(card,&press);QMouseEvent release(QEvent::MouseButtonRelease,pos,card->mapToGlobal(pos),Qt::RightButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(card,&release);settle();CHECK(cleared&&!session.snapshot()[0].eventLatched&&session.snapshot()[0].matches==1&&ctx.connected);
    }
    {
        StatusItemDialog dialog(p.items[0],p.items,ctx);dialog.show();dialog.activateWindow();settle();dialog.findChild<QComboBox*>("statusMonitorMode")->setCurrentIndex(1);auto filters=dialog.findChild<QTableWidget*>("statusEventSilence");CHECK(filters->isColumnHidden(3)&&filters->horizontalHeaderItem(2)->text()=="屏蔽内容");
        QStyleOptionViewItem option;auto index=filters->model()->index(0,2);auto delegate=filters->itemDelegateForColumn(2);auto number=delegate->createEditor(&dialog,option,index);CHECK(qobject_cast<QLineEdit*>(number)&&qobject_cast<QLineEdit*>(number)->validator()&&number->findChildren<QLineEdit*>().isEmpty());delete number;
        filters->item(0,1)->setText("数值区间");filters->scrollToItem(filters->item(0,2));filters->editItem(filters->item(0,2));settle();auto range=filters->findChild<QWidget*>("statusSilenceRangeEditor");CHECK(range);auto lower=range->findChild<QLineEdit*>("statusSilenceLower");auto upper=range->findChild<QLineEdit*>("statusSilenceUpper");CHECK(lower&&upper&&lower->validator()&&upper->validator());CHECK(filters->columnWidth(2)>filters->viewport()->width()/2);auto outer=dialog.findChild<QScrollArea*>("statusEditorScroll");outer->ensureWidgetVisible(filters);settle();dialog.grab().save(artifacts+"/status-silence-range-editor.png");int inclusive=0;for(auto label:range->findChildren<QLabel*>())if(label->text().contains("（含）"))++inclusive;CHECK(inclusive==2);lower->setText("10");upper->setText("20");upper->setFocus();settle();CHECK(upper->hasFocus());QKeyEvent save(QEvent::KeyPress,Qt::Key_S,Qt::ControlModifier,"s");QApplication::sendEvent(upper,&save);settle();CHECK(dialog.isVisible()&&dialog.resultRule().event.silence.size()==1&&dialog.resultRule().event.silence[0].lower==10&&dialog.resultRule().event.silence[0].upper==20);
        dialog.findChild<QPushButton*>("statusSaveItem")->click();CHECK(dialog.result()==QDialog::Accepted&&dialog.resultRule().event.silence[0].upper==20);
    }
    {
        auto panel=p;panel.bindingMode=BindingMode::Locked;panel.protocolId="different-device";panel.protocolShape="different-shape";panel.protocolName="测试设备协议";
        StatusMonitorWindow window([&]{return ctx;},[&](const QVector<Rule>&r,QString&e){return session.configure(r,e);},nullptr,dir.filePath("panel-binding.json"));CHECK(window.replacePanels({panel},error));window.show();publisher.send(1);session.feedReceived("DEVICE READY\r\n",1792000000000000000LL);settle();bool numberMismatch=false,textValid=false;for(const auto &state:session.snapshot()){if(state.id==panel.items[0].id)numberMismatch=state.quality==Quality::SourceMismatch;if(state.id==panel.items[1].id)textValid=state.quality==Quality::Valid;}CHECK(numberMismatch&&textValid);CHECK(window.findChild<QLabel*>("statusSourceSummary")->text().contains("测试设备协议"));
        QTimer::singleShot(0,&window,[&]{auto settings=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!settings)return;settings->findChild<QComboBox*>("statusProtocolBinding")->setCurrentIndex(1);settings->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});window.findChild<QPushButton*>("statusPanelSettings")->click();CHECK(window.panels()[0].bindingMode==BindingMode::Follow);publisher.send(1);settle();CHECK(session.snapshot()[0].quality==Quality::Valid);for(const auto &r:window.panels()[0].items)if(r.source==Source::Channel)CHECK(r.protocolId.isEmpty());
    }
    for(bool dark:{false,true})for(int type:{0,2,3,1}){
        app.setStyleSheet(dark?"QWidget {background:#1e1e1e;color:#eeeeee;}":"QWidget {background:#f0f0f0;color:#202020;}");StatusItemDialog editor(p.items[type],p.items,ctx);editor.show();settle();auto table=editor.findChild<QTableWidget*>("statusRuleTable");const int col=type==1?3:type==3?6:2;auto cell=table->item(0,col);const auto original=cell->text();const auto cancelText=QCoreApplication::translate("QDialogButtonBox","Cancel");
        auto choose=[&](int action){bool checked=false;QTimer::singleShot(0,&editor,[&]{auto color=qobject_cast<QColorDialog*>(QApplication::activeModalWidget());if(!color)return;settle();auto screen=color->findChild<QPushButton*>("statusColorScreenPicker");auto transparent=color->findChild<QPushButton*>("statusColorTransparent");checked=color->objectName()=="statusColorDialog"&&color->testOption(QColorDialog::DontUseNativeDialog)&&screen&&transparent&&screen->text()=="拾取屏幕颜色"&&transparent->text()=="使用透明颜色"&&transparent->mapTo(color,QPoint()).y()>screen->mapTo(color,QPoint()).y();bool okay=false,cancel=false;for(auto button:color->findChildren<QPushButton*>()){okay|=button->text()=="确定";cancel|=button->text()=="取消";}checked=checked&&okay&&cancel;if(type==0&&action==2)color->grab().save(artifacts+(dark?"/status-color-picker-dark.png":"/status-color-picker-light.png"));if(action==0)color->reject();else if(action==1){color->setCurrentColor(QColor("#125abc"));color->accept();}else if(transparent)transparent->click();else color->reject();});QMetaObject::invokeMethod(table,"cellDoubleClicked",Qt::DirectConnection,Q_ARG(int,0),Q_ARG(int,col));return checked;};
        CHECK(choose(0)&&cell->text()==original);CHECK(choose(1)&&cell->text()=="#125abc");auto delegate=qobject_cast<QStyledItemDelegate*>(table->itemDelegateForColumn(col));CHECK(delegate&&delegate->displayText(cell->text(),QLocale::c()).isEmpty());QStyleOptionViewItem option;option.initFrom(table);option.rect=QRect(0,0,120,32);CHECK(!delegate->createEditor(&editor,option,table->model()->index(0,col)));QPixmap preview(120,32);preview.fill(Qt::transparent);QPainter painter(&preview);delegate->paint(&painter,option,table->model()->index(0,col));painter.end();auto pixels=preview.toImage();int colored=0;for(int y=0;y<pixels.height();++y)for(int x=0;x<pixels.width();++x)if(pixels.pixelColor(x,y)==QColor("#125abc"))++colored;CHECK(colored>300);if(type==0){editor.findChild<QScrollArea*>("statusEditorScroll")->ensureWidgetVisible(table);settle();editor.grab().save(artifacts+(dark?"/status-color-swatch-dark.png":"/status-color-swatch-light.png"));}CHECK(choose(2)&&cell->text().isEmpty());CHECK(QCoreApplication::translate("QDialogButtonBox","Cancel")==cancelText);Rule saved;CHECK(editor.collect(saved,error));QString savedColor=type==1?saved.textMatches[0].color:type==3?saved.ranges[0].color:saved.mappings[0].color;CHECK(savedColor.isEmpty());auto panel=p;panel.items={saved};QVector<Panel> restored;CHECK(decode(encode({panel},error),restored,error));
    }
    {
        auto fresh=p.items[0];CHECK(fresh.timeoutMs==0);StatusItemDialog dialog(fresh,p.items,ctx);auto preset=dialog.findChild<QComboBox*>("statusTimeoutPreset");CHECK(preset->currentData().toInt()==0&&preset->currentText()=="不自动过期");Rule result;CHECK(dialog.collect(result,error)&&result.timeoutMs==0);auto source=dialog.findChild<QComboBox*>("statusSourceKind");source->setCurrentIndex(1);auto textRules=dialog.findChild<QTableWidget*>("statusRuleTable");textRules->item(0,1)->setText("DEVICE READY");textRules->item(0,2)->setText("就绪");CHECK(dialog.collect(result,error)&&result.timeoutMs==0);source->setCurrentIndex(0);CHECK(dialog.collect(result,error)&&result.timeoutMs==0);
        fresh.timeoutMs=5000;StatusItemDialog existing(fresh,p.items,ctx);CHECK(existing.findChild<QComboBox*>("statusTimeoutPreset")->currentData().toInt()==5000&&existing.collect(result,error)&&result.timeoutMs==5000);auto oldSource=existing.findChild<QComboBox*>("statusSourceKind");oldSource->setCurrentIndex(1);oldSource->setCurrentIndex(0);CHECK(existing.collect(result,error)&&result.timeoutMs==5000);
    }
    std::cout<<"Status UI: localized/resettable rule colors, panel binding, independent text, toggles, persistence and shortcuts passed.\n";return 0;
}
