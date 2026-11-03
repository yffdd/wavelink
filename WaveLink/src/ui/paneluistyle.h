#pragma once
#include <QWidget>
#include <QStringList>
namespace PanelUi {
inline void windowControls(QWidget *widget) {
    widget->setWindowFlags((widget->windowFlags() & ~Qt::WindowContextHelpButtonHint)
        | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
}
inline QString listStyle(bool dark) {
    return QString("QListWidget {background:%1;color:%2;border:none;outline:0;}"
        "QListWidget::item {padding:8px 10px;border-bottom:1px solid %3;}"
        "QListWidget::item:hover:!selected {background:%4;}"
        "QListWidget::item:selected:active,QListWidget::item:selected:!active {background:#00a85a;color:white;}")
        .arg(dark?"#1e1e1e":"#ffffff",dark?"#dddddd":"#333333",dark?"#333333":"#e2e2e2",dark?"#303a36":"#eaf5ef");
}
inline QString uniqueName(const QString &base,const QStringList &names) {
    QString result=base;int suffix=1;while(names.contains(result))result=base+"_"+QString::number(suffix++);return result;
}
}
