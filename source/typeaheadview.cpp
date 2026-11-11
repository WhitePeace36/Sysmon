#include "typeaheadview.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QItemSelectionModel>
#include <QKeyEvent>

TypeAheadView::TypeAheadView(QWidget* parent) : QTreeView(parent)
{
    setUniformRowHeights(true);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setAllColumnsShowFocus(true);
    setSortingEnabled(true);
}

void TypeAheadView::keyboardSearch(const QString& search)
{
    QAbstractItemModel* m = model();
    if (!m || search.isEmpty() || m->rowCount() == 0)
        return;

    // A pause longer than the platform's keyboard interval starts a new word.
    if (!m_lastKey.isValid() || m_lastKey.elapsed() > QApplication::keyboardInputInterval())
        m_typed.clear();
    m_lastKey.start();
    m_typed += search;

    // QAbstractItemModel::match() walks the rows in view order from the given
    // start and compares case-insensitively.
    const QModelIndex start = m->index(0, m_searchColumn);
    QModelIndexList hits = m->match(start, Qt::DisplayRole, m_typed, 1, Qt::MatchFlags(Qt::MatchStartsWith));
    if (hits.isEmpty())
        hits = m->match(start, Qt::DisplayRole, m_typed, 1, Qt::MatchFlags(Qt::MatchContains));
    if (hits.isEmpty())
        return;

    const QModelIndex hit = hits.first();
    selectionModel()->setCurrentIndex(hit, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    scrollTo(hit);
}

void TypeAheadView::keyPressEvent(QKeyEvent* event)
{
    const Qt::KeyboardModifiers held = event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    if (event->key() == Qt::Key_Delete && !held) {
        emit deletePressed();
        event->accept();
        return;
    }
    QTreeView::keyPressEvent(event);
}
