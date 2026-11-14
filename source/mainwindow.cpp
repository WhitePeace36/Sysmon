#include "mainwindow.h"

#include "format.h"
#include "performancetab.h"
#include "processmodel.h"
#include "processtab.h"
#include "servicestab.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>

MainWindow::MainWindow()
{
    setWindowTitle(tr("System Monitor"));
    resize(1180, 760);

    QSettings settings;
    m_intervalMs = qBound(250, settings.value(QStringLiteral("updateIntervalMs"), 1000).toInt(), 10000);

    m_tabs = new QTabWidget;
    // A tab bar that takes keyboard focus gets an underline under the
    // current tab's title from some styles (Breeze). Tabs are still switched
    // with the mouse, Ctrl+Tab and Ctrl+PgUp/PgDown.
    m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    m_performance = new PerformanceTab;
    m_details = new ProcessTab;
    m_services = new ServicesTab;
    m_tabs->addTab(m_performance, tr("Performance"));
    m_tabs->addTab(m_details, tr("Details"));
    m_tabs->addTab(m_services, tr("Services"));
    setCentralWidget(m_tabs);

    m_statusText = new QLabel;
    statusBar()->addPermanentWidget(m_statusText);

    buildMenus();

    m_details->restoreState(settings);
    m_services->restoreState(settings);
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());

    // Earlier versions saved the index among Performance, Processes, Details
    // and Services; Processes is gone.
    int tab = settings.value(QStringLiteral("window/tab-v2"), -1).toInt();
    if (tab < 0) {
        static const int fromOld[] = {0, 1, 1, 2};
        const int old = settings.value(QStringLiteral("window/tab"), 0).toInt();
        tab = old >= 0 && old < 4 ? fromOld[old] : 0;
        settings.remove(QStringLiteral("window/tab"));
    }
    m_tabs->setCurrentIndex(qBound(0, tab, m_tabs->count() - 1));
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Sampling happens on a worker thread; snapshots arrive through a queued
    // signal and are only ever read from here on.
    m_worker = new SamplerWorker(m_intervalMs);
    m_worker->setCollectThreads(m_tabs->currentWidget() == m_details);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, m_worker, &SamplerWorker::start);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SamplerWorker::sampled, this, &MainWindow::onSnapshot);
    m_thread.start();
}

MainWindow::~MainWindow()
{
    m_thread.quit();
    m_thread.wait();
}

void MainWindow::buildMenus()
{
    QSettings settings;

    QMenu* file = menuBar()->addMenu(tr("&File"));
    QAction* quit = file->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence(QKeySequence::Quit));
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    QMenu* speed = view->addMenu(tr("Update &speed"));
    auto* speedGroup = new QActionGroup(this);
    struct Speed {
        const char* label;
        int ms;
    };
    static const Speed speeds[] = {
        {"High (0.5 s)", 500}, {"Normal (1 s)", 1000}, {"Low (2 s)", 2000}, {"Very low (5 s)", 5000}};
    for (const Speed& s : speeds) {
        QAction* action = speed->addAction(tr(s.label));
        action->setCheckable(true);
        action->setChecked(s.ms == m_intervalMs);
        speedGroup->addAction(action);
        const int ms = s.ms;
        connect(action, &QAction::triggered, this, [this, ms] { setUpdateInterval(ms); });
    }
    view->addSeparator();

    m_showKernelThreads = view->addAction(tr("Show &kernel threads"));
    m_showKernelThreads->setCheckable(true);
    m_showKernelThreads->setChecked(settings.value(QStringLiteral("showKernelThreads"), false).toBool());
    connect(m_showKernelThreads, &QAction::toggled, this, [this] { refreshProcessTabs(); });

    m_perCoreCpu = view->addAction(tr("CPU % relative to &one core (like top)"));
    m_perCoreCpu->setCheckable(true);
    m_perCoreCpu->setChecked(settings.value(QStringLiteral("perCoreCpu"), false).toBool());
    connect(m_perCoreCpu, &QAction::toggled, this, [this] { refreshProcessTabs(); });

    m_confirmKill = view->addAction(tr("&Ask before killing with Del"));
    m_confirmKill->setCheckable(true);
    m_confirmKill->setChecked(settings.value(QStringLiteral("confirmKill"), true).toBool());
    const auto applyConfirm = [this] {
        m_details->setConfirmKill(m_confirmKill->isChecked());
    };
    connect(m_confirmKill, &QAction::toggled, this, applyConfirm);
    applyConfirm();

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    QAction* keys = help->addAction(tr("&Keys and tips"));
    connect(keys, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this, tr("Keys and tips"),
            tr("<p><b>Details</b></p>"
               "<ul>"
               "<li><b>Del</b> kills the selected process (SIGKILL).</li>"
               "<li>Expand a process to see its threads.</li>"
               "<li>Right-click &gt; <b>Send Signal</b> lists every signal; pick SIGTERM to ask a process to quit.</li>"
               "<li>Click a row and type: the selection jumps to the first name that matches.</li>"
               "<li>Click a column header to sort; right-click the header to choose columns.</li>"
               "<li><b>Ctrl+F</b> filters the list.</li>"
               "</ul>"
               "<p><b>Services</b></p>"
               "<ul><li><b>F5</b> reloads the list; right-click a service for its actions.</li></ul>"
               "<p>Network and disk figures of other users' processes are only visible to root.</p>"));
    });
    QAction* aboutQt = help->addAction(tr("About &Qt"));
    connect(aboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::setUpdateInterval(int intervalMs)
{
    m_intervalMs = intervalMs;
    QMetaObject::invokeMethod(m_worker, "setInterval", Qt::QueuedConnection, Q_ARG(int, intervalMs));
}

void MainWindow::onSnapshot(SnapshotPtr snapshot)
{
    m_latest = std::move(snapshot);
    if (!m_latest)
        return;
    const sys::Snapshot& s = *m_latest;

    // The graphs collect history even while another tab is in front.
    m_performance->refresh(s);
    refreshProcessTabs();

    m_statusText->setText(tr("Processes: %1    Threads: %2    CPU: %3    Memory: %4 of %5")
                              .arg(s.cpu.processes)
                              .arg(s.cpu.threads)
                              .arg(Format::percent(s.cpu.usage), Format::bytes(double(s.mem.used())),
                                   Format::bytes(double(s.mem.total))));
}

// Only the process list that is on screen is kept up to date.
void MainWindow::refreshProcessTabs()
{
    if (!m_latest)
        return;
    QWidget* current = m_tabs->currentWidget();
    if (current != m_details)
        return;
    const QVector<ProcRow> rows =
        ProcRow::fromSnapshot(*m_latest, m_showKernelThreads->isChecked(), m_perCoreCpu->isChecked());
    m_details->setRows(rows);
}

// Threads are read only while the Details tab, which lists them, is open.
void MainWindow::onTabChanged()
{
    QMetaObject::invokeMethod(m_worker, "setCollectThreads", Qt::QueuedConnection,
                              Q_ARG(bool, m_tabs->currentWidget() == m_details));
    refreshProcessTabs();
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/tab-v2"), m_tabs->currentIndex());
    settings.setValue(QStringLiteral("updateIntervalMs"), m_intervalMs);
    settings.setValue(QStringLiteral("showKernelThreads"), m_showKernelThreads->isChecked());
    settings.setValue(QStringLiteral("perCoreCpu"), m_perCoreCpu->isChecked());
    settings.setValue(QStringLiteral("confirmKill"), m_confirmKill->isChecked());
    m_details->saveState(settings);
    m_services->saveState(settings);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}
