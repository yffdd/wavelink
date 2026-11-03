#ifndef CONNECTIONHINTS_H
#define CONNECTIONHINTS_H

#include <QFormLayout>
#include <QLayout>
#include <QWidget>

namespace ConnectionHints {
// Keep detailed safety tips already present on controls; give their labels a
// short introduction as well, including when a connection locks the controls.
inline void row(QFormLayout *form, QWidget *field, const QString &tip) {
    if (auto label = form->labelForField(field)) label->setToolTip(tip);
    if (field->toolTip().isEmpty()) field->setToolTip(tip);
}
inline void row(QFormLayout *form, QLayout *fields, const QString &tip) {
    if (auto label = form->labelForField(fields)) label->setToolTip(tip);
    for (int i = 0; i < fields->count(); ++i)
        if (auto widget = fields->itemAt(i)->widget())
            if (widget->toolTip().isEmpty()) widget->setToolTip(tip);
}
}
#endif
