#include "StatusMonitor.h"
#include "ProtocolParser.h"
#include "StatusTextMonitor.h"
#include <QMutexLocker>
#include <QSet>
#include <cmath>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDateTime>
#include <algorithm>
#include <QDataStream>

namespace StatusMonitoring {
QJsonObject eventOptionsJson(const EventOptions &o){
    QJsonArray conditions;for(const auto &c:o.silence){QJsonObject row{{"kind",QStringList{"number","range","text"}.value(int(c.kind))},{"enabled",c.enabled}};
        if(c.kind==SilenceKind::Number)row["value"]=c.value;else if(c.kind==SilenceKind::Range){row["lower"]=c.lower;row["upper"]=c.upper;}else row["text"]=c.text;conditions.append(row);}
    return {{"enabled",o.enabled},{"onChange",o.onChange},{"displayMs",o.displayMs},{"silence",conditions}};
}
EventOptions readEventOptions(const QJsonObject &object){EventOptions o;o.enabled=object["enabled"].toBool();o.onChange=object["onChange"].toBool();o.displayMs=object["displayMs"].toInt(-1);
    for(const auto &value:object["silence"].toArray()){const auto row=value.toObject();SilenceCondition c;c.kind=SilenceKind(QStringList{"number","range","text"}.indexOf(row["kind"].toString()));c.enabled=row["enabled"].toBool();c.value=row["value"].toDouble();c.lower=row["lower"].toDouble();c.upper=row["upper"].toDouble();c.text=row["text"].toString();o.silence.append(c);}return o;
}
bool validateEventOptions(const Rule &r,QString &error){
    auto fail=[&](const QString &s){error=s;return false;};const auto &o=r.event;
    if(o.displayMs<0||o.displayMs>86400000||o.silence.size()>64)return fail("事件提示保持时间应为 0–86400000 ms，屏蔽条件最多 64 项。");
    if(o.enabled&&(r.holdMs!=0||r.hysteresis!=0))return fail("事件提示不使用生效延迟或恢复回差，避免漏掉单帧事件。");
    for(const auto &c:o.silence){if(c.kind==SilenceKind::Number){if(!std::isfinite(c.value))return fail("屏蔽数值必须为有限数字。");}
        else if(c.kind==SilenceKind::Range){if(!std::isfinite(c.lower)||!std::isfinite(c.upper)||c.lower>c.upper)return fail("屏蔽区间必须是有限数字，且下限不大于上限（包含两端）。");}
        else if(c.kind==SilenceKind::Text){if(r.source!=Source::Text||c.text.isEmpty()||c.text.size()>4096)return fail("固定文本屏蔽仅适用于接收文本，正文须为 1–4096 个字符。");for(auto ch:c.text)if(ch.category()==QChar::Other_Control&&ch!='\t')return fail("屏蔽文本不能包含换行或控制字符。");
            if(r.textSource.trim&&(c.text.startsWith(' ')||c.text.startsWith('\t')||c.text.endsWith(' ')||c.text.endsWith('\t')))return fail("已去除首尾空白，屏蔽文本的首尾不能包含空格或 TAB。");
            if(r.textSource.encoding=="ASCII"){for(auto ch:c.text)if(ch.unicode()>127)return fail("ASCII 屏蔽文本含非 ASCII 字符。");}
            else {auto codec=QTextCodec::codecForName(r.textSource.encoding.toLatin1());if(codec){QTextCodec::ConverterState state;codec->fromUnicode(c.text.constData(),c.text.size(),&state);if(state.invalidChars||state.remainingChars)return fail("屏蔽文本不能用所选编码表示。");}}
        }else return fail("未知事件屏蔽条件类型。");}
    return true;
}
bool silencesNumber(const EventOptions &o,double value){if(!std::isfinite(value))return false;for(const auto &c:o.silence)if(c.enabled&&((c.kind==SilenceKind::Number&&value==c.value)||(c.kind==SilenceKind::Range&&value>=c.lower&&value<=c.upper)))return true;return false;}
bool silencesText(const Rule &r,const QString &text){for(const auto &c:r.event.silence)if(c.enabled&&c.kind==SilenceKind::Text&&(r.textSource.caseSensitive?text==c.text:TextEngine::foldAscii(text)==TextEngine::foldAscii(c.text)))return true;bool ok=false;const double value=text.toDouble(&ok);return ok&&silencesNumber(r.event,value);}
bool Engine::validate(const QVector<Rule> &rules, QString &error)
{
    error.clear(); QSet<QString> ids;
    auto fail = [&](const QString &message) { error = message; return false; };
    if (rules.size() > 1024) return fail(QStringLiteral("监视项不能超过 1024 个。"));
    if (!TextEngine::validate(rules,error)) return false;
    for (const auto &r : rules) {
        if(!validateEventOptions(r,error))return false;
        if (r.id.isEmpty() || ids.contains(r.id) || r.name.trimmed().isEmpty())
            return fail(QStringLiteral("监视项 ID 必须唯一且名称不能为空。"));
        ids.insert(r.id);
        if (r.name.size() > 1024 || r.id.size() > 256 || r.protocolId.size() > 256 || r.protocolShape.size() > 256)
            return fail(QStringLiteral("监视项名称或来源标识过长。"));
        if (r.timeoutMs < 0 || r.holdMs < 0 || !std::isfinite(r.hysteresis) || r.hysteresis < 0)
            return fail(QStringLiteral("超时、保持时间和回差必须为非负数。"));
        if (r.source == Source::Text) {
            const auto &t = r.textSource;
            if (r.kind != RuleKind::TextMatch || (!t.datagram && (t.delimiter.isEmpty() || t.delimiter.size() > 16))
                || t.maxBytes < 1 || t.maxBytes > 65536 || t.partialTimeoutMs < 0
                || (t.encoding != "UTF-8" && t.encoding != "ASCII" && t.encoding != "GBK")
                || r.textMatches.isEmpty() || r.textMatches.size() > 256)
                return fail(QStringLiteral("文本来源、分帧或匹配配置无效。"));
            QSet<QString> patterns;
            for (const auto &m : r.textMatches) {
                const auto key = QString::number(m.prefix) + m.text;
                if (m.text.isEmpty() || m.text.size() > 4096 || m.state.isEmpty() || m.state.size() > 1024 || patterns.contains(key))
                    return fail(QStringLiteral("文本匹配不能为空、重复或过长。"));
                patterns.insert(key);
            }
            continue;
        }
        if (r.source != Source::Channel || r.channel < 0 || r.channel >= 256)
            return fail(QStringLiteral("来源类型或通道序号无效。"));
        if (!r.protocolId.isEmpty() && r.protocolShape.isEmpty())
            return fail(QStringLiteral("指定协议绑定必须包含协议结构标识。"));
        if (r.kind == RuleKind::Mapping || r.kind == RuleKind::Bits) {
            if ((r.width != 8 && r.width != 16 && r.width != 32) || r.mappings.isEmpty() || r.mappings.size() > 256)
                return fail(QStringLiteral("映射/按位配置需要有效位宽和 1–256 个映射。"));
            if (r.kind == RuleKind::Bits && (r.firstBit < 0 || r.bitCount < 1 || r.firstBit + r.bitCount > r.width))
                return fail(QStringLiteral("位段超出状态字位宽。"));
            QSet<quint32> values;
            const int bits = r.kind == RuleKind::Bits ? r.bitCount : r.width;
            const quint64 maximum = (quint64(1) << bits) - 1;
            for (const auto &m : r.mappings) {
                if (m.value > maximum || values.contains(m.value) || m.state.isEmpty() || m.state.size() > 1024)
                    return fail(QStringLiteral("映射值重复、越界或状态名称无效。"));
                values.insert(m.value);
            }
        } else if (r.kind == RuleKind::Threshold) {
            if (r.ranges.isEmpty() || r.ranges.size() > 256) return fail(QStringLiteral("阈值区间数量无效。"));
            for (int i = 0; i < r.ranges.size(); ++i) {
                const auto &a = r.ranges[i];
                if (std::isnan(a.lower) || std::isnan(a.upper) || a.lower >= a.upper || a.state.isEmpty() || a.state.size() > 1024)
                    return fail(QStringLiteral("阈值边界或状态名称无效。"));
                for (int j = 0; j < i; ++j) {
                    const auto &b = r.ranges[j];
                    if (qMax(a.lower, b.lower) < qMin(a.upper, b.upper)
                        || (a.upper == b.lower && a.includeUpper && b.includeLower)
                        || (b.upper == a.lower && b.includeUpper && a.includeLower))
                        return fail(QStringLiteral("阈值区间重叠。"));
                }
            }
        } else return fail(QStringLiteral("数值来源的规则类型无效。"));
    }
    return true;
}
bool Engine::configure(const QVector<Rule> &rules, qint64 now, QString &error)
{
    if (!validate(rules, error)) return false;
    const auto previous=m_items;m_items.clear();
    for (const auto &r : rules) {
        Item item; item.rule = r; item.state.id = r.id;
        QDataStream key(&item.key,QIODevice::WriteOnly);key<<int(r.source)<<int(r.kind)<<r.enabled<<r.converted<<r.protocolId<<r.protocolShape<<r.channel<<r.width<<r.firstBit<<r.bitCount<<r.timeoutMs<<r.holdMs<<r.hysteresis;
        key<<r.mappings.size()<<r.ranges.size();
        key<<r.event.enabled;if(r.event.enabled)key<<QJsonDocument(eventOptionsJson(r.event)).toJson(QJsonDocument::Compact);
        for(const auto &m:r.mappings)key<<m.value<<m.state;
        for(const auto &v:r.ranges)key<<v.lower<<v.upper<<v.includeLower<<v.includeUpper<<v.state<<v.retainWithHysteresis;
        item.state.eventMode=r.event.enabled;
        bool retained=false;for(const auto &old:previous)if(old.rule.id==r.id&&old.key==item.key){item.state=old.state;item.pending=old.pending;item.pendingSince=old.pendingSince;item.activeRange=old.activeRange;item.lastEventKey=old.lastEventKey;item.lastInputMs=old.lastInputMs;retained=true;break;}
        for (const auto &m : r.mappings) item.mapping.insert(m.value, m.state);
        if(!retained){const bool wrong=!r.protocolId.isEmpty()&&(r.protocolId!=m_protocolId||r.protocolShape!=m_shape);const auto quality=!r.enabled?Quality::Disabled:r.source==Source::Text?Quality::Unavailable:wrong?Quality::SourceMismatch:m_connected?Quality::Waiting:Quality::Disconnected;update(item,quality,{},quality==Quality::Unavailable?QStringLiteral("文本来源由独立文本引擎处理"):QString(),now);}
        m_items.append(item);
    }
    return true;
}
void Engine::update(Item &item, Quality quality, const QString &state, const QString &reason, qint64 now, const State *previous,bool force)
{
    const auto old = previous ? *previous : item.state;
    item.state.quality = quality; item.state.state = state; item.state.reason = reason;
    item.state.session = m_session; item.state.sequence = m_sequence;
    if (quality != Quality::Valid) { item.pending.clear(); item.pendingSince = -1; item.activeRange = -1;item.state.eventLatched=false; }
    if(!force&&old.quality == quality && old.state == state && old.reason == reason && old.session == m_session) return;
    if(!m_eventLimit) {++m_dropped;return;}
    if (m_events.size() == m_eventLimit) { m_events.dequeue(); ++m_dropped; }
    m_events.enqueue({old, item.state, now});
}
void Engine::reset(bool connected, const QString &id, const QByteArray &shape, qint64 now)
{
    m_connected = connected; m_protocolId = id; m_shape = shape; ++m_session; m_sequence = 0;
    for (auto &item : m_items) {
        const auto previous = item.state;
        item.state.lastValidMs = -1; item.state.receiveUnixNs = 0; item.state.value = std::numeric_limits<double>::quiet_NaN();
        item.pending.clear(); item.pendingSince = -1; item.activeRange = -1;
        const auto &r = item.rule;
        item.lastEventKey.clear();item.lastInputMs=-1;item.state.eventMode=r.event.enabled;item.state.eventLatched=false;item.state.lastEventMs=-1;item.state.matches=0;item.state.suppressed=0;
        const bool wrong = !r.protocolId.isEmpty() && (r.protocolId != id || r.protocolShape != shape);
        const auto quality = !r.enabled ? Quality::Disabled : r.source == Source::Text ? Quality::Unavailable
            : wrong ? Quality::SourceMismatch : connected ? Quality::Waiting : Quality::Disconnected;
        update(item, quality, {}, quality == Quality::Unavailable ? QStringLiteral("文本来源由独立文本引擎处理") : QString(), now, &previous);
    }
}
void Engine::consume(const QVector<double> &raw, const QVector<double> &converted, qint64 now, qint64 receiveUnixNs)
{
    if (!m_connected || m_items.isEmpty()) return;
    ++m_sequence;
    for (auto &item : m_items) {
        const auto &r = item.rule;
        if (!r.enabled || r.source != Source::Channel || item.state.quality == Quality::SourceMismatch) continue;
        const auto previous = item.state;
        if(r.event.enabled){
            const auto &values=r.converted?converted:raw;
            if(r.channel>=values.size()){item.lastEventKey.clear();update(item,Quality::Missing,{},"本帧缺少来源通道",now,&previous);continue;}
            const double value=values[r.channel];
            if(!std::isfinite(value)){item.lastEventKey.clear();update(item,Quality::Invalid,{},"数值为 NaN 或 Inf",now,&previous);continue;}
            item.lastInputMs=now;QString candidate,eventKey;int row=-1;
            if(silencesNumber(r.event,value)){++item.state.suppressed;}
            else if(r.kind==RuleKind::Threshold){for(int i=0;i<r.ranges.size();++i){const auto &a=r.ranges[i];if((value>a.lower||(a.includeLower&&value==a.lower))&&(value<a.upper||(a.includeUpper&&value==a.upper))){candidate=a.state;row=i;eventKey="range:"+QString::number(i);break;}}}
            else {const double maximum=double((quint64(1)<<r.width)-1);if(value<0||value>maximum||std::floor(value)!=value){item.lastEventKey.clear();update(item,Quality::Invalid,{},"状态字必须为位宽范围内的无符号整数",now,&previous);continue;}quint32 key=quint32(value);if(r.kind==RuleKind::Bits)key=quint32((quint64(key)>>r.firstBit)&((quint64(1)<<r.bitCount)-1));candidate=item.mapping.value(key);if(!candidate.isEmpty())eventKey="map:"+QString::number(key);}
            if(candidate.isEmpty()){item.lastEventKey.clear();if(item.state.quality!=Quality::Valid&&item.state.quality!=Quality::Waiting)update(item,Quality::Waiting,{},"等待未屏蔽的匹配事件",now,&previous);continue;}
            const bool trigger=!r.event.onChange||item.lastEventKey!=eventKey;item.lastEventKey=eventKey;if(!trigger)continue;
            item.state.eventLatched=true;item.state.lastEventMs=now;item.state.lastValidMs=now;item.state.value=value;item.state.receiveUnixNs=receiveUnixNs;item.state.matchedRow=row;++item.state.matches;
            update(item,Quality::Valid,candidate,"事件已触发",now,&previous,true);continue;
        }
        item.state.receiveUnixNs = receiveUnixNs;
        const auto &values = r.converted ? converted : raw;
        if (r.channel >= values.size()) { update(item, Quality::Missing, {}, QStringLiteral("本帧缺少来源通道"), now, &previous); continue; }
        const double value = values[r.channel]; item.state.value = value; item.state.sequence = m_sequence;
        if (!std::isfinite(value)) { update(item, Quality::Invalid, {}, QStringLiteral("数值为 NaN 或 Inf"), now, &previous); continue; }
        QString candidate; int rangeIndex = -1;
        if (r.kind == RuleKind::Threshold) {
            if (item.activeRange >= 0 && r.ranges[item.activeRange].retainWithHysteresis && r.hysteresis > 0 && item.state.quality == Quality::Valid) {
                const auto &a = r.ranges[item.activeRange];
                if (value > a.lower - r.hysteresis && value < a.upper + r.hysteresis) { candidate = a.state; rangeIndex = item.activeRange; }
            }
            if (candidate.isEmpty()) for (int i = 0; i < r.ranges.size(); ++i) {
                const auto &a = r.ranges[i];
                if ((value > a.lower || (a.includeLower && value == a.lower)) && (value < a.upper || (a.includeUpper && value == a.upper))) {
                    candidate = a.state; rangeIndex = i; break;
                }
            }
        } else {
            const double maximum = double((quint64(1) << r.width) - 1);
            if (value < 0 || value > maximum || std::floor(value) != value) {
                update(item, Quality::Invalid, {}, QStringLiteral("状态字必须为位宽范围内的无符号整数"), now, &previous); continue;
            }
            quint32 key = quint32(value);
            if (r.kind == RuleKind::Bits) key = quint32((quint64(key) >> r.firstBit) & ((quint64(1) << r.bitCount) - 1));
            candidate = item.mapping.value(key);
        }
        item.state.lastValidMs = now;
        if (candidate.isEmpty()) { update(item, Quality::Unknown, {}, QStringLiteral("没有匹配的状态规则"), now, &previous); continue; }
        if (candidate != item.state.state || item.state.quality != Quality::Valid) {
            if (r.holdMs > 0) {
                if (item.pending != candidate) { item.pending = candidate; item.pendingSince = now; }
                if (now - item.pendingSince < r.holdMs) continue;
            }
            update(item, Quality::Valid, candidate, {}, now, &previous);
        }
        item.activeRange = rangeIndex; item.state.matchedRow=rangeIndex;item.pending.clear(); item.pendingSince = -1;
    }
}
void Engine::tick(qint64 now)
{
    if (!m_connected) return;
    for(auto &item:m_items)if(item.rule.enabled&&item.rule.event.enabled){
        if(item.state.eventLatched&&item.rule.event.displayMs>0&&now-item.state.lastEventMs>=item.rule.event.displayMs)update(item,Quality::Waiting,{},"提示保持时间已结束，等待下一事件",now);
        if(item.rule.timeoutMs>0&&item.lastInputMs>=0&&now-item.lastInputMs>=item.rule.timeoutMs&&(item.state.quality==Quality::Valid||item.state.quality==Quality::Waiting)){item.lastEventKey.clear();update(item,Quality::Stale,{},"通道数据超时",now);}
    }
    for (auto &item : m_items) if (item.rule.enabled && item.rule.source == Source::Channel
        && !item.rule.event.enabled
        && item.rule.timeoutMs > 0 && item.state.lastValidMs >= 0 && now - item.state.lastValidMs >= item.rule.timeoutMs
        && (item.state.quality == Quality::Valid || item.state.quality == Quality::Unknown || item.state.quality == Quality::Waiting))
        update(item, Quality::Stale, {}, QStringLiteral("通道数据超时"), now);
}
bool Engine::clearEvent(const QString &id,qint64 now){for(auto &item:m_items)if(item.rule.id==id&&item.rule.event.enabled&&item.state.eventLatched){update(item,Quality::Waiting,{},"事件提示已手动清除",now);return true;}return false;}
QVector<State> Engine::states() const { QVector<State> result; for (const auto &item : m_items) result.append(item.state); return result; }
QVector<Event> Engine::takeEvents() { QVector<Event> result; result.reserve(m_events.size()); while (!m_events.isEmpty()) result.append(m_events.dequeue()); return result; }

Session::Session(QObject *parent) : QObject(parent), m_timer(new QTimer(this)), m_text(new TextEngine)
{
    m_epochNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    m_clock.start(); m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, [this] {
        QMutexLocker lock(&m_mutex); const auto now=m_clock.elapsed();m_engine.tick(now);
        if(m_textQueue.isEmpty()) m_text->tick(now);
    });
    m_timer->start();
}
Session::~Session() = default;
bool Session::clearEvent(const QString &id){QMutexLocker lock(&m_mutex);const auto now=m_clock.elapsed();const bool number=m_engine.clearEvent(id,now);return m_text->clearEvent(id,now)||number;}
bool Session::configure(const QVector<Rule> &rules, QString &error) {
    QMutexLocker lock(&m_mutex);
    if(!Engine::validate(rules,error)) return false;
    const bool textChanged=!m_text->sameConfiguration(rules);
    QVector<Rule> numeric;for(const auto &r:rules) if(r.source==Source::Channel) numeric.append(r);
    const int total=qMax(1,rules.size());
    const int numericBudget=rules.isEmpty() ? 2000 : 2000*numeric.size()/total;
    m_engine.setEventLimit(numericBudget);m_text->setEventLimit(2000-numericBudget);
    m_engine.configure(numeric,m_clock.elapsed(),error);
    m_text->configure(rules,m_clock.elapsed(),error);
    if(textChanged && !m_textQueue.isEmpty()) {const auto lost=m_textQueuedBytes;clearTextQueue();m_text->gap(m_clock.elapsed(),lost);}
    return true;
}
void Session::bind(ProtocolParser *parser, const QString &id, const QByteArray &shape)
{
    QObject::disconnect(m_connection);
    { QMutexLocker lock(&m_mutex); m_protocolId = id; m_shape = shape; m_engine.reset(m_connected, id, shape, m_clock.elapsed()); }
    if (parser) m_connection = connect(parser, &ProtocolParser::monitorFrameReady, this,
        [this](const QVector<double> &raw, const QVector<double> &converted) {
            QMutexLocker lock(&m_mutex); const auto now = m_clock.elapsed();
            m_engine.consume(raw, converted, now, m_receiveUnixNs ? m_receiveUnixNs : m_epochNs + now * 1000000LL);
        }, Qt::DirectConnection);
}
void Session::clearTextQueue() {m_textQueue.clear();m_textQueuedBytes=0;m_textScheduled=false;++m_textEpoch;}
void Session::setConnected(bool connected) {
    QMutexLocker lock(&m_mutex);m_connected=connected;m_receiveUnixNs=0;m_textOffset=0;clearTextQueue();
    m_engine.reset(connected,m_protocolId,m_shape,m_clock.elapsed());m_text->reset(connected,m_clock.elapsed());
}
void Session::setTextTransport(bool datagram,bool available) {
    QMutexLocker lock(&m_mutex);clearTextQueue();m_text->setTransport(datagram,available,m_clock.elapsed());
}
void Session::scheduleText() {
    if(m_textScheduled || m_textQueue.isEmpty()) return;
    m_textScheduled=true;const auto epoch=m_textEpoch;
    QTimer::singleShot(0,this,[this,epoch] {drainText(epoch);});
}
void Session::feedReceived(const QByteArray &bytes,qint64 unixNs,bool datagram,qint64 receivedMonotonicMs) {
    QMutexLocker lock(&m_mutex);const auto offset=m_textOffset;m_textOffset+=bytes.size();
    if(!m_text->active()) return;
    if(m_textQueuedBytes+bytes.size()>1024*1024 || m_textQueue.size()>=4096) {
        const auto lost=m_textQueuedBytes+bytes.size();clearTextQueue();m_text->gap(m_clock.elapsed(),lost);return;
    }
    const auto now=m_clock.elapsed();const auto received=receivedMonotonicMs>=0 && receivedMonotonicMs<=now ? receivedMonotonicMs : now;
    m_textQueue.enqueue({bytes,0,received,unixNs,offset,datagram});m_textQueuedBytes+=bytes.size();scheduleText();
}
void Session::drainText(quint64 epoch) {
    QMutexLocker lock(&m_mutex);if(epoch!=m_textEpoch) return;
    m_textScheduled=false;QElapsedTimer budget;budget.start();int processed=0;
    while(!m_textQueue.isEmpty()) {
        auto &batch=m_textQueue.head();const int n=qMin(128,batch.bytes.size()-batch.position);
        const bool finished=batch.position+n==batch.bytes.size();
        m_text->consume(batch.bytes.mid(batch.position,n),batch.offset+batch.position,batch.time,batch.unixNs,finished && batch.datagram);
        batch.position+=n;m_textQueuedBytes-=n;processed+=n;
        if(finished) m_textQueue.dequeue();
        if(processed>=64*1024 || budget.elapsed()>=4) break;
    }
    if(m_textQueue.isEmpty()) m_text->tick(m_clock.elapsed());
    scheduleText();
}
void Session::setReceiveTimestamp(qint64 unixNs) { QMutexLocker lock(&m_mutex); m_receiveUnixNs = unixNs; }
QByteArray Session::structure(const DataProtocols::ProtocolSelection &selection, const DataProtocols::ProtocolProfile &profile)
{
    auto definition = selection.kind == "custom" ? profile.definition : selection.toJson();
    for (const auto &key : {"name", "description", "revision"}) definition.remove(key);
    auto channels = definition.value("channels").toArray();
    for (int i = 0; i < channels.size(); ++i) {
        auto field = channels[i].toObject();
        for (const auto &key : {"name", "unit", "note"}) field.remove(key);
        channels[i] = field;
    }
    if (definition.contains("channels")) definition.insert("channels", channels);
    definition.remove("recordingValueMode");
    return QCryptographicHash::hash(QJsonDocument(definition).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
}
void Session::bindProtocol(ProtocolParser *parser, const DataProtocols::ProtocolSelection &selection, const DataProtocols::ProtocolProfile &profile)
{ bind(parser, selection.id, structure(selection, profile)); }
void Session::restart() { QMutexLocker lock(&m_mutex); clearTextQueue();m_textOffset=0;m_engine.reset(m_connected,m_protocolId,m_shape,m_clock.elapsed());m_text->reset(m_connected,m_clock.elapsed()); }
QVector<State> Session::snapshot() const {
    QMutexLocker lock(&m_mutex);auto states=m_engine.states();auto text=m_text->states();
    const bool delayed=!m_textQueue.isEmpty() && m_clock.elapsed()-m_textQueue.head().time>100;
    for(auto &state:text) state.processingDelayed=delayed;
    states+=text;return states;
}
QVector<Event> Session::takeEvents() {
    QMutexLocker lock(&m_mutex);auto events=m_engine.takeEvents();events+=m_text->takeEvents();
    for(auto &event:events) event.unixNs=m_epochNs+event.timeMs*1000000LL;
    std::stable_sort(events.begin(),events.end(),[](const Event &a,const Event &b){return a.timeMs<b.timeMs;});return events;
}
QVector<TextDiagnostic> Session::textDiagnostics() const {QMutexLocker lock(&m_mutex);return m_text->diagnostics();}
QHash<QString,QHash<QString,quint64>> Session::textStatistics() const {QMutexLocker lock(&m_mutex);return m_text->statistics();}
quint64 Session::droppedEvents() const { QMutexLocker lock(&m_mutex); return m_engine.droppedEvents()+m_text->droppedEvents(); }
}
