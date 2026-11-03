#pragma once
#include <QObject>
#include <QVector>
#include <QQueue>
#include <QMutex>
#include <QElapsedTimer>
#include <QTimer>
#include <QHash>
#include <limits>
#include <memory>
#include "ProtocolProfile.h"

class ProtocolParser;
namespace StatusMonitoring {
class TextEngine;
enum class Source { Channel, Text };
enum class RuleKind { Mapping, Bits, Threshold, TextMatch };
enum class Quality { Waiting, Valid, Unknown, Invalid, Missing, Stale, Disconnected, SourceMismatch, Disabled, Unavailable };
struct Mapping { quint32 value = 0; QString state; QString color, severity, note; };
struct Range {
    double lower = -std::numeric_limits<double>::infinity();
    double upper = std::numeric_limits<double>::infinity();
    bool includeLower = true, includeUpper = false;
    QString state;
    // Enable only on alarm ranges for entry at 80 and recovery below 78.
    bool retainWithHysteresis = false;
    QString color,severity,note;
};
struct TextSource {
    QString id;
    QByteArray delimiter = QByteArray("\r\n");
    QString encoding = QStringLiteral("UTF-8");
    bool datagram = false, trim = false, caseSensitive = true, strictSync = false;
    int maxBytes = 4096, partialTimeoutMs = 5000;
};
struct TextMatch { QString text, state; bool prefix = false; QString id, color, severity, note; bool enabled = true; };
enum class SilenceKind { Number, Range, Text };
struct SilenceCondition {SilenceKind kind=SilenceKind::Number;bool enabled=true;double value=0,lower=0,upper=1;QString text;};
struct EventOptions {bool enabled=false,onChange=true;int displayMs=0;QVector<SilenceCondition> silence;};
QJsonObject eventOptionsJson(const EventOptions &options);
EventOptions readEventOptions(const QJsonObject &object);
struct Rule {
    QString id, name, note;
    Source source = Source::Channel;
    RuleKind kind = RuleKind::Mapping;
    bool enabled = true, converted = false;
    // Empty protocolId explicitly means following the current channel index.
    QString protocolId;
    QByteArray protocolShape;
    int channel = 0, width = 32, firstBit = 0, bitCount = 1;
    int timeoutMs = 0, holdMs = 0;
    double hysteresis = 0;
    QVector<Mapping> mappings;
    QVector<Range> ranges;
    TextSource textSource;
    QVector<TextMatch> textMatches;
    bool recordEveryMatch = false;
    EventOptions event;
    static Rule textRule(const QString &id, const QString &name) {
        Rule r; r.id=id; r.name=name; r.source=Source::Text; r.kind=RuleKind::TextMatch; r.timeoutMs=0;r.event.onChange=false; return r;
    }
};
bool validateEventOptions(const Rule &rule,QString &error);
bool silencesNumber(const EventOptions &options,double value);
bool silencesText(const Rule &rule,const QString &normalized);
struct State {
    QString id, state, reason;
    Quality quality = Quality::Waiting;
    double value = std::numeric_limits<double>::quiet_NaN();
    qint64 lastValidMs = -1;
    qint64 receiveUnixNs = 0;
    quint64 session = 0, sequence = 0;
    QString text, detail, matchedRuleId, textSourceId, color, severity;
    int matchedRow = -1;
    quint64 matches = 0;
    qint64 byteStart = 0, byteEnd = 0;
    bool feedbackMayBeLost = false, processingDelayed = false;
    bool eventMode=false,eventLatched=false;
    qint64 lastEventMs=-1;
    quint64 suppressed=0;
};
struct Event { State previous, current; qint64 timeMs = 0, unixNs = 0; };
class Engine {
public:
    static bool validate(const QVector<Rule> &rules, QString &error);
    bool configure(const QVector<Rule> &rules, qint64 now, QString &error);
    void reset(bool connected, const QString &protocolId, const QByteArray &shape, qint64 now);
    void consume(const QVector<double> &raw, const QVector<double> &converted, qint64 now, qint64 receiveUnixNs = 0);
    void tick(qint64 now);
    bool clearEvent(const QString &id,qint64 now);
    QVector<State> states() const;
    QVector<Event> takeEvents();
    quint64 droppedEvents() const { return m_dropped; }
    quint64 session() const { return m_session; }
    void setEventLimit(int limit) {m_eventLimit=qBound(0,limit,2000);while(m_events.size()>m_eventLimit) {m_events.dequeue();++m_dropped;} }
private:
    struct Item { Rule rule; State state; QHash<quint32, QString> mapping; QString pending,lastEventKey; qint64 pendingSince = -1,lastInputMs=-1; int activeRange = -1; QByteArray key; };
    QVector<Item> m_items;
    QQueue<Event> m_events;
    quint64 m_session = 0, m_sequence = 0, m_dropped = 0;
    QString m_protocolId;
    QByteArray m_shape;
    bool m_connected = false;
    int m_eventLimit=2000;
    void update(Item &item, Quality quality, const QString &state, const QString &reason, qint64 now, const State *previous = nullptr,bool force=false);
};

// Own one per transport (Demo included). Engine execution precedes plot mailboxes.
// Configuration and snapshot access are thread-safe; bind/lifecycle run on owner thread.
class Session : public QObject {
public:
    explicit Session(QObject *parent = nullptr);
    ~Session() override;
    bool configure(const QVector<Rule> &rules, QString &error);
    void bind(ProtocolParser *parser, const QString &protocolId, const QByteArray &shape = {});
    void bindProtocol(ProtocolParser *parser, const DataProtocols::ProtocolSelection &selection, const DataProtocols::ProtocolProfile &profile);
    static QByteArray structure(const DataProtocols::ProtocolSelection &selection, const DataProtocols::ProtocolProfile &profile);
    void setConnected(bool connected);
    void restart();
    bool clearEvent(const QString &id);
    void setReceiveTimestamp(qint64 unixNs);
    // Owner-thread RX tap; once per actual read. Datagram end is explicit even for empty packets.
    void feedReceived(const QByteArray &bytes, qint64 unixNs, bool datagram = false, qint64 receivedMonotonicMs = -1);
    qint64 monotonicNow() const {return m_clock.elapsed();}
    void setTextTransport(bool datagram, bool available = true);
    QVector<struct TextDiagnostic> textDiagnostics() const;
    QHash<QString,QHash<QString,quint64>> textStatistics() const;
    QVector<State> snapshot() const;
    QVector<Event> takeEvents();
    quint64 droppedEvents() const;
private:
    mutable QMutex m_mutex;
    Engine m_engine;
    QElapsedTimer m_clock;
    QTimer *m_timer;
    QMetaObject::Connection m_connection;
    QString m_protocolId;
    QByteArray m_shape;
    bool m_connected = false;
    qint64 m_epochNs = 0, m_receiveUnixNs = 0;
    std::unique_ptr<TextEngine> m_text;
    struct PendingText { QByteArray bytes; int position=0; qint64 time=0, unixNs=0, offset=0; bool datagram=false; };
    QQueue<PendingText> m_textQueue;
    qint64 m_textQueuedBytes=0, m_textOffset=0;
    quint64 m_textEpoch=0;
    bool m_textScheduled=false;
    void scheduleText();
    void drainText(quint64 epoch);
    void clearTextQueue();
};
}
