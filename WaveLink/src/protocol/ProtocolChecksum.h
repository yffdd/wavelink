#pragma once
#include <QString>
#include <QVector>
#include <array>

namespace DataProtocols {
struct ChecksumPreset {
    QString id, name, parameters;
    int bytes;
    quint32 polynomial, initial, xorOut;
    bool reflected;
};

// Compile once on protocol application. No JSON/string dispatch per byte.
class ProtocolChecksum {
public:
    explicit ProtocolChecksum(const QString &algorithm = QString());
    static const QVector<ChecksumPreset> &presets();
    static int width(const QString &algorithm);
    static QString description(const QString &algorithm);
    static QString cExample(const QString &algorithm);
    static QString detailedHelp(const QString &algorithm);
    int width() const { return m_width; }
    quint32 calculate(const char *data, int length) const;
private:
    int m_kind = 0, m_width = 0; // 0=none, 1=sum, 2=xor, 3=CRC
    quint32 m_mask = 0, m_initial = 0, m_xorOut = 0;
    bool m_reflected = false;
    std::array<quint32, 256> m_table{};
};
} // namespace DataProtocols
