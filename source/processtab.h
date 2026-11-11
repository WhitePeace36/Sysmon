// The Processes tab (Grouped: programs with their processes underneath) and
// the Details tab (Flat: every PID). Same widget, two modes.
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
    enum Mode { Grouped, Flat };

    explicit ProcessTab(Mode mode, QWidget* parent = nullptr);

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
    QString settingsKey() const;

    Mode m_mode;
    bool m_confirmKill = true;
    QLineEdit* m_filter = nullptr;
    TypeAheadView* m_view = nullptr;
    ProcessTableModel* m_table = nullptr;
    ProcessTreeModel* m_tree = nullptr;
    RowSortProxy* m_proxy = nullptr;
};
