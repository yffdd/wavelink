#include "StatusTextMonitor.h"
#include "networkhandler.h"
#include "ble/blestreamworker.h"
#include "fakeserialbackend.h"
#include <QCoreApplication>
#include <QThread>
#include <QTemporaryDir>
#include <QFile>
#include <QNetworkProxy>
#include <iostream>
using namespace StatusMonitoring;
#define CHECK(c) do {if(!(c)) {std::cerr<<"Text status test line "<<__LINE__<<": " #c "\n";return 1;}}while(0)
static bool until(const std::function<bool()> &f) {QElapsedTimer t;t.start();while(!f() && t.elapsed()<3000) {QCoreApplication::processEvents();QThread::msleep(1);}return f();}
static Rule feedback() {
    auto r=Rule::textRule("feedback","设备反馈");r.textSource.id="responses";
    r.textMatches={{"DEVICE OK","ready",false},{"DEVICE FAIL","failed",false},{"FAULT:","fault",true}};return r;
}
class ReceivePort : public FakePort {
public:
    void feed(const QByteArray &bytes) {incoming+=bytes;emit readyRead();}
    qint64 bytesAvailable() const override {return incoming.size()+QIODevice::bytesAvailable();}
protected:
    qint64 readData(char *data,qint64 size) override {auto n=qMin<qint64>(size,incoming.size());memcpy(data,incoming.constData(),size_t(n));incoming.remove(0,int(n));return n;}
private:QByteArray incoming;
};
static int core() {
    QString error;auto r=feedback();TextEngine e;CHECK(e.configure({r},0,error));e.reset(true,0);
    e.consume("DEVICE O",0,1,100,false);CHECK(e.states()[0].quality==Quality::Waiting);
    e.consume("K\r\nDEVICE FAIL\r\n",8,2,200,false);CHECK(e.states()[0].state=="failed" && e.states()[0].matches==2);
    auto events=e.takeEvents();CHECK(events.last().previous.state=="ready" && events.last().current.state=="failed");
    CHECK(e.states()[0].byteStart==11 && e.states()[0].receiveUnixNs==200);
    e.consume("NOT DEVICE OK\r\nDEVICE OKAY\r\n",29,3,300,false);CHECK(e.states()[0].matches==2);
    e.consume("FAULT:传感器断开\r\n",58,4,400,false);CHECK(e.states()[0].state=="fault" && e.states()[0].detail==QStringLiteral("传感器断开"));
    e.tick(100000);CHECK(e.states()[0].quality==Quality::Valid); // factory defaults to no expiry
    r.timeoutMs=50;CHECK(e.configure({r},0,error));e.consume("DEVICE OK\r\n",0,10,100,false);
    e.consume("other\r\n",11,40,400,false);e.tick(60);CHECK(e.states()[0].quality==Quality::Stale);
    e.consume("DEVICE OK\r\n",18,70,700,false);CHECK(e.states()[0].quality==Quality::Valid);
    auto shared=r;shared.id="other";CHECK(e.configure({r,shared},0,error));e.consume("DEVICE FAIL\r\n",0,100,1000,false);
    CHECK(e.states()[0].state=="failed" && e.states()[1].state=="failed");
    shared.textSource.encoding="GBK";CHECK(!e.configure({r,shared},0,error));CHECK(e.states()[0].state=="failed");
    r.textSource.caseSensitive=false;r.textSource.trim=true;CHECK(e.configure({r},0,error));
    e.consume("  device ok\t\r\n",0,101,1000,false);CHECK(e.states()[0].state=="ready");
    auto duplicate=r;duplicate.textMatches.append({"device ok","duplicate",false});CHECK(!Engine::validate({duplicate},error));
    r=feedback();r.textMatches.prepend({"DEVICE ","prefix-wins",true});CHECK(e.configure({r},0,error));
    e.consume("DEVICE OK\r\n",0,1,100,false);CHECK(e.states()[0].state=="prefix-wins" && e.states()[0].matchedRow==0);
    r=feedback();r.textSource.maxBytes=9;CHECK(e.configure({r},0,error));e.reset(true,0);
    e.consume("DEVICE OK\r",0,1,100,false);CHECK(e.states()[0].quality==Quality::Waiting);
    e.consume("\n",10,2,200,false);CHECK(e.states()[0].state=="ready"); // exact limit with split delimiter
    e.consume("XDEVICE OK\r\nDEVICE OK\r\n",11,3,300,false);CHECK(e.states()[0].matches==2 && !e.states()[0].feedbackMayBeLost);
    r.textSource.partialTimeoutMs=10;CHECK(e.configure({r},0,error));e.reset(true,0);
    e.consume("DEVICE ",0,1,100,false);e.tick(11);CHECK(e.states()[0].feedbackMayBeLost);
    e.consume("OK\r\nDEVICE FAIL\r\n",7,12,1200,false); // FAIL exceeds 9 bytes, remains lost
    CHECK(e.states()[0].matches==0);e.consume("DEVICE OK\r\n",24,13,1300,false);CHECK(e.states()[0].matches==1);
    r=feedback();r.textSource.strictSync=true;CHECK(e.configure({r},0,error));e.reset(true,0);
    e.consume("DEVICE OK\r\nDEVICE FAIL\r\n",0,1,100,false);CHECK(e.states()[0].matches==1 && e.states()[0].state=="failed");
    e.setTransport(true,true,2);e.consume("DEVICE O",0,3,300,true);e.consume("K",8,4,400,true);CHECK(e.states()[0].matches==0);
    e.consume("DEVICE OK\r\n",9,5,500,true);CHECK(e.states()[0].state=="ready");
    r.textSource.datagram=true;CHECK(e.configure({r},0,error));e.reset(true,0);
    e.consume("DEVICE OK",0,1,100,true);CHECK(e.states()[0].state=="ready");
    e.setTransport(false,true,2);CHECK(e.states()[0].quality==Quality::Unavailable);
    r=feedback();r.textMatches={{QStringLiteral("采集完成"),"done",false}};CHECK(e.configure({r},0,error));e.reset(true,0);
    const auto chinese=QStringLiteral("采集完成\r\n").toUtf8();
    for(int i=0;i<chinese.size();++i) e.consume(chinese.mid(i,1),i,i+1,100,false);
    CHECK(e.states()[0].state=="done");e.consume(QByteArray::fromHex("FF0D0A"),100,20,200,false);CHECK(e.states()[0].feedbackMayBeLost);
    CHECK(e.diagnostics().last().code=="encoding");
    r.textSource.encoding="GBK";CHECK(e.configure({r},0,error));e.reset(true,0);
    const auto gbk=QTextCodec::codecForName("GBK")->fromUnicode(QStringLiteral("采集完成\r\n"));
    for(int i=0;i<gbk.size();++i) e.consume(gbk.mid(i,1),i,i+1,100,false);
    CHECK(e.states()[0].state=="done");
    r=feedback();r.textSource.delimiter="abab";CHECK(e.configure({r},0,error));e.reset(true,0);
    e.consume("DEVICE OKaba",0,1,100,false);e.consume("bDEVICE FAILabab",12,2,200,false);CHECK(e.states()[0].matches==2);
    r=feedback();r.recordEveryMatch=true;CHECK(e.configure({r},0,error));e.reset(true,0);
    for(int i=0;i<2200;++i)e.consume("DEVICE OK\r\n",i*11,i+1,100,false);
    CHECK(e.takeEvents().size()==2000 && e.droppedEvents()>0 && e.diagnostics().size()<=20);
    CHECK(e.statistics()["responses"]["matched"]>=2200);
    auto preview=testTextStream({feedback()},{"DEVICE O","K\r\n"},false,error);CHECK(error.isEmpty() && preview.states[0].state=="ready");
    preview=testTextMessage(feedback(),"FAULT:42",error);CHECK(error.isEmpty() && preview.states[0].detail=="42");
    const auto samples=generateTextSamples(feedback(),error);CHECK(error.isEmpty() && samples.size()==3);
    preview=testTextStream({feedback()},samples,false,error);CHECK(preview.states[0].matches==3 && preview.states[0].detail=="example");
    CHECK(e.states()[0].matches==2200); // Preview never mutates the live engine
    r=feedback();CHECK(e.configure({r},0,error));e.reset(true,0);e.consume("DEVICE O",0,1,100,false);
    r.name="renamed";r.textMatches[0].color="#123456";CHECK(e.configure({r},1,error));e.consume("K\r\n",8,2,200,false);
    CHECK(e.states()[0].state=="ready" && e.states()[0].color=="#123456");
    e.consume(QByteArray("DEVICE\0 OK\r\n",12),11,3,300,false);CHECK(e.states()[0].feedbackMayBeLost);
    r.textSource.encoding="ASCII";CHECK(e.configure({r},0,error));e.reset(true,0);
    e.consume(QByteArray::fromHex("C4E30D0A"),0,1,100,false);CHECK(e.diagnostics().last().code=="encoding");
    return 0;
}
template<class Worker> static int checkWorker(Worker &w) {
    CHECK(until([&]{auto s=w.statusMonitor()->snapshot();return s.size()==1 && s[0].matches==2;}));
    CHECK(w.statusMonitor()->snapshot()[0].state=="failed");return 0;
}
static int transports(const QString &dir) {
    for(int mode:{0,1,2,4,5}) {
        const QByteArray data="1,2\r\nDEVICE OK\r\nDEVICE FAIL\r\n1,2\r\n";QString error;
        const auto path=dir+QString("/%1.csv").arg(mode);
        if(mode==0) {
            auto p=new ReceivePort;TestSerialHandler w(p);CHECK(w.statusMonitor()->configure({feedback()},error));
            w.openPort("SOFTWARE_ONLY",115200,8,"None","1",1,false,false);w.startRecording(path,100);
            p->feed(data.left(12));p->feed(data.mid(12));CHECK(checkWorker(w)==0);w.stopRecording();
            w.setProtocol(0);CHECK(w.statusMonitor()->snapshot()[0].state=="failed");p->feed("FAULT:RAW\r\n");
            CHECK(until([&]{return w.statusMonitor()->snapshot()[0].state=="fault";}));CHECK(p->written.isEmpty());w.closePort();
            CHECK(w.statusMonitor()->snapshot()[0].quality==Quality::Disconnected);
        } else if(mode==4) {
            BleStreamWorker w;CHECK(w.statusMonitor()->configure({feedback()},error));w.beginSession(1,1,false);w.startRecording(path,100,1);
            CHECK(w.enqueueData(1,data.left(12),100));CHECK(w.enqueueData(1,data.mid(12),200));
            CHECK(w.stopRecordingNow().isEmpty());CHECK(checkWorker(w)==0);
            w.endSession();w.stopRecordingNow();CHECK(w.statusMonitor()->snapshot()[0].quality==Quality::Disconnected);
        } else {
            NetworkHandler w;w.setProtocol(1);CHECK(w.statusMonitor()->configure({feedback()},error));
            QTcpServer server;QTcpSocket client;QUdpSocket udp;QTcpSocket *peer=nullptr;
            server.setProxy(QNetworkProxy::NoProxy);client.setProxy(QNetworkProxy::NoProxy);udp.setProxy(QNetworkProxy::NoProxy);
            if(mode==1) {CHECK(server.listen(QHostAddress::LocalHost,0));CHECK(w.startConnection(1,"127.0.0.1",server.serverPort()));CHECK(until([&]{return server.hasPendingConnections();}));peer=server.nextPendingConnection();}
            else if(mode==2) {CHECK(w.startConnection(2,"127.0.0.1",0));auto listener=w.findChild<QTcpServer*>();CHECK(listener);client.connectToHost(QHostAddress::LocalHost,listener->serverPort());CHECK(until([&]{return w.sendReady();}));peer=&client;}
            else CHECK(w.startUdp("127.0.0.1",0,"",0,false));
            w.startRecording(path,100);
            if(mode==5) {auto socket=w.findChild<QUdpSocket*>();CHECK(socket);CHECK(udp.writeDatagram(data,QHostAddress::LocalHost,socket->localPort())==data.size());}
            else {CHECK(peer->write(data)==data.size());peer->flush();}
            CHECK(checkWorker(w)==0);w.stopRecording();w.setProtocol(0);CHECK(w.statusMonitor()->snapshot()[0].state=="failed");w.stopConnection();
            CHECK(w.statusMonitor()->snapshot()[0].quality==Quality::Disconnected);
        }
        QFile file(path);CHECK(file.open(QIODevice::ReadOnly));const auto lines=file.readAll().trimmed().split('\n');CHECK(lines.size()==3); // Numeric recording continues alongside feedback
    }
    return 0;
}
static int sessionCases() {
    Session s;QString error;CHECK(s.configure({feedback()},error));s.setConnected(true);
    s.feedReceived(QByteArray(1024*1024+1,'X'),100);
    CHECK(s.textStatistics()["responses"]["input-lost"]==1);
    s.feedReceived("DEVICE OK\r\nDEVICE FAIL\r\n",200);CHECK(until([&]{return s.snapshot()[0].matches==1;}));CHECK(s.snapshot()[0].state=="failed");
    s.feedReceived("DEVICE O",300);s.setConnected(false);s.setConnected(true);s.feedReceived("K\r\n",400);
    QCoreApplication::processEvents();CHECK(s.snapshot()[0].matches==0);
    s.feedReceived("DEVICE OK\r\n",500);CHECK(until([&]{return s.snapshot()[0].state=="ready";}));
    s.bind(nullptr,"other-protocol");CHECK(s.snapshot()[0].state=="ready");
    auto bad=feedback();bad.textSource.delimiter.clear();CHECK(!s.configure({bad},error));CHECK(s.snapshot()[0].state=="ready");
    auto slow=feedback();slow.timeoutMs=30;CHECK(s.configure({slow},error));s.feedReceived("DEVICE OK\r\n",600);
    CHECK(until([&]{return s.snapshot()[0].quality==Quality::Stale;}));
    s.setTextTransport(false,false);CHECK(s.snapshot()[0].quality==Quality::Unavailable);
    s.setTextTransport(false,true);auto config=feedback();CHECK(s.configure({config},error));s.setConnected(true);
    s.feedReceived("DEVICE O",1);config.textSource.encoding="GBK";CHECK(s.configure({config},error));
    s.feedReceived("K\r\nDEVICE FAIL\r\n",2);CHECK(until([&]{return s.snapshot()[0].state=="failed";}));CHECK(s.snapshot()[0].matches==1);
    CHECK(s.textStatistics()["responses"]["input-lost"]>=1);
    return 0;
}
int main(int argc,char **argv) {QCoreApplication app(argc,argv);QTemporaryDir dir;CHECK(dir.isValid());
    CHECK(core()==0);CHECK(sessionCases()==0);CHECK(transports(dir.path())==0);
    std::cout<<"Text framing, arbitrary feedback, recovery, bounded queues, timeout, five transport paths and numeric recording passed.\n";return 0;}
