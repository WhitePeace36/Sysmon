#include "graphwidget.h"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPolygonF>

#include <cmath>

GraphWidget::GraphWidget(const QString& title, QWidget* parent) : QWidget(parent), m_title(title)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

int GraphWidget::addSeries(const QString& name, const QColor& color)
{
    Series s;
    s.name = name;
    s.color = color;
    m_series.append(s);
    return m_series.size() - 1;
}

void GraphWidget::append(int series, double value)
{
    if (series < 0 || series >= m_series.size())
        return;
    QVector<double>& values = m_series[series].values;
    values.append(value);
    while (values.size() > m_history)
        values.removeFirst();
    if (series == 0)
        ++m_ticks;
    update();
}

void GraphWidget::setTitle(const QString& title)
{
    if (m_title == title)
        return;
    m_title = title;
    update();
}

void GraphWidget::setFixedMax(double max)
{
    m_auto = false;
    m_max = max > 0 ? max : 1.0;
    update();
}

void GraphWidget::setAutoMax(double floor)
{
    m_auto = true;
    m_max = floor > 0 ? floor : 1.0;
    update();
}

void GraphWidget::setFormatter(std::function<QString(double)> formatter)
{
    m_formatter = std::move(formatter);
    update();
}

void GraphWidget::setCompact(bool compact)
{
    m_compact = compact;
    update();
}

QSize GraphWidget::sizeHint() const
{
    return m_compact ? QSize(120, 70) : QSize(320, 150);
}

QSize GraphWidget::minimumSizeHint() const
{
    return m_compact ? QSize(48, 32) : QSize(120, 64);
}

QString GraphWidget::format(double value) const
{
    if (std::isnan(value))
        return QStringLiteral("–");
    return m_formatter ? m_formatter(value) : QString::number(value, 'f', 0);
}

// Upper end of the scale: the configured maximum, or for auto-scaled graphs
// the largest visible value rounded up to 1, 2 or 5 times a power of ten.
double GraphWidget::scaleMax() const
{
    if (!m_auto)
        return m_max;
    double peak = 0;
    for (const Series& s : m_series)
        for (double v : s.values)
            if (!std::isnan(v) && v > peak)
                peak = v;
    if (peak <= m_max)
        return m_max;
    const double magnitude = std::pow(10.0, std::floor(std::log10(peak)));
    const double fraction = peak / magnitude;
    const double nice = fraction <= 1.0 ? 1.0 : fraction <= 2.0 ? 2.0 : fraction <= 5.0 ? 5.0 : 10.0;
    return nice * magnitude;
}

void GraphWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QPalette pal = palette();
    const QFontMetrics fm(font());
    const int header = m_compact ? 0 : fm.height() + 6;
    const QRectF plot(0.5, header + 0.5, width() - 1.0, height() - header - 1.0);
    if (plot.width() < 8 || plot.height() < 8)
        return;

    const QColor textColor = pal.color(QPalette::WindowText);
    const QColor accent = m_series.isEmpty() ? pal.color(QPalette::Highlight) : m_series.first().color;

    // Header: title on the left, current value(s) on the right.
    if (!m_compact) {
        p.setPen(textColor);
        p.drawText(QRectF(0, 0, width(), header), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        qreal right = width();
        for (int i = m_series.size() - 1; i >= 0; --i) {
            const Series& s = m_series.at(i);
            const double current = s.values.isEmpty() ? std::nan("") : s.values.last();
            QString text = format(current);
            if (!s.name.isEmpty())
                text = s.name + QLatin1Char(' ') + text;
            const int w = fm.horizontalAdvance(text);
            p.setPen(m_series.size() > 1 ? s.color : textColor);
            p.drawText(QRectF(right - w, 0, w + 1, header), Qt::AlignLeft | Qt::AlignVCenter, text);
            right -= w + 16;
        }
    }

    p.fillRect(plot, pal.color(QPalette::Base));

    // Grid: fixed horizontal lines, vertical lines that scroll with the data.
    QColor grid = pal.color(QPalette::Text);
    grid.setAlpha(26);
    p.setPen(QPen(grid, 1));
    for (int i = 1; i < 4; ++i) {
        const qreal y = std::floor(plot.top() + plot.height() * i / 4.0) + 0.5;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    const qreal step = plot.width() / qreal(m_history - 1);
    const int every = 10;
    for (qreal x = plot.right() - (m_ticks % every) * step; x > plot.left(); x -= every * step) {
        const qreal gx = std::floor(x) + 0.5;
        p.drawLine(QPointF(gx, plot.top()), QPointF(gx, plot.bottom()));
    }

    // Series, newest sample at the right edge.
    const double maxValue = scaleMax();
    p.save();
    p.setClipRect(plot);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const Series& s : m_series) {
        const int n = s.values.size();
        QPolygonF run;
        const auto flush = [&]() {
            if (run.isEmpty())
                return;
            if (run.size() == 1)
                run.append(run.first() + QPointF(1.0, 0.0));
            QPainterPath line;
            line.addPolygon(run);
            QPainterPath area = line;
            area.lineTo(run.last().x(), plot.bottom());
            area.lineTo(run.first().x(), plot.bottom());
            area.closeSubpath();
            QColor fill = s.color;
            fill.setAlpha(m_series.size() > 1 ? 36 : 56);
            p.fillPath(area, fill);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(s.color, 1.5));
            p.drawPath(line);
            run.clear();
        };
        for (int i = 0; i < n; ++i) {
            const double v = s.values.at(i);
            if (std::isnan(v)) {
                flush();
                continue;
            }
            const double ratio = qBound(0.0, v / maxValue, 1.0);
            run.append(QPointF(plot.right() - (n - 1 - i) * step, plot.bottom() - ratio * (plot.height() - 1.0)));
        }
        flush();
    }
    p.restore();

    // Frame in the colour of the first series.
    QColor frame = accent;
    frame.setAlpha(170);
    p.setPen(QPen(frame, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(plot);

    // Labels inside the plot.
    QFont labelFont = font();
    labelFont.setPointSizeF(qMax(6.0, labelFont.pointSizeF() * 0.82));
    p.setFont(labelFont);
    QColor dim = textColor;
    dim.setAlpha(170);
    p.setPen(dim);
    const QRectF inner = plot.adjusted(4, 2, -4, -2);
    if (m_compact) {
        p.drawText(inner, Qt::AlignLeft | Qt::AlignTop, m_title);
        if (!m_series.isEmpty() && !m_series.first().values.isEmpty())
            p.drawText(inner, Qt::AlignRight | Qt::AlignTop, format(m_series.first().values.last()));
    } else if (m_auto) {
        p.drawText(inner, Qt::AlignLeft | Qt::AlignTop, format(maxValue));
    }
}
