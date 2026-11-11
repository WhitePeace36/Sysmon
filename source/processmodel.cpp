#include "processmodel.h"

#include "format.h"

#include <QCoreApplication>
#include <QHash>
#include <QMetaType>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace ProcColumns {

QString title(int column)
{
    switch (column) {
    case Name: return QCoreApplication::translate("ProcColumns", "Name");
    case Pid: return QCoreApplication::translate("ProcColumns", "PID");
    case User: return QCoreApplication::translate("ProcColumns", "User");
    case State: return QCoreApplication::translate("ProcColumns", "State");
    case Cpu: return QCoreApplication::translate("ProcColumns", "CPU %");
    case Gpu: return QCoreApplication::translate("ProcColumns", "GPU %");
    case GpuMem: return QCoreApplication::translate("ProcColumns", "GPU memory");
    case Memory: return QCoreApplication::translate("ProcColumns", "Memory");
    case Resident: return QCoreApplication::translate("ProcColumns", "Resident");
    case Network: return QCoreApplication::translate("ProcColumns", "Network");
    case NetRx: return QCoreApplication::translate("ProcColumns", "Net receive");
    case NetTx: return QCoreApplication::translate("ProcColumns", "Net send");
    case Disk: return QCoreApplication::translate("ProcColumns", "Disk");
    case DiskRead: return QCoreApplication::translate("ProcColumns", "Disk read");
    case DiskWrite: return QCoreApplication::translate("ProcColumns", "Disk write");
    case Nice: return QCoreApplication::translate("ProcColumns", "Nice");
    case Priority: return QCoreApplication::translate("ProcColumns", "PRIO");
    case Sched: return QCoreApplication::translate("ProcColumns", "Sched");
    case Threads: return QCoreApplication::translate("ProcColumns", "Threads");
    case Ppid: return QCoreApplication::translate("ProcColumns", "PPID");
    case Command: return QCoreApplication::translate("ProcColumns", "Command line");
    default: return QString();
    }
}

}  // namespace ProcColumns

using namespace ProcColumns;

namespace {

bool isTextColumn(int column)
{
    return column == Name || column == User || column == State || column == Sched || column == Command;
}

QString rateOrBlank(double value)
{
    return value < 0 ? QString() : Format::rate(value);
}

// Sum of the members of a program group.
ProcRow summarize(const QString& name, const QVector<int>& members, const QVector<ProcRow>& rows)
{
    if (members.size() == 1) {
        ProcRow single = rows.at(members.first());
        single.group = false;
        return single;
    }
    ProcRow sum;
    sum.name = name;
    sum.group = true;
    sum.cpu = 0;
    bool anyGpu = false, anyNet = false, anyDisk = false, sameUser = true;
    double gpu = 0, netRx = 0, netTx = 0, diskRead = 0, diskWrite = 0;
    for (int idx : members) {
        const ProcRow& r = rows.at(idx);
        sum.pids.append(r.pid);
        if (sum.user.isEmpty())
            sum.user = r.user;
        else if (sum.user != r.user)
            sameUser = false;
        sum.cpu += r.cpu;
        sum.memory += r.memory;
        sum.resident += r.resident;
        sum.gpuMem += r.gpuMem;
        sum.threads += r.threads;
        if (r.gpu >= 0) {
            anyGpu = true;
            gpu += r.gpu;
        }
        if (r.netRx >= 0) {
            anyNet = true;
            netRx += r.netRx;
            netTx += r.netTx;
        }
        if (r.diskRead >= 0) {
            anyDisk = true;
            diskRead += r.diskRead;
            diskWrite += r.diskWrite;
        }
    }
    std::sort(sum.pids.begin(), sum.pids.end());
    if (!sameUser)
        sum.user = QCoreApplication::translate("ProcColumns", "(several)");
    if (anyGpu)
        sum.gpu = gpu;
    if (anyNet) {
        sum.netRx = netRx;
        sum.netTx = netTx;
    }
    if (anyDisk) {
        sum.diskRead = diskRead;
        sum.diskWrite = diskWrite;
    }
    return sum;
}

}  // namespace

QVector<ProcRow> ProcRow::fromSnapshot(const sys::Snapshot& snapshot, bool showKernelThreads, bool perCoreCpu)
{
    QVector<ProcRow> rows;
    rows.reserve(static_cast<int>(snapshot.procs.size()));
    const double cpuDivisor = perCoreCpu ? 1.0 : double(qMax(1, snapshot.cpu.logical));
    for (const sys::ProcStats& p : snapshot.procs) {
        if (p.kernelThread && !showKernelThreads)
            continue;
        ProcRow r;
        r.pid = p.pid;
        r.pids.append(p.pid);
        r.ppid = p.ppid;
        r.name = QString::fromStdString(p.name);
        r.user = QString::fromStdString(p.user);
        r.command = QString::fromStdString(p.cmdline);
        r.state = p.state;
        r.cpu = p.cpuPct / cpuDivisor;
        r.gpu = sys::has(p.gpuPct) ? p.gpuPct : -1.0;
        r.gpuMem = double(p.gpuMemBytes);
        r.memory = double(p.memBytes);
        r.resident = double(p.rssBytes);
        if (sys::has(p.netRxBps)) {
            r.netRx = p.netRxBps;
            r.netTx = p.netTxBps;
        }
        if (sys::has(p.diskReadBps)) {
            r.diskRead = p.diskReadBps;
            r.diskWrite = p.diskWriteBps;
        }
        r.nice = p.nice;
        r.priority = p.priority;
        r.policy = p.policy;
        r.threads = p.threads;
        rows.append(r);
    }
    return rows;
}

QVariant procRowData(const ProcRow& r, int column, int role)
{
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Name: return r.group ? QStringLiteral("%1 (%2)").arg(r.name).arg(r.pids.size()) : r.name;
        case Pid: return r.pid >= 0 ? QString::number(r.pid) : QString();
        case User: return r.user;
        case State: return r.group ? QString() : Format::procState(r.state);
        case Cpu: return QString::number(r.cpu, 'f', 1);
        case Gpu: return r.gpu < 0 ? QString() : QString::number(r.gpu, 'f', 1);
        case GpuMem: return r.gpuMem > 0 ? Format::bytes(r.gpuMem) : QString();
        case Memory: return Format::bytes(r.memory);
        case Resident: return Format::bytes(r.resident);
        case Network: return r.netRx < 0 ? QString() : Format::rate(r.netRx + r.netTx);
        case NetRx: return rateOrBlank(r.netRx);
        case NetTx: return rateOrBlank(r.netTx);
        case Disk: return r.diskRead < 0 ? QString() : Format::rate(r.diskRead + r.diskWrite);
        case DiskRead: return rateOrBlank(r.diskRead);
        case DiskWrite: return rateOrBlank(r.diskWrite);
        case Nice: return r.group ? QString() : QString::number(r.nice);
        case Priority: return r.group ? QString() : Format::priority(r.priority);
        case Sched: return r.group ? QString() : Format::schedPolicy(r.policy);
        case Threads: return QString::number(r.threads);
        case Ppid: return r.group ? QString() : QString::number(r.ppid);
        case Command: return r.command;
        default: return QVariant();
        }
    }
    if (role == SortRole) {
        switch (column) {
        case Name: return r.name;
        case Pid: return r.pid;
        case User: return r.user;
        case State: return r.group ? QString() : Format::procState(r.state);
        case Cpu: return r.cpu;
        case Gpu: return r.gpu;
        case GpuMem: return r.gpuMem;
        case Memory: return r.memory;
        case Resident: return r.resident;
        case Network: return r.netRx < 0 ? -1.0 : r.netRx + r.netTx;
        case NetRx: return r.netRx;
        case NetTx: return r.netTx;
        case Disk: return r.diskRead < 0 ? -1.0 : r.diskRead + r.diskWrite;
        case DiskRead: return r.diskRead;
        case DiskWrite: return r.diskWrite;
        case Nice: return r.nice;
        case Priority: return r.priority;
        case Sched: return r.group ? QString() : Format::schedPolicy(r.policy);
        case Threads: return r.threads;
        case Ppid: return r.ppid;
        case Command: return r.command;
        default: return QVariant();
        }
    }
    if (role == Qt::TextAlignmentRole)
        return isTextColumn(column) ? int(Qt::AlignLeft | Qt::AlignVCenter) : int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case Name:
        case Command:
            return r.command.isEmpty() ? QVariant() : QVariant(r.command);
        case Network:
        case NetRx:
        case NetTx:
            if (r.netRx < 0)
                return QCoreApplication::translate(
                    "ProcColumns", "Not readable: the sockets of other users' processes are only visible to root.");
            return QCoreApplication::translate("ProcColumns", "TCP traffic. Receiving %1, sending %2")
                .arg(Format::rate(r.netRx), Format::rate(r.netTx));
        case Disk:
        case DiskRead:
        case DiskWrite:
            if (r.diskRead < 0)
                return QCoreApplication::translate(
                    "ProcColumns", "Not readable: the I/O counters of other users' processes are only visible to root.");
            return QCoreApplication::translate("ProcColumns", "Reading %1, writing %2")
                .arg(Format::rate(r.diskRead), Format::rate(r.diskWrite));
        default:
            return QVariant();
        }
    }
    if (role == PidsRole) {
        QVariantList pids;
        for (int pid : r.pids)
            pids.append(pid);
        return pids;
    }
    return QVariant();
}

// ---------------------------------------------------------------------------
// ProcessTableModel
// ---------------------------------------------------------------------------

ProcessTableModel::ProcessTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int ProcessTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int ProcessTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(Count);
}

QVariant ProcessTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();
    return procRowData(m_rows.at(index.row()), index.column(), role);
}

QVariant ProcessTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        return title(section);
    return QVariant();
}

void ProcessTableModel::setRows(const QVector<ProcRow>& rows)
{
    QHash<int, int> incoming;  // pid -> index in rows
    incoming.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i)
        incoming.insert(rows.at(i).pid, i);

    // Remove processes that exited, as contiguous ranges from the back.
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (incoming.contains(m_rows.at(i).pid))
            continue;
        const int last = i;
        while (i > 0 && !incoming.contains(m_rows.at(i - 1).pid))
            --i;
        beginRemoveRows(QModelIndex(), i, last);
        m_rows.erase(m_rows.begin() + i, m_rows.begin() + last + 1);
        endRemoveRows();
    }

    // Refresh the survivors in place.
    QSet<int> present;
    present.reserve(m_rows.size());
    for (ProcRow& row : m_rows) {
        present.insert(row.pid);
        row = rows.at(incoming.value(row.pid));
    }
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, Count - 1));

    // Append new processes; the proxy model sorts them into place.
    QVector<ProcRow> fresh;
    for (const ProcRow& row : rows)
        if (!present.contains(row.pid))
            fresh.append(row);
    if (!fresh.isEmpty()) {
        beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size() + fresh.size() - 1);
        m_rows += fresh;
        endInsertRows();
    }
}

// ---------------------------------------------------------------------------
// ProcessTreeModel
// ---------------------------------------------------------------------------

struct ProcessTreeModel::Group {
    QString key;
    int row = 0;
    ProcRow summary;
    QVector<ProcRow> children;  // empty while the program has a single process
};

ProcessTreeModel::ProcessTreeModel(QObject* parent) : QAbstractItemModel(parent) {}
ProcessTreeModel::~ProcessTreeModel() = default;

// Top-level indexes carry no internal pointer; a child carries its group.
QModelIndex ProcessTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (row < 0 || column < 0 || column >= Count)
        return QModelIndex();
    if (!parent.isValid())
        return row < static_cast<int>(m_groups.size()) ? createIndex(row, column) : QModelIndex();
    if (parent.internalPointer() != nullptr || parent.column() != 0
        || parent.row() >= static_cast<int>(m_groups.size()))
        return QModelIndex();
    Group* group = m_groups[static_cast<size_t>(parent.row())].get();
    return row < group->children.size() ? createIndex(row, column, group) : QModelIndex();
}

QModelIndex ProcessTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return QModelIndex();
    const Group* group = static_cast<const Group*>(child.internalPointer());
    return group ? createIndex(group->row, 0) : QModelIndex();
}

int ProcessTreeModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return static_cast<int>(m_groups.size());
    if (parent.internalPointer() != nullptr || parent.column() != 0
        || parent.row() >= static_cast<int>(m_groups.size()))
        return 0;
    return m_groups[static_cast<size_t>(parent.row())]->children.size();
}

int ProcessTreeModel::columnCount(const QModelIndex&) const
{
    return int(Count);
}

QVariant ProcessTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();
    const Group* group = static_cast<const Group*>(index.internalPointer());
    if (group) {
        if (index.row() >= group->children.size())
            return QVariant();
        return procRowData(group->children.at(index.row()), index.column(), role);
    }
    if (index.row() >= static_cast<int>(m_groups.size()))
        return QVariant();
    return procRowData(m_groups[static_cast<size_t>(index.row())]->summary, index.column(), role);
}

QVariant ProcessTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        return title(section);
    return QVariant();
}

void ProcessTreeModel::renumber()
{
    for (size_t i = 0; i < m_groups.size(); ++i)
        m_groups[i]->row = static_cast<int>(i);
}

void ProcessTreeModel::setRows(const QVector<ProcRow>& rows)
{
    QHash<QString, QVector<int>> byName;  // program name -> indexes into rows
    for (int i = 0; i < rows.size(); ++i)
        byName[rows.at(i).name].append(i);

    // 1. Programs with no process left.
    for (int i = static_cast<int>(m_groups.size()) - 1; i >= 0; --i) {
        if (byName.contains(m_groups[static_cast<size_t>(i)]->key))
            continue;
        beginRemoveRows(QModelIndex(), i, i);
        m_groups.erase(m_groups.begin() + i);
        renumber();
        endRemoveRows();
    }

    // 2. Programs that are still there: sync their children, refresh the sums.
    QSet<QString> known;
    for (const std::unique_ptr<Group>& holder : m_groups) {
        Group* group = holder.get();
        known.insert(group->key);
        const QVector<int> members = byName.value(group->key);
        const QModelIndex groupIndex = createIndex(group->row, 0);

        QHash<int, int> wanted;  // pid -> index in rows; empty for a single process
        if (members.size() > 1)
            for (int idx : members)
                wanted.insert(rows.at(idx).pid, idx);

        for (int c = group->children.size() - 1; c >= 0; --c) {
            if (wanted.contains(group->children.at(c).pid))
                continue;
            beginRemoveRows(groupIndex, c, c);
            group->children.removeAt(c);
            endRemoveRows();
        }
        for (ProcRow& child : group->children)
            child = rows.at(wanted.take(child.pid));
        if (!wanted.isEmpty()) {
            const int first = group->children.size();
            beginInsertRows(groupIndex, first, first + wanted.size() - 1);
            for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it)
                group->children.append(rows.at(it.value()));
            endInsertRows();
        }
        group->summary = summarize(group->key, members, rows);
        if (!group->children.isEmpty())
            emit dataChanged(index(0, 0, groupIndex), index(group->children.size() - 1, Count - 1, groupIndex));
    }
    if (!m_groups.empty())
        emit dataChanged(index(0, 0), index(static_cast<int>(m_groups.size()) - 1, Count - 1));

    // 3. Programs seen for the first time, inserted complete with children.
    QStringList fresh;
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it)
        if (!known.contains(it.key()))
            fresh.append(it.key());
    if (fresh.isEmpty())
        return;
    const int first = static_cast<int>(m_groups.size());
    beginInsertRows(QModelIndex(), first, first + fresh.size() - 1);
    for (const QString& key : std::as_const(fresh)) {
        auto group = std::make_unique<Group>();
        group->key = key;
        group->row = static_cast<int>(m_groups.size());
        const QVector<int> members = byName.value(key);
        if (members.size() > 1)
            for (int idx : members)
                group->children.append(rows.at(idx));
        group->summary = summarize(key, members, rows);
        m_groups.push_back(std::move(group));
    }
    endInsertRows();
}

// ---------------------------------------------------------------------------
// RowSortProxy
// ---------------------------------------------------------------------------

RowSortProxy::RowSortProxy(QObject* parent) : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    setRecursiveFilteringEnabled(true);
}

void RowSortProxy::setFilterColumns(const QVector<int>& columns)
{
    m_columns = columns;
    invalidateFilter();
}

void RowSortProxy::setFilterText(const QString& text)
{
    if (m_filter == text)
        return;
    m_filter = text;
    invalidateFilter();
}

bool RowSortProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const QVariant a = left.data(SortRole);
    const QVariant b = right.data(SortRole);
    if (a.userType() == QMetaType::QString || b.userType() == QMetaType::QString) {
        const int order = QString::compare(a.toString(), b.toString(), Qt::CaseInsensitive);
        if (order != 0)
            return order < 0;
    } else {
        const double x = a.toDouble();
        const double y = b.toDouble();
        if (x != y)
            return x < y;
    }
    const QVariantList leftPids = left.data(PidsRole).toList();
    const QVariantList rightPids = right.data(PidsRole).toList();
    const int lp = leftPids.isEmpty() ? 0 : leftPids.first().toInt();
    const int rp = rightPids.isEmpty() ? 0 : rightPids.first().toInt();
    return lp < rp;
}

bool RowSortProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_filter.isEmpty())
        return true;
    const QAbstractItemModel* model = sourceModel();
    for (int column : m_columns) {
        const QString text = model->index(sourceRow, column, sourceParent).data(Qt::DisplayRole).toString();
        if (text.contains(m_filter, Qt::CaseInsensitive))
            return true;
    }
    return false;
}
