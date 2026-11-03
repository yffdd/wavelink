#include "StatusTextMonitor.h"
#include <QJsonDocument>
#include <QSet>

namespace StatusMonitoring {
QString TextEngine::foldAscii(QString text) {
    for (auto &c : text) if(c >= QLatin1Char('A') && c <= QLatin1Char('Z')) c=QChar(c.unicode()+32);
    return text;
}
QByteArray TextEngine::sourceKey(const TextSource &s) {
    return QJsonDocument(QJsonObject{{"delimiter",QString::fromLatin1(s.delimiter.toHex())},{"encoding",s.encoding},
        {"datagram",s.datagram},{"trim",s.trim},{"case",s.caseSensitive},{"sync",s.strictSync},
        {"max",s.maxBytes},{"timeout",s.partialTimeoutMs}}).toJson(QJsonDocument::Compact);
}
QString TextEngine::sourceId(const Rule &r) {
    return r.textSource.id.isEmpty() ? QStringLiteral("item:")+r.id : r.textSource.id;
}
QByteArray TextEngine::ruleKey(const Rule &r) {
    QJsonArray rows;
    for(const auto &m:r.textMatches) rows.append(QJsonObject{{"text",m.text},{"state",m.state},{"prefix",m.prefix},{"enabled",m.enabled},{"id",m.id}});
    auto object=QJsonObject{{"rows",rows},{"enabled",r.enabled},{"timeout",r.timeoutMs},{"every",r.recordEveryMatch}};if(r.event.enabled)object["event"]=eventOptionsJson(r.event);return sourceKey(r.textSource)+QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool TextEngine::decode(const QByteArray &bytes, const QString &encoding, QString &text, int &badByte) {
    badByte=-1;
    if(encoding=="ASCII") {
        for(int i=0;i<bytes.size();++i) if(uchar(bytes[i])>127) {badByte=i; return false;}
        text=QString::fromLatin1(bytes); return true;
    }
    auto codec=QTextCodec::codecForName(encoding.toLatin1()); if(!codec) {badByte=0; return false;}
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
    text=codec->toUnicode(bytes.constData(),bytes.size(),&state);
    if(!state.invalidChars && !state.remainingChars) return true;
    // Identify the smallest failing byte span's start with the same stateful decoder.
    QTextCodec::ConverterState scan(QTextCodec::IgnoreHeader); int pendingStart=0;
    for(int i=0;i<bytes.size();++i) {
        codec->toUnicode(bytes.constData()+i,1,&scan);
        if(scan.invalidChars) {badByte=pendingStart; return false;}
        if(!scan.remainingChars) pendingStart=i+1;
    }
    badByte=pendingStart; return false;
}
bool TextEngine::validate(const QVector<Rule> &rules, QString &error) {
    error.clear();QSet<QString> itemIds;
    if(rules.size()>1024) {error=QStringLiteral("监视项不能超过 1024 个。");return false;}
    QHash<QString,QByteArray> sources;
    for(const auto &r:rules) if(r.source==Source::Text) {
        if(!validateEventOptions(r,error))return false;
        if(r.id.isEmpty() || r.id.size()>256 || r.name.trimmed().isEmpty() || r.name.size()>1024 || itemIds.contains(r.id) || r.timeoutMs<0) {
            error=QStringLiteral("文本监视项 ID、名称或有效期无效。");return false;
        }
        itemIds.insert(r.id);
        const auto &s=r.textSource;
        if(r.kind!=RuleKind::TextMatch || s.id.size()>256 || s.id.startsWith("item:") || (!s.datagram && (s.delimiter.isEmpty() || s.delimiter.size()>16))
           || s.maxBytes<1 || s.maxBytes>65536 || s.partialTimeoutMs<0 || r.holdMs!=0 || r.hysteresis!=0
           || (s.encoding!="UTF-8" && s.encoding!="ASCII" && s.encoding!="GBK") || r.textMatches.isEmpty() || r.textMatches.size()>256) {
            error=QStringLiteral("文本来源、结束符、编码或规则配置无效；文本不使用数值保持时间/回差。"); return false;
        }
        auto id=sourceId(r); auto key=sourceKey(s);
        if(sources.contains(id) && sources[id]!=key) {error=QStringLiteral("同一个文本来源 ID 的设置不一致。"); return false;}
        if(r.enabled) sources.insert(id,key);
        QSet<QString> patterns, ids;
        for(const auto &m:r.textMatches) {
            auto pattern=s.caseSensitive ? m.text : foldAscii(m.text);
            const auto condition=QString::number(m.prefix)+pattern;
            if(m.text.isEmpty() || m.text.size()>4096 || m.state.isEmpty() || m.state.size()>1024
                || m.id.size()>256 || m.color.size()>64 || m.severity.size()>64 || m.note.size()>32768
                || patterns.contains(condition) || (!m.id.isEmpty() && ids.contains(m.id))) {
                error=QStringLiteral("文本匹配不能为空、重复或过长。"); return false;
            }
            for(auto c:m.text) if((c.unicode()<32 && c!=QLatin1Char('\t')) || c.unicode()==127) {error=QStringLiteral("匹配正文不能包含换行或控制字符。"); return false;}
            if(s.trim && (m.text.startsWith(' ') || m.text.startsWith('\t') || (!m.prefix && (m.text.endsWith(' ') || m.text.endsWith('\t'))))) {
                error=QStringLiteral("去除首尾空白后，此规则无法匹配。"); return false;
            }
            if(s.encoding=="ASCII") {for(auto c:m.text) if(c.unicode()>127) {error=QStringLiteral("ASCII 规则含非 ASCII 字符。"); return false;}}
            else {QTextCodec::ConverterState state; auto codec=QTextCodec::codecForName(s.encoding.toLatin1()); codec->fromUnicode(m.text.constData(),m.text.size(),&state);
                if(state.invalidChars || state.remainingChars) {error=QStringLiteral("规则含所选编码无法表示的字符。"); return false;}}
            patterns.insert(condition); if(!m.id.isEmpty()) ids.insert(m.id);
        }
    }
    if(sources.size()>16) {error=QStringLiteral("同时启用的文本来源不能超过 16 个。"); return false;}
    return true;
}
void TextEngine::initialize(Framer &f) {
    f.buffer.clear(); f.matched=0; f.lastByte=-1; f.syncing=f.config.strictSync;
    f.failure.fill(0,f.config.delimiter.size());
    for(int i=1,j=0;i<f.failure.size();++i) {
        while(j && f.config.delimiter[i]!=f.config.delimiter[j]) j=f.failure[j-1];
        if(f.config.delimiter[i]==f.config.delimiter[j]) ++j;
        f.failure[i]=j;
    }
}
bool TextEngine::configure(const QVector<Rule> &rules, qint64 now, QString &error) {
    if(!validate(rules,error)) return false;
    QHash<QString,Framer> sources; QVector<Item> items;
    for(const auto &r:rules) if(r.source==Source::Text) {
        Item item; item.rule=r; item.key=ruleKey(r); item.sourceId=sourceId(r); item.state.id=r.id; item.state.textSourceId=item.sourceId;
        for(int i=0;i<r.textMatches.size();++i) if(r.textMatches[i].enabled) {
            const auto &m=r.textMatches[i]; if(m.prefix) item.prefixes.append(i);
            else item.exact.insert(r.textSource.caseSensitive ? m.text : foldAscii(m.text),i);
        }
        bool retained=false;
        item.state.eventMode=r.event.enabled;
        for(const auto &old:m_items) if(old.rule.id==r.id && old.sourceId==item.sourceId && old.key==item.key) {item.state=old.state;item.lastEventRow=old.lastEventRow;item.lastInputMs=old.lastInputMs; retained=true; break;}
        if(!retained) {item.state.session=m_session; item.state.quality=!r.enabled ? Quality::Disabled : !m_available || (r.textSource.datagram && !m_datagram) ? Quality::Unavailable : m_connected ? Quality::Waiting : Quality::Disconnected;}
        if(retained && item.state.matchedRow>=0) {const auto &row=r.textMatches[item.state.matchedRow];item.state.color=row.color;item.state.severity=row.severity;}
        if(item.state.quality==Quality::Unavailable) item.state.reason=QStringLiteral("当前接口不提供此文本来源");
        items.append(item);
        if(r.enabled && !sources.contains(item.sourceId)) {
            Framer f; f.config=r.textSource; f.key=sourceKey(f.config); initialize(f);
            if(m_sources.contains(item.sourceId) && m_sources[item.sourceId].key==f.key) f=m_sources[item.sourceId];
            if(m_datagram) f.syncing=false;
            sources.insert(item.sourceId,f);
        }
    }
    m_items=items; m_sources=sources;
    for(auto it=m_counts.begin();it!=m_counts.end();) {if(!sources.contains(it.key())) it=m_counts.erase(it);else ++it;}
    m_now=now;
    for(const auto &item:m_items) if(item.rule.enabled) {
        const auto &rows=item.rule.textMatches;
        for(int i=0;i<rows.size();++i) if(rows[i].enabled) for(int j=0;j<i;++j) if(rows[j].enabled && rows[j].prefix) {
            const auto a=item.rule.textSource.caseSensitive ? rows[i].text : foldAscii(rows[i].text);
            const auto b=item.rule.textSource.caseSensitive ? rows[j].text : foldAscii(rows[j].text);
            if(a.startsWith(b)) {diagnostic(item.sourceId,"shadowed-rule",QStringLiteral("监视项 %1：规则 %2 被前面的前缀规则 %3 遮挡").arg(item.rule.name).arg(i+1).arg(j+1),0,0,0,false);break;}
        }
    }
    return true;
}
bool TextEngine::sameConfiguration(const QVector<Rule> &rules) const {
    int count=0;
    for(const auto &r:rules) if(r.source==Source::Text) {
        ++count;bool found=false;for(const auto &i:m_items) if(i.rule.id==r.id && i.sourceId==sourceId(r) && i.key==ruleKey(r)) {found=true;break;}
        if(!found) return false;
    }
    return count==m_items.size();
}
void TextEngine::record(Item &item, const State &old, qint64 now, bool force) {
    if(!force && old.state==item.state.state && old.quality==item.state.quality && old.feedbackMayBeLost==item.state.feedbackMayBeLost && old.session==item.state.session) return;
    if(!m_eventLimit) {++m_dropped;return;}
    if(m_events.size()==m_eventLimit) {m_events.dequeue();++m_dropped;}
    m_events.enqueue({old,item.state,now});
}
void TextEngine::reset(bool connected, qint64 now) {
    m_connected=connected; ++m_session;m_now=now;
    for(auto &f:m_sources) {initialize(f);if(m_datagram) f.syncing=false;f.sequence=0;}
    for(auto &item:m_items) {
        auto old=item.state; item.state=State(); item.state.id=item.rule.id; item.state.textSourceId=item.sourceId; item.state.session=m_session;
        item.state.eventMode=item.rule.event.enabled;item.lastEventRow=-1;item.lastInputMs=-1;
        item.state.quality=!item.rule.enabled ? Quality::Disabled : !m_available || (item.rule.textSource.datagram && !m_datagram) ? Quality::Unavailable : connected ? Quality::Waiting : Quality::Disconnected;
        if(item.state.quality==Quality::Unavailable) item.state.reason=QStringLiteral("当前接口不提供此文本来源");
        record(item,old,now);
    }
}
void TextEngine::setTransport(bool datagram, bool available, qint64 now) {
    if(m_datagram==datagram && m_available==available) return;
    m_datagram=datagram;m_available=available;reset(m_connected,now);
}
bool TextEngine::active() const {return m_connected && m_available && !m_sources.isEmpty();}
void TextEngine::diagnostic(const QString &id,const QString &code,const QString &reason,qint64 start,qint64 end,qint64 unixNs,bool lost) {
    appendDiagnostic({id,code,reason,{},start,end,unixNs});
    if(lost) for(auto &item:m_items) if(item.rule.enabled && item.sourceId==id) {
        const auto old=item.state; item.state.feedbackMayBeLost=true; item.state.reason=reason;item.lastEventRow=-1; record(item,old,m_now);
    }
}
void TextEngine::appendDiagnostic(TextDiagnostic d) {
    d.totalForCode=++m_counts[d.sourceId][d.code];
    int count=0;for(const auto &old:m_diagnostics) if(old.sourceId==d.sourceId) ++count;
    if(count>=20) for(int i=0;i<m_diagnostics.size();++i) if(m_diagnostics[i].sourceId==d.sourceId) {
        m_diagnosticBytes-=m_diagnostics[i].text.size()*2;m_diagnostics.removeAt(i);break;
    }
    const auto bytes=d.text.size()*2;
    while(!m_diagnostics.isEmpty() && (m_diagnostics.size()>=320 || m_diagnosticBytes+bytes>4*1024*1024)) m_diagnosticBytes-=m_diagnostics.dequeue().text.size()*2;
    m_diagnosticBytes+=bytes;m_diagnostics.enqueue(d);
}
void TextEngine::message(const QString &id,Framer &f,const QByteArray &bytes,qint64 start,qint64 end,qint64 now,qint64 unixNs) {
    ++f.sequence;
    if(bytes.isEmpty()) {diagnostic(id,"empty",QStringLiteral("空消息"),start,end,unixNs,false);return;}
    QString text; int bad=-1;
    if(!decode(bytes,f.config.encoding,text,bad)) {diagnostic(id,"encoding",QStringLiteral("文本编码错误，字节位置 %1").arg(start+qMax(0,bad)),start+qMax(0,bad),end,unixNs,true);return;}
    for(int i=0;i<bytes.size();++i) if((uchar(bytes[i])<32 && bytes[i]!='\t') || uchar(bytes[i])==127) {
        diagnostic(id,"control",QStringLiteral("不允许的控制字符，字节位置 %1").arg(start+i),start+i,start+i+1,unixNs,true);return;
    }
    for(const auto &c:text) if(c.category()==QChar::Other_Control && c!=QLatin1Char('\t')) {
        diagnostic(id,"control",QStringLiteral("正文包含不允许的 Unicode 控制字符"),start,end,unixNs,true);return;
    }
    QString normalized=text;
    if(f.config.trim) {int a=0,b=normalized.size(); while(a<b && (normalized[a]==' ' || normalized[a]=='\t')) ++a; while(b>a && (normalized[b-1]==' ' || normalized[b-1]=='\t')) --b; normalized=normalized.mid(a,b-a);}
    const auto comparable=f.config.caseSensitive ? normalized : foldAscii(normalized); bool any=false,silenced=false;
    for(auto &item:m_items) if(item.rule.enabled && item.sourceId==id) {
        if(item.rule.event.enabled){item.lastInputMs=now;if(silencesText(item.rule,normalized)){silenced=true;++item.state.suppressed;item.lastEventRow=-1;if(item.state.quality==Quality::Stale){const auto old=item.state;item.state.quality=Quality::Waiting;item.state.state.clear();item.state.reason="等待未屏蔽的匹配事件";record(item,old,now);}continue;}}
        int winner=item.exact.value(comparable,-1);
        for(auto index:item.prefixes) {if(winner>=0 && index>winner) break; const auto &m=item.rule.textMatches[index];
            if(comparable.startsWith(f.config.caseSensitive ? m.text : foldAscii(m.text))) {winner=index;break;}}
        if(winner<0){if(item.rule.event.enabled){item.lastEventRow=-1;if(item.state.quality==Quality::Stale){const auto old=item.state;item.state.quality=Quality::Waiting;item.state.state.clear();item.state.reason="等待未屏蔽的匹配事件";record(item,old,now);}}continue;}
        if(item.rule.event.enabled){const bool trigger=!item.rule.event.onChange||item.lastEventRow!=winner;item.lastEventRow=winner;if(!trigger){any=true;continue;}}
        any=true; const auto old=item.state; const auto &m=item.rule.textMatches[winner];
        item.state.state=m.state;item.state.quality=Quality::Valid;item.state.reason.clear();item.state.feedbackMayBeLost=false;
        item.state.text=text.left(512);item.state.detail=m.prefix ? normalized.mid(m.text.size()).left(512) : QString();
        item.state.color=m.color;item.state.severity=m.severity;item.state.matchedRow=winner;item.state.matchedRuleId=m.id;
        item.state.lastValidMs=now;item.state.receiveUnixNs=unixNs;item.state.sequence=f.sequence;++item.state.matches;
        if(item.rule.event.enabled){item.state.eventLatched=true;item.state.lastEventMs=now;}
        item.state.byteStart=start;item.state.byteEnd=end;record(item,old,now,item.rule.recordEveryMatch||item.rule.event.enabled);
    }
    appendDiagnostic({id,any?"matched":silenced?"silenced":"unmatched",any?QStringLiteral("已匹配"):silenced?QStringLiteral("已屏蔽（事件静默）"):QStringLiteral("未匹配"),text,start,end,unixNs,f.sequence,any});
}
void TextEngine::consume(const QByteArray &bytes,qint64 offset,qint64 now,qint64 unixNs,bool endDatagram) {
    if(!active()) return;
    m_now=now;
    for(auto it=m_sources.begin();it!=m_sources.end();++it) {
        auto &f=it.value(); if(f.config.datagram && !m_datagram) continue;
        if(f.config.datagram) {
            if(f.buffer.isEmpty()) f.start=offset;
            if(!f.syncing && f.buffer.size()+bytes.size()>f.config.maxBytes) {f.buffer.clear();f.syncing=true;diagnostic(it.key(),"length",QStringLiteral("报文超过消息上限"),f.start,offset+bytes.size(),unixNs,true);}
            if(!f.syncing) f.buffer+=bytes;
            if(endDatagram) {if(!f.syncing) message(it.key(),f,f.buffer,f.start,offset+bytes.size(),now,unixNs);initialize(f);f.syncing=false;}
            continue;
        }
        for(int i=0;i<bytes.size();++i) {
            const char c=bytes[i];if(f.buffer.isEmpty() && !f.syncing) f.start=offset+i;
            f.lastByte=now;
            if(!f.syncing) f.buffer.append(c);
            while(f.matched && c!=f.config.delimiter[f.matched]) f.matched=f.failure[f.matched-1];
            if(c==f.config.delimiter[f.matched]) ++f.matched;
            if(f.matched==f.config.delimiter.size()) {
                if(!f.syncing) message(it.key(),f,f.buffer.left(f.buffer.size()-f.config.delimiter.size()),f.start,offset+i+1-f.config.delimiter.size(),now,unixNs);
                f.buffer.clear();f.syncing=false;f.matched=0;f.lastByte=-1;
            } else if(!f.syncing && f.buffer.size()-f.matched>f.config.maxBytes) {
                diagnostic(it.key(),"length",QStringLiteral("正文超过消息上限"),f.start,offset+i+1,unixNs,true);f.buffer.clear();f.syncing=true;
            }
        }
        if(endDatagram) {
            if(!f.buffer.isEmpty()) diagnostic(it.key(),"incomplete-datagram",QStringLiteral("UDP 报文末尾残片已丢弃"),f.start,offset+bytes.size(),unixNs,true);
            initialize(f);f.syncing=false;
        }
    }
}
void TextEngine::tick(qint64 now) {
    if(!active()) return;
    m_now=now;
    for(auto it=m_sources.begin();it!=m_sources.end();++it) {auto &f=it.value();
        if(!m_datagram && !f.syncing && !f.buffer.isEmpty() && f.config.partialTimeoutMs>0 && now-f.lastByte>=f.config.partialTimeoutMs) {
            diagnostic(it.key(),"partial-timeout",QStringLiteral("半包超时，等待结束符重同步"),f.start,f.start+f.buffer.size(),0,true);f.buffer.clear();f.syncing=true;
        }
    }
    for(auto &item:m_items)if(item.rule.enabled&&item.rule.event.enabled){
        if(item.state.eventLatched&&item.rule.event.displayMs>0&&now-item.state.lastEventMs>=item.rule.event.displayMs){auto old=item.state;item.state.quality=Quality::Waiting;item.state.state.clear();item.state.eventLatched=false;item.state.reason="提示保持时间已结束，等待下一事件";record(item,old,now);}
        if(item.rule.timeoutMs>0&&item.lastInputMs>=0&&now-item.lastInputMs>=item.rule.timeoutMs&&(item.state.quality==Quality::Valid||item.state.quality==Quality::Waiting)){auto old=item.state;item.state.quality=Quality::Stale;item.state.eventLatched=false;item.lastEventRow=-1;item.state.reason="反馈数据超时";record(item,old,now);}
    }
    for(auto &item:m_items) if(item.rule.enabled && !item.rule.event.enabled&&item.rule.timeoutMs>0 && item.state.lastValidMs>=0
        && now-item.state.lastValidMs>=item.rule.timeoutMs && item.state.quality==Quality::Valid) {
        auto old=item.state;item.state.quality=Quality::Stale;item.state.reason=QStringLiteral("反馈超时");record(item,old,now);
    }
}
bool TextEngine::clearEvent(const QString &id,qint64 now){for(auto &item:m_items)if(item.rule.id==id&&item.rule.event.enabled&&item.state.eventLatched){auto old=item.state;item.state.quality=Quality::Waiting;item.state.state.clear();item.state.eventLatched=false;item.state.reason="事件提示已手动清除";record(item,old,now);return true;}return false;}
void TextEngine::gap(qint64 now,qint64 bytes) {
    m_now=now;
    for(auto it=m_sources.begin();it!=m_sources.end();++it) {initialize(it.value());it->syncing=!m_datagram;
        diagnostic(it.key(),"input-lost",QStringLiteral("文本处理队列溢出，丢弃 %1 字节，等待重同步").arg(bytes),0,0,0,true);}
}
QVector<State> TextEngine::states() const {QVector<State> result;for(const auto &item:m_items) result.append(item.state);return result;}
QVector<Event> TextEngine::takeEvents() {QVector<Event> result;while(!m_events.isEmpty()) result.append(m_events.dequeue());return result;}
QVector<TextDiagnostic> TextEngine::diagnostics() const {QVector<TextDiagnostic> result;for(const auto &d:m_diagnostics) result.append(d);return result;}
TextTestResult testTextStream(const QVector<Rule> &rules,const QVector<QByteArray> &chunks,bool datagram,QString &error) {
    TextEngine engine;TextTestResult result;if(!Engine::validate(rules,error) || !engine.configure(rules,0,error)) return result;
    engine.setTransport(datagram,true,0);engine.reset(true,0);qint64 offset=0,time=0;
    for(const auto &chunk:chunks) {engine.consume(chunk,offset,++time,0,datagram);offset+=chunk.size();}
    result.states=engine.states();result.events=engine.takeEvents();result.messages=engine.diagnostics();return result;
}
TextTestResult testTextMessage(const Rule &rule,const QString &message,QString &error) {
    if(rule.source!=Source::Text) {error=QStringLiteral("请选择接收文本监视项。");return {};}
    if(!Engine::validate({rule},error)) return {};
    auto copy=rule;copy.textSource.datagram=true;copy.textSource.strictSync=false;
    const auto codecName=copy.textSource.encoding=="ASCII" ? QByteArray("ISO-8859-1") : copy.textSource.encoding.toLatin1();
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);auto codec=QTextCodec::codecForName(codecName);
    auto bytes=codec->fromUnicode(message.constData(),message.size(),&state);
    if(state.invalidChars || state.remainingChars) {error=QStringLiteral("试算消息无法用所选编码表示。");return {};}
    return testTextStream({copy},{bytes},true,error);
}
QVector<QByteArray> generateTextSamples(const Rule &rule,QString &error) {
    if(rule.source!=Source::Text) {error=QStringLiteral("请选择接收文本监视项。");return {};}
    if(!Engine::validate({rule},error)) return {};
    QVector<QByteArray> result;const auto &s=rule.textSource;
    const auto codecName=s.encoding=="ASCII" ? QByteArray("ISO-8859-1") : s.encoding.toLatin1();
    auto codec=QTextCodec::codecForName(codecName);
    for(const auto &row:rule.textMatches) if(row.enabled) {
        QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
        const auto text=row.text+(row.prefix ? QStringLiteral("example") : QString());
        if(rule.event.enabled&&silencesText(rule,text))continue;
        auto bytes=codec->fromUnicode(text.constData(),text.size(),&state);
        if(bytes.size()>s.maxBytes) {error=QStringLiteral("生成样例超过消息长度上限，请调整规则或长度上限。");return {};}
        if(!s.datagram) {
            if(result.isEmpty() && s.strictSync) bytes.prepend(s.delimiter);
            bytes.append(s.delimiter);
        }
        result.append(bytes);
    }
    if(result.isEmpty()&&rule.event.enabled)error="自动样例均被屏蔽或规则未启用，请调整屏蔽条件或手动输入。";return result;
}
}
