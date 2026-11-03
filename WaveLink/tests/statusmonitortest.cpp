#include "StatusMonitor.h"
#include "ProtocolParser.h"
#include <QCoreApplication>
#include <QThread>
#include <iostream>
using namespace StatusMonitoring;
#define CHECK(c) do { if (!(c)) { std::cerr << "Status test line " << __LINE__ << ": " #c "\n"; return 1; } } while (0)
class Publisher : public ProtocolParser {
public:
    int processData(const char *, int n) override { return n; }
    void sample(const QVector<double> &a, const QVector<double> &b) { publishChannels(a,b); }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc,argv); QString error;
    Rule r; r.id="status"; r.name="Status"; r.mappings={{0,"off"},{1,"on"},{5,"fault"}};
    CHECK(r.timeoutMs==0);Engine indefinite;CHECK(indefinite.configure({r},0,error));indefinite.reset(true,"test",{},0);indefinite.consume({0},{0},1);indefinite.tick(86400000);CHECK(indefinite.states()[0].quality==Quality::Valid);indefinite.reset(false,"test",{},86400001);CHECK(indefinite.states()[0].quality==Quality::Disconnected);r.timeoutMs=3000;
    Engine e; CHECK(e.configure({r},0,error)); e.reset(true,"test",{},0);
    e.consume({0},{1},1); CHECK(e.states()[0].state=="off");
    e.consume({1},{0},2); e.consume({0},{1},3);
    auto events=e.takeEvents(); CHECK(events.size()>=3);
    CHECK(events[events.size()-2].current.state=="on" && events.last().current.state=="off");
    CHECK(events.last().previous.value==1 && events.last().current.value==0);
    e.consume({0},{0},2000); e.tick(4000); CHECK(e.states()[0].quality==Quality::Valid);
    e.tick(5000); CHECK(e.states()[0].quality==Quality::Stale);
    e.consume({5},{0},5001); CHECK(e.states()[0].state=="fault");
    e.consume({1.9},{0},5002); CHECK(e.states()[0].quality==Quality::Invalid);
    e.consume({-1},{0},5003); CHECK(e.states()[0].quality==Quality::Invalid);
    e.consume({qQNaN()},{0},5004); CHECK(e.states()[0].quality==Quality::Invalid);
    e.consume({}, {},5005); CHECK(e.states()[0].quality==Quality::Missing);
    e.consume({1},{0},5006); CHECK(e.states()[0].quality==Quality::Valid);
    const auto oldSession=e.session(); e.reset(false,"test",{},5007);
    e.consume({1},{1},5008); CHECK(e.states()[0].quality==Quality::Disconnected && e.session()!=oldSession);
    e.reset(true,"test",{},5009); CHECK(e.states()[0].quality==Quality::Waiting);
    r.converted=true; CHECK(e.configure({r},5010,error)); e.consume({0},{5},5011); CHECK(e.states()[0].state=="fault");
    r.protocolId="A"; r.protocolShape="shape"; CHECK(e.configure({r},5012,error));
    e.consume({1},{1},5013); CHECK(e.states()[0].quality==Quality::SourceMismatch);
    e.reset(true,"A","shape",5014); e.consume({0},{1},5015); CHECK(e.states()[0].state=="on");
    r.protocolId.clear(); r.protocolShape.clear(); r.converted=false; r.kind=RuleKind::Bits;
    r.firstBit=31; r.bitCount=1; r.mappings={{0,"low"},{1,"high"}};
    CHECK(e.configure({r},0,error)); e.consume({double(0x80000000u)},{},1); CHECK(e.states()[0].state=="high");
    e.consume({double(0xffffffffu)},{},2); CHECK(e.states()[0].state=="high");
    e.consume({4294967296.0},{},3); CHECK(e.states()[0].quality==Quality::Invalid);
    r.firstBit=0; r.bitCount=3; r.mappings={{5,"bits5"}};
    CHECK(e.configure({r},0,error)); e.consume({13},{},1); CHECK(e.states()[0].state=="bits5");
    r.bitCount=33; CHECK(!e.configure({r},0,error)); CHECK(e.states()[0].state=="bits5");
    r.kind=RuleKind::Threshold; r.bitCount=1;
    Range low, high; low.upper=80; low.state="normal"; high.lower=80; high.state="hot";
    high.retainWithHysteresis=true;
    r.ranges={low,high}; r.holdMs=200;
    CHECK(e.configure({r},0,error)); e.consume({79},{},0); CHECK(e.states()[0].quality==Quality::Waiting);
    e.consume({79},{},200); CHECK(e.states()[0].state=="normal");
    e.consume({80},{},201); e.consume({79},{},300); e.consume({80},{},400);
    CHECK(e.states()[0].state=="normal"); e.consume({80},{},600); CHECK(e.states()[0].state=="hot");
    r.holdMs=0; r.hysteresis=2; CHECK(e.configure({r},0,error));
    e.consume({81},{},1); e.consume({79},{},2); CHECK(e.states()[0].state=="hot");
    e.consume({78},{},3); CHECK(e.states()[0].state=="normal");
    e.consume({80},{},4); CHECK(e.states()[0].state=="hot");
    r.ranges[0].includeUpper=true; CHECK(!Engine::validate({r},error)); r.ranges[0].includeUpper=false;
    r.hysteresis=0; CHECK(e.configure({r},0,error));
    for(int i=0;i<3000;++i) e.consume({double(i%2 ? 100 : 0)},{},i);
    CHECK(e.takeEvents().size()==2000 && e.droppedEvents()>0);
    Rule text; text.id="text"; text.name="Feedback"; text.source=Source::Text; text.kind=RuleKind::TextMatch;
    text.textMatches={{"ACK OK","OK",false}}; CHECK(Engine::validate({text},error));
    CHECK(e.configure({text},0,error)); CHECK(e.states()[0].quality==Quality::Unavailable);
    text.textSource.delimiter.clear(); CHECK(!Engine::validate({text},error));
    r.timeoutMs=30; Session session; Publisher parser;
    session.bind(&parser,"test"); CHECK(session.configure({r},error)); session.setConnected(true);
    session.setReceiveTimestamp(1791370000123456700LL);
    parser.sample({1},{100}); CHECK(session.snapshot()[0].state=="normal");
    CHECK(session.snapshot()[0].receiveUnixNs==1791370000123456700LL);
    QElapsedTimer wait; wait.start();
    while (session.snapshot()[0].quality!=Quality::Stale && wait.elapsed()<1000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    CHECK(session.snapshot()[0].quality==Quality::Stale);
    session.restart(); CHECK(session.snapshot()[0].quality==Quality::Waiting);
    FireWaterParser csv; session.bind(&csv,"builtin.firewater");
    CHECK(session.configure({r},error)); csv.processData("1,2\n100,2\n1,2\n",14);
    CHECK(session.snapshot()[0].sequence==3 && session.snapshot()[0].state=="normal");
    auto transitions=session.takeEvents(); bool sawHot=false;
    for(const auto &event:transitions) if(event.current.state=="hot") sawHot=true;
    CHECK(sawHot);
    Rule full=r; full.id="other"; CHECK(!Engine::validate({r,r},error));
    QVector<Rule> many;
    for(int i=0;i<1024;++i) { auto copy=r; copy.id=QString::number(i); many.append(copy); }
    CHECK(e.configure(many,0,error)); e.reset(true,"test",{},0);
    for(int i=0;i<1000;++i) e.consume({1},{},i);
    CHECK(e.states().size()==1024 && e.states().last().sequence==1000);
    std::cout<<"Mappings, bits, thresholds, hold/hysteresis, quality, bounded events, paired samples and idle timeout passed.\n";
    return 0;
}
