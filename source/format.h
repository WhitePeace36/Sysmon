// Human-readable formatting shared by all tabs.
#pragma once

#include <QString>

namespace Format {

QString bytes(double value, int decimals = 1);  // 1.5 GiB
QString rate(double bytesPerSecond);            // 1.5 MiB/s
QString percent(double value, int decimals = 0);
QString frequency(double mhz);                  // 3.42 GHz
QString temperature(double celsius);            // 54 °C
QString duration(double seconds);               // 3d 04:12:55
QString schedPolicy(int policy);                // OTHER, FIFO, RR, ...
QString procState(char state);                  // Running, Sleeping, ...
QString priority(int kernelPriority);           // like top's PR column

}  // namespace Format
