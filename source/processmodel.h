// Item models behind the Processes tab (programs grouped by name) and the
// Details tab (one row per PID).
//
// Both models are updated in place on every refresh - rows are inserted,
// removed and changed, never reset - so the selection, the scroll position
// and expanded groups survive while the list keeps updating.
#pragma once

#include "sys/sysinfo.h"

#include <QAbstractItemModel>
#include <QAbstractTableModel>
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
    SortRole = Qt::UserRole,     // raw value used for ordering
    PidsRole = Qt::UserRole + 1  // QVariantList of the PIDs behind a row
};

// One displayed row: a single process, or the sum of a group of processes.
struct ProcRow {
    QString name, user, command;
    QVector<int> pids;
    int pid = -1;  // -1 for a group of several processes
    int ppid = 0;
    char state = '?';
    double cpu = 0;
    double gpu = -1;                   // negative: not available
    double gpuMem = 0;
    double memory = 0, resident = 0;
    double netRx = -1, netTx = -1;     // negative: not available
    double diskRead = -1, diskWrite = -1;
    int nice = 0, priority = 0, policy = 0, threads = 0;
    bool group = false;

    // perCoreCpu: 100 % means one core; otherwise 100 % means the whole machine.
    static QVector<ProcRow> fromSnapshot(const sys::Snapshot& snapshot, bool showKernelThreads, bool perCoreCpu);
};

QVariant procRowData(const ProcRow& row, int column, int role);

// Flat list: one row per process.
class ProcessTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit ProcessTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setRows(const QVector<ProcRow>& rows);

private:
    QVector<ProcRow> m_rows;
};

// Two levels: programs, and below each program its processes when it has
// more than one.
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

// Sorting and filtering for all three list tabs.
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
    QString m_filter;
    QVector<int> m_columns;
};
