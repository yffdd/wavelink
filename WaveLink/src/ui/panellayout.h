#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <optional>

namespace PanelLayout {

constexpr int CommandMaxColumns = 10;
constexpr int StatusMaxColumns = 8;

struct Item {
    QString id;
    QString name;
    bool fullRow = false;
};

using Rows = QVector<QStringList>;
using Layout = std::optional<Rows>; // Absent means automatic arrangement.

inline bool canAppend(const QStringList &row, const Item &item, int limit, const QSet<QString> &fullRows)
{
    return row.isEmpty() || (!item.fullRow && row.size() < limit && !fullRows.contains(row.first()));
}

// Keep existing row boundaries. Fill the last row with new items when possible.
// Changing item type or reducing the limit can split a row without hiding items.
inline Rows arrange(const QVector<Item> &items, int limit, const Rows &preferred = {})
{
    QHash<QString, Item> byId;
    QSet<QString> fullRows;
    for (const auto &item : items) {
        byId.insert(item.id, item);
        if (item.fullRow) fullRows.insert(item.id);
    }

    Rows rows;
    QSet<QString> placed;
    const auto append = [&](const Item &item) {
        if (rows.isEmpty() || !canAppend(rows.last(), item, limit, fullRows)) rows.append(QStringList{});
        rows.last().append(item.id);
        placed.insert(item.id);
    };
    for (const auto &row : preferred) {
        rows.append(QStringList{});
        for (const auto &id : row) {
            const auto item = byId.constFind(id);
            if (item != byId.cend() && !placed.contains(id)) append(item.value());
        }
        if (rows.last().isEmpty()) rows.removeLast();
    }
    for (const auto &item : items) if (!placed.contains(item.id)) append(item);
    return rows;
}

inline int widestRow(const Rows &rows)
{
    int width = 0;
    for (const auto &row : rows) width = qMax(width, row.size());
    return width;
}

inline void write(QJsonObject &object, const Layout &layout)
{
    if (!layout) return;
    QJsonArray rows;
    for (const auto &row : *layout) rows.append(QJsonArray::fromStringList(row));
    object.insert(QStringLiteral("layoutRows"), rows);
}

inline bool read(const QJsonValue &value, const QVector<Item> &items, int limit, Layout &layout, QString &error)
{
    if (value.isUndefined()) {
        layout.reset();
        return true;
    }
    const auto invalid = [&] {
        error = QStringLiteral("自定义分行无效：每个项目必须出现一次，每行不得超出上限，滑条需独占一行。");
        return false;
    };
    const auto array = value.toArray();
    if (!value.isArray() || array.size() > items.size()) return invalid();

    Rows rows;
    int count = 0;
    for (const auto &entry : array) {
        const auto entries = entry.toArray();
        if (entries.isEmpty() || entries.size() > limit) return invalid();
        QStringList row;
        for (const auto &id : entries) {
            if (!id.isString() || id.toString().isEmpty() || ++count > items.size()) return invalid();
            row.append(id.toString());
        }
        rows.append(row);
    }
    if (count != items.size() || arrange(items, limit, rows) != rows) return invalid();
    layout = rows;
    return true;
}

inline void remap(Layout &layout, const QHash<QString, QString> &newIds)
{
    if (layout) for (auto &row : *layout) for (auto &id : row) id = newIds.value(id, id);
}

} // namespace PanelLayout
