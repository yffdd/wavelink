#include "ProtocolChecksum.h"
#include <QStringList>

namespace DataProtocols {
const QVector<ChecksumPreset> &ProtocolChecksum::presets() {
    // Exact parameters / independent "123456789" check values are listed at
    // https://reveng.sourceforge.io/crc-catalogue/all.htm . CRC field byte
    // order is a separate wire-format setting, not implied by reflection.
    static const QVector<ChecksumPreset> values{
        {"none", "无校验", "不检查报文完整性。", 0, 0, 0, 0, false},
        {"sum8", "SUM8", "对指定字节求和，保留低 8 位。", 1, 0, 0, 0, false},
        {"xor8", "XOR8", "对指定字节逐个异或，初值 0。", 1, 0, 0, 0, false},
        {"crc16-modbus", "CRC16/MODBUS", "Poly=8005, Init=FFFF, RefIn/RefOut=true, XorOut=0000", 2, 0xa001, 0xffff, 0, true},
        {"crc8-smbus", "CRC8/SMBUS", "Poly=07, Init=00, RefIn/RefOut=false, XorOut=00", 1, 0x07, 0, 0, false},
        {"crc16-ccitt-false", "CRC16/CCITT-FALSE (IBM-3740)", "Poly=1021, Init=FFFF, RefIn/RefOut=false, XorOut=0000", 2, 0x1021, 0xffff, 0, false},
        {"crc16-xmodem", "CRC16/XMODEM", "Poly=1021, Init=0000, RefIn/RefOut=false, XorOut=0000", 2, 0x1021, 0, 0, false},
        {"crc32-iso-hdlc", "CRC32/ISO-HDLC", "Poly=04C11DB7, Init=FFFFFFFF, RefIn/RefOut=true, XorOut=FFFFFFFF", 4, 0xedb88320u, 0xffffffffu, 0xffffffffu, true}
    };
    return values;
}
int ProtocolChecksum::width(const QString &algorithm) {
    for (const auto &p : presets()) if (p.id == algorithm) return p.bytes;
    return -1;
}
QString ProtocolChecksum::description(const QString &algorithm) {
    for (const auto &p : presets()) if (p.id == algorithm) return p.parameters;
    return {};
}
QString ProtocolChecksum::cExample(const QString &algorithm) {
    for (const auto &p : presets()) if (p.id == algorithm) {
        const QString function = "wavelink_" + QString(p.id).replace('-', '_');
        QString code = "/* C99 reference; data is the selected checksum range only. */\n#include <stdint.h>\n#include <stddef.h>\n\nuint32_t " + function + "(const uint8_t *data, size_t length)\n{\n";
        if (p.id == "none") return code + "    (void)data; (void)length;\n    return 0; /* No checksum field is sent. */\n}\n";
        if (p.id == "sum8" || p.id == "xor8") return code + "    uint32_t value = 0;\n    for (size_t i = 0; i < length; ++i)\n        value "
            + (p.id == "sum8" ? "= (value + data[i]) & UINT32_C(0xFF);" : "^= data[i];") + "\n    return value;\n}\n";
        const quint32 mask = p.bytes == 4 ? 0xffffffffu : (quint32(1) << (p.bytes * 8)) - 1;
        auto hex = [](quint32 v) { return QString("UINT32_C(0x%1)").arg(QString::number(v, 16).toUpper()); };
        code += "    uint32_t crc = " + hex(p.initial) + ";\n    for (size_t i = 0; i < length; ++i) {\n        crc ^= (uint32_t)data[i]" + (p.reflected ? QString() : QString(" << %1").arg(p.bytes * 8 - 8)) + ";\n        for (unsigned bit = 0; bit < 8; ++bit)\n";
        code += p.reflected ? "            crc = (crc & 1u) ? (crc >> 1) ^ " + hex(p.polynomial) + " : crc >> 1;\n"
            : "            crc = ((crc & " + hex(quint32(1) << (p.bytes * 8 - 1)) + ") ? (crc << 1) ^ " + hex(p.polynomial) + " : crc << 1) & " + hex(mask) + ";\n";
        return code + "    }\n    return (crc ^ " + hex(p.xorOut) + ") & " + hex(mask) + ";\n}\n";
    }
    return {};
}
QString ProtocolChecksum::detailedHelp(const QString &algorithm) {
    const QStringList use{
        "不生成校验字段，只按帧格式筛选。适合设备协议没有校验的情况，不能检测有效格式内的数据损坏。",
        "把所选范围的字节累加并取低 8 位。实现简单，但字节交换或相互抵消的错误可能漏检。",
        "把所选范围的字节逐个异或。实现简单，但不能可靠检测字节顺序变化或相互抵消的错误。",
        "Modbus RTU 使用的 16 位 CRC，右移/反射实现；通常在线上先发低字节，再发高字节。",
        "SMBus 的 8 位 CRC，也称 PEC；左移、非反射实现。请按设备协议确定是否包含地址/命令等字节。",
        "非反射 16 位 CRC，初值 FFFF；标准名称 IBM-3740。CCITT 是多项式族，不能仅凭名称猜测初值或反射参数。",
        "非反射 16 位 CRC，初值 0000；与 CCITT-FALSE 的多项式相同，但初值不同，结果不同。",
        "反射 32 位 CRC，初值及结果异或均为 FFFFFFFF；常见于 ZIP、以太网等，但校验范围及线上排列仍由设备协议决定。"};
    const QStringList checks{"0（不发送校验字段）", "DD", "31", "4B37", "F4", "29B1", "31C3", "CBF43926"};
    const auto &all = presets();
    for (int i = 0; i < all.size(); ++i) if (all[i].id == algorithm) {
        const auto &p = all[i];
        QString html = "<h2>" + p.name.toHtmlEscaped() + "</h2><p>" + use.value(i).toHtmlEscaped() + "</p><p><b>参数：</b>" + p.parameters.toHtmlEscaped()
            + QString("<br><b>校验字段：</b>%1 字节</p>").arg(p.bytes);
        html += "<p><b>标准自检输入：</b>ASCII <code>123456789</code>，即 <code>31 32 33 34 35 36 37 38 39</code>（不含字符串结尾 NUL、帧头/帧尾）。<br><b>返回整数（HEX）：</b><code>" + checks.value(i) + "</code></p>";
        html += "<h3>配置与发送注意事项</h3><p>校验范围从 0 起计；不含校验字段本身，是否包含帧头/长度由设备协议决定。RefIn/RefOut 描述 CRC 位处理方向，与校验字段的字节大小端不同。下面函数返回整数，不负责拼帧或追加字节。</p>"
            "<p>多字节结果的小端发送：<code>out[i] = (uint8_t)(crc &gt;&gt; (8 * i));</code>；大端发送：<code>out[i] = (uint8_t)(crc &gt;&gt; (8 * (bytes - 1 - i)));</code>。bytes 为校验字段字节数。输入指针在 length &gt; 0 时必须有效；每帧重新初始化，不把上一帧的结果当初值。</p>"
            "<p>CRC/SUM/XOR 用于检测通信差错，不用于鉴权或抵御恶意篡改。仅凭 CRC 名称不能确定全部参数，请与设备协议逐项匹配。</p><h3>C99 参考实现</h3><p>可复制使用；软件测试会实际编译此代码，并核对标准向量与软件解析器的结果。</p><pre>";
        html += cExample(algorithm).toHtmlEscaped() + "</pre><p>参数与自检向量参考：<a href=\"https://reveng.sourceforge.io/crc-catalogue/all.htm\">CRC RevEng 参数目录</a>。Modbus 的帧格式、校验范围及线序参考设备及 Modbus 串行规范。此代码为本项目的独立参考实现，不是标准原文摘录。</p>";
        return html;
    }
    return {};
}
ProtocolChecksum::ProtocolChecksum(const QString &algorithm) {
    for (const auto &p : presets()) if (p.id == algorithm) {
        m_width = p.bytes; m_initial = p.initial; m_xorOut = p.xorOut; m_reflected = p.reflected;
        m_kind = p.id == "none" ? 0 : p.id == "sum8" ? 1 : p.id == "xor8" ? 2 : 3;
        m_mask = m_width == 4 ? 0xffffffffu : m_width ? (quint32(1) << (m_width * 8)) - 1 : 0;
        if (m_kind == 3) for (int i = 0; i < 256; ++i) {
            quint32 value = m_reflected ? quint32(i) : quint32(i) << (m_width * 8 - 8);
            const quint32 top = quint32(1) << (m_width * 8 - 1);
            for (int bit = 0; bit < 8; ++bit)
                value = m_reflected ? (value & 1 ? (value >> 1) ^ p.polynomial : value >> 1)
                    : (value & top ? (value << 1) ^ p.polynomial : value << 1);
            m_table[size_t(i)] = value & m_mask;
        }
        break;
    }
}
quint32 ProtocolChecksum::calculate(const char *data, int length) const {
    quint32 value = m_initial;
    for (int i = 0; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(data[i]);
        if (m_kind == 1) value = (value + byte) & 0xff;
        else if (m_kind == 2) value ^= byte;
        else if (m_kind == 3) value = m_reflected ? (value >> 8) ^ m_table[(value ^ byte) & 0xff]
            : ((value << 8) ^ m_table[((value >> (m_width * 8 - 8)) ^ byte) & 0xff]) & m_mask;
    }
    return (value ^ m_xorOut) & m_mask;
}
} // namespace DataProtocols
