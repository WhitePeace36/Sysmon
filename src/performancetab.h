// Performance tab: a device list on the left (CPU, memory, every disk,
// every network adapter, every GPU, sensors) and a page of graphs and
// figures for the selected device on the right.
#pragma once

#include "sys/sysinfo.h"

#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QStackedWidget;
class QVBoxLayout;
class PerfPage;
class SidebarButton;

class PerformanceTab : public QWidget {
public:
    explicit PerformanceTab(QWidget* parent = nullptr);

    void refresh(const sys::Snapshot& snapshot);

private:
    struct Entry {
        QString key;
        int category = 0;
        SidebarButton* button = nullptr;
        PerfPage* page = nullptr;
    };

    PerfPage* ensure(const QString& key, int category, const std::function<PerfPage*(SidebarButton*)>& make);
    void prune(int category, const QSet<QString>& alive);

    QVector<Entry> m_entries;
    QVBoxLayout* m_sidebar = nullptr;
    QStackedWidget* m_pages = nullptr;
};
