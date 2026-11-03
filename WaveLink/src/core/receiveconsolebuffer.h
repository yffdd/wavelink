#ifndef RECEIVECONSOLEBUFFER_H
#define RECEIVECONSOLEBUFFER_H

#include <QString>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

// Display-only retention. Never apply these limits to parsers or recordings.
namespace ReceiveConsoleBuffer {
constexpr int DefaultLines = 1000;
constexpr int DefaultCharacters = 50000;
constexpr int MinCharacters = 10000;
constexpr int MaxCharacters = 250000;
constexpr int CharacterUnit = 10000;
inline int validLines(int lines) {
    if (lines == 10000) return 5000; // Retire the old oversized preset.
    return lines == 1000 || lines == 2000 || lines == 5000 ? lines : DefaultLines;
}
inline int validCharacters(int characters) {
    // Migrate the old installation default to the new default. Other legacy
    // limits are rounded to whole 万字符, bounded by the new 1–25 range.
    if (characters == 65536) return DefaultCharacters;
    if (characters < 8192 || characters > 262144) return DefaultCharacters;
    return qBound(MinCharacters, ((characters + CharacterUnit / 2) / CharacterUnit) * CharacterUnit, MaxCharacters);
}
inline void trimPending(QString &text, bool hex, int lines = 0, int characters = DefaultCharacters) {
    int cut = text.size() - qMax(0, characters);
    if (cut > 0) {
        if (cut < text.size() && text.at(cut).isLowSurrogate() && text.at(cut - 1).isHighSurrogate()) ++cut;
        // HEX display uses whitespace-separated bytes: do not leave half a byte.
        if (hex && !text.at(cut - 1).isSpace()) {
            int aligned = cut;
            while (aligned < text.size() && !text.at(aligned).isSpace() && aligned - cut < 64) ++aligned;
            if (aligned < text.size() && text.at(aligned).isSpace()) cut = aligned + 1;
        }
        text.remove(0, cut);
    }
    // Avoid laying out thousands of already-obsolete lines from a large batch.
    int search = text.size() - 1;
    for (int i = 0; i < lines; ++i) {
        if (search < 0) return;
        const int newline = text.lastIndexOf('\n', search);
        if (newline < 0) return;
        if (i == lines - 1) { text.remove(0, newline + 1); return; }
        search = newline - 1;
    }
}
inline void trimDocument(QTextDocument *document, bool hex, int characters = DefaultCharacters) {
    // QTextDocument includes a final paragraph sentinel, not received data.
    const int length = document->characterCount() - 1;
    int cut = length - qMax(0, characters);
    if (cut <= 0) return;
    // Prefer complete old lines, but retain a useful suffix of very long lines.
    const auto block = document->findBlock(cut);
    if (block.isValid() && block.position() < cut && block.next().isValid() &&
        block.next().position() - cut <= 4096) cut = block.next().position();
    if (cut < length && document->characterAt(cut).isLowSurrogate() &&
        document->characterAt(cut - 1).isHighSurrogate()) ++cut;
    if (hex && cut > 0 && !document->characterAt(cut - 1).isSpace()) {
        int aligned = cut;
        while (aligned < length && !document->characterAt(aligned).isSpace() && aligned - cut < 64) ++aligned;
        if (aligned < length && document->characterAt(aligned).isSpace()) cut = aligned + 1;
    }
    QTextCursor oldest(document);
    oldest.setPosition(0);
    oldest.setPosition(cut, QTextCursor::KeepAnchor);
    oldest.removeSelectedText();
}
}
#endif
