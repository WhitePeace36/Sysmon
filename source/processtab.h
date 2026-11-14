// The Details tab: every process, expandable to its threads.
//
// Del kills the selection (SIGKILL). Any other signal, including the polite
// SIGTERM, is sent through the context menu: Send Signal > ...
#pragma once

#include "processmodel.h"

#include <QList>
#include <QString>
#include <QVector>
#include <QWidget>

class QLineEdit;
class QPoint;
class QSettings;
class TypeAheadView;

class ProcessTab : public QWidget {
    Q_OBJECT

public:
    explicit ProcessTab(QWidget* parent = nullptr);

    void setRows(const QVector<ProcRow>& rows);
    void setConfirmKill(bool confirm) { m_confirmKill = confirm; }

    void saveState(QSettings& settings) const;
    void restoreState(QSettings& settings);

private slots:
    void killSelected();
    void showContextMenu(const QPoint& pos);
    void showHeaderMenu(const QPoint& pos);

private:
    struct Target {
        QString name;
        QVector<int> pids;
    };

    Target selection() const;
    void deliver(const QVector<int>& pids, int signo);
    void renice(const QVector<int>& pids, int niceValue);
    bool m_confirmKill = true;
    QLineEdit* m_filter = nullptr;
    TypeAheadView* m_view = nullptr;
    ProcessTreeModel* m_tree = nullptr;
    RowSortProxy* m_proxy = nullptr;
};
