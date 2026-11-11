// Data model and public entry point of the sampling backend.
//
// Everything in src/sys is plain C++17 with no Qt dependency. A Sampler is
// polled periodically and returns an immutable Snapshot that the UI renders.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace sys {

// "Not available": used for every reading the machine may simply not expose.
constexpr double NA = std::numeric_limits<double>::quiet_NaN();
inline bool has(double v) { return !std::isnan(v); }

struct CpuStats {
    std::string model;
    std::string caches;
    int logical = 0;
    int cores = 0;
    int sockets = 0;

    double usage = 0;                 // 0..100, all cores
    std::vector<double> coreUsage;    // 0..100 per logical CPU
    std::vector<double> coreFreqMHz;  // NA where unknown
    double freqMHz = NA;              // average of the cores
    double maxFreqMHz = NA;

    double tempC = NA;
    std::string tempSource;
    double fanRpm = NA;
    std::string fanSource;

    double load1 = 0, load5 = 0, load15 = 0;
    double uptimeSec = 0;
    int processes = 0;
    int threads = 0;
};

struct Dimm {
    std::string locator;
    std::string type;          // DDR4, DDR5, ...
    std::string formFactor;    // DIMM, SODIMM, ...
    std::string manufacturer;
    std::string partNumber;
    uint64_t sizeBytes = 0;
    int speedMTs = 0;            // rated speed
    int configuredSpeedMTs = 0;  // speed it actually runs at
    int rank = 0;
};

struct MemStats {
    uint64_t total = 0, available = 0, free = 0;
    uint64_t buffers = 0, cached = 0, shared = 0, dirty = 0;
    uint64_t swapTotal = 0, swapFree = 0;
    uint64_t used() const { return total > available ? total - available : 0; }
    uint64_t swapUsed() const { return swapTotal > swapFree ? swapTotal - swapFree : 0; }

    // Hardware description (from the firmware's DMI tables, read once).
    std::vector<Dimm> dimms;  // populated slots only
    int slotCount = 0;        // all module slots, 0 if unknown
};

struct DiskStats {
    std::string name;   // sda, nvme0n1, ...
    std::string model;
    uint64_t sizeBytes = 0;
    bool rotational = false;
    bool removable = false;
    double readBps = 0, writeBps = 0;
    double busyPct = 0;  // share of the interval the device had I/O in flight
    uint64_t totalRead = 0, totalWritten = 0;
};

struct MountStats {
    std::string device, mountPoint, fsType;
    std::string disk;  // owning whole disk, matches DiskStats::name
    uint64_t total = 0, used = 0, avail = 0;
};

struct NetStats {
    std::string name;
    std::string type;   // Ethernet, Wi-Fi, Virtual
    std::string state;  // up, down, ...
    std::string mac, ipv4, ipv6;
    int speedMbps = -1;
    double rxBps = 0, txBps = 0;
    uint64_t rxTotal = 0, txTotal = 0;
};

struct GpuStats {
    std::string id;      // stable key: nvidia0, card1, ...
    std::string name;
    std::string driver;
    double usage = NA;          // 0..100
    bool usageApprox = false;   // summed from per-process stats (Intel)
    double memUsed = NA, memTotal = NA;  // bytes
    double tempC = NA;
    double fanPct = NA, fanRpm = NA;
    double coreClockMHz = NA, memClockMHz = NA;
    double powerW = NA;
};

struct Sensor {
    enum Kind { Temperature, Fan };
    std::string chip, label;
    Kind kind = Temperature;
    double value = 0;  // degrees Celsius or RPM
};

struct ProcStats {
    int pid = 0, ppid = 0;
    std::string name;     // executable name
    std::string cmdline;
    std::string user;
    char state = '?';
    bool kernelThread = false;

    double cpuPct = 0;        // 100 = one full core
    double gpuPct = NA;       // NA = process does not use a GPU / unknown
    uint64_t gpuMemBytes = 0;
    uint64_t memBytes = 0;    // resident minus shared: memory private to it
    uint64_t rssBytes = 0;    // full resident set
    double netRxBps = NA, netTxBps = NA;       // TCP only; NA = not readable
    double diskReadBps = NA, diskWriteBps = NA;  // NA = not readable

    int nice = 0;
    int priority = 0;   // kernel priority as shown by top's PR column
    int policy = 0;     // SCHED_* constant
    int threads = 0;
};

struct Snapshot {
    double intervalSec = 0;  // time since the previous snapshot (0 for the first)
    CpuStats cpu;
    MemStats mem;
    std::vector<DiskStats> disks;
    std::vector<MountStats> mounts;
    std::vector<NetStats> nets;
    std::vector<GpuStats> gpus;
    std::vector<Sensor> sensors;
    std::vector<ProcStats> procs;
};

class Sampler {
public:
    Sampler();
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    // Rates are computed against the previous call, so the first snapshot
    // reports zero for every rate.
    std::shared_ptr<Snapshot> sample();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace sys
