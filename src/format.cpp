#include "format.h"

#include <QChar>

#include <cmath>

namespace Format {

QString bytes(double value, int decimals)
{
    if (std::isnan(value))
        return QStringLiteral("–");
    static const char* const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    int unit = 0;
    double v = value < 0 ? 0 : value;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    return QString::number(v, 'f', unit == 0 ? 0 : decimals) + QLatin1Char(' ') + QString::fromLatin1(units[unit]);
}

QString rate(double bytesPerSecond)
{
    if (std::isnan(bytesPerSecond))
        return QStringLiteral("–");
    return bytes(bytesPerSecond) + QStringLiteral("/s");
}

QString percent(double value, int decimals)
{
    if (std::isnan(value))
        return QStringLiteral("–");
    return QString::number(value, 'f', decimals) + QStringLiteral(" %");
}

QString frequency(double mhz)
{
    if (std::isnan(mhz))
        return QStringLiteral("–");
    if (mhz >= 1000.0)
        return QString::number(mhz / 1000.0, 'f', 2) + QStringLiteral(" GHz");
    return QString::number(mhz, 'f', 0) + QStringLiteral(" MHz");
}

QString temperature(double celsius)
{
    if (std::isnan(celsius))
        return QStringLiteral("–");
    return QString::number(celsius, 'f', 0) + QStringLiteral(" °C");
}

QString duration(double seconds)
{
    qint64 s = static_cast<qint64>(seconds);
    const qint64 days = s / 86400;
    s %= 86400;
    const QString clock = QStringLiteral("%1:%2:%3")
                              .arg(s / 3600, 2, 10, QLatin1Char('0'))
                              .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
                              .arg(s % 60, 2, 10, QLatin1Char('0'));
    return days > 0 ? QStringLiteral("%1d %2").arg(days).arg(clock) : clock;
}

QString schedPolicy(int policy)
{
    switch (policy) {
    case 0: return QStringLiteral("OTHER");
    case 1: return QStringLiteral("FIFO");
    case 2: return QStringLiteral("RR");
    case 3: return QStringLiteral("BATCH");
    case 4: return QStringLiteral("ISO");
    case 5: return QStringLiteral("IDLE");
    case 6: return QStringLiteral("DEADLINE");
    case 7: return QStringLiteral("EXT");
    default: return QString::number(policy);
    }
}

QString procState(char state)
{
    switch (state) {
    case 'R': return QStringLiteral("Running");
    case 'S': return QStringLiteral("Sleeping");
    case 'D': return QStringLiteral("Disk wait");
    case 'I': return QStringLiteral("Idle");
    case 'Z': return QStringLiteral("Zombie");
    case 'T': return QStringLiteral("Stopped");
    case 't': return QStringLiteral("Traced");
    case 'X':
    case 'x': return QStringLiteral("Dead");
    default: return QString(QLatin1Char(state));
    }
}

QString priority(int kernelPriority)
{
    // The kernel reports -100 for the highest real-time priority; top shows "rt".
    return kernelPriority <= -100 ? QStringLiteral("RT") : QString::number(kernelPriority);
}

}  // namespace Format
