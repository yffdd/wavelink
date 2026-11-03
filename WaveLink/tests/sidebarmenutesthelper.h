#pragma once
#include <QApplication>
#include <QListWidget>
#include <QMenu>
#include <QKeyEvent>
#include <QTimer>

// Exercise the actual nested right-click menu, not just the copy slot.
// A local timer keeps callbacks scoped to this call (no dangling captures).
inline bool chooseSidebarActionAt(QWidget &window, QListWidget *list, const QPoint &pos,
                                  const char *menuName, const char *actionName)
{
    bool chosen = false;
    QTimer driver; driver.setSingleShot(true);
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        auto menu = window.findChild<QMenu *>(menuName);
        auto action = menu ? menu->findChild<QAction *>(actionName) : nullptr;
        if (!action || !action->isEnabled()) { if (menu) menu->close(); return; }
        chosen = true; menu->setActiveAction(action);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(menu, &enter);
    });
    driver.start(0);
    return QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection,
                                    Q_ARG(QPoint, pos)) && chosen;
}

inline bool chooseSidebarAction(QWidget &window, QListWidget *list, int row,
                                const char *menuName, const char *actionName)
{
    list->scrollToItem(list->item(row));
    QCoreApplication::processEvents();
    return chooseSidebarActionAt(window, list, list->visualItemRect(list->item(row)).center(), menuName, actionName);
}
