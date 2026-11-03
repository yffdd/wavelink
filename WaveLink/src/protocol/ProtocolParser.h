#ifndef PROTOCOLPARSER_H
#define PROTOCOLPARSER_H

#include <QObject>
#include <QByteArray>
#include <QVector>
#include <QStringList>

class ProtocolParser : public QObject
{
    Q_OBJECT
public:
    explicit ProtocolParser(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~ProtocolParser() = default;

    virtual int processData(const char *data, int size) = 0;
    virtual void reset() {} // New connection/client: no previous partial frame.
    // UDP packets never continue partial frames. The caller reports whether
    // processData consumed the whole datagram.
    virtual void endDatagram(bool = true) { reset(); }
    virtual QStringList channelNames() const { return {}; }
    virtual bool allowsDynamicChannelCount() const { return false; }

signals:
    // Paired, synchronous numeric source, before recording/display can discard frames.
    void monitorFrameReady(const QVector<double> &raw, const QVector<double> &converted);
    // Decoded numeric samples before display scale/bias, range-to-NaN or GUI
    // filtering, irrespective of the selected recording mode.
    void rawFrameReady(const QVector<double> &data);
    // Complete recordings use this output, independently of GUI filtering.
    void recordingFrameReady(const QVector<double> &data);
    void channelsUpdated(const QVector<double> &data);
    void textReady(const QByteArray &text);
    void bufferOverflow();
protected:
    static bool isTextNaN(const QString &token);
    bool m_recordConverted = false;
    void publishChannels(const QVector<double> &raw, const QVector<double> &display) {
        emit monitorFrameReady(raw, display);
        emit rawFrameReady(raw);
        emit recordingFrameReady(m_recordConverted ? display : raw);
        emit channelsUpdated(display);
    }
};

// RawData Parser
class RawParser : public ProtocolParser
{
    Q_OBJECT
public:
    explicit RawParser(QObject *parent = nullptr) : ProtocolParser(parent) {}
    int processData(const char *data, int size) override;
};

// FireWater Parser (CSV style)
class FireWaterParser : public ProtocolParser
{
    Q_OBJECT
public:
    explicit FireWaterParser(QObject *parent = nullptr) : ProtocolParser(parent) {}
    int processData(const char *data, int size) override;
};

// JustFloat Parser (Hex struct)
class JustFloatParser : public ProtocolParser
{
    Q_OBJECT
public:
    explicit JustFloatParser(QObject *parent = nullptr) 
        : ProtocolParser(parent), m_expectedChannels(0), m_consecutiveCount(0), m_isBigEndian(false) {}
    
    int processData(const char *data, int size) override;
    void reset() override { m_expectedChannels = 0; m_consecutiveCount = 0; }
    // The JustFloat parser owns no partial bytes. Keep channel-count validation
    // across consecutive datagrams from the same peer.
    void endDatagram(bool = true) override {}
    
    void setBigEndian(bool isBig) { m_isBigEndian = isBig; }

private:
    int m_expectedChannels;
    int m_consecutiveCount;
    bool m_isBigEndian;
};

// AMU-4 Parser
class AMU4Parser : public ProtocolParser
{
    Q_OBJECT
public:
    explicit AMU4Parser(QObject *parent = nullptr) : ProtocolParser(parent) {}
    int processData(const char *data, int size) override;
    void reset() override {
        m_buffer.clear(); m_currentFrame.clear();
        for (bool &received : m_receivedChannels) received = false;
    }
private:
    QByteArray m_buffer;
    QVector<double> m_currentFrame;
    bool m_receivedChannels[3] = {false, false, false};
};

#endif // PROTOCOLPARSER_H
