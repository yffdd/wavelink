#include "panellayouteditor.h"
#include "paneluistyle.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace PanelLayout {
namespace {

const char RowMime[] = "application/x-wavelink-panel-layout-item";

class RowList final : public QListWidget {
public:
    using Move = std::function<bool(const QString &, int, bool)>;

    RowList(QWidget *parent, int columns, Move move)
        : QListWidget(parent), m_columns(columns), m_move(std::move(move))
    {
        setObjectName("panelLayoutRow");
        setFlow(LeftToRight);
        setWrapping(false);
        setSpacing(4);
        setFixedHeight(66);
        setDragEnabled(true);
        setDragDropMode(DragDrop);
        setAcceptDrops(true);
        setDefaultDropAction(Qt::MoveAction);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_marker = new QFrame(viewport());
        m_marker->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_marker->setStyleSheet("background: #2296df;");
        m_marker->hide();
    }

    void fitCards()
    {
        const bool fullRow = count() == 1 && item(0)->data(Qt::UserRole + 1).toBool();
        const int width = fullRow ? qMax(168, viewport()->width() - 2 * spacing())
                                  : qBound(168, viewport()->width() / m_columns - 2 * spacing(), 320);
        for (int i = 0; i < count(); ++i) {
            const QSize size(width, 42);
            if (item(i)->sizeHint() != size) item(i)->setSizeHint(size);
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QListWidget::resizeEvent(event);
        fitCards();
    }

    void startDrag(Qt::DropActions) override
    {
        if (!currentItem()) return;
        auto data = new QMimeData;
        data->setData(RowMime, currentItem()->data(Qt::UserRole).toString().toUtf8());
        QDrag drag(this);
        drag.setMimeData(data);
        drag.setPixmap(viewport()->grab(visualItemRect(currentItem())));
        drag.exec(Qt::MoveAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (sameEditor(event)) event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const int index = insertionIndex(event->pos());
        if (!sameEditor(event) || !m_move(id(event), index, false)) {
            m_marker->hide();
            event->ignore();
            return;
        }
        const int x = index < count() ? visualItemRect(item(index)).left() - 2
                    : count() ? visualItemRect(item(count() - 1)).right() + 2 : 3;
        m_marker->setGeometry(qBound(0, x, qMax(0, viewport()->width() - 3)), 4, 3, 42);
        m_marker->show();
        event->acceptProposedAction();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        m_marker->hide();
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        m_marker->hide();
        if (sameEditor(event) && m_move(id(event), insertionIndex(event->pos()), true))
            event->acceptProposedAction();
    }

private:
    bool sameEditor(const QDropEvent *event) const
    {
        const auto source = dynamic_cast<RowList *>(event->source());
        return source && source->window() == window() && event->mimeData()->hasFormat(RowMime);
    }

    QString id(const QDropEvent *event) const
    {
        return QString::fromUtf8(event->mimeData()->data(RowMime));
    }

    int insertionIndex(const QPoint &position) const
    {
        for (int i = 0; i < count(); ++i)
            if (position.x() < visualItemRect(item(i)).center().x()) return i;
        return count();
    }

    int m_columns;
    Move m_move;
    QFrame *m_marker;
};

class LayoutEditor final : public QDialog {
public:
    LayoutEditor(QWidget *parent, EditorKind kind, const QVector<Item> &items, int limit, const Layout &current)
        : QDialog(parent), m_itemsInOrder(items), m_limit(limit), m_hasCustomDraft(current.has_value()),
          m_automatic(arrange(items, limit)), m_rows(current.value_or(m_automatic))
    {
        const bool commands = kind == EditorKind::Commands;
        setObjectName(commands ? "commandLayoutEditor" : "statusLayoutEditor");
        setWindowTitle(commands ? QStringLiteral("编辑命令布局") : QStringLiteral("编辑状态监视布局"));
        PanelUi::windowControls(this);
        resize(960, 620);
        auto bodyFont = font();
        bodyFont.setPointSizeF(qMax(12.0, bodyFont.pointSizeF()));
        setFont(bodyFont);
        auto headingFont = bodyFont;
        headingFont.setPointSizeF(qMax(13.0, bodyFont.pointSizeF()));
        headingFont.setBold(true);
        // The command window styles QLabel descendants in pixels. Keep
        // these labels at the editor's sizes in either parent panel.
        setStyleSheet(QStringLiteral(
            "QLabel#panelLayoutModeLabel, QLabel#panelLayoutColumnsLabel { font-size: %1pt; }"
            "QLabel#panelLayoutHint, QLabel#panelLayoutRowLabel { font-size: %2pt; }")
            .arg(headingFont.pointSizeF()).arg(bodyFont.pointSizeF()));
        for (const auto &item : items) {
            m_items.insert(item.id, item);
            if (item.fullRow) m_fullRows.insert(item.id);
        }

        auto outer = new QHBoxLayout(this);
        outer->setContentsMargins(16, 14, 16, 14);
        auto content = new QWidget;
        content->setObjectName("panelLayoutContent");
        content->setMaximumWidth(1160);
        outer->addStretch(1);
        outer->addWidget(content, 100);
        outer->addStretch(1);
        auto root = new QVBoxLayout(content);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(10);

        auto toolbarFrame = new QFrame;
        toolbarFrame->setObjectName("panelLayoutToolbar");
        auto toolbar = new QHBoxLayout(toolbarFrame);
        toolbar->setContentsMargins(14, 10, 14, 10);
        toolbar->setSpacing(10);
        root->addWidget(toolbarFrame);
        auto modeLabel = new QLabel(QStringLiteral("排列方式"));
        modeLabel->setObjectName("panelLayoutModeLabel");
        modeLabel->setFont(headingFont);
        toolbar->addWidget(modeLabel);
        m_mode = new QComboBox;
        m_mode->setObjectName("panelLayoutMode");
        m_mode->setFont(headingFont);
        m_mode->view()->setFont(headingFont);
        m_mode->setMinimumWidth(166);
        m_mode->addItems({QStringLiteral("自动排列"), QStringLiteral("自定义分行")});
        m_mode->setCurrentIndex(current ? 1 : 0);
        modeLabel->setBuddy(m_mode);
        toolbar->addWidget(m_mode);

        toolbar->addSpacing(12);
        auto columnsLabel = new QLabel(QStringLiteral("每行最多列数"));
        columnsLabel->setObjectName("panelLayoutColumnsLabel");
        columnsLabel->setFont(headingFont);
        toolbar->addWidget(columnsLabel);
        m_columns = new QSpinBox;
        m_columns->setObjectName("panelLayoutColumns");
        m_columns->setFont(headingFont);
        m_columns->setRange(1, commands ? CommandMaxColumns : StatusMaxColumns);
        m_columns->setValue(m_limit);
        m_columns->setKeyboardTracking(false);
        m_columns->setMinimumWidth(66);
        m_columns->setToolTip(QStringLiteral("与面板设置共用此列数。调小列数时，超出的卡片会另起一行。"));
        columnsLabel->setBuddy(m_columns);
        toolbar->addWidget(m_columns);
        toolbar->addStretch();

        const auto theme = qApp->property("waveLinkDarkTheme");
        const bool dark = theme.isValid() ? theme.toBool() : palette().color(QPalette::Window).lightness() < 128;
        toolbarFrame->setStyleSheet(QStringLiteral(
            "QFrame#panelLayoutToolbar { background: %1; border: 1px solid %2; border-radius: 6px; }"
            "QFrame#panelLayoutToolbar QLabel { background: transparent; color: %3; }"
            "QComboBox#panelLayoutMode, QSpinBox#panelLayoutColumns { background: %4; color: %3;"
            " border: 1px solid %5; border-radius: 4px; padding: 4px 8px; min-height: 28px; }"
            "QComboBox#panelLayoutMode:focus, QSpinBox#panelLayoutColumns:focus { border-color: %3; }")
            .arg(dark ? "#273442" : "#eaf2fa", dark ? "#455b72" : "#b3cbe3",
                 dark ? "#f3f7fc" : "#1e3c58", dark ? "#182634" : "#ffffff",
                 dark ? "#7396b9" : "#698fb6"));
        m_add = new QPushButton(QStringLiteral("新增行"));
        m_add->setObjectName("panelLayoutAddRow");
        m_add->setFont(bodyFont);
        toolbar->addWidget(m_add);

        auto hint = new QLabel(commands
            ? QStringLiteral("在自定义分行中拖动命令卡片。滑条独占一行，可拖到空行。排版操作不会发送指令。")
            : QStringLiteral("在自定义分行中拖动监视卡片，可调整行内顺序或移到其他行。空行在保存时自动移除。"));
        hint->setObjectName("panelLayoutHint");
        hint->setFont(bodyFont);
        hint->setWordWrap(true);
        root->addWidget(hint);
        m_scroll = new QScrollArea;
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setWidgetResizable(true);
        m_body = new QWidget;
        m_rowsLayout = new QVBoxLayout(m_body);
        m_rowsLayout->setAlignment(Qt::AlignTop);
        m_rowsLayout->setContentsMargins(0, 0, 2, 0);
        m_rowsLayout->setSpacing(8);
        m_scroll->setWidget(m_body);
        root->addWidget(m_scroll, 100);
        buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        for (auto button : buttons->buttons()) {
            button->setFont(bodyFont);
            button->setMinimumSize(80, 32);
        }
        root->addWidget(buttons);
        root->addStretch(1);

        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            if (custom()) m_hasCustomDraft = true;
            rebuild();
        });
        connect(m_columns, QOverload<int>::of(&QSpinBox::valueChanged), this, &LayoutEditor::updateColumns);
        connect(m_add, &QPushButton::clicked, this, [this] {
            m_rows.append(QStringList{});
            rebuild();
            m_scroll->ensureWidgetVisible(m_lists.last());
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        rebuild();
    }

    int columns() const { return m_columns->value(); }

    Layout resultLayout() const
    {
        if (!custom()) return std::nullopt;
        Rows result;
        for (const auto &row : m_rows) if (!row.isEmpty()) result.append(row);
        return result;
    }

    QDialogButtonBox *buttons;

private:
    bool custom() const { return m_mode->currentIndex() == 1; }

    void updateColumns(int columns)
    {
        m_limit = columns;
        m_automatic = arrange(m_itemsInOrder, columns);
        if (!m_hasCustomDraft) {
            m_rows = m_automatic;
        } else if (widestRow(m_rows) > columns) {
            Rows split;
            for (const auto &row : m_rows) {
                if (row.isEmpty()) split.append(row);
                for (int i = 0; i < row.size(); i += columns)
                    split.append(row.mid(i, columns));
            }
            m_rows = std::move(split);
        }
        rebuild();
    }

    bool moveItem(const QString &id, int target, int column, bool commit)
    {
        const auto item = m_items.constFind(id);
        if (!custom() || item == m_items.cend() || target < 0 || target >= m_rows.size()) return false;
        for (int source = 0; source < m_rows.size(); ++source) {
            const int oldColumn = m_rows[source].indexOf(id);
            if (oldColumn < 0) continue;
            if (source != target && !canAppend(m_rows[target], item.value(), m_limit, m_fullRows)) return false;
            if (commit) {
                m_rows[source].removeAt(oldColumn);
                if (source == target && oldColumn < column) --column;
                m_rows[target].insert(column, id);
                populate(source, m_rows[source]);
                if (source != target) populate(target, m_rows[target]);
            }
            return true;
        }
        return false;
    }

    void populate(int index, const QStringList &row)
    {
        auto list = m_lists[index];
        list->clear();
        for (const auto &id : row) {
            const auto &item = m_items[id];
            auto card = new QListWidgetItem(item.fullRow ? item.name + QStringLiteral("（滑条）") : item.name, list);
            card->setData(Qt::UserRole, id);
            card->setData(Qt::UserRole + 1, item.fullRow);
            card->setToolTip(item.name.toHtmlEscaped());
            card->setTextAlignment(Qt::AlignCenter);
        }
        list->fitCards();
        const bool slider = row.size() == 1 && m_fullRows.contains(row.first());
        m_counts[index]->setText(slider
            ? QStringLiteral("第 %1 行 · 滑条独占").arg(index + 1)
            : QStringLiteral("第 %1 行 · %2 / %3 项").arg(index + 1).arg(row.size()).arg(m_limit));
        m_remove[index]->setVisible(custom() && row.isEmpty());
    }

    void rebuild()
    {
        while (auto item = m_rowsLayout->takeAt(0)) {
            item->widget()->hide();
            item->widget()->deleteLater();
            delete item;
        }
        m_lists.clear();
        m_counts.clear();
        m_remove.clear();
        m_add->setEnabled(custom() && m_rows.size() < qMax(1, m_items.size()));
        const auto &rows = custom() ? m_rows : m_automatic;
        for (int i = 0; i < rows.size(); ++i) {
            auto row = new QFrame(m_body);
            row->setObjectName("panelLayoutRowFrame");
            row->setFrameShape(QFrame::StyledPanel);
            row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto layout = new QVBoxLayout(row);
            layout->setContentsMargins(10, 6, 10, 6);
            layout->setSpacing(4);
            auto controls = new QHBoxLayout;
            layout->addLayout(controls);
            auto label = new QLabel;
            label->setObjectName("panelLayoutRowLabel");
            auto labelFont = font();
            labelFont.setBold(true);
            label->setFont(labelFont);
            m_counts.append(label);
            controls->addWidget(label);
            controls->addStretch();
            for (int delta : {-1, 1}) {
                auto move = new QPushButton(delta < 0 ? QStringLiteral("上移") : QStringLiteral("下移"));
                move->setObjectName(delta < 0 ? "panelLayoutRowUp" : "panelLayoutRowDown");
                move->setFont(font());
                move->setEnabled(custom() && i + delta >= 0 && i + delta < rows.size());
                controls->addWidget(move);
                connect(move, &QPushButton::clicked, this, [this, i, delta] {
                    m_rows.swapItemsAt(i, i + delta);
                    rebuild();
                });
            }
            auto remove = new QPushButton(QStringLiteral("移除空行"));
            remove->setFont(font());
            m_remove.append(remove);
            controls->addWidget(remove);
            connect(remove, &QPushButton::clicked, this, [this, i] {
                m_rows.removeAt(i);
                rebuild();
            });
            auto list = new RowList(row, m_limit, [this, i](const QString &id, int column, bool commit) {
                return moveItem(id, i, column, commit);
            });
            list->setDragEnabled(custom());
            list->setAcceptDrops(custom());
            list->setStyleSheet(QStringLiteral("QListWidget::item { border: 1px solid palette(mid); border-radius: 4px; }"));
            list->setFont(font());
            layout->addWidget(list);
            m_lists.append(list);
            populate(i, rows[i]);
            m_rowsLayout->addWidget(row);
            // A visible editor may queue the new row's show event. Include
            // it in the layout before using the size hint as a height cap.
            row->show();
        }
        m_scroll->setMaximumHeight(qMax(72, m_rowsLayout->sizeHint().height()));
    }

    QVector<Item> m_itemsInOrder;
    int m_limit;
    bool m_hasCustomDraft;
    Rows m_automatic, m_rows;
    QHash<QString, Item> m_items;
    QSet<QString> m_fullRows;
    QComboBox *m_mode;
    QSpinBox *m_columns;
    QPushButton *m_add;
    QScrollArea *m_scroll;
    QWidget *m_body;
    QVBoxLayout *m_rowsLayout;
    QVector<RowList *> m_lists;
    QVector<QLabel *> m_counts;
    QVector<QPushButton *> m_remove;
};

} // namespace

void edit(QWidget *parent, EditorKind kind, const QVector<Item> &items, int limit, const Layout &current,
          const std::function<bool(int, const Layout &, QString &)> &save)
{
    LayoutEditor dialog(parent, kind, items, limit, current);
    QObject::connect(dialog.buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString error;
        if (save(dialog.columns(), dialog.resultLayout(), error)) dialog.accept();
        else QMessageBox::warning(&dialog, QStringLiteral("布局未保存"), error);
    });
    dialog.exec();
}

} // namespace PanelLayout
