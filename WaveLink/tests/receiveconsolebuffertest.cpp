#include "receiveconsolebuffer.h"
#include <QApplication>
#include <QPlainTextEdit>
#include <QElapsedTimer>
#include "systempreferences.h"
#include <QTemporaryDir>
#include <iostream>
using namespace ReceiveConsoleBuffer;
#define VERIFY(c) do {if (!(c)) {std::cerr << "Receive cache failure at " << __LINE__ << ": " #c "\n"; return 1;}} while (0)
static void append(QTextDocument &document, const QString &text, bool hex = false) {
    QTextCursor cursor(&document); cursor.movePosition(QTextCursor::End); cursor.insertText(text);
    trimDocument(&document, hex);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    VERIFY(validLines(0) == 1000 && validLines(10000) == 5000 && validLines(1000) == 1000 && validLines(2000) == 2000);
    for (int lines : {1000, 2000, 5000}) {
        QTextDocument document; document.setMaximumBlockCount(lines);
        QString input; for (int i = 0; i < lines + 5; ++i) input += QString("数据%1\n").arg(i);
        append(document, input);
        QString pending = input; trimPending(pending, false, lines);
        VERIFY(pending == document.toPlainText());
        VERIFY(document.blockCount() == lines && document.toPlainText().startsWith("数据6\n") &&
               document.toPlainText().endsWith(QString("数据%1\n").arg(lines + 4)));
        append(document, "最新数据\n");
        VERIFY(document.blockCount() == lines && document.toPlainText().startsWith("数据7\n") && document.toPlainText().endsWith("最新数据\n"));
    }
    QTextDocument document;
    append(document, QString(60000, 'A')); append(document, "最新");
    VERIFY(document.toPlainText() == QString(DefaultCharacters - 2, 'A') + "最新"); // Trim oldest, never clear all.
    document.setPlainText(QString(DefaultCharacters - 20, 'A'));
    append(document, QString(30, 'B'));
    VERIFY(document.toPlainText() == QString(DefaultCharacters - 30, 'A') + QString(30, 'B'));
    const auto emoji = QString::fromUtf8("😀");
    QString pending = "A" + emoji + QString(DefaultCharacters - 1, 'Z');
    trimPending(pending, false); VERIFY(pending == QString(DefaultCharacters - 1, 'Z'));
    document.setPlainText("A" + emoji + QString(DefaultCharacters - 1, 'Z'));
    trimDocument(&document, false); VERIFY(document.toPlainText() == pending);
    QString hex = QString("AA ").repeated(DefaultCharacters / 3 + 40);
    trimPending(hex, true);
    VERIFY(hex.size() <= DefaultCharacters && hex.startsWith("AA ") && hex.endsWith("AA ") && hex.size() % 3 == 0);
    document.setPlainText(QString("AA ").repeated(DefaultCharacters / 3 + 40));
    trimDocument(&document, true); VERIFY(document.toPlainText() == hex);
    pending = QString(DefaultCharacters + 50000, 'C'); trimPending(pending, false);
    VERIFY(pending.size() == DefaultCharacters && pending == QString(DefaultCharacters, 'C'));
    document.setPlainText(QString(DefaultCharacters + 50000, 'C')); trimDocument(&document, false);
    VERIFY(document.toPlainText() == pending);
    for (int limit : {MinCharacters, DefaultCharacters, MaxCharacters}) {
        pending = QString(limit + 10000, 'D'); trimPending(pending, false, 1000, limit);
        VERIFY(pending.size() == limit);
        document.setPlainText(QString(limit + 10000, 'D')); trimDocument(&document, false, limit);
        VERIFY(document.toPlainText() == pending);
        trimDocument(&document, false, 0); VERIFY(document.isEmpty());
    }
    QTemporaryDir folder; QSettings settings(folder.filePath("preferences.ini"), QSettings::IniFormat);
    auto preferences = SystemPreferences::read(settings);
    VERIFY(preferences.receiveLines == 1000 && preferences.receiveCharacters == 50000 && preferences.receiveRefreshMs == 100 && preferences.plotFps == 30 && preferences.defaultBottomPercent == 30);
    preferences.receiveLines = 2000; preferences.receiveCharacters = 30000; preferences.receiveRefreshMs = 200; preferences.plotFps = 20; preferences.defaultBottomPercent = 40; preferences.write(settings);
    preferences = SystemPreferences::read(settings);
    VERIFY(preferences.receiveLines == 2000 && preferences.receiveCharacters == 30000 && preferences.receiveRefreshMs == 200 && preferences.plotFps == 20 && preferences.defaultBottomPercent == 40);
    VERIFY(validCharacters(65536) == 50000 && validCharacters(8192) == 10000 && validCharacters(262144) == 250000 && validCharacters(32768) == 30000 && validCharacters(12345) == 10000);
    settings.setValue("Console/ReceiveMaxCharacters", 1000000); settings.setValue("Console/ReceiveRefreshMs", -1); settings.setValue("Display/PlotFps", 10000);
    preferences = SystemPreferences::read(settings);
    VERIFY(preferences.receiveCharacters == 50000 && preferences.receiveRefreshMs == 100 && preferences.plotFps == 30);
    for (int value : {0, -1, 19, 51, 100}) { settings.setValue("Layout/DefaultBottomPercent", value); VERIFY(SystemPreferences::read(settings).defaultBottomPercent == 30); }
    VERIFY(SystemPreferences::validBottomPercent(20) == 20 && SystemPreferences::validBottomPercent(50) == 50);
    // One minute's worth of 15-channel JustFloat HEX at 500 Hz, delivered in
    // 100 ms display batches (600 x 50 frames). No wall-clock sleep/radio.
    QPlainTextEdit console; console.setReadOnly(true); console.setUndoRedoEnabled(false); console.setMaximumBlockCount(DefaultLines);
    console.resize(850, 100); console.show(); app.processEvents();
    const QString batch = QString::fromLatin1(QByteArray(64 * 50, char(0xAA)).toHex(' ').toUpper()) + " ";
    QElapsedTimer performance; performance.start();
    qint64 worstUs = 0, earlyUs = 0, lateUs = 0;
    for (int i = 0; i < 600; ++i) {
        QElapsedTimer update; update.start(); QString incoming = batch; trimPending(incoming, true, DefaultLines, DefaultCharacters);
        QTextCursor cursor(console.document()); cursor.beginEditBlock();
        trimDocument(console.document(), true, qMax(0, DefaultCharacters - incoming.size()));
        cursor.movePosition(QTextCursor::End); cursor.insertText(incoming); cursor.endEditBlock();
        console.setTextCursor(cursor); console.ensureCursorVisible(); app.processEvents();
        const auto elapsed = update.nsecsElapsed() / 1000; worstUs = qMax(worstUs, elapsed);
        if (i >= 100 && i < 200) earlyUs += elapsed; if (i >= 500) lateUs += elapsed;
        VERIFY(console.document()->characterCount() - 1 <= DefaultCharacters);
    }
    VERIFY(console.toPlainText().endsWith("AA ") && console.toPlainText().startsWith("AA "));
    std::cout << "60-second-equivalent 500Hz/15-channel HEX display: " << performance.elapsed() << " ms total, worst update " << worstUs / 1000.0
              << " ms, early/late average " << earlyUs / 100000.0 << "/" << lateUs / 100000.0 << " ms; bounded to 50000 characters.\n";
    std::cout << "Receive cache: rolling lines/characters, huge single lines, Unicode boundaries and HEX byte alignment passed.\n";
    return 0;
}
