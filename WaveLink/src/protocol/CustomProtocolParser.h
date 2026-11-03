#pragma once
#include "ProtocolParser.h"
#include "ProtocolProfile.h"
#include "ProtocolChecksum.h"
#include <QRegularExpression>
#include <QSet>
#include <QMap>
#include <QElapsedTimer>

namespace DataProtocols {
struct ProtocolParseStats {
    quint64 validFrames = 0, rejectedFrames = 0, checksumFailures = 0;
    quint64 discardedBytes = 0, consumedBytes = 0;
    quint64 outOfRangeValues = 0;
    QString lastError;
};

// Caller owns incomplete bytes, exactly like FireWater/JustFloat. Rule data
// is compiled once; processData never reads widgets or reparses JSON.
class CustomProtocolParser : public ProtocolParser {
    Q_OBJECT
public:
    int processData(const char *data, int size) override;
    void reset() override {
        endDatagram(false);
        m_stats = {};
    }
    void endDatagram(bool complete = true) override {
        m_discardingLine = false;
        m_delimiterDiscarding = false;
        m_delimiterScanOffset = 0;
        if (!complete || m_fixedTailDiscardedBytes) m_fixedTailValidStreak = 0;
        m_fixedTailDiscardedBytes = 0;
        resetKeyValueGroup();
    }
    ProtocolParseStats stats() const { return m_stats; }
    QStringList channelNames() const override { return m_channelNames; }
    bool allowsDynamicChannelCount() const override { return m_array; }
    // End-of-sample checks for the offline tester ONLY. A live stream must
    // continue waiting for partial frames; this never consumes or resets it.
    QStringList testInputDiagnostics(const QByteArray &input, const QByteArray &remaining) const;
signals:
    void frameDecoded(const QVector<double> &raw, const QVector<double> &converted);
    void frameRejected(quint64 byteOffset, const QString &reason);
private:
    friend class ProtocolParserFactory;
    explicit CustomProtocolParser(const ProtocolProfile &validated, QObject *parent, bool confirmLiveSync);
    enum ValueType { Signed8, Unsigned8, Signed16, Unsigned16, Signed32, Unsigned32, Float32, Float64 };
    struct Field {
        int offset = 0, width = 0;
        ValueType type = Float64;
        bool big = false, hasMin = false, hasMax = false;
        double scale = 1, bias = 0, min = 0, max = 0;
    };
    struct Failure {
        QString message;
        int offset = 0; // Relative to the candidate frame/line, in raw bytes.
        bool nonTarget = false;
        Failure() = default;
        Failure(const QString &text, int position = 0, bool ignoredEnvelope = false) : message(text), offset(position), nonTarget(ignoredEnvelope) {}
        Failure &operator=(const QString &text) { message = text; offset = 0; nonTarget = false; return *this; }
    };
    int processText(const QByteArray &bytes);
    int processBinary(const QByteArray &bytes);
    int processTailDelimitedFixed(const QByteArray &bytes);
    int processDelimitedArray(const QByteArray &bytes);
    // Zero: incomplete header; negative: invalid header/length; positive:
    // bounded candidate size (the body may still be incomplete).
    int binaryFrameSize(const char *frame, int available, Failure &error, bool &checksumFailure) const;
    int findCompleteBinaryFrame(const QByteArray &bytes, int start) const;
    bool decodeText(const QByteArray &line, QVector<double> &raw, QVector<double> &values, Failure &error) const;
    bool decodeKeyValue(const QByteArray &line, int offset, QVector<double> &raw, QVector<double> &values, Failure &error);
    void resetKeyValueGroup();
    bool decodeBinary(const char *frame, int size, QVector<double> &raw, QVector<double> &values, Failure &error, bool &checksumFailure) const;
    bool parseTextNumber(const QString &token, double &number) const;
    bool convert(double raw, const Field &field, double &output) const;
    QString valueFailure(double raw, const Field &field) const;
    QString delimiterHint(const QString &token) const;
    int columnByteOffset(const QString &text, const QString &body, int column) const;
    bool decodeLineEnvelope(const QByteArray &line, QString &text, Failure &error) const;
    static double readNumber(const char *data, const Field &field);
    void reject(int offset, const QString &reason, bool checksum = false);
    void reject(int frameOffset, const Failure &error, bool checksum = false);
    void publish(const QVector<double> &raw, const QVector<double> &converted);
    bool m_text = false, m_ascii = false, m_whitespace = false, m_trim = true, m_discardingLine = false;
    QString m_prefix, m_suffix, m_delimiter;
    QByteArray m_ending, m_header, m_tail;
    int m_size = 0, m_limit = 4096;
    bool m_variable = false, m_lengthBig = false;
    bool m_delimiterFramed = false, m_delimiterDiscarding = false;
    int m_delimiterScanOffset = 0;
    quint64 m_fixedTailDiscardedBytes = 0;
    const bool m_confirmLiveSync;
    int m_fixedTailValidStreak = 0;
    int m_minSize = 0, m_lengthOffset = 0, m_lengthWidth = 0, m_lengthUnit = 1, m_lengthAdjustment = 0;
    QVector<Field> m_fields;
    bool m_array = false;
    int m_payloadOffset = 0, m_payloadEndOffset = 0, m_arrayMin = 1, m_arrayMax = MaxChannels;
    Field m_arrayField;
    QMap<int, Field> m_arrayOverrides;
    QSet<int> m_textConfiguredColumns;
    QVector<QPair<int, QByteArray>> m_conditions;
    ProtocolChecksum m_checksum;
    ProtocolChecksum m_headerChecksum;
    QString m_checksumAlgorithm, m_headerChecksumAlgorithm;
    int m_headerCheckStart = 0, m_headerCheckLength = 0, m_headerCheckOffset = 0;
    bool m_headerCheckBig = false;
    int m_checkStart = 0, m_checkLength = 0, m_checkOffset = 0, m_checkEndOffset = 0;
    bool m_checkBig = false, m_checkFromEnd = false, m_checkToEnd = false;
    QRegularExpression m_number, m_spaces;
    ProtocolParseStats m_stats;
    QStringList m_channelNames;
    bool m_markRangeNaN = false;
    bool m_keyValue = false, m_unknownKeysIgnored = true, m_replaceDuplicateKeys = false;
    int m_groupTimeoutMs = 1000, m_receivedKeys = 0;
    QMap<QString, int> m_keyIndices;
    QVector<bool> m_keyReceived;
    QVector<double> m_groupRaw, m_groupValues;
    QElapsedTimer m_groupClock;
};
} // namespace DataProtocols

