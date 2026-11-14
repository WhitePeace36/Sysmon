#include <cerrno>
#include <csignal>
#include <cstring>

#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>

#include "processtab.h"

#include "typeaheadview.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QStringList>
#include <QVBoxLayout>

#include <utility>

namespace {

struct SignalDef {
    int number;
    const char* name;
    const char* description;
};

// Every standard signal of this platform, in numeric order on x86 and ARM.
const SignalDef kSignals[] = {
    {SIGHUP, "HUP", "hang up, often: reload configuration"},
    {SIGINT, "INT", "interrupt, like Ctrl+C"},
    {SIGQUIT, "QUIT", "quit and dump core"},
    {SIGILL, "ILL", "illegal instruction"},
    {SIGTRAP, "TRAP", "trace trap"},
    {SIGABRT, "ABRT", "abort"},
    {SIGBUS, "BUS", "bus error"},
    {SIGFPE, "FPE", "arithmetic error"},
    {SIGKILL, "KILL", "kill, cannot be caught"},
    {SIGUSR1, "USR1", "user-defined 1"},
    {SIGSEGV, "SEGV", "segmentation fault"},
    {SIGUSR2, "USR2", "user-defined 2"},
    {SIGPIPE, "PIPE", "broken pipe"},
    {SIGALRM, "ALRM", "alarm clock"},
    {SIGTERM, "TERM", "terminate, asks the process to quit"},
#ifdef SIGSTKFLT
    {SIGSTKFLT, "STKFLT", "stack fault"},
#endif
    {SIGCHLD, "CHLD", "child status changed"},
    {SIGCONT, "CONT", "continue if stopped"},
    {SIGSTOP, "STOP", "pause, cannot be caught"},
    {SIGTSTP, "TSTP", "pause, like Ctrl+Z"},
    {SIGTTIN, "TTIN", "background read from terminal"},
    {SIGTTOU, "TTOU", "background write to terminal"},
    {SIGURG, "URG", "urgent data on socket"},
    {SIGXCPU, "XCPU", "CPU time limit exceeded"},
    {SIGXFSZ, "XFSZ", "file size limit exceeded"},
    {SIGVTALRM, "VTALRM", "virtual timer expired"},
    {SIGPROF, "PROF", "profiling timer expired"},
    {SIGWINCH, "WINCH", "window size changed"},
    {SIGIO, "IO", "I/O possible"},
#ifdef SIGPWR
    {SIGPWR, "PWR", "power failure"},
#endif
    {SIGSYS, "SYS", "bad system call"},
};

QString pidList(const QVector<int>& pids)
{
    QStringList parts;
    for (int pid : pids)
        parts << QString::number(pid);
    return parts.join(QStringLiteral(", "));
}

// Runs a small privileged helper through polkit when the user agrees.
void runAsAdministrator(QWidget* parent, const QString& question, const QStringList& command)
{
    if (QMessageBox::question(parent, QObject::tr("Permission denied"), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    if (!QProcess::startDetached(QStringLiteral("pkexec"), command))
        QMessageBox::warning(parent, QObject::tr("Permission denied"),
                             QObject::tr("pkexec could not be started. Run the program as root to manage "
                                         "processes of other users."));
}

}  // namespace

ProcessTab::ProcessTab(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter by name, PID, user or command line (Ctrl+F)"));
    m_filter->setClearButtonEnabled(true);
    root->addWidget(m_filter);

    m_view = new TypeAheadView;
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setSearchColumn(ProcColumns::Name);

    m_proxy = new RowSortProxy(this);
    m_proxy->setFilterColumns({ProcColumns::Name, ProcColumns::Pid, ProcColumns::User, ProcColumns::Command});
    m_tree = new ProcessTreeModel(this);
    m_proxy->setSourceModel(m_tree);
    m_view->setModel(m_proxy);
    root->addWidget(m_view, 1);

    // The stretch setting has to be in place before any column is hidden.
    // Switching it off afterwards makes QHeaderView give the hidden columns
    // their width back without showing them, which leaves empty gaps between
    // the visible columns (seen with Qt 5.15 and Qt 6).
    QHeaderView* header = m_view->header();
    header->setStretchLastSection(true);
    header->setSectionsMovable(true);
    header->setContextMenuPolicy(Qt::CustomContextMenu);

    // Columns shown by default; the rest is one right-click on the header away.
    using namespace ProcColumns;
    const QSet<int> visible{Name, Pid, User, Cpu, Gpu, Memory, Network, Disk, Nice, Priority, Sched, Command};
    for (int c = 0; c < Count; ++c)
        m_view->setColumnHidden(c, !visible.contains(c));

    m_view->setColumnWidth(Name, 200);
    m_view->setColumnWidth(Pid, 80);
    m_view->setColumnWidth(Command, 400);
    m_view->sortByColumn(Name, Qt::AscendingOrder);

    connect(m_filter, &QLineEdit::textChanged, m_proxy, &RowSortProxy::setFilterText);
    connect(m_view, &TypeAheadView::deletePressed, this, &ProcessTab::killSelected);
    connect(m_view, &QWidget::customContextMenuRequested, this, &ProcessTab::showContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this, &ProcessTab::showHeaderMenu);

    auto* find = new QShortcut(QKeySequence(QKeySequence::Find), this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] {
        m_filter->setFocus();
        m_filter->selectAll();
    });
}

void ProcessTab::setRows(const QVector<ProcRow>& rows)
{
    m_tree->setRows(rows);
}

void ProcessTab::saveState(QSettings& settings) const
{
    settings.setValue(QStringLiteral("details/header-v2"), m_view->header()->saveState());
}

void ProcessTab::restoreState(QSettings& settings)
{
    QByteArray state = settings.value(QStringLiteral("details/header-v2")).toByteArray();

    // The layout of the first version is carried over, minus the State
    // column, which is no longer shown by default. Layouts of the Processes
    // tab of earlier versions, which grouped processes by name, are dropped.
    const QString oldDetails = QStringLiteral("details/header-v1");
    bool carriedOver = false;
    if (state.isEmpty()) {
        state = settings.value(oldDetails).toByteArray();
        carriedOver = !state.isEmpty();
    }
    for (const char* key : {"details/header-v1", "processes/header-v1", "processes/header-v2"})
        settings.remove(QString::fromLatin1(key));

    QHeaderView* header = m_view->header();
    if (!state.isEmpty() && header->restoreState(state))
        m_view->sortByColumn(header->sortIndicatorSection(), header->sortIndicatorOrder());
    if (carriedOver)
        m_view->setColumnHidden(ProcColumns::State, true);
    m_view->setColumnHidden(ProcColumns::Name, false);
}

// The processes behind the selected rows. Thread rows carry no PIDs, as
// signals go to a whole process.
ProcessTab::Target ProcessTab::selection() const
{
    Target target;
    QSet<int> seen;
    QStringList names;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows(ProcColumns::Name);
    for (const QModelIndex& index : rows) {
        const QString name = index.data(SortRole).toString();
        if (!names.contains(name))
            names << name;
        const QVariantList pids = index.data(PidsRole).toList();
        for (const QVariant& value : pids) {
            const int pid = value.toInt();
            if (pid > 0 && !seen.contains(pid)) {
                seen.insert(pid);
                target.pids.append(pid);
            }
        }
    }
    target.name = names.join(QStringLiteral(", "));
    return target;
}

void ProcessTab::killSelected()
{
    const Target target = selection();
    if (target.pids.isEmpty())
        return;
    if (m_confirmKill) {
        const QString what = target.pids.size() == 1
            ? tr("%1 (PID %2)").arg(target.name).arg(target.pids.first())
            : tr("%1 processes (%2)").arg(target.pids.size()).arg(target.name);
        QMessageBox box(QMessageBox::Warning, tr("Kill process"), tr("Kill %1?").arg(what),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setInformativeText(tr("The process is ended at once with SIGKILL and cannot save anything. "
                                  "To ask it to quit instead, right-click it and choose Send Signal > SIGTERM."));
        box.setDefaultButton(QMessageBox::Yes);
        if (box.exec() != QMessageBox::Yes)
            return;
    }
    deliver(target.pids, SIGKILL);
}

void ProcessTab::deliver(const QVector<int>& pids, int signo)
{
    QVector<int> denied;
    QStringList errors;
    for (int pid : pids) {
        if (::kill(static_cast<pid_t>(pid), signo) == 0)
            continue;
        const int error = errno;
        if (error == EPERM)
            denied.append(pid);
        else if (error != ESRCH)  // already gone is fine
            errors << tr("PID %1: %2").arg(pid).arg(QString::fromLocal8Bit(std::strerror(error)));
    }
    if (!errors.isEmpty())
        QMessageBox::warning(this, tr("Signal not delivered"), errors.join(QLatin1Char('\n')));
    if (!denied.isEmpty()) {
        QStringList command;
        command << QStringLiteral("kill") << QStringLiteral("-%1").arg(signo);
        for (int pid : std::as_const(denied))
            command << QString::number(pid);
        runAsAdministrator(this,
                           tr("You may not signal PID %1, it belongs to another user.\n\n"
                              "Send the signal as administrator?")
                               .arg(pidList(denied)),
                           command);
    }
}

void ProcessTab::renice(const QVector<int>& pids, int niceValue)
{
    QVector<int> denied;
    for (int pid : pids) {
        if (::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), niceValue) != 0 && (errno == EACCES || errno == EPERM))
            denied.append(pid);
    }
    if (denied.isEmpty())
        return;
    QStringList command;
    command << QStringLiteral("renice") << QStringLiteral("-n") << QString::number(niceValue) << QStringLiteral("-p");
    for (int pid : std::as_const(denied))
        command << QString::number(pid);
    runAsAdministrator(this,
                       tr("Raising the priority of a process (a lower nice value), or changing a process of "
                          "another user, needs administrator rights.\n\nSet nice %1 for PID %2 as administrator?")
                           .arg(niceValue)
                           .arg(pidList(denied)),
                       command);
}

void ProcessTab::showContextMenu(const QPoint& pos)
{
    // Captured now, so the menu acts on what was clicked even if the list
    // keeps moving underneath it.
    const Target target = selection();
    if (target.pids.isEmpty())
        return;
    const QVector<int> pids = target.pids;

    QMenu menu(this);
    QAction* killAction = menu.addAction(tr("Kill") + QStringLiteral("\tDel"));
    connect(killAction, &QAction::triggered, this, &ProcessTab::killSelected);

    QMenu* signalMenu = menu.addMenu(tr("Send Signal"));
    for (const SignalDef& def : kSignals) {
        QAction* action = signalMenu->addAction(QStringLiteral("SIG%1 (%2) - %3")
                                                    .arg(QString::fromLatin1(def.name))
                                                    .arg(def.number)
                                                    .arg(tr(def.description)));
        const int signo = def.number;
        connect(action, &QAction::triggered, this, [this, pids, signo] { deliver(pids, signo); });
    }
    signalMenu->addSeparator();
    QMenu* realtime = signalMenu->addMenu(tr("Real-time signals"));
    const int rtMin = SIGRTMIN;
    const int rtMax = SIGRTMAX;
    for (int signo = rtMin; signo <= rtMax; ++signo) {
        QAction* action = realtime->addAction(QStringLiteral("SIGRTMIN+%1 (%2)").arg(signo - rtMin).arg(signo));
        connect(action, &QAction::triggered, this, [this, pids, signo] { deliver(pids, signo); });
    }

    QMenu* niceMenu = menu.addMenu(tr("Change Priority"));
    struct Preset {
        const char* label;
        int value;
    };
    static const Preset presets[] = {
        {"Very high (nice -20)", -20}, {"High (nice -10)", -10}, {"Normal (nice 0)", 0},
        {"Low (nice 10)", 10},         {"Very low (nice 19)", 19},
    };
    for (const Preset& preset : presets) {
        QAction* action = niceMenu->addAction(tr(preset.label));
        const int value = preset.value;
        connect(action, &QAction::triggered, this, [this, pids, value] { renice(pids, value); });
    }
    niceMenu->addSeparator();
    QAction* custom = niceMenu->addAction(tr("Custom..."));
    connect(custom, &QAction::triggered, this, [this, pids] {
        bool ok = false;
        const int value = QInputDialog::getInt(this, tr("Change Priority"),
                                               tr("Nice value (-20 = highest priority, 19 = lowest):"), 0, -20, 19, 1,
                                               &ok);
        if (ok)
            renice(pids, value);
    });

    menu.addSeparator();
    QAction* copyPid = menu.addAction(pids.size() == 1 ? tr("Copy PID") : tr("Copy PIDs"));
    connect(copyPid, &QAction::triggered, this, [pids] {
        QStringList parts;
        for (int pid : pids)
            parts << QString::number(pid);
        QApplication::clipboard()->setText(parts.join(QLatin1Char(' ')));
    });
    const QModelIndex current = m_view->currentIndex();
    if (current.isValid()) {
        const QString command = current.sibling(current.row(), ProcColumns::Command).data(Qt::DisplayRole).toString();
        if (!command.isEmpty()) {
            QAction* copyCommand = menu.addAction(tr("Copy Command Line"));
            connect(copyCommand, &QAction::triggered, this, [command] { QApplication::clipboard()->setText(command); });
        }
    }

    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void ProcessTab::showHeaderMenu(const QPoint& pos)
{
    QMenu menu(this);
    for (int column = 1; column < ProcColumns::Count; ++column) {  // the name column always stays
        QAction* action = menu.addAction(ProcColumns::title(column));
        action->setCheckable(true);
        action->setChecked(!m_view->isColumnHidden(column));
        connect(action, &QAction::toggled, this, [this, column](bool on) { m_view->setColumnHidden(column, !on); });
    }
    menu.exec(m_view->header()->mapToGlobal(pos));
}
