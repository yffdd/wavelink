#include "commandpanelwindow.h"
#include <QElapsedTimer>
#include <QThread>
#include <QSignalBlocker>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QDir>
#include <iostream>
#include <functional>
#define CHECK(c) do{if(!(c)){std::cerr<<"Command interaction failure "<<__LINE__<<": " #c "\n";return 1;}}while(0)
static bool until(const std::function<bool()> &done,int timeout=1600){QElapsedTimer timer;timer.start();while(!done()&&timer.elapsed()<timeout){QApplication::processEvents();QThread::msleep(2);}return done();}
static void pump(int ms=20){QElapsedTimer timer;timer.start();while(timer.elapsed()<ms){QApplication::processEvents();QThread::msleep(2);}}
static void mouse(QWidget *target,QEvent::Type type,Qt::MouseButton button){const auto point=target->rect().center();QMouseEvent event(type,point,target->mapToGlobal(point),button,type==QEvent::MouseButtonPress?Qt::MouseButtons(button):Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(target,&event);}
static QMenu *menuFor(PanelContentWidget &panel,CommandButton *button,QWidget *target=nullptr){for(auto menu:panel.findChildren<QMenu*>("commandItemMenu"))menu->close();pump();if(!target)target=button;mouse(target,QEvent::MouseButtonPress,Qt::RightButton);mouse(target,QEvent::MouseButtonRelease,Qt::RightButton);for(auto menu:panel.findChildren<QMenu*>("commandItemMenu"))if(menu->isVisible())return menu;return nullptr;}
static bool choose(QMenu *menu,const char *name){if(!menu)return false;auto action=menu->findChild<QAction*>(name);if(!action||!action->isEnabled())return false;menu->close();action->trigger();pump(5);return true;}
int main(int argc,char **argv){QApplication app(argc,argv);app.setAttribute(Qt::AA_DontUseNativeDialogs);const int font=QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");if(font>=0)app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(),10));
    PanelDef def;def.id="interactions";def.name="命令右键发送";def.columns=3;
    CommandDef ordinary;ordinary.id="once";ordinary.name="普通";ordinary.payload="ONCE";ordinary.intervalMs=0;ordinary.appendIndex=1;
    auto timed=ordinary;timed.id="timed";timed.name="定时";timed.payload="TIMER";timed.intervalMs=200;
    auto toggle=ordinary;toggle.id="switch";toggle.name="开关";toggle.widgetType=2;toggle.isHex=true;toggle.switchStates={{{"AA 00","关闭",""},{"AA 01","开启",""}}};
    auto slider=ordinary;slider.id="slider";slider.name="常规滑条";slider.widgetType=1;slider.payload="SET:{value};";slider.minValue=0;slider.maxValue=10;slider.stepValue=1;slider.defaultValue=4;
    auto list=slider;list.id="list";list.name="列表滑条";list.sliderMode=1;list.isHex=true;list.payload="5A{value}00";list.sliderValues=QStringList{"01","AB CD","FF"};list.defaultListIndex=1;
    def.commands={ordinary,timed,toggle,slider,list};PanelContentWidget panel(def);panel.resize(950,650);panel.show();pump();
    QHash<QString,CommandButton*> widgets;for(auto button:panel.findChildren<CommandButton*>())widgets[button->getDef().id]=button;CHECK(widgets.size()==5);
    QVector<QByteArray> packets;QVector<bool> automatic;QObject::connect(&panel,&PanelContentWidget::sendRequested,[&](const QByteArray &data,bool autoSend){packets.append(data);automatic.append(autoSend);});
    QVector<QByteArray> switchPackets;bool admit=true;panel.setSwitchSendHandler([&](const QByteArray &data){switchPackets.append(data);return admit;});
    auto button=widgets["once"];auto menu=menuFor(panel,button);CHECK(menu&&packets.isEmpty()&&menu->actions().first()->text()=="发送一次"&&menu->findChild<QAction*>("commandMenuEdit"));CHECK(choose(menu,"commandMenuSendOnce"));CHECK(packets.size()==1&&packets.last()=="ONCE\r\n"&&!automatic.last());
    mouse(button,QEvent::MouseButtonPress,Qt::LeftButton);mouse(button,QEvent::MouseButtonRelease,Qt::LeftButton);CHECK(packets.size()==2&&packets.last()=="ONCE\r\n");
    auto timer=widgets["timed"];menu=menuFor(panel,timer);CHECK(menu&&!menu->findChild<QAction*>("commandMenuStopTimed")->isEnabled());int before=packets.size();CHECK(choose(menu,"commandMenuSendOnce"));CHECK(packets.size()==before+1&&!timer->timedSending()&&!automatic.last());
    menu=menuFor(panel,timer);CHECK(choose(menu,"commandMenuStartTimed"));CHECK(timer->timedSending()&&automatic.last());before=packets.size();timer->setTimedSending(true);CHECK(packets.size()==before);CHECK(until([&]{return packets.size()>before;}));
    menu=menuFor(panel,timer);CHECK(!menu->findChild<QAction*>("commandMenuStartTimed")->isEnabled());CHECK(choose(menu,"commandMenuSendOnce")&&timer->timedSending()&&!automatic.last());menu=menuFor(panel,timer);CHECK(choose(menu,"commandMenuStopTimed"));before=packets.size();pump(100);CHECK(!timer->timedSending()&&packets.size()==before);
    auto sw=qobject_cast<SwitchCommandWidget*>(widgets["switch"]);CHECK(sw&&sw->currentState()==0);menu=menuFor(panel,sw);CHECK(switchPackets.isEmpty());CHECK(choose(menu,"commandMenuState1"));CHECK(sw->currentState()==1&&switchPackets.last()==QByteArray::fromHex("AA010D0A"));menu=menuFor(panel,sw);CHECK(choose(menu,"commandMenuState1"));CHECK(sw->currentState()==1&&switchPackets.size()==2);
    admit=false;menu=menuFor(panel,sw);CHECK(choose(menu,"commandMenuState0"));CHECK(sw->currentState()==1);admit=true;menu=menuFor(panel,sw);CHECK(choose(menu,"commandMenuToggle"));CHECK(sw->currentState()==0&&switchPackets.last()==QByteArray::fromHex("AA000D0A"));
    auto regular=qobject_cast<SliderCommandWidget*>(widgets["slider"]);auto spin=regular->findChild<QDoubleSpinBox*>();CHECK(spin);{QSignalBlocker block(spin);spin->setValue(7);}menu=menuFor(panel,regular,spin);before=packets.size();CHECK(choose(menu,"commandMenuSendOnce"));CHECK(packets.size()==before+1&&packets.last()=="SET:7;\r\n");menu=menuFor(panel,regular);CHECK(choose(menu,"commandMenuSendOnce"));CHECK(packets.size()==before+2);menu=menuFor(panel,regular);CHECK(choose(menu,"commandMenuSendDefault"));CHECK(spin->value()==4&&packets.last()=="SET:4;\r\n");
    auto values=qobject_cast<SliderCommandWidget*>(widgets["list"]);auto index=values->findChild<QDoubleSpinBox*>();CHECK(index);menu=menuFor(panel,values,index->findChild<QLineEdit*>());CHECK(choose(menu,"commandMenuListValue2"));CHECK(index->value()==2&&packets.last()==QByteArray::fromHex("5AFF000D0A"));menu=menuFor(panel,values);CHECK(choose(menu,"commandMenuSendDefault"));CHECK(index->value()==1&&packets.last()==QByteArray::fromHex("5AABCD000D0A"));
    // Keyboard context menus use the same menu, never edit or transmit.
    for(auto item:widgets){before=packets.size();const int switchBefore=switchPackets.size();QContextMenuEvent context(QContextMenuEvent::Keyboard,item->rect().center(),item->mapToGlobal(item->rect().center()));QApplication::sendEvent(item,&context);CHECK(packets.size()==before&&switchPackets.size()==switchBefore);for(auto popup:panel.findChildren<QMenu*>("commandItemMenu"))popup->close();pump();}
    // A held right button edits each widget type; releasing it cannot reopen
    // the menu or invoke any send action, including on slider child controls.
    for(auto item:widgets){int edits=0;QTimer driver;QObject::connect(&driver,&QTimer::timeout,[&]{for(auto editor:panel.findChildren<CommandEditDialog*>())if(editor->isVisible()){++edits;editor->reject();}});driver.start(5);before=packets.size();const int switchBefore=switchPackets.size();QWidget *target=item;if(auto s=qobject_cast<SliderCommandWidget*>(item))target=s->findChild<QSlider*>();mouse(target,QEvent::MouseButtonPress,Qt::RightButton);CHECK(until([&]{return edits==1;}));driver.stop();mouse(target,QEvent::MouseButtonRelease,Qt::RightButton);pump();CHECK(edits==1&&packets.size()==before&&switchPackets.size()==switchBefore);for(auto popup:panel.findChildren<QMenu*>("commandItemMenu"))CHECK(!popup->isVisible());}
    // Capture all four menu forms for both themes without sending anything.
    QDir().mkpath("artifacts");for(bool dark:{false,true}){panel.applyTheme(dark);for(const auto &id:{"once","timed","switch","slider","list"}){menu=menuFor(panel,widgets[id]);CHECK(menu);menu->grab().save(QString("artifacts/command-menu-%1-%2.png").arg(id,dark?"dark":"light"));menu->close();pump();}}
    std::cout<<"Short/held right click, keyboard/child menus, one-shot/timed/switch/slider/list sends, repeats and failed switch admission passed.\n";return 0;
}
