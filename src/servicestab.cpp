#include "servicestab.h"

#include "processmodel.h"
#include "typeaheadview.h"

#include <QAction>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <utility>

// ---------------------------------------------------------------------------
// ServiceModel
// ---------------------------------------------------------------------------

ServiceModel::ServiceModel(QObject* parent) : QAbstractTableModel(parent) {}

int ServiceModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int ServiceModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(Count);
}

const ServiceRow* ServiceModel::rowAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? &m_rows.at(row) : nullptr;
}

QVariant ServiceModel::data(const QModelIndex& index, int role) const
{
    const ServiceRow* r = index.isValid() ? rowAt(index.row()) : nullptr;
    if (!r)
        return QVariant();
    if (role == Qt::DisplayRole || role == SortRole) {
        switch (index.column()) {
        case Unit: return r->unit;
        case Active: return r->active;
        case Sub: return r->sub;
        case FileState: return r->fileState;
        case Load: return r->load;
        case Description: return r->description;
        default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (index.column() == Active || index.column() == Sub) {
            if (r->active == QLatin1String("active"))
                return QColor(39, 174, 96);
            if (r->active == QLatin1String("failed"))
                return QColor(231, 76, 60);
            if (r->active.endsWith(QLatin1String("ing")))  // activating, deactivating, reloading
                return QColor(230, 126, 34);
        }
        if (index.column() == FileState && r->fileState.startsWith(QLatin1String("masked")))
            return QColor(231, 76, 60);
    }
    if (role == Qt::ToolTipRole && index.column() == Description)
        return r->description;
    return QVariant();
}

QVariant ServiceModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();
    switch (section) {
    case Unit: return tr("Service");
    case Active: return tr("Active");
    case Sub: return tr("State");
    case FileState: return tr("At boot");
    case Load: return tr("Loaded");
    case Description: return tr("Description");
    default: return QVariant();
    }
}

// In-place update keyed by unit name, so the selection is kept.
void ServiceModel::setRows(const QVector<ServiceRow>& rows)
{
    QHash<QString, int> incoming;
    incoming.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i)
        incoming.insert(rows.at(i).unit, i);

    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (incoming.contains(m_rows.at(i).unit))
            continue;
        beginRemoveRows(QModelIndex(), i, i);
        m_rows.removeAt(i);
        endRemoveRows();
    }

    QSet<QString> present;
    for (ServiceRow& row : m_rows) {
        present.insert(row.unit);
        row = rows.at(incoming.value(row.unit));
    }
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, Count - 1));

    QVector<ServiceRow> fresh;
    for (const ServiceRow& row : rows)
        if (!present.contains(row.unit))
            fresh.append(row);
    if (!fresh.isEmpty()) {
        beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size() + fresh.size() - 1);
        m_rows += fresh;
        endInsertRows();
    }
}

// ---------------------------------------------------------------------------
// ServicesTab
// ---------------------------------------------------------------------------

ServicesTab::ServicesTab(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    auto* top = new QHBoxLayout;
    m_scope = new QComboBox;
    m_scope->addItem(tr("System services"));
    m_scope->addItem(tr("User services"));
    top->addWidget(m_scope);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter services (Ctrl+F)"));
    m_filter->setClearButtonEnabled(true);
    top->addWidget(m_filter, 1);
    root->addLayout(top);

    auto* bar = new QToolBar;
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_start = makeAction(tr("Start"), QStringLiteral("start"), tr("Start the selected service"));
    m_stop = makeAction(tr("Stop"), QStringLiteral("stop"), tr("Stop the selected service"));
    m_restart = makeAction(tr("Restart"), QStringLiteral("restart"), tr("Stop and start the selected service"));
    m_enable = makeAction(tr("Enable"), QStringLiteral("enable"), tr("Start the service automatically at boot"));
    m_disable = makeAction(tr("Disable"), QStringLiteral("disable"), tr("Do not start the service at boot"));
    m_mask = makeAction(tr("Mask"), QStringLiteral("mask"),
                        tr("Make it impossible to start the service, by hand or as a dependency"));
    m_unmask = makeAction(tr("Unmask"), QStringLiteral("unmask"), tr("Undo Mask"));
    m_refresh = new QAction(tr("Refresh"), this);
    m_refresh->setShortcut(QKeySequence(QKeySequence::Refresh));
    m_refresh->setToolTip(tr("Reload the list (F5). It also refreshes itself every few seconds."));
    connect(m_refresh, &QAction::triggered, this, &ServicesTab::reload);
    bar->addAction(m_start);
    bar->addAction(m_stop);
    bar->addAction(m_restart);
    bar->addSeparator();
    bar->addAction(m_enable);
    bar->addAction(m_disable);
    bar->addSeparator();
    bar->addAction(m_mask);
    bar->addAction(m_unmask);
    bar->addSeparator();
    bar->addAction(m_refresh);
    auto* barRow = new QHBoxLayout;
    barRow->addWidget(bar);
    barRow->addStretch(1);
    m_summary = new QLabel;
    barRow->addWidget(m_summary);
    root->addLayout(barRow);

    auto* splitter = new QSplitter(Qt::Vertical);
    m_view = new TypeAheadView;
    m_view->setRootIsDecorated(false);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setSearchColumn(ServiceModel::Unit);
    m_model = new ServiceModel(this);
    m_proxy = new RowSortProxy(this);
    m_proxy->setFilterColumns({ServiceModel::Unit, ServiceModel::Active, ServiceModel::Sub, ServiceModel::FileState,
                               ServiceModel::Description});
    m_proxy->setSourceModel(m_model);
    m_view->setModel(m_proxy);
    m_view->setColumnWidth(ServiceModel::Unit, 320);
    m_view->sortByColumn(ServiceModel::Unit, Qt::AscendingOrder);
    splitter->addWidget(m_view);

    m_status = new QPlainTextEdit;
    m_status->setReadOnly(true);
    m_status->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_status->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_status->setPlaceholderText(tr("Select a service to see its status."));
    splitter->addWidget(m_status);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(5000);
    connect(m_timer, &QTimer::timeout, this, &ServicesTab::reload);

    connect(m_filter, &QLineEdit::textChanged, m_proxy, &RowSortProxy::setFilterText);
    connect(m_scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        m_loading = false;  // abandon a reply for the other scope
        m_model->setRows(QVector<ServiceRow>());
        m_status->clear();
        m_statusUnit.clear();
        reload();
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, &ServicesTab::showContextMenu);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &ServicesTab::onCurrentChanged);

    auto* find = new QShortcut(QKeySequence(QKeySequence::Find), this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] {
        m_filter->setFocus();
        m_filter->selectAll();
    });
    addAction(m_refresh);
    m_refresh->setShortcutContext(Qt::WidgetWithChildrenShortcut);

    updateActions();
}

QAction* ServicesTab::makeAction(const QString& text, const QString& verb, const QString& tip)
{
    auto* action = new QAction(text, this);
    action->setToolTip(tip);
    connect(action, &QAction::triggered, this, [this, verb] { perform(verb); });
    return action;
}

void ServicesTab::saveState(QSettings& settings) const
{
    settings.setValue(QStringLiteral("services/header-v1"), m_view->header()->saveState());
    settings.setValue(QStringLiteral("services/scope"), m_scope->currentIndex());
}

void ServicesTab::restoreState(QSettings& settings)
{
    const QByteArray state = settings.value(QStringLiteral("services/header-v1")).toByteArray();
    QHeaderView* header = m_view->header();
    if (!state.isEmpty() && header->restoreState(state))
        m_view->sortByColumn(header->sortIndicatorSection(), header->sortIndicatorOrder());
    m_scope->setCurrentIndex(qBound(0, settings.value(QStringLiteral("services/scope"), 0).toInt(), 1));
}

// The list is only kept fresh while the tab is on screen.
void ServicesTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    reload();
    m_timer->start();
}

void ServicesTab::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

QStringList ServicesTab::scopeArguments() const
{
    return m_scope->currentIndex() == 1 ? QStringList{QStringLiteral("--user")} : QStringList();
}

// Runs systemctl without blocking the interface and hands the result to done.
void ServicesTab::run(const QStringList& arguments, const Done& done)
{
    auto* process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SYSTEMD_COLORS"), QStringLiteral("0"));
    env.insert(QStringLiteral("SYSTEMD_PAGER"), QStringLiteral("cat"));
    process->setProcessEnvironment(env);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [process, done](int exitCode, QProcess::ExitStatus status) {
                done(status == QProcess::NormalExit ? exitCode : -1,
                     QString::fromLocal8Bit(process->readAllStandardOutput()),
                     QString::fromLocal8Bit(process->readAllStandardError()));
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;  // other errors are followed by finished()
        done(-1, QString(), ServicesTab::tr("systemctl could not be started. Is systemd installed?"));
        process->deleteLater();
    });
    process->start(QStringLiteral("systemctl"), arguments);
}

void ServicesTab::reload()
{
    if (m_loading)
        return;
    m_loading = true;
    const int generation = ++m_generation;
    const QStringList scope = scopeArguments();
    const QStringList common{QStringLiteral("--type=service"), QStringLiteral("--plain"),
                             QStringLiteral("--no-legend"), QStringLiteral("--no-pager")};

    // Loaded units carry the live state; unit files add what is installed but
    // not loaded, and whether a unit is enabled or masked.
    run(scope + QStringList{QStringLiteral("list-units"), QStringLiteral("--all")} + common,
        [this, generation, scope, common](int exitCode, const QString& units, const QString& error) {
            if (generation != m_generation)
                return;
            if (exitCode != 0 && units.trimmed().isEmpty()) {
                m_loading = false;
                m_summary->setText(error.trimmed().isEmpty() ? tr("systemctl failed") : error.trimmed());
                return;
            }
            run(scope + QStringList{QStringLiteral("list-unit-files")} + common,
                [this, generation, units](int, const QString& unitFiles, const QString&) {
                    if (generation != m_generation)
                        return;
                    m_loading = false;
                    apply(units, unitFiles);
                });
        });
}

void ServicesTab::apply(const QString& units, const QString& unitFiles)
{
    const QString suffix = QStringLiteral(".service");
    QMap<QString, ServiceRow> rows;

    const QStringList unitLines = units.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : unitLines) {
        // UNIT LOAD ACTIVE SUB DESCRIPTION..., possibly led by a status bullet.
        QStringList parts = line.simplified().split(QLatin1Char(' '));
        if (!parts.isEmpty() && !parts.first().contains(QLatin1Char('.')))
            parts.removeFirst();
        if (parts.size() < 4 || !parts.first().endsWith(suffix))
            continue;
        if (parts.at(1) == QLatin1String("not-found"))
            continue;  // only referenced by another unit; there is nothing to manage
        ServiceRow row;
        row.unit = parts.at(0);
        row.load = parts.at(1);
        row.active = parts.at(2);
        row.sub = parts.at(3);
        row.description = QStringList(parts.mid(4)).join(QLatin1Char(' '));
        rows.insert(row.unit, row);
    }

    const QStringList fileLines = unitFiles.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : fileLines) {
        // UNIT-FILE STATE [PRESET]
        const QStringList parts = line.simplified().split(QLatin1Char(' '));
        if (parts.size() < 2 || !parts.first().endsWith(suffix))
            continue;
        ServiceRow& row = rows[parts.at(0)];
        if (row.unit.isEmpty()) {
            row.unit = parts.at(0);
            row.load = QStringLiteral("not loaded");
            row.active = QStringLiteral("inactive");
            row.sub = QStringLiteral("dead");
        }
        row.fileState = parts.at(1);
    }

    QVector<ServiceRow> list;
    list.reserve(rows.size());
    int running = 0, failed = 0;
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        list.append(it.value());
        running += it.value().sub == QLatin1String("running");
        failed += it.value().active == QLatin1String("failed");
    }
    m_model->setRows(list);
    m_summary->setText(tr("%1 services, %2 running, %3 failed").arg(list.size()).arg(running).arg(failed));
    updateActions();
    refreshStatus();
}

const ServiceRow* ServicesTab::current() const
{
    const QModelIndex index = m_view->currentIndex();
    if (!index.isValid())
        return nullptr;
    return m_model->rowAt(m_proxy->mapToSource(index).row());
}

void ServicesTab::onCurrentChanged()
{
    updateActions();
    refreshStatus();
}

void ServicesTab::updateActions()
{
    const ServiceRow* row = current();
    const bool usable = row && !m_busy;
    const QString active = row ? row->active : QString();
    const QString file = row ? row->fileState : QString();
    const bool masked = row && (file.startsWith(QLatin1String("masked")) || row->load == QLatin1String("masked"));
    const bool running = active == QLatin1String("active") || active.endsWith(QLatin1String("ing"));
    const bool isTemplate = row && row->unit.endsWith(QLatin1String("@.service"));

    m_start->setEnabled(usable && !running && !masked && !isTemplate);
    m_stop->setEnabled(usable && running);
    m_restart->setEnabled(usable && !masked && !isTemplate);
    m_enable->setEnabled(usable && file == QLatin1String("disabled"));
    m_disable->setEnabled(usable && file.startsWith(QLatin1String("enabled")));
    m_mask->setEnabled(usable && !masked && !file.isEmpty());
    m_unmask->setEnabled(usable && masked);
}

void ServicesTab::perform(const QString& verb)
{
    const ServiceRow* row = current();
    if (!row || m_busy)
        return;
    const QString unit = row->unit;
    m_busy = true;
    updateActions();
    run(scopeArguments() + QStringList{verb, unit}, [this, verb, unit](int exitCode, const QString& out, const QString& error) {
        m_busy = false;
        if (exitCode != 0) {
            QString message = (error.trimmed().isEmpty() ? out : error).trimmed();
            if (message.contains(QLatin1String("authentication"), Qt::CaseInsensitive))
                message += tr("\n\nManaging system services as a normal user needs a running polkit "
                              "authentication agent (every desktop environment starts one).");
            QMessageBox::warning(this, tr("systemctl %1 %2").arg(verb, unit), message);
        }
        m_loading = false;
        reload();
    });
}

void ServicesTab::refreshStatus()
{
    const ServiceRow* row = current();
    if (!row) {
        m_status->clear();
        m_statusUnit.clear();
        return;
    }
    const QString unit = row->unit;
    run(scopeArguments() + QStringList{QStringLiteral("status"), QStringLiteral("--no-pager"), QStringLiteral("--full"),
                                       QStringLiteral("--lines=25"), unit},
        [this, unit](int, const QString& out, const QString& error) {
            const ServiceRow* now = current();
            if (!now || now->unit != unit)
                return;  // the selection moved on while systemctl was running
            const QString text = out.trimmed().isEmpty() ? error.trimmed() : out;
            if (m_statusUnit == unit && m_status->toPlainText() == text)
                return;
            // Keep the reading position when the same unit is refreshed.
            const int scroll = m_statusUnit == unit ? m_status->verticalScrollBar()->value() : 0;
            m_status->setPlainText(text);
            m_status->verticalScrollBar()->setValue(scroll);
            m_statusUnit = unit;
        });
}

void ServicesTab::showContextMenu(const QPoint& pos)
{
    if (!current())
        return;
    QMenu menu(this);
    menu.addAction(m_start);
    menu.addAction(m_stop);
    menu.addAction(m_restart);
    menu.addSeparator();
    menu.addAction(m_enable);
    menu.addAction(m_disable);
    menu.addSeparator();
    menu.addAction(m_mask);
    menu.addAction(m_unmask);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
