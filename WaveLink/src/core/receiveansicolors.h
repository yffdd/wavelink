#ifndef RECEIVEANSICOLORS_H
#define RECEIVEANSICOLORS_H

#include <QString>
#include <QTextCharFormat>
#include <QVector>

// Interpret only SGR display attributes. Other escape sequences remain text.
class ReceiveAnsiColors {
public:
    void reset() {
        m_partial.clear();
        m_format = QTextCharFormat();
    }

    template<class Append>
    void parse(const QString &text, Append append) {
        QString input = text;
        if (!m_partial.isEmpty()) {
            input.prepend(m_partial);
            m_partial.clear();
        }

        constexpr int maxSequenceLength = 128;
        int start = 0;
        int escape;
        while ((escape = input.indexOf(QChar(0x1b), start)) >= 0) {
            append(input.midRef(start, escape - start), m_format);
            int end = escape + 1;
            if (end < input.size() && input.at(end) == '[') {
                const int parameters = ++end;
                while (end < input.size() && end - escape < maxSequenceLength) {
                    const auto ch = input.at(end);
                    if (ch != ';' && (ch < '0' || ch > '9')) break;
                    ++end;
                }
                if (end < input.size() && end - escape < maxSequenceLength && input.at(end) == 'm') {
                    apply(input.midRef(parameters, end - parameters));
                    start = end + 1;
                    continue;
                }
            }
            if (end == input.size() && end - escape < maxSequenceLength) {
                m_partial = input.mid(escape);
                return;
            }
            append(input.midRef(escape, 1), m_format);
            start = escape + 1;
        }
        append(input.midRef(start), m_format);
    }

private:
    static bool isByte(int value) { return value >= 0 && value <= 255; }

    static QColor indexedColor(int index) {
        static const QRgb palette[] = {
            0xff000000, 0xffcd3131, 0xff198038, 0xff986800, 0xff3366cc, 0xffa64c9e, 0xff007e88, 0xffc0c0c0,
            0xff808080, 0xfff14c4c, 0xff27a35a, 0xffbd8800, 0xff5788e6, 0xffc967c0, 0xff00a1ad, 0xffffffff
        };
        if (index < 16) return QColor::fromRgb(palette[index]);
        if (index >= 232) {
            const int gray = 8 + (index - 232) * 10;
            return QColor(gray, gray, gray);
        }
        static const int levels[] = {0, 95, 135, 175, 215, 255};
        const int cube = index - 16;
        return QColor(levels[cube / 36], levels[(cube / 6) % 6], levels[cube % 6]);
    }

    void apply(const QStringRef &parameters) {
        QVector<int> codes;
        for (const auto &part : parameters.split(';')) {
            bool ok = false;
            const int code = part.toInt(&ok);
            codes.append(ok ? code : part.isEmpty() ? 0 : -1);
        }
        for (int i = 0; i < codes.size(); ++i) {
            const int code = codes[i];
            if (code == 0) m_format = QTextCharFormat();
            else if (code == 1) m_format.setFontWeight(QFont::Bold);
            else if (code == 22) m_format.clearProperty(QTextFormat::FontWeight);
            else if (code == 39) m_format.clearForeground();
            else if (code == 49) m_format.clearBackground();
            else if (code >= 30 && code <= 37) m_format.setForeground(indexedColor(code - 30));
            else if (code >= 90 && code <= 97) m_format.setForeground(indexedColor(code - 90 + 8));
            else if (code >= 40 && code <= 47) m_format.setBackground(indexedColor(code - 40));
            else if (code >= 100 && code <= 107) m_format.setBackground(indexedColor(code - 100 + 8));
            else if (code == 38 || code == 48) {
                if (++i >= codes.size()) break;
                const int mode = codes[i];
                QColor color;
                if (mode == 5) {
                    if (++i >= codes.size()) break;
                    if (isByte(codes[i])) color = indexedColor(codes[i]);
                } else if (mode == 2) {
                    if (i + 3 >= codes.size()) break;
                    const int red = codes[i + 1], green = codes[i + 2], blue = codes[i + 3];
                    if (isByte(red) && isByte(green) && isByte(blue)) color = QColor(red, green, blue);
                    i += 3;
                }
                if (!color.isValid()) continue;
                if (code == 38) m_format.setForeground(color);
                else m_format.setBackground(color);
            }
        }
    }

    QString m_partial;
    QTextCharFormat m_format;
};

#endif
