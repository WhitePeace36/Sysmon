#include "performancetab.h"

#include "format.h"
#include "graphwidget.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

// ---------------------------------------------------------------------------
// Sidebar entry: a small sparkline, a title and two lines of live figures.
// ---------------------------------------------------------------------------

class SidebarButton : public QAbstractButton {
public:
    explicit SidebarButton(QWidget* parent = nullptr) : QAbstractButton(parent)
    {
        setCheckable(true);
        setAutoExclusive(true);
        setAttribute(Qt::WA_Hover, true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setColor(const QColor& color) { m_color = color; }
    void setTitle(const QString& title)
    {
        m_title = title;
        update();
    }
    void setLines(const QString& first, const QString& second = QString())
    {
        m_line1 = first;
        m_line2 = second;
        update();
    }
    // max <= 0 scales the sparkline to its own peak.
    void push(double value, double max)
    {
        m_values.append(std::isnan(value) ? 0.0 : value);
        while (m_values.size() > 60)
            m_values.removeFirst();
        m_max = max;
        update();
    }

    QSize sizeHint() const override { return QSize(230, fontMetrics().height() * 3 + 18); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QPalette pal = palette();
        QColor highlight = pal.color(QPalette::Highlight);
        if (isChecked()) {
            highlight.setAlpha(80);
            p.fillRect(rect(), highlight);
        } else if (underMouse()) {
            highlight.setAlpha(30);
            p.fillRect(rect(), highlight);
        }

        const QRectF spark(8.5, 8.5, 64.0, height() - 17.0);
        p.fillRect(spark, pal.color(QPalette::Base));
        double max = m_max;
        if (max <= 0) {
            max = 1.0;
            for (double v : m_values)
                max = std::max(max, v);
        }
        if (m_values.size() > 1) {
            const qreal step = spark.width() / 59.0;
            QPolygonF points;
            for (int i = 0; i < m_values.size(); ++i) {
                const double ratio = qBound(0.0, m_values.at(i) / max, 1.0);
                points.append(QPointF(spark.right() - (m_values.size() - 1 - i) * step,
                                      spark.bottom() - ratio * (spark.height() - 1.0)));
            }
            QPainterPath line;
            line.addPolygon(points);
            QPainterPath area = line;
            area.lineTo(points.last().x(), spark.bottom());
            area.lineTo(points.first().x(), spark.bottom());
            area.closeSubpath();
            QColor fill = m_color;
            fill.setAlpha(60);
            p.save();
            p.setClipRect(spark);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.fillPath(area, fill);
            p.setPen(QPen(m_color, 1.2));
            p.drawPath(line);
            p.restore();
        }
        p.setPen(m_color);
        p.setBrush(Qt::NoBrush);
        p.drawRect(spark);

        const int x = 82;
        const int w = width() - x - 6;
        const QFontMetrics fm(font());
        int y = 8;
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(pal.color(QPalette::WindowText));
        p.drawText(QRect(x, y, w, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(bold).elidedText(m_title, Qt::ElideRight, w));
        p.setFont(font());
        QColor dim = pal.color(QPalette::WindowText);
        dim.setAlpha(190);
        p.setPen(dim);
        y += fm.height();
        p.drawText(QRect(x, y, w, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(m_line1, Qt::ElideRight, w));
        y += fm.height();
        p.drawText(QRect(x, y, w, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(m_line2, Qt::ElideRight, w));
    }

private:
    QString m_title, m_line1, m_line2;
    QColor m_color = QColor(59, 142, 208);
    QVector<double> m_values;
    double m_max = 100.0;
};

// ---------------------------------------------------------------------------
// Base class of the detail pages.
// ---------------------------------------------------------------------------

class PerfPage : public QWidget {
public:
    explicit PerfPage(SidebarButton* button) : m_button(button) {}
    virtual void refresh(const sys::Snapshot& snapshot) = 0;

protected:
    SidebarButton* m_button;
};

namespace {

enum Category { CatCpu, CatMemory, CatDisk, CatNetwork, CatGpu, CatSensors };

QColor cpuColor() { return QColor(59, 142, 208); }
QColor memColor() { return QColor(155, 89, 182); }
QColor diskReadColor() { return QColor(67, 160, 71); }
QColor diskWriteColor() { return QColor(230, 126, 34); }
QColor netRxColor() { return QColor(211, 84, 0); }
QColor netTxColor() { return QColor(41, 128, 185); }
QColor gpuColor() { return QColor(22, 160, 133); }
QColor tempColor() { return QColor(231, 76, 60); }
QColor fanColor() { return QColor(93, 173, 226); }
QColor clockColor() { return QColor(175, 122, 197); }
QColor powerColor() { return QColor(212, 172, 13); }

QString fmtPercent(double v) { return Format::percent(v); }
QString fmtBytes(double v) { return Format::bytes(v); }
QString fmtRate(double v) { return Format::rate(v); }
QString fmtTemp(double v) { return Format::temperature(v); }
QString fmtFreq(double v) { return Format::frequency(v); }
QString fmtRpm(double v) { return QString::number(v, 'f', 0) + QStringLiteral(" RPM"); }
QString fmtWatt(double v) { return QString::number(v, 'f', 1) + QStringLiteral(" W"); }

QString str(const std::string& s) { return QString::fromStdString(s); }
QString orDash(const QString& s) { return s.isEmpty() ? QStringLiteral("–") : s; }

QLabel* makeTitle(const QString& text)
{
    auto* label = new QLabel(text);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() * 1.7);
    label->setFont(f);
    return label;
}

QLabel* makeSubtitle()
{
    auto* label = new QLabel;
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

GraphWidget* makeGraph(const QString& title, const QColor& color, QString (*formatter)(double))
{
    auto* graph = new GraphWidget(title);
    graph->addSeries(QString(), color);
    graph->setFormatter(formatter);
    return graph;
}

// Shows an optional graph the first time its metric turns out to exist.
void feedOptional(GraphWidget* graph, double value)
{
    if (sys::has(value) && graph->isHidden())
        graph->show();
    graph->append(0, value);
}

// Captioned figures laid out in a grid.
class StatGrid : public QWidget {
public:
    explicit StatGrid(int columns) : m_columns(columns)
    {
        m_grid = new QGridLayout(this);
        m_grid->setContentsMargins(0, 6, 0, 0);
        m_grid->setHorizontalSpacing(24);
        m_grid->setVerticalSpacing(8);
    }

    QLabel* add(const QString& caption)
    {
        auto* cell = new QWidget(this);
        auto* box = new QVBoxLayout(cell);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(1);
        auto* cap = new QLabel(caption, cell);
        QFont small = cap->font();
        small.setPointSizeF(small.pointSizeF() * 0.88);
        cap->setFont(small);
        QPalette pal = cap->palette();
        QColor dim = pal.color(QPalette::WindowText);
        dim.setAlpha(165);
        pal.setColor(QPalette::WindowText, dim);
        cap->setPalette(pal);
        auto* value = new QLabel(QStringLiteral("–"), cell);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont big = value->font();
        big.setPointSizeF(big.pointSizeF() * 1.12);
        value->setFont(big);
        box->addWidget(cap);
        box->addWidget(value);
        m_grid->addWidget(cell, m_count / m_columns, m_count % m_columns);
        ++m_count;
        return value;
    }

private:
    QGridLayout* m_grid = nullptr;
    int m_columns;
    int m_count = 0;
};

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------

class CpuPage : public PerfPage {
public:
    explicit CpuPage(SidebarButton* button) : PerfPage(button)
    {
        button->setTitle(tr("CPU"));
        button->setColor(cpuColor());

        auto* root = new QVBoxLayout(this);
        auto* head = new QHBoxLayout;
        head->addWidget(makeTitle(tr("CPU")));
        head->addStretch(1);
        m_model = makeSubtitle();
        head->addWidget(m_model);
        root->addLayout(head);

        auto* bar = new QHBoxLayout;
        bar->addWidget(new QLabel(tr("Graph:")));
        m_mode = new QComboBox;
        m_mode->addItem(tr("Overall utilization"));
        m_mode->addItem(tr("One graph per logical processor"));
        bar->addWidget(m_mode);
        bar->addStretch(1);
        root->addLayout(bar);

        m_stack = new QStackedWidget;
        m_overall = makeGraph(tr("Utilization"), cpuColor(), fmtPercent);
        m_overall->setFixedMax(100.0);
        m_coreHost = new QWidget;
        m_coreGrid = new QGridLayout(m_coreHost);
        m_coreGrid->setContentsMargins(0, 0, 0, 0);
        m_coreGrid->setSpacing(4);
        m_stack->addWidget(m_overall);
        m_stack->addWidget(m_coreHost);
        root->addWidget(m_stack, 3);

        auto* lower = new QHBoxLayout;
        m_temp = makeGraph(tr("Temperature"), tempColor(), fmtTemp);
        m_temp->setAutoMax(100.0);
        m_fan = makeGraph(tr("Fan speed"), fanColor(), fmtRpm);
        m_fan->setAutoMax(2000.0);
        m_freq = makeGraph(tr("Clock speed"), clockColor(), fmtFreq);
        m_freq->setAutoMax(1000.0);
        for (GraphWidget* g : {m_temp, m_fan, m_freq}) {
            g->hide();
            lower->addWidget(g);
        }
        root->addLayout(lower, 1);

        auto* stats = new StatGrid(4);
        m_usage = stats->add(tr("Utilization"));
        m_speed = stats->add(tr("Speed"));
        m_tempLabel = stats->add(tr("Temperature"));
        m_fanLabel = stats->add(tr("Fan"));
        m_procs = stats->add(tr("Processes"));
        m_threads = stats->add(tr("Threads"));
        m_load = stats->add(tr("Load average (1, 5, 15 min)"));
        m_uptime = stats->add(tr("Up time"));
        m_topology = stats->add(tr("Sockets / cores / logical"));
        m_maxSpeed = stats->add(tr("Maximum speed"));
        m_caches = stats->add(tr("Caches"));
        root->addWidget(stats);

        QSettings settings;
        m_mode->setCurrentIndex(qBound(0, settings.value(QStringLiteral("performance/cpuGraphMode"), 0).toInt(), 1));
        m_stack->setCurrentIndex(m_mode->currentIndex());
        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            m_stack->setCurrentIndex(index);
            QSettings().setValue(QStringLiteral("performance/cpuGraphMode"), index);
        });
    }

    void refresh(const sys::Snapshot& s) override
    {
        const sys::CpuStats& c = s.cpu;
        m_model->setText(str(c.model));

        m_overall->append(0, c.usage);
        const int n = static_cast<int>(c.coreUsage.size());
        if (m_cores.size() != n)
            buildCores(n);
        for (int i = 0; i < n; ++i)
            m_cores.at(i)->append(0, c.coreUsage[static_cast<size_t>(i)]);

        feedOptional(m_temp, c.tempC);
        feedOptional(m_fan, c.fanRpm);
        feedOptional(m_freq, c.freqMHz);
        if (sys::has(c.maxFreqMHz))
            m_freq->setAutoMax(c.maxFreqMHz);

        m_usage->setText(Format::percent(c.usage, 1));
        m_speed->setText(Format::frequency(c.freqMHz));
        m_tempLabel->setText(Format::temperature(c.tempC));
        m_tempLabel->setToolTip(str(c.tempSource));
        m_fanLabel->setText(sys::has(c.fanRpm) ? fmtRpm(c.fanRpm) : QStringLiteral("–"));
        m_fanLabel->setToolTip(str(c.fanSource));
        m_procs->setText(QString::number(c.processes));
        m_threads->setText(QString::number(c.threads));
        m_load->setText(QStringLiteral("%1  %2  %3")
                            .arg(QString::number(c.load1, 'f', 2), QString::number(c.load5, 'f', 2),
                                 QString::number(c.load15, 'f', 2)));
        m_uptime->setText(Format::duration(c.uptimeSec));
        m_topology->setText(QStringLiteral("%1 / %2 / %3").arg(c.sockets).arg(c.cores).arg(c.logical));
        m_maxSpeed->setText(Format::frequency(c.maxFreqMHz));
        m_caches->setText(orDash(str(c.caches)));

        m_button->setLines(Format::percent(c.usage) + QStringLiteral("   ") + Format::frequency(c.freqMHz),
                           sys::has(c.tempC) ? Format::temperature(c.tempC) : QString());
        m_button->push(c.usage, 100.0);
    }

private:
    void buildCores(int n)
    {
        qDeleteAll(m_cores);
        m_cores.clear();
        // A wide grid suits the landscape shape of the page.
        const int columns = n <= 4 ? qMax(1, n) : static_cast<int>(std::ceil(std::sqrt(2.0 * n)));
        for (int i = 0; i < n; ++i) {
            auto* graph = makeGraph(tr("CPU %1").arg(i), cpuColor(), fmtPercent);
            graph->setFixedMax(100.0);
            graph->setCompact(true);
            m_coreGrid->addWidget(graph, i / columns, i % columns);
            m_cores.append(graph);
        }
    }

    QLabel* m_model = nullptr;
    QComboBox* m_mode = nullptr;
    QStackedWidget* m_stack = nullptr;
    GraphWidget* m_overall = nullptr;
    QWidget* m_coreHost = nullptr;
    QGridLayout* m_coreGrid = nullptr;
    QVector<GraphWidget*> m_cores;
    GraphWidget* m_temp = nullptr;
    GraphWidget* m_fan = nullptr;
    GraphWidget* m_freq = nullptr;
    QLabel* m_usage = nullptr;
    QLabel* m_speed = nullptr;
    QLabel* m_tempLabel = nullptr;
    QLabel* m_fanLabel = nullptr;
    QLabel* m_procs = nullptr;
    QLabel* m_threads = nullptr;
    QLabel* m_load = nullptr;
    QLabel* m_uptime = nullptr;
    QLabel* m_topology = nullptr;
    QLabel* m_maxSpeed = nullptr;
    QLabel* m_caches = nullptr;
};

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

class MemoryPage : public PerfPage {
public:
    explicit MemoryPage(SidebarButton* button) : PerfPage(button)
    {
        button->setTitle(tr("Memory"));
        button->setColor(memColor());

        auto* root = new QVBoxLayout(this);
        auto* head = new QHBoxLayout;
        head->addWidget(makeTitle(tr("Memory")));
        head->addStretch(1);
        m_total = makeSubtitle();
        head->addWidget(m_total);
        root->addLayout(head);

        m_usage = makeGraph(tr("Memory in use"), memColor(), fmtBytes);
        root->addWidget(m_usage, 3);
        m_swap = makeGraph(tr("Swap in use"), diskWriteColor(), fmtBytes);
        m_swap->hide();
        root->addWidget(m_swap, 1);

        auto* stats = new StatGrid(4);
        m_used = stats->add(tr("In use"));
        m_available = stats->add(tr("Available"));
        m_cached = stats->add(tr("Cached"));
        m_free = stats->add(tr("Free"));
        m_buffers = stats->add(tr("Buffers"));
        m_shared = stats->add(tr("Shared"));
        m_dirty = stats->add(tr("Waiting to be written"));
        m_swapLabel = stats->add(tr("Swap"));
        root->addWidget(stats);

        m_hwSummary = new QLabel;
        m_hwSummary->setWordWrap(true);
        m_hwSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        root->addWidget(m_hwSummary);

        m_dimms = new QTreeWidget;
        m_dimms->setRootIsDecorated(false);
        m_dimms->setAlternatingRowColors(true);
        m_dimms->setSelectionMode(QAbstractItemView::NoSelection);
        m_dimms->setHeaderLabels({tr("Slot"), tr("Size"), tr("Type"), tr("Form factor"), tr("Speed"),
                                  tr("Rated speed"), tr("Rank"), tr("Manufacturer"), tr("Part number")});
        m_dimms->setMaximumHeight(150);
        m_dimms->hide();
        root->addWidget(m_dimms);
    }

    void refresh(const sys::Snapshot& s) override
    {
        const sys::MemStats& m = s.mem;
        m_total->setText(tr("%1 total").arg(Format::bytes(double(m.total))));
        m_usage->setFixedMax(double(m.total));
        m_usage->append(0, double(m.used()));
        if (m.swapTotal > 0) {
            m_swap->setFixedMax(double(m.swapTotal));
            feedOptional(m_swap, double(m.swapUsed()));
        }

        const double usedPct = m.total ? 100.0 * double(m.used()) / double(m.total) : 0.0;
        m_used->setText(QStringLiteral("%1 (%2)").arg(Format::bytes(double(m.used())), Format::percent(usedPct)));
        m_available->setText(Format::bytes(double(m.available)));
        m_cached->setText(Format::bytes(double(m.cached)));
        m_free->setText(Format::bytes(double(m.free)));
        m_buffers->setText(Format::bytes(double(m.buffers)));
        m_shared->setText(Format::bytes(double(m.shared)));
        m_dirty->setText(Format::bytes(double(m.dirty)));
        m_swapLabel->setText(m.swapTotal ? QStringLiteral("%1 / %2").arg(Format::bytes(double(m.swapUsed())),
                                                                        Format::bytes(double(m.swapTotal)))
                                         : tr("none"));

        if (!m_hardwareShown) {
            m_hardwareShown = true;
            showHardware(m);
        }

        m_button->setLines(QStringLiteral("%1 / %2").arg(Format::bytes(double(m.used())),
                                                         Format::bytes(double(m.total))),
                           Format::percent(usedPct));
        m_button->push(double(m.used()), double(m.total));
    }

private:
    static QString speedText(int mts) { return mts > 0 ? QStringLiteral("%1 MT/s").arg(mts) : QStringLiteral("–"); }

    void showHardware(const sys::MemStats& m)
    {
        if (m.dimms.empty()) {
            m_hwSummary->setText(tr("Module details (type, speed, slots) are not available on this machine. "
                                    "They come from the firmware through udev and need systemd 248 or newer; "
                                    "virtual machines usually do not provide them."));
            return;
        }
        QStringList types, speeds;
        for (const sys::Dimm& d : m.dimms) {
            auto* item = new QTreeWidgetItem(m_dimms);
            item->setText(0, orDash(str(d.locator)));
            item->setText(1, Format::bytes(double(d.sizeBytes), 0));
            item->setText(2, orDash(str(d.type)));
            item->setText(3, orDash(str(d.formFactor)));
            item->setText(4, speedText(d.configuredSpeedMTs > 0 ? d.configuredSpeedMTs : d.speedMTs));
            item->setText(5, speedText(d.speedMTs));
            item->setText(6, d.rank > 0 ? QString::number(d.rank) : QStringLiteral("–"));
            item->setText(7, orDash(str(d.manufacturer)));
            item->setText(8, orDash(str(d.partNumber)));
            if (!d.type.empty() && !types.contains(str(d.type)))
                types << str(d.type);
            const QString speed = speedText(d.configuredSpeedMTs > 0 ? d.configuredSpeedMTs : d.speedMTs);
            if (!speeds.contains(speed))
                speeds << speed;
        }
        for (int i = 0; i < m_dimms->columnCount(); ++i)
            m_dimms->resizeColumnToContents(i);
        m_dimms->show();
        m_hwSummary->setText(tr("Installed modules: %1 of %2 slots used, %3, running at %4")
                                 .arg(m.dimms.size())
                                 .arg(m.slotCount > 0 ? QString::number(m.slotCount) : QStringLiteral("?"))
                                 .arg(types.join(QStringLiteral(" + ")), speeds.join(QStringLiteral(" / "))));
    }

    QLabel* m_total = nullptr;
    GraphWidget* m_usage = nullptr;
    GraphWidget* m_swap = nullptr;
    QLabel* m_used = nullptr;
    QLabel* m_available = nullptr;
    QLabel* m_cached = nullptr;
    QLabel* m_free = nullptr;
    QLabel* m_buffers = nullptr;
    QLabel* m_shared = nullptr;
    QLabel* m_dirty = nullptr;
    QLabel* m_swapLabel = nullptr;
    QLabel* m_hwSummary = nullptr;
    QTreeWidget* m_dimms = nullptr;
    bool m_hardwareShown = false;
};

// ---------------------------------------------------------------------------
// Disk
// ---------------------------------------------------------------------------

class DiskPage : public PerfPage {
public:
    DiskPage(const QString& name, SidebarButton* button) : PerfPage(button), m_name(name.toStdString())
    {
        button->setTitle(tr("Disk (%1)").arg(name));
        button->setColor(diskReadColor());

        auto* root = new QVBoxLayout(this);
        auto* head = new QHBoxLayout;
        head->addWidget(makeTitle(tr("Disk %1").arg(name)));
        head->addStretch(1);
        m_model = makeSubtitle();
        head->addWidget(m_model);
        root->addLayout(head);

        m_rate = new GraphWidget(tr("Transfer rate"));
        m_rate->addSeries(tr("Read"), diskReadColor());
        m_rate->addSeries(tr("Write"), diskWriteColor());
        m_rate->setAutoMax(100.0 * 1024.0);
        m_rate->setFormatter(fmtRate);
        root->addWidget(m_rate, 3);

        m_busy = makeGraph(tr("Active time"), diskReadColor(), fmtPercent);
        m_busy->setFixedMax(100.0);
        root->addWidget(m_busy, 2);

        auto* stats = new StatGrid(4);
        m_read = stats->add(tr("Read speed"));
        m_write = stats->add(tr("Write speed"));
        m_active = stats->add(tr("Active time"));
        m_capacity = stats->add(tr("Capacity"));
        m_totalRead = stats->add(tr("Read since boot"));
        m_totalWritten = stats->add(tr("Written since boot"));
        m_type = stats->add(tr("Type"));
        root->addWidget(stats);

        m_mounts = new QLabel;
        m_mounts->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_mounts->setWordWrap(true);
        root->addWidget(m_mounts);
    }

    void refresh(const sys::Snapshot& s) override
    {
        const sys::DiskStats* d = nullptr;
        for (const sys::DiskStats& candidate : s.disks)
            if (candidate.name == m_name)
                d = &candidate;
        if (!d)
            return;

        QString type;
        if (d->name.rfind("nvme", 0) == 0)
            type = tr("NVMe SSD");
        else if (d->name.rfind("mmcblk", 0) == 0)
            type = tr("SD / eMMC");
        else
            type = d->rotational ? tr("HDD") : tr("SSD");
        if (d->removable)
            type += tr(" (removable)");

        m_model->setText(str(d->model));
        m_rate->append(0, d->readBps);
        m_rate->append(1, d->writeBps);
        m_busy->append(0, d->busyPct);
        m_read->setText(Format::rate(d->readBps));
        m_write->setText(Format::rate(d->writeBps));
        m_active->setText(Format::percent(d->busyPct));
        m_capacity->setText(Format::bytes(double(d->sizeBytes)));
        m_totalRead->setText(Format::bytes(double(d->totalRead)));
        m_totalWritten->setText(Format::bytes(double(d->totalWritten)));
        m_type->setText(type);

        QStringList lines;
        for (const sys::MountStats& m : s.mounts) {
            if (m.disk != m_name)
                continue;
            const double pct = m.total ? 100.0 * double(m.used) / double(m.total) : 0.0;
            lines << tr("%1 (%2, %3): %4 of %5 used (%6), %7 free")
                         .arg(str(m.mountPoint), str(m.device), str(m.fsType), Format::bytes(double(m.used)),
                              Format::bytes(double(m.total)), Format::percent(pct), Format::bytes(double(m.avail)));
        }
        m_mounts->setText(lines.isEmpty() ? QString() : tr("Mounted file systems:\n") + lines.join(QLatin1Char('\n')));

        m_button->setLines(type + QStringLiteral("   ") + Format::percent(d->busyPct),
                           tr("R %1   W %2").arg(Format::rate(d->readBps), Format::rate(d->writeBps)));
        m_button->push(d->busyPct, 100.0);
    }

private:
    std::string m_name;
    QLabel* m_model = nullptr;
    GraphWidget* m_rate = nullptr;
    GraphWidget* m_busy = nullptr;
    QLabel* m_read = nullptr;
    QLabel* m_write = nullptr;
    QLabel* m_active = nullptr;
    QLabel* m_capacity = nullptr;
    QLabel* m_totalRead = nullptr;
    QLabel* m_totalWritten = nullptr;
    QLabel* m_type = nullptr;
    QLabel* m_mounts = nullptr;
};

// ---------------------------------------------------------------------------
// Network adapter
// ---------------------------------------------------------------------------

class NetPage : public PerfPage {
public:
    NetPage(const QString& name, SidebarButton* button) : PerfPage(button), m_name(name.toStdString())
    {
        button->setTitle(name);
        button->setColor(netRxColor());

        auto* root = new QVBoxLayout(this);
        auto* head = new QHBoxLayout;
        head->addWidget(makeTitle(tr("Network %1").arg(name)));
        head->addStretch(1);
        m_kind = makeSubtitle();
        head->addWidget(m_kind);
        root->addLayout(head);

        m_rate = new GraphWidget(tr("Throughput"));
        m_rate->addSeries(tr("Receive"), netRxColor());
        m_rate->addSeries(tr("Send"), netTxColor());
        m_rate->setAutoMax(10.0 * 1024.0);
        m_rate->setFormatter(fmtRate);
        root->addWidget(m_rate, 1);

        auto* stats = new StatGrid(4);
        m_rx = stats->add(tr("Receive"));
        m_tx = stats->add(tr("Send"));
        m_rxTotal = stats->add(tr("Received since boot"));
        m_txTotal = stats->add(tr("Sent since boot"));
        m_state = stats->add(tr("State"));
        m_speed = stats->add(tr("Link speed"));
        m_mac = stats->add(tr("MAC address"));
        m_ipv4 = stats->add(tr("IPv4 address"));
        m_ipv6 = stats->add(tr("IPv6 address"));
        root->addWidget(stats);
    }

    void refresh(const sys::Snapshot& s) override
    {
        const sys::NetStats* n = nullptr;
        for (const sys::NetStats& candidate : s.nets)
            if (candidate.name == m_name)
                n = &candidate;
        if (!n)
            return;

        m_kind->setText(str(n->type));
        m_rate->append(0, n->rxBps);
        m_rate->append(1, n->txBps);
        m_rx->setText(Format::rate(n->rxBps));
        m_tx->setText(Format::rate(n->txBps));
        m_rxTotal->setText(Format::bytes(double(n->rxTotal)));
        m_txTotal->setText(Format::bytes(double(n->txTotal)));
        m_state->setText(orDash(str(n->state)));
        m_speed->setText(n->speedMbps > 0 ? tr("%1 Mbit/s").arg(n->speedMbps) : QStringLiteral("–"));
        m_mac->setText(orDash(str(n->mac)));
        m_ipv4->setText(orDash(str(n->ipv4)));
        m_ipv6->setText(orDash(str(n->ipv6)));

        m_button->setLines(str(n->type) + QStringLiteral(", ") + str(n->state),
                           QStringLiteral("↓ %1   ↑ %2").arg(Format::rate(n->rxBps), Format::rate(n->txBps)));
        m_button->push(n->rxBps + n->txBps, 0.0);
    }

private:
    std::string m_name;
    QLabel* m_kind = nullptr;
    GraphWidget* m_rate = nullptr;
    QLabel* m_rx = nullptr;
    QLabel* m_tx = nullptr;
    QLabel* m_rxTotal = nullptr;
    QLabel* m_txTotal = nullptr;
    QLabel* m_state = nullptr;
    QLabel* m_speed = nullptr;
    QLabel* m_mac = nullptr;
    QLabel* m_ipv4 = nullptr;
    QLabel* m_ipv6 = nullptr;
};

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------

class GpuPage : public PerfPage {
public:
    GpuPage(const QString& id, int number, SidebarButton* button) : PerfPage(button), m_id(id.toStdString())
    {
        button->setTitle(tr("GPU %1").arg(number));
        button->setColor(gpuColor());

        auto* root = new QVBoxLayout(this);
        auto* head = new QHBoxLayout;
        head->addWidget(makeTitle(tr("GPU %1").arg(number)));
        head->addStretch(1);
        m_name = makeSubtitle();
        head->addWidget(m_name);
        root->addLayout(head);

        m_grid = new QGridLayout;
        m_grid->setSpacing(8);
        root->addLayout(m_grid, 1);

        m_usage = makeGraph(tr("Utilization"), gpuColor(), fmtPercent);
        m_usage->setFixedMax(100.0);
        m_memory = makeGraph(tr("Memory in use"), memColor(), fmtBytes);
        m_temp = makeGraph(tr("Temperature"), tempColor(), fmtTemp);
        m_temp->setAutoMax(100.0);
        m_fan = makeGraph(tr("Fan speed"), fanColor(), fmtPercent);
        m_fan->setFixedMax(100.0);
        m_coreClock = makeGraph(tr("GPU clock"), clockColor(), fmtFreq);
        m_coreClock->setAutoMax(500.0);
        m_memClock = makeGraph(tr("Memory clock"), QColor(142, 68, 173), fmtFreq);
        m_memClock->setAutoMax(500.0);
        m_power = makeGraph(tr("Power draw"), powerColor(), fmtWatt);
        m_power->setAutoMax(50.0);
        m_graphs = {m_usage, m_memory, m_temp, m_fan, m_coreClock, m_memClock, m_power};
        // Graphs stay hidden children of the page until their metric shows up.
        for (GraphWidget* g : std::as_const(m_graphs)) {
            g->setParent(this);
            g->hide();
        }

        auto* stats = new StatGrid(4);
        m_usageLabel = stats->add(tr("Utilization"));
        m_memLabel = stats->add(tr("Memory"));
        m_tempLabel = stats->add(tr("Temperature"));
        m_fanLabel = stats->add(tr("Fan"));
        m_coreLabel = stats->add(tr("GPU clock"));
        m_memClockLabel = stats->add(tr("Memory clock"));
        m_powerLabel = stats->add(tr("Power draw"));
        m_driver = stats->add(tr("Driver"));
        root->addWidget(stats);
    }

    void refresh(const sys::Snapshot& s) override
    {
        const sys::GpuStats* g = nullptr;
        for (const sys::GpuStats& candidate : s.gpus)
            if (candidate.id == m_id)
                g = &candidate;
        if (!g)
            return;

        m_name->setText(str(g->name));
        m_usage->setTitle(g->usageApprox ? tr("Utilization (sum of the processes visible to you)") : tr("Utilization"));
        if (sys::has(g->memTotal) && g->memTotal > 0)
            m_memory->setFixedMax(g->memTotal);

        // The fan is reported as a percentage (NVIDIA, AMD pwm) or only as RPM.
        const bool fanAsRpm = !sys::has(g->fanPct) && sys::has(g->fanRpm);
        if (fanAsRpm && !m_fanRpmMode) {
            m_fanRpmMode = true;
            m_fan->setFormatter(fmtRpm);
            m_fan->setAutoMax(2000.0);
        }

        feed(m_usage, g->usage);
        feed(m_memory, g->memUsed);
        feed(m_temp, g->tempC);
        feed(m_fan, m_fanRpmMode ? g->fanRpm : g->fanPct);
        feed(m_coreClock, g->coreClockMHz);
        feed(m_memClock, g->memClockMHz);
        feed(m_power, g->powerW);
        if (m_layoutDirty)
            relayout();

        m_usageLabel->setText(Format::percent(g->usage));
        m_memLabel->setText(sys::has(g->memUsed) ? QStringLiteral("%1 / %2").arg(Format::bytes(g->memUsed),
                                                                                Format::bytes(g->memTotal))
                                                 : QStringLiteral("–"));
        m_tempLabel->setText(Format::temperature(g->tempC));
        QStringList fan;
        if (sys::has(g->fanPct))
            fan << Format::percent(g->fanPct);
        if (sys::has(g->fanRpm))
            fan << fmtRpm(g->fanRpm);
        m_fanLabel->setText(fan.isEmpty() ? QStringLiteral("–") : fan.join(QStringLiteral(", ")));
        m_coreLabel->setText(Format::frequency(g->coreClockMHz));
        m_memClockLabel->setText(Format::frequency(g->memClockMHz));
        m_powerLabel->setText(sys::has(g->powerW) ? fmtWatt(g->powerW) : QStringLiteral("–"));
        m_driver->setText(orDash(str(g->driver)));

        m_button->setLines(str(g->name),
                           Format::percent(g->usage) + QStringLiteral("   ")
                               + (sys::has(g->tempC) ? Format::temperature(g->tempC) : QString()));
        m_button->push(sys::has(g->usage) ? g->usage : 0.0, 100.0);
    }

private:
    void feed(GraphWidget* graph, double value)
    {
        if (sys::has(value) && !m_available.contains(graph)) {
            m_available.insert(graph);
            m_layoutDirty = true;
        }
        graph->append(0, value);
    }

    // Only metrics this GPU actually reports get a graph; two per row.
    void relayout()
    {
        m_layoutDirty = false;
        int shown = 0;
        for (GraphWidget* graph : std::as_const(m_graphs)) {
            m_grid->removeWidget(graph);
            if (m_available.contains(graph)) {
                m_grid->addWidget(graph, shown / 2, shown % 2);
                graph->show();
                ++shown;
            } else {
                graph->hide();
            }
        }
    }

    std::string m_id;
    QLabel* m_name = nullptr;
    QGridLayout* m_grid = nullptr;
    GraphWidget* m_usage = nullptr;
    GraphWidget* m_memory = nullptr;
    GraphWidget* m_temp = nullptr;
    GraphWidget* m_fan = nullptr;
    GraphWidget* m_coreClock = nullptr;
    GraphWidget* m_memClock = nullptr;
    GraphWidget* m_power = nullptr;
    QVector<GraphWidget*> m_graphs;
    QSet<GraphWidget*> m_available;
    bool m_layoutDirty = false;
    bool m_fanRpmMode = false;
    QLabel* m_usageLabel = nullptr;
    QLabel* m_memLabel = nullptr;
    QLabel* m_tempLabel = nullptr;
    QLabel* m_fanLabel = nullptr;
    QLabel* m_coreLabel = nullptr;
    QLabel* m_memClockLabel = nullptr;
    QLabel* m_powerLabel = nullptr;
    QLabel* m_driver = nullptr;
};

// ---------------------------------------------------------------------------
// All temperature and fan sensors
// ---------------------------------------------------------------------------

class SensorsPage : public PerfPage {
public:
    explicit SensorsPage(SidebarButton* button) : PerfPage(button)
    {
        button->setTitle(tr("Sensors"));
        button->setColor(tempColor());

        auto* root = new QVBoxLayout(this);
        root->addWidget(makeTitle(tr("Sensors")));
        m_tree = new QTreeWidget;
        m_tree->setHeaderLabels({tr("Sensor"), tr("Reading")});
        m_tree->setAlternatingRowColors(true);
        m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        root->addWidget(m_tree, 1);
    }

    void refresh(const sys::Snapshot& s) override
    {
        QStringList signature;
        for (const sys::Sensor& x : s.sensors)
            signature << str(x.chip) + QLatin1Char('/') + str(x.label);
        if (signature != m_signature) {
            m_signature = signature;
            m_tree->clear();
            m_items.clear();
            QTreeWidgetItem* chip = nullptr;
            std::string current;
            for (const sys::Sensor& x : s.sensors) {
                if (!chip || x.chip != current) {
                    current = x.chip;
                    chip = new QTreeWidgetItem(m_tree);
                    chip->setText(0, str(x.chip));
                    chip->setExpanded(true);
                }
                auto* item = new QTreeWidgetItem(chip);
                item->setText(0, str(x.label));
                item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                m_items.append(item);
            }
        }

        int temps = 0, fans = 0;
        double hottest = 0;
        for (int i = 0; i < m_items.size() && i < static_cast<int>(s.sensors.size()); ++i) {
            const sys::Sensor& x = s.sensors[static_cast<size_t>(i)];
            if (x.kind == sys::Sensor::Fan) {
                ++fans;
                m_items.at(i)->setText(1, fmtRpm(x.value));
            } else {
                ++temps;
                hottest = std::max(hottest, x.value);
                m_items.at(i)->setText(1, QString::number(x.value, 'f', 1) + QStringLiteral(" °C"));
            }
        }
        m_button->setLines(tr("%1 temperatures, %2 fans").arg(temps).arg(fans),
                           temps ? tr("Hottest: %1").arg(Format::temperature(hottest)) : QString());
        m_button->push(hottest, 100.0);
    }

private:
    QTreeWidget* m_tree = nullptr;
    QVector<QTreeWidgetItem*> m_items;
    QStringList m_signature;
};

}  // namespace

// ---------------------------------------------------------------------------
// PerformanceTab
// ---------------------------------------------------------------------------

PerformanceTab::PerformanceTab(QWidget* parent) : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(250);
    auto* host = new QWidget;
    m_sidebar = new QVBoxLayout(host);
    m_sidebar->setContentsMargins(0, 0, 0, 0);
    m_sidebar->setSpacing(0);
    m_sidebar->addStretch(1);
    scroll->setWidget(host);

    auto* divider = new QFrame;
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Sunken);

    m_pages = new QStackedWidget;
    root->addWidget(scroll);
    root->addWidget(divider);
    root->addWidget(m_pages, 1);
}

// Returns the page for a device, creating it (and its sidebar entry) on first
// sight. Entries stay grouped by category in the order devices appeared.
PerfPage* PerformanceTab::ensure(const QString& key, int category,
                                 const std::function<PerfPage*(SidebarButton*)>& make)
{
    for (const Entry& e : std::as_const(m_entries))
        if (e.key == key)
            return e.page;

    int pos = 0;
    while (pos < m_entries.size() && m_entries.at(pos).category <= category)
        ++pos;

    Entry entry;
    entry.key = key;
    entry.category = category;
    entry.button = new SidebarButton;
    entry.page = make(entry.button);
    m_sidebar->insertWidget(pos, entry.button);
    m_pages->addWidget(entry.page);
    PerfPage* page = entry.page;
    connect(entry.button, &QAbstractButton::clicked, this, [this, page] { m_pages->setCurrentWidget(page); });
    m_entries.insert(pos, entry);
    if (m_entries.size() == 1) {
        entry.button->setChecked(true);
        m_pages->setCurrentWidget(page);
    }
    return page;
}

// Drops the pages of devices that were unplugged.
void PerformanceTab::prune(int category, const QSet<QString>& alive)
{
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        const Entry entry = m_entries.at(i);
        if (entry.category != category || alive.contains(entry.key))
            continue;
        const bool wasCurrent = m_pages->currentWidget() == entry.page;
        m_entries.removeAt(i);
        delete entry.button;
        delete entry.page;
        if (wasCurrent && !m_entries.isEmpty()) {
            m_entries.first().button->setChecked(true);
            m_pages->setCurrentWidget(m_entries.first().page);
        }
    }
}

void PerformanceTab::refresh(const sys::Snapshot& snapshot)
{
    ensure(QStringLiteral("cpu"), CatCpu, [](SidebarButton* b) -> PerfPage* { return new CpuPage(b); });
    ensure(QStringLiteral("memory"), CatMemory, [](SidebarButton* b) -> PerfPage* { return new MemoryPage(b); });

    QSet<QString> alive;
    for (const sys::DiskStats& d : snapshot.disks) {
        const QString name = str(d.name);
        const QString key = QStringLiteral("disk:") + name;
        alive.insert(key);
        ensure(key, CatDisk, [name](SidebarButton* b) -> PerfPage* { return new DiskPage(name, b); });
    }
    prune(CatDisk, alive);

    alive.clear();
    for (const sys::NetStats& n : snapshot.nets) {
        const QString name = str(n.name);
        const QString key = QStringLiteral("net:") + name;
        alive.insert(key);
        ensure(key, CatNetwork, [name](SidebarButton* b) -> PerfPage* { return new NetPage(name, b); });
    }
    prune(CatNetwork, alive);

    alive.clear();
    int number = 0;
    for (const sys::GpuStats& g : snapshot.gpus) {
        const QString id = str(g.id);
        const QString key = QStringLiteral("gpu:") + id;
        alive.insert(key);
        const int index = number++;
        ensure(key, CatGpu, [id, index](SidebarButton* b) -> PerfPage* { return new GpuPage(id, index, b); });
    }
    prune(CatGpu, alive);

    if (!snapshot.sensors.empty())
        ensure(QStringLiteral("sensors"), CatSensors, [](SidebarButton* b) -> PerfPage* { return new SensorsPage(b); });

    for (const Entry& e : std::as_const(m_entries))
        e.page->refresh(snapshot);
}
