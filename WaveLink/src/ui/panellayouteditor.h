#pragma once

#include "panellayout.h"
#include <functional>

class QWidget;

namespace PanelLayout {

enum class EditorKind { Commands, Status };

void edit(QWidget *parent, EditorKind kind, const QVector<Item> &items, int limit, const Layout &current,
          const std::function<bool(int, const Layout &, QString &)> &save);

} // namespace PanelLayout
