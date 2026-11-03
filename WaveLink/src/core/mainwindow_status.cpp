#include "mainwindow.h"
#include "statusmonitorwindow.h"
StatusUiContext MainWindow::statusMonitorContext() const {
    StatusUiContext context;const int type=connectionIsReady()?m_activeInterfaceType:comboInterface->currentData().toInt();
    context.interfaceName=comboInterface->itemText(comboInterface->findData(type));context.connected=connectionIsReady();
    auto selection=m_appliedProtocol;auto profile=m_appliedProfile;
    if(type==0)context.session=serialHandler->statusMonitor();else if(isNetworkInterface(type))context.session=networkHandler->statusMonitor();else if(type==4&&bleWorker)context.session=bleWorker->statusMonitor();else if(type==3){context.session=m_demoStatusMonitor;selection=DataProtocols::ProtocolSelection::fromLegacy(comboDemoProtocol->currentIndex()==0?1:2);profile={};}
    context.protocolId=selection.id;context.protocolShape=StatusMonitoring::Session::structure(selection,profile);
    context.protocolNames={{"builtin.raw","Raw（文本 / HEX，不输出数值通道）"},{"builtin.firewater","FireWater（CSV 文本）"},{"builtin.justfloat","JustFloat（二进制浮点数）"},{"builtin.amu4","CBF/IR/RED（旧版设备协议）"}};
    if(m_protocolStore)for(const auto &saved:m_protocolStore->snapshot().profiles)context.protocolNames.insert(saved.id,saved.name);
    context.protocolName=selection.kind=="custom"?profile.name:context.protocolNames.value(selection.id,selection.id);
    if(selection.kind=="builtin"&&selection.id=="builtin.justfloat")context.protocolName+=selection.bigEndian?" · 大端":" · 小端";
    if(selection.kind=="custom")context.channelNames=profile.outputChannelNames(256);
    context.protocolCatalog=[this]{
        QVector<StatusProtocolOption> options;
        for(int index:{0,1,2})for(bool big:{false,true}){
            if(index!=2&&big)continue;
            const auto chosen=DataProtocols::ProtocolSelection::fromLegacy(index,big);
            const auto name=index==0?QString("Raw（文本 / HEX，无数值通道）"):index==1?QString("FireWater（CSV 文本）"):QString("JustFloat（二进制浮点数） · ")+(big?"大端":"小端");
            StatusProtocolOption option;option.id=chosen.id;option.name=name;option.shape=StatusMonitoring::Session::structure(chosen,{});option.numeric=index!=0;options.append(option);
        }
        if(m_protocolStore)for(const auto &saved:m_protocolStore->snapshot().profiles){
            DataProtocols::ProtocolSelection chosen;chosen.kind="custom";chosen.id=saved.id;
            StatusProtocolOption option;option.id=saved.id;option.name=saved.name;option.shape=StatusMonitoring::Session::structure(chosen,saved);option.channelNames=saved.outputChannelNames(256);option.custom=true;options.append(option);
        }
        return options;
    };
    return context;
}
void MainWindow::setupStatusMonitor(){
    m_statusWindow=new StatusMonitorWindow([this]{return statusMonitorContext();},[this](const QVector<StatusMonitoring::Rule> &rules,QString &error){
        if(!StatusMonitoring::Engine::validate(rules,error))return false;
        for(auto session:{serialHandler->statusMonitor(),networkHandler->statusMonitor(),m_demoStatusMonitor,bleWorker?bleWorker->statusMonitor():nullptr})if(session&&!session->configure(rules,error))return false;
        return true;
    },this);
    const auto theme=qApp->property("waveLinkDarkTheme");m_statusWindow->applyTheme(theme.isValid()?theme.toBool():!comboTheme||comboTheme->currentIndex()==0);
}
void MainWindow::showStatusMonitor(){if(!m_statusWindow)return;m_statusWindow->showNormal();m_statusWindow->raise();m_statusWindow->activateWindow();}
