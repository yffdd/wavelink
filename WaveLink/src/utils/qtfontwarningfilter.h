#pragma once

#include <QRegularExpression>
#include <QString>
#include <QtGlobal>
#include <atomic>

// Qt can try many fallback fonts for one unsupported script. Reporting every
// attempt makes the log viewer and its synchronous disk writes unnecessarily
// expensive. This gate affects logging only, never text decoding or data paths.
class QtFontWarningFilter {
public:
    QString messageForLog(QtMsgType type, const QString &message) {
        if (type != QtWarningMsg) return message;
        const QString trimmed = message.trimmed();
        if (!trimmed.startsWith(QLatin1String("OpenType support missing for \""))) return message;
        static const QRegularExpression fontWarning(
            QStringLiteral("^OpenType support missing for \"[^\"]*\", script [0-9]+$"));
        if (!fontWarning.match(trimmed).hasMatch()) return message;
        if (m_reported.exchange(true, std::memory_order_relaxed)) return {};
        return QStringLiteral(
            "字体显示兼容提示：部分特殊字符可能无法正常显示。"
            "如果接收的是二进制数据，请使用“HEX 接收”；文本数据请核对编码。"
            "本次运行仅记录一次此类字体提示，其他警告仍正常记录。原始提示：%1").arg(trimmed);
    }

private:
    std::atomic<bool> m_reported {false};
};
