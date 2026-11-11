// A scrolling line graph with one or more series, painted with QPainter.
// Colours follow the application palette, so it fits light and dark themes.
#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class GraphWidget : public QWidget {
public:
    explicit GraphWidget(const QString& title = QString(), QWidget* parent = nullptr);

    int addSeries(const QString& name, const QColor& color);

    // Appends one sample; NaN leaves a gap in the line.
    void append(int series, double value);

    void setTitle(const QString& title);

    // Fixed scale from zero to max (percentages, memory totals).
    void setFixedMax(double max);
    // Scale follows the data but never drops below floor (rates, clocks).
    void setAutoMax(double floor);

    void setFormatter(std::function<QString(double)> formatter);

    // Compact graphs draw title and value inside the plot (per-core grid).
    void setCompact(bool compact);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Series {
        QString name;
        QColor color;
        QVector<double> values;
    };

    double scaleMax() const;
    QString format(double value) const;

    QString m_title;
    QVector<Series> m_series;
    std::function<QString(double)> m_formatter;
    int m_history = 120;  // samples kept and shown
    int m_ticks = 0;      // samples received; scrolls the grid
    bool m_auto = false;
    double m_max = 100.0;
    bool m_compact = false;
};
