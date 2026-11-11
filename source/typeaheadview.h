// Tree/list view shared by the Processes, Details and Services tabs.
//
// Type-ahead: click a row, start typing, and the selection jumps to the first
// row (top to bottom, in the current sort order) whose name starts with what
// was typed. Keystrokes typed in quick succession extend the search text.
#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QTreeView>

class TypeAheadView : public QTreeView {
    Q_OBJECT

public:
    explicit TypeAheadView(QWidget* parent = nullptr);

    // Column whose text is searched; the name column by default.
    void setSearchColumn(int column) { m_searchColumn = column; }

    void keyboardSearch(const QString& search) override;

signals:
    void deletePressed();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QString m_typed;
    QElapsedTimer m_lastKey;
    int m_searchColumn = 0;
};
