// Item model behind the Details tab: every process, with its threads.
//
// The model is updated in place on every refresh - rows are inserted,
// removed and changed, never reset - so the selection, the scroll position
// and expanded processes survive while the list keeps updating.
#pragma once

#include "sys/sysinfo.h"

#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVariant>
#include <QVector>

#include <memory>
#include <vector>

namespace ProcColumns {
enum Column {
    Name,
    Pid,
    User,
    State,
    Cpu,
    Gpu,
    GpuMem,
    Memory,
    Resident,
    Network,
    NetRx,
    NetTx,
    Disk,
    DiskRead,
    DiskWrite,
    Nice,
    Priority,
    Sched,
    Threads,
    Ppid,
    Command,
    Count
};
QString title(int column);
}  // namespace ProcColumns

enum ProcRoles {
    SortRole = Qt::UserRole,          // raw value used for ordering
    PidsRole = Qt::UserRole + 1,      // QVariantList of the PIDs behind a row
    TieBreakRole = Qt::UserRole + 2   // int: PID (or TID) of the row, orders equal rows
};

// One displayed row: a process, or one of its threads.
struct ProcRow {
    QString name, user, command;
    QVector<int> pids;  // the PID signals go to; empty for a thread
    int pid = -1;
    int ppid = 0;
    char state = '?';
    double cpu = 0;
    double gpu = -1;                   // negative: not available
    double gpuMem = 0;
    double memory = 0, resident = 0;
    double netRx = -1, netTx = -1;     // negative: not available
    double diskRead = -1, diskWrite = -1;
    int nice = 0, priority = 0, policy = 0, threads = 0;
    bool thread = false;                // a thread row: pid is the TID, ppid its process
    std::vector<ProcRow> threadRows;    // the threads of a process, while they are collected

    // perCoreCpu: 100 % means one core; otherwise 100 % means the whole machine.
    static QVector<ProcRow> fromSnapshot(const sys::Snapshot& snapshot, bool showKernelThreads, bool perCoreCpu);
};

QVariant procRowData(const ProcRow& row, int column, int role);

// Two levels: every process, and below it its threads when it has more
// than one.
class ProcessTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit ProcessTreeModel(QObject* parent = nullptr);
    ~ProcessTreeModel() override;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setRows(const QVector<ProcRow>& rows);

private:
    struct Group;
    void renumber();

    std::vector<std::unique_ptr<Group>> m_groups;
};

// Sorting and filtering for all list tabs.
//
// Orders by the raw values (SortRole, not the formatted text) and breaks ties
// by PID so that rows with equal values do not swap places between refreshes.
// The filter is a case-insensitive "contains" over a chosen set of columns.
class RowSortProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit RowSortProxy(QObject* parent = nullptr);

    void setFilterColumns(const QVector<int>& columns);
    void setFilterText(const QString& text);

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    bool matches(int sourceRow, const QModelIndex& sourceParent) const;

    QString m_filter;
    QVector<int> m_columns;
};
