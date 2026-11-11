#pragma once

#include "samplerworker.h"

#include <QMainWindow>
#include <QThread>

class QAction;
class QCloseEvent;
class QLabel;
class QTabWidget;
class PerformanceTab;
class ProcessTab;
class ServicesTab;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onSnapshot(SnapshotPtr snapshot);
    void onTabChanged();

private:
    void buildMenus();
    void refreshProcessTabs();
    void setUpdateInterval(int intervalMs);
    void saveSettings();

    QTabWidget* m_tabs = nullptr;
    PerformanceTab* m_performance = nullptr;
    ProcessTab* m_processes = nullptr;
    ProcessTab* m_details = nullptr;
    ServicesTab* m_services = nullptr;
    QLabel* m_statusText = nullptr;

    QAction* m_showKernelThreads = nullptr;
    QAction* m_perCoreCpu = nullptr;
    QAction* m_confirmKill = nullptr;

    QThread m_thread;
    SamplerWorker* m_worker = nullptr;
    SnapshotPtr m_latest;
    int m_intervalMs = 1000;
};
