#pragma once
#include "StatusMonitor.h"
#include <QTextCodec>
#include <functional>

namespace StatusMonitoring {
struct TextDiagnostic {
    QString sourceId, code, reason, text;
    qint64 byteStart=0, byteEnd=0, receiveUnixNs=0;
    quint64 sequence=0;
    bool matched=false;
    quint64 totalForCode=0;
};
class TextEngine {
public:
    static bool validate(const QVector<Rule> &rules, QString &error);
    static QString foldAscii(QString text);
    static bool decode(const QByteArray &bytes, const QString &encoding, QString &text, int &badByte);
    bool sameConfiguration(const QVector<Rule> &rules) const;
    bool configure(const QVector<Rule> &rules, qint64 now, QString &error);
    void reset(bool connected, qint64 now);
    void setTransport(bool datagram, bool available, qint64 now);
    void consume(const QByteArray &bytes, qint64 offset, qint64 now, qint64 unixNs, bool endDatagram);
    void tick(qint64 now);
    bool clearEvent(const QString &id,qint64 now);
    void gap(qint64 now, qint64 bytes);
    bool active() const;
    QVector<State> states() const;
    QVector<Event> takeEvents();
    QVector<TextDiagnostic> diagnostics() const;
    QHash<QString,QHash<QString,quint64>> statistics() const {return m_counts;}
    quint64 droppedEvents() const { return m_dropped; }
    void setEventLimit(int limit) {m_eventLimit=qBound(0,limit,2000);while(m_events.size()>m_eventLimit) {m_events.dequeue();++m_dropped;} }
private:
    struct Framer {
        TextSource config; QByteArray key, buffer;
        QVector<int> failure; int matched=0;
        bool syncing=false; qint64 start=0, lastByte=-1;
        quint64 sequence=0;
    };
    struct Item { Rule rule; State state; QString sourceId; QByteArray key; QHash<QString,int> exact; QVector<int> prefixes;int lastEventRow=-1;qint64 lastInputMs=-1; };
    QHash<QString,Framer> m_sources;
    QVector<Item> m_items;
    QQueue<Event> m_events;
    QQueue<TextDiagnostic> m_diagnostics;
    QHash<QString,QHash<QString,quint64>> m_counts;
    qint64 m_diagnosticBytes=0, m_now=0;
    quint64 m_session=0, m_dropped=0;
    int m_eventLimit=2000;
    bool m_connected=false, m_datagram=false, m_available=true;
    static QByteArray sourceKey(const TextSource &source);
    static QByteArray ruleKey(const Rule &rule);
    static QString sourceId(const Rule &rule);
    static void initialize(Framer &f);
    void appendDiagnostic(TextDiagnostic d);
    void record(Item &item, const State &old, qint64 now, bool force=false);
    void diagnostic(const QString &id, const QString &code, const QString &reason, qint64 start, qint64 end, qint64 unixNs, bool lost);
    void message(const QString &id, Framer &f, const QByteArray &bytes, qint64 start, qint64 end, qint64 now, qint64 unixNs);
};
// Isolated replay/preview uses the same engine, never a live transport.
struct TextTestResult { QVector<State> states; QVector<Event> events; QVector<TextDiagnostic> messages; };
TextTestResult testTextStream(const QVector<Rule> &rules, const QVector<QByteArray> &chunks, bool datagram, QString &error);
TextTestResult testTextMessage(const Rule &rule, const QString &message, QString &error);
QVector<QByteArray> generateTextSamples(const Rule &rule, QString &error);
}
