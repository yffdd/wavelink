#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

namespace BleIpc {
constexpr quint32 MaxPayload = 1024 * 1024;
inline QByteArray encode(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray frame(4, '\0');
    qToLittleEndian<quint32>(quint32(payload.size()), reinterpret_cast<uchar *>(frame.data()));
    return frame + payload;
}
// Parse incrementally. A malformed frame is terminal; never attempt to resync
// to arbitrary bytes in a control/data stream.
inline bool take(QByteArray &buffer, QJsonObject &object, QString &error)
{
    error.clear();
    if (buffer.size() < 4) return false;
    const quint32 length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
    if (!length || length > MaxPayload) {
        error = QStringLiteral("BLE 通信消息长度无效。"); return false;
    }
    if (quint32(buffer.size() - 4) < length) return false;
    QJsonParseError parseError;
    auto document = QJsonDocument::fromJson(buffer.mid(4, int(length)), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("BLE 通信消息格式无效。"); return false;
    }
    buffer.remove(0, int(length) + 4);
    object = document.object();
    return true;
}
}
