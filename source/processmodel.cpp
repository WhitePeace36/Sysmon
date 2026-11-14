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
        r.threadRows.reserve(p.threadList.size());
        for (const sys::ThreadStats& t : p.threadList) {
            ProcRow tr;
            tr.thread = true;
            tr.pid = t.tid;  // no pids: signals go to processes, not threads
            tr.ppid = p.pid;
            tr.name = QString::fromStdString(t.name);
            tr.user = r.user;
            tr.state = t.state;
            tr.cpu = t.cpuPct / cpuDivisor;
            tr.nice = t.nice;
            tr.priority = t.priority;
            tr.policy = t.policy;
            r.threadRows.push_back(tr);
        }
        rows.append(r);
    }
    return rows;
}

QVariant procRowData(const ProcRow& r, int column, int role)
{
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Name: return r.name;
        case Pid: return r.pid >= 0 ? QString::number(r.pid) : QString();
        case User: return r.user;
        case State: return Format::procState(r.state);
        case Cpu: return QString::number(r.cpu, 'f', 1);
        case Gpu: return r.gpu < 0 ? QString() : QString::number(r.gpu, 'f', 1);
        case GpuMem: return r.gpuMem > 0 ? Format::bytes(r.gpuMem) : QString();
        case Memory: return r.thread ? QString() : Format::bytes(r.memory);
        case Resident: return r.thread ? QString() : Format::bytes(r.resident);
        case Network: return r.netRx < 0 ? QString() : Format::rate(r.netRx + r.netTx);
        case NetRx: return rateOrBlank(r.netRx);
        case NetTx: return rateOrBlank(r.netTx);
        case Disk: return r.diskRead < 0 ? QString() : Format::rate(r.diskRead + r.diskWrite);
        case DiskRead: return rateOrBlank(r.diskRead);
        case DiskWrite: return rateOrBlank(r.diskWrite);
        case Nice: return QString::number(r.nice);
        case Priority: return Format::priority(r.priority);
        case Sched: return Format::schedPolicy(r.policy);
        case Threads: return r.thread ? QString() : QString::number(r.threads);
        case Ppid: return QString::number(r.ppid);
        case Command: return r.command;
        default: return QVariant();
        }
    }
    if (role == SortRole) {
        switch (column) {
        case Name: return r.name;
        case Pid: return r.pid;
        case User: return r.user;
        case State: return Format::procState(r.state);
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
        case Sched: return Format::schedPolicy(r.policy);
        case Threads: return r.threads;
        case Ppid: return r.ppid;
        case Command: return r.command;
        default: return QVariant();
        }
    }
    if (role == Qt::TextAlignmentRole)
        return isTextColumn(column) ? int(Qt::AlignLeft | Qt::AlignVCenter) : int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole && r.thread) {
        if (column == Name || column == Pid)
            return QCoreApplication::translate("ProcColumns", "Thread %1 of PID %2").arg(r.pid).arg(r.ppid);
        return QVariant();
    }
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
    if (role == TieBreakRole)
        return r.pid;
    if (role == PidsRole) {
        QVariantList pids;
        for (int pid : r.pids)
            pids.append(pid);
        return pids;
    }
    return QVariant();
}

// ---------------------------------------------------------------------------
// ProcessTreeModel
// ---------------------------------------------------------------------------

// A process row with its threads.
struct ProcessTreeModel::Group {
    int pid = 0;
    int row = 0;
    ProcRow process;
    QVector<ProcRow> threads;  // empty for a single-threaded process
};

ProcessTreeModel::ProcessTreeModel(QObject* parent) : QAbstractItemModel(parent) {}
ProcessTreeModel::~ProcessTreeModel() = default;

// Top-level indexes carry no internal pointer; a thread carries its process.
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
    return row < group->threads.size() ? createIndex(row, column, group) : QModelIndex();
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
    return m_groups[static_cast<size_t>(parent.row())]->threads.size();
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
        if (index.row() >= group->threads.size())
            return QVariant();
        return procRowData(group->threads.at(index.row()), index.column(), role);
    }
    if (index.row() >= static_cast<int>(m_groups.size()))
        return QVariant();
    return procRowData(m_groups[static_cast<size_t>(index.row())]->process, index.column(), role);
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
    QHash<int, int> incoming;  // pid -> index in rows
    incoming.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i)
        incoming.insert(rows.at(i).pid, i);

    // 1. Processes that exited.
    for (int i = static_cast<int>(m_groups.size()) - 1; i >= 0; --i) {
        if (incoming.contains(m_groups[static_cast<size_t>(i)]->pid))
            continue;
        beginRemoveRows(QModelIndex(), i, i);
        m_groups.erase(m_groups.begin() + i);
        renumber();
        endRemoveRows();
    }

    // 2. Processes still there: sync their threads, refresh the figures.
    for (const std::unique_ptr<Group>& holder : m_groups) {
        Group* group = holder.get();
        const ProcRow& row = rows.at(incoming.take(group->pid));
        const QModelIndex groupIndex = createIndex(group->row, 0);

        QHash<int, int> wanted;  // tid -> index in row.threadRows
        for (int t = 0; t < static_cast<int>(row.threadRows.size()); ++t)
            wanted.insert(row.threadRows[static_cast<size_t>(t)].pid, t);
        for (int c = group->threads.size() - 1; c >= 0; --c) {
            if (wanted.contains(group->threads.at(c).pid))
                continue;
            beginRemoveRows(groupIndex, c, c);
            group->threads.removeAt(c);
            endRemoveRows();
        }
        for (ProcRow& thread : group->threads)
            thread = row.threadRows[static_cast<size_t>(wanted.take(thread.pid))];
        if (!wanted.isEmpty()) {
            const int first = group->threads.size();
            beginInsertRows(groupIndex, first, first + wanted.size() - 1);
            for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it)
                group->threads.append(row.threadRows[static_cast<size_t>(it.value())]);
            endInsertRows();
        }
        group->process = row;
        group->process.threadRows.clear();
        if (!group->threads.isEmpty())
            emit dataChanged(index(0, 0, groupIndex), index(group->threads.size() - 1, Count - 1, groupIndex));
    }
    if (!m_groups.empty())
        emit dataChanged(index(0, 0), index(static_cast<int>(m_groups.size()) - 1, Count - 1));

    // 3. New processes, inserted with their threads; the proxy sorts them in.
    if (incoming.isEmpty())
        return;
    const int first = static_cast<int>(m_groups.size());
    beginInsertRows(QModelIndex(), first, first + incoming.size() - 1);
    for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) {
        const ProcRow& row = rows.at(it.value());
        auto group = std::make_unique<Group>();
        group->pid = row.pid;
        group->row = static_cast<int>(m_groups.size());
        for (const ProcRow& thread : row.threadRows)
            group->threads.append(thread);
        group->process = row;
        group->process.threadRows.clear();
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
    // Ties are common (most processes sit at 0.0 % CPU) and the proxy re-sorts
    // on every refresh, so this must not build a QVariantList per comparison.
    return left.data(TieBreakRole).toInt() < right.data(TieBreakRole).toInt();
}

// A row is shown if it matches, if one of its threads does (recursive
// filtering), or if its process does, so that a matching process keeps its
// threads.
bool RowSortProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_filter.isEmpty())
        return true;
    return matches(sourceRow, sourceParent)
        || (sourceParent.isValid() && matches(sourceParent.row(), sourceParent.parent()));
}

bool RowSortProxy::matches(int sourceRow, const QModelIndex& sourceParent) const
{
    const QAbstractItemModel* model = sourceModel();
    for (int column : m_columns) {
        const QString text = model->index(sourceRow, column, sourceParent).data(Qt::DisplayRole).toString();
        if (text.contains(m_filter, Qt::CaseInsensitive))
            return true;
    }
    return false;
}
