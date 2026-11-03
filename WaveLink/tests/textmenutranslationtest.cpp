#include "chinesetextmenus.h"
#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QLabel>
#include <QContextMenuEvent>
#include <QTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>
#include <QDebug>
#include <memory>

#define CHECK(c) do { if (!(c)) { qCritical() << "Text menu failure" << __LINE__ << #c; return 1; } } while (0)
static QAction *find(QMenu *menu, const QString &label) {
    for (auto action : menu->actions()) if (action->text().startsWith(label)) return action;
    return nullptr;
}
static bool chineseMenu(QMenu *menu) {
    for (auto action : menu->actions()) {
        if (action->isSeparator()) continue;
        bool chinese = false;
        for (auto c : action->text()) if (c.unicode() >= 0x4e00 && c.unicode() <= 0x9fff) chinese = true;
        if (!chinese || (action->menu() && !chineseMenu(action->menu()))) {
            qCritical() << "Untranslated menu item:" << action->text(); return false;
        }
    }
    return true;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int fontId = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (fontId >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first(), 9));
    installChineseTextMenus(app); installChineseTextMenus(app);
    CHECK(app.findChildren<QTranslator *>("waveLinkChineseTextMenus").size() == 1);
    for (bool readOnly : {false, true}) {
        QLineEdit line; line.setText("温度 25.34"); line.setReadOnly(readOnly); line.selectAll();
        std::unique_ptr<QMenu> menu(line.createStandardContextMenu());
        CHECK(chineseMenu(menu.get()) && find(menu.get(), "复制") && find(menu.get(), "全选"));
        find(menu.get(), "复制")->trigger(); CHECK(app.clipboard()->text() == line.text());
        if (!readOnly) {
            app.clipboard()->setText("粘贴测试"); find(menu.get(), "粘贴")->trigger();
            CHECK(line.text() == "粘贴测试"); line.undo(); CHECK(line.text() == "温度 25.34");
        }
        QTextEdit rich; rich.setPlainText("多行\n说明"); rich.setReadOnly(readOnly); rich.selectAll();
        std::unique_ptr<QMenu> richMenu(rich.createStandardContextMenu());
        CHECK(chineseMenu(richMenu.get()) && find(richMenu.get(), "复制"));
        find(richMenu.get(), "复制")->trigger(); CHECK(app.clipboard()->text() == rich.toPlainText());
        QPlainTextEdit plain; plain.setPlainText("接收文本\nACK OK"); plain.setReadOnly(readOnly); plain.selectAll();
        std::unique_ptr<QMenu> plainMenu(plain.createStandardContextMenu());
        CHECK(chineseMenu(plainMenu.get()) && find(plainMenu.get(), "复制"));
        find(plainMenu.get(), "复制")->trigger(); CHECK(app.clipboard()->text() == plain.toPlainText());
    }
    QTextBrowser browser; browser.resize(600, 300);
    browser.setHtml("<a href='https://github.com/yffdd/wavelink/releases'>GitHub 下载</a>");
    browser.show(); app.processEvents();
    auto cursor = browser.textCursor(); cursor.setPosition(3);
    std::unique_ptr<QMenu> linkMenu(browser.createStandardContextMenu(browser.cursorRect(cursor).center()));
    CHECK(chineseMenu(linkMenu.get()) && find(linkMenu.get(), "复制链接地址"));
    find(linkMenu.get(), "复制链接地址")->trigger();
    CHECK(app.clipboard()->text() == "https://github.com/yffdd/wavelink/releases");
    // Version information uses selectable QLabel text rather than a text editor.
    QLabel label("版本信息 WaveLink v0.13"); label.setTextInteractionFlags(Qt::TextBrowserInteraction);
    label.show(); app.processEvents(); label.setFocus(); label.setSelection(0, 4);
    bool labelMenuChinese = false;
    QTimer::singleShot(30, &label, [&] {
        auto menu = qobject_cast<QMenu *>(app.activePopupWidget());
        if (!menu) return;
        auto copy = find(menu, "复制"); labelMenuChinese = chineseMenu(menu) && copy && copy->isEnabled();
        if (copy) copy->trigger(); menu->close();
    });
    QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, QPoint(10, 10), label.mapToGlobal(QPoint(10, 10)));
    QApplication::sendEvent(&label, &contextEvent);
    QEventLoop popupEvents; QTimer::singleShot(80, &popupEvents, &QEventLoop::quit); popupEvents.exec();
    if (!labelMenuChinese || app.clipboard()->text() != "版本信息")
        qCritical() << "Label menu:" << labelMenuChinese << "selection:" << label.selectedText() << "clipboard:" << app.clipboard()->text();
    CHECK(labelMenuChinese && app.clipboard()->text() == "版本信息");
    CHECK(QCoreApplication::translate("UserCommands", "Copy") == "Copy");
    qInfo() << "PASS: Chinese editable/read-only text menus and link copying; clipboard, paste, undo unchanged";
}
