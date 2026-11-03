#include "ProtocolParser.h"
#include <QtEndian>
#include <QStringList>
#include <QDebug>

bool ProtocolParser::isTextNaN(const QString &token)
{
    const bool signedToken = token.size() == 4 && (token[0] == '+' || token[0] == '-');
    return token.midRef(signedToken ? 1 : 0).compare(QLatin1String("NaN"), Qt::CaseInsensitive) == 0;
}

// --- RawParser ---
int RawParser::processData(const char *data, int size)
{
    if (size <= 0) return 0;
    
    // SerialHandler emits dataReceived for all incoming data now.
    return size;
}

// --- FireWaterParser ---
int FireWaterParser::processData(const char *data, int size)
{
    int begin = 0;
    for (int i = 0; i < size; ++i) {
        if (data[i] != '\n') continue;
        
        int end = i;
        int frameLength = end - begin + 1;
        QByteArray line(data + begin, frameLength);
        
        // Parse sampling data
        QString strLine = QString::fromUtf8(line).trimmed();
        if (!strLine.isEmpty() && !strLine.startsWith("image:")) {
            // Remove prefix if exists
            int colonIndex = strLine.indexOf(':');
            if (colonIndex != -1) {
                strLine = strLine.mid(colonIndex + 1);
            }
            
            QStringList parts = strLine.split(',');
            QVector<double> channels;
            bool hasNumericValue = false;
            for (const QString &p : parts) {
                bool ok = true;
                const auto token = p.trimmed();
                const double val = isTextNaN(token) ? qQNaN() : token.toDouble(&ok);
                if (ok) {
                    hasNumericValue = true;
                    channels.append(val);
                } else {
                    channels.append(qQNaN());
                }
            }
            
            if (hasNumericValue) {
                publishChannels(channels, channels);
            }
        }
        
        begin = end + 1;
    }
    return begin;
}

// --- JustFloatParser ---
int JustFloatParser::processData(const char *data, int size)
{
    int begin = 0;
    for (int i = 3; i < size; ++i) {
        unsigned char b0 = static_cast<unsigned char>(data[i-3]);
        unsigned char b1 = static_cast<unsigned char>(data[i-2]);
        unsigned char b2 = static_cast<unsigned char>(data[i-1]);
        unsigned char b3 = static_cast<unsigned char>(data[i]);
        
        bool isTail = false;
        // Only the configured endian's standard tail is a delimiter. Other
        // byte patterns can be ordinary finite float payloads.
        if (m_isBigEndian)
            isTail = b0 == 0x7F && b1 == 0x80 && b2 == 0x00 && b3 == 0x00;
        else
            isTail = b0 == 0x00 && b1 == 0x00 && b2 == 0x80 && b3 == 0x7F;
        
        if (!isTail) continue;
        
        int end = i;
        int frameLength = end - begin + 1;
        QByteArray frame(data + begin, frameLength);
        
        int payloadLength = frameLength - 4;
        if (payloadLength > 0) {
            // Ignore any garbage bytes at the start of the frame so we align perfectly to 4-byte floats
            int validPayloadLength = (payloadLength / 4) * 4;
            int garbageOffset = payloadLength - validPayloadLength;
            int numChannels = validPayloadLength / 4;
            
            // Robustness: wait for 2 consecutive frames with same channel count to avoid glitches
            if (numChannels == m_expectedChannels) {
                m_consecutiveCount++;
            } else {
                m_expectedChannels = numChannels;
                m_consecutiveCount = 1;
            }
            
            if (m_consecutiveCount >= 2) {
                QVector<double> channels(numChannels);
                
                for (int j = 0; j < numChannels; ++j) {
                    float val;
                    // data + begin + garbageOffset ensures we read the floats immediately preceding the tail
                    memcpy(&val, data + begin + garbageOffset + j * 4, 4);
                    
                    if (m_isBigEndian) {
                        val = qFromBigEndian<float>(val);
                    }
                    
                    channels[j] = val; // Convert float to double
                }
                
                publishChannels(channels, channels);
            }
        }
        
        begin = end + 1;
    }
    return begin;
}

// AMU-4 Parser
int AMU4Parser::processData(const char *data, int size)
{
    m_buffer.append(data, size);

    static const int kMaxAmuBuffer = 1024 * 1024;
    if (m_buffer.size() > kMaxAmuBuffer) {
        emit bufferOverflow();
        // Preserve only a small suffix in case it contains the beginning of a
        // delimiter split across reads. This parser only needs CRLF framing.
        m_buffer = m_buffer.right(1);
        m_currentFrame = QVector<double>(3, 0.0);
        m_receivedChannels[0] = false;
        m_receivedChannels[1] = false;
        m_receivedChannels[2] = false;
    }

    while (true) {
        int newlineIdx = m_buffer.indexOf("\r\n");
        if (newlineIdx == -1) break;

        QByteArray line = m_buffer.left(newlineIdx);
        m_buffer.remove(0, newlineIdx + 2);

        int colonIdx = line.indexOf(':');
        if (colonIdx != -1) {
            bool okCh, okVal = true;
            int ch = line.left(colonIdx).toInt(&okCh);
            const auto token = QString::fromLatin1(line.mid(colonIdx + 1)).trimmed();
            const double val = isTextNaN(token) ? qQNaN() : token.toDouble(&okVal);

            if (okCh && okVal && ch >= 0 && ch < 3) {
                if (m_currentFrame.size() != 3) {
                    m_currentFrame = QVector<double>(3, 0.0);
                }
                m_currentFrame[ch] = val;
                m_receivedChannels[ch] = true;

                if (m_receivedChannels[0] && m_receivedChannels[1] && m_receivedChannels[2]) {
                    publishChannels(m_currentFrame, m_currentFrame);
                    m_currentFrame = QVector<double>(3, 0.0);
                    m_receivedChannels[0] = false;
                    m_receivedChannels[1] = false;
                    m_receivedChannels[2] = false;
                }
            }
        }
    }
    return size;
}
