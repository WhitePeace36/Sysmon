// Backend-internal pieces: the process scanner and the GPU monitor.
#pragma once

#include "sysinfo.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sys {

struct GpuProcUse {
    double pct = 0;
    uint64_t memBytes = 0;
};

// Walks /proc and keeps the per-process state needed to turn counters into
// rates. The expensive part (every file descriptor of every process, needed
// for per-process network and GPU numbers) runs at most every two seconds.
class ProcScanner {
public:
    ProcScanner();
    ~ProcScanner();

    void scan(double dt, bool wantDrm, bool wantThreads, std::vector<ProcStats>& out);

    // GPU load per PCI device, summed from the DRM fdinfo of all processes
    // that could be read. Keyed by PCI address ("0000:00:02.0").
    const std::map<std::string, double>& drmUsageByPdev() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// NVIDIA through NVML (loaded at run time), AMD and Intel through sysfs.
class GpuMonitor {
public:
    GpuMonitor();
    ~GpuMonitor();

    // True if a GPU is present whose per-process load has to come from fdinfo.
    bool wantsFdinfo() const;

    void sample(const std::map<std::string, double>& drmUsageByPdev,
                std::vector<GpuStats>& out);

    // Per-process load reported by NVML during the last sample().
    const std::unordered_map<int, GpuProcUse>& processUse() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Parses `udevadm info --query=property` output for the DMI device.
// Returns the populated memory modules; *slotCount receives the number of slots.
std::vector<Dimm> parseUdevDimms(const std::string& text, int* slotCount);

}  // namespace sys
