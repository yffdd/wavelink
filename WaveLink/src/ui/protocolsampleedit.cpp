#include "protocolsampleedit.h"
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QTextCursor>

ProtocolSampleEdit::ProtocolSampleEdit(QWidget *parent) : QPlainTextEdit(parent) {
    connect(this, &QPlainTextEdit::textChanged, this, &ProtocolSampleEdit::normalize);
}
bool ProtocolSampleEdit::digits(const QString &text, QString &result) {
    QString value; value.reserve(text.size());
    for (QChar ch : text) {
        if (ch.isSpace()) continue;
        if (ch >= 'a' && ch <= 'f') ch = QChar(ch.unicode() - ('a' - 'A'));
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) return false;
        value += ch;
    }
    if (value.size() > 2 * 256 * 1024) return false;
    result = value; return true;
}
int ProtocolSampleEdit::digitPosition(const QString &text, int position) {
    int count = 0; for (int i = 0; i < qMin(position, text.size()); ++i) if (!text[i].isSpace()) ++count; return count;
}
QString ProtocolSampleEdit::format(const QString &value) const {
    if (!m_spaced) return value;
    QString result; result.reserve(value.size() * 3 / 2);
    for (int i = 0; i < value.size(); ++i) { if (i && i % 2 == 0) result += ' '; result += value[i]; } return result;
}
void ProtocolSampleEdit::replaceHex(const QString &value, int position, bool joinPrevious) {
    const auto displayed = format(value); m_normalizing = true;
    auto cursor = textCursor();
    if (joinPrevious) cursor.joinPreviousEditBlock(); else cursor.beginEditBlock();
    cursor.select(QTextCursor::Document); cursor.insertText(displayed); cursor.endEditBlock();
    const int rawPosition = qBound(0, position, value.size());
    cursor.setPosition(rawPosition + (m_spaced && rawPosition > 0 ? (rawPosition - 1) / 2 : 0)); setTextCursor(cursor);
    m_valid = displayed; m_normalizing = false;
}
void ProtocolSampleEdit::normalize() {
    if (m_normalizing || !m_hex) return;
    const auto text = toPlainText(); QString value;
    if (!digits(text, value)) {
        const auto previous = m_valid; QString old; digits(previous, old); replaceHex(old, old.size(), true); emit invalidInput(); return;
    }
    if (format(value) != text) replaceHex(value, digitPosition(text, textCursor().position()), true);
    else m_valid = text;
}
void ProtocolSampleEdit::setHexMode(bool hex) {
    m_hex = hex;
    if (hex) { m_valid.clear(); normalize(); }
}
void ProtocolSampleEdit::setSpacedHex(bool spaced) {
    if (m_spaced == spaced) return; m_spaced = spaced;
    if (m_hex) normalize();
}
void ProtocolSampleEdit::insertHex(const QString &text) {
    QString incoming, value;
    if (!digits(text, incoming) || !digits(toPlainText(), value)) { emit invalidInput(); return; }
    if (incoming.isEmpty()) return; // Space/newline separators are automatic.
    const auto cursor = textCursor(); const auto displayed = toPlainText();
    const int start = digitPosition(displayed, cursor.selectionStart()), end = digitPosition(displayed, cursor.selectionEnd());
    if (value.size() - (end - start) + incoming.size() > 2 * 256 * 1024) { emit invalidInput(); return; }
    value.replace(start, end - start, incoming); replaceHex(value, start + incoming.size());
}
void ProtocolSampleEdit::keyPressEvent(QKeyEvent *event) {
    if (!m_hex || isReadOnly()) { QPlainTextEdit::keyPressEvent(event); return; }
    if (event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo) || event->matches(QKeySequence::Copy)
        || event->matches(QKeySequence::Paste) || event->matches(QKeySequence::SelectAll) || event->matches(QKeySequence::Cut)) {
        QPlainTextEdit::keyPressEvent(event); return;
    }
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        QString value; digits(toPlainText(), value); const auto cursor = textCursor();
        int start = digitPosition(toPlainText(), cursor.selectionStart()), end = digitPosition(toPlainText(), cursor.selectionEnd());
        if (start == end) { if (event->key() == Qt::Key_Backspace && start > 0) --start; else if (event->key() == Qt::Key_Delete && end < value.size()) ++end; }
        if (end > start) { value.remove(start, end - start); replaceHex(value, start); } event->accept(); return;
    }
    if (!event->text().isEmpty() && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) { insertHex(event->text()); event->accept(); return; }
    QPlainTextEdit::keyPressEvent(event);
}
void ProtocolSampleEdit::inputMethodEvent(QInputMethodEvent *event) {
    if (!m_hex || isReadOnly()) { QPlainTextEdit::inputMethodEvent(event); return; }
    if (!event->commitString().isEmpty()) insertHex(event->commitString()); event->accept();
}
bool ProtocolSampleEdit::canInsertFromMimeData(const QMimeData *source) const {
    if (!m_hex) return QPlainTextEdit::canInsertFromMimeData(source);
    QString value; return source->hasText() && digits(source->text(), value);
}
void ProtocolSampleEdit::insertFromMimeData(const QMimeData *source) {
    if (!m_hex) QPlainTextEdit::insertFromMimeData(source);
    else if (source->hasText() && !isReadOnly()) insertHex(source->text());
}
QString ProtocolSampleEdit::displayBytes(const QByteArray &bytes, bool hex, bool spaced, bool escapeAll) {
    if (hex) return QString::fromLatin1(spaced ? bytes.toHex(' ').toUpper() : bytes.toHex().toUpper());
    QString result; result.reserve(bytes.size() * 4);
    static const char hexDigits[] = "0123456789ABCDEF";
    // Binary protocols use one explicit \xHH per byte, including printable
    // ASCII and CR/LF. Text protocols keep their readable text/short escapes.
    // Both representations decode to the exact same original bytes.
    for (unsigned char byte : bytes) {
        if (escapeAll) { result += "\\x"; result += QLatin1Char(hexDigits[byte >> 4]); result += QLatin1Char(hexDigits[byte & 0x0f]); }
        else if (byte == '\r') result += "\\r"; else if (byte == '\n') result += "\\n"; else if (byte == '\t') result += "\\t";
        else if (byte == '\\') result += "\\\\";
        else if (byte >= 0x20 && byte < 0x7f) result += QChar(byte);
        else result += QString("\\x%1").arg(byte, 2, 16, QChar('0')).toUpper().replace("\\X", "\\x");
    } return result;
}
bool ProtocolSampleEdit::textBytes(const QString &text, QByteArray &bytes, QString &error) {
    QByteArray result; QString pending;
    auto flush = [&] { result += pending.toUtf8(); pending.clear(); };
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) { pending += text[i]; continue; }
        const auto next = text[i + 1];
        if (next == 'r' || next == 'n' || next == 't' || next == '\\') { flush(); result += next == 'r' ? '\r' : next == 'n' ? '\n' : next == 't' ? '\t' : '\\'; ++i; }
        else if (next == 'x') {
            QString value;
            if (i + 3 >= text.size() || !digits(text.mid(i + 2, 2), value) || value.size() != 2) { error = "\\x 后必须是两位十六进制字符，例如 \\x00。"; return false; }
            flush(); result += char(value.toUInt(nullptr, 16)); i += 3;
        } else pending += text[i]; // Preserve legacy unrecognized escapes.
        if (result.size() + pending.size() > 256 * 1024) { error = "样例超过 256 KiB。"; return false; }
    }
    flush(); if (result.size() > 256 * 1024) { error = "样例超过 256 KiB。"; return false; }
    bytes = result; error.clear(); return true;
}
