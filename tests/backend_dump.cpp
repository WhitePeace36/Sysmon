// Prints what the backend sees, without any GUI. Handy for checking a new
// machine:   sysmon-dump [samples]
#include "sys/internal.h"
#include "sys/sysinfo.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace sys;

static void print(const Snapshot& s)
{
    std::printf("=== interval %.2fs ===\n", s.intervalSec);
    const CpuStats& c = s.cpu;
    std::printf("CPU   %s | %d sockets, %d cores, %d logical | caches: %s\n", c.model.c_str(), c.sockets, c.cores,
                c.logical, c.caches.c_str());
    std::printf("      usage %.1f%% | freq %.0f MHz (max %.0f) | temp %.1f C [%s] | fan %.0f rpm [%s]\n", c.usage,
                c.freqMHz, c.maxFreqMHz, c.tempC, c.tempSource.c_str(), c.fanRpm, c.fanSource.c_str());
    std::printf("      load %.2f %.2f %.2f | up %.0fs | %d processes, %d threads | per core:", c.load1, c.load5,
                c.load15, c.uptimeSec, c.processes, c.threads);
    for (double u : c.coreUsage)
        std::printf(" %.0f%%", u);
    std::printf("\n");

    const MemStats& m = s.mem;
    std::printf("MEM   used %.2f / %.2f GiB | avail %.2f | cached %.2f | swap %.2f / %.2f GiB | %zu DIMMs in %d slots\n",
                m.used() / 1073741824.0, m.total / 1073741824.0, m.available / 1073741824.0,
                m.cached / 1073741824.0, m.swapUsed() / 1073741824.0, m.swapTotal / 1073741824.0, m.dimms.size(),
                m.slotCount);
    for (const Dimm& d : m.dimms)
        std::printf("      %s: %.0f GiB %s %s @ %d MT/s (rated %d) %s %s\n", d.locator.c_str(),
                    d.sizeBytes / 1073741824.0, d.type.c_str(), d.formFactor.c_str(), d.configuredSpeedMTs,
                    d.speedMTs, d.manufacturer.c_str(), d.partNumber.c_str());

    for (const DiskStats& d : s.disks)
        std::printf("DISK  %-10s %-28s %.1f GiB %s | R %.0f KiB/s W %.0f KiB/s busy %.1f%%\n", d.name.c_str(),
                    d.model.c_str(), d.sizeBytes / 1073741824.0, d.rotational ? "HDD" : "SSD", d.readBps / 1024,
                    d.writeBps / 1024, d.busyPct);
    for (const MountStats& mt : s.mounts)
        std::printf("MOUNT %-20s on %-16s %-6s disk=%-8s %.1f / %.1f GiB\n", mt.device.c_str(),
                    mt.mountPoint.c_str(), mt.fsType.c_str(), mt.disk.c_str(), mt.used / 1073741824.0,
                    mt.total / 1073741824.0);
    for (const NetStats& n : s.nets)
        std::printf("NET   %-10s %-8s %-6s %s %s %s | rx %.1f KiB/s tx %.1f KiB/s | link %d Mb/s\n", n.name.c_str(),
                    n.type.c_str(), n.state.c_str(), n.mac.c_str(), n.ipv4.c_str(), n.ipv6.c_str(),
                    n.rxBps / 1024, n.txBps / 1024, n.speedMbps);
    for (const GpuStats& g : s.gpus)
        std::printf("GPU   %s [%s] usage %.0f%%%s | mem %.0f / %.0f MiB | %.0f C | fan %.0f%% %.0f rpm | core %.0f MHz "
                    "mem %.0f MHz | %.1f W\n",
                    g.name.c_str(), g.driver.c_str(), g.usage, g.usageApprox ? " (approx)" : "",
                    g.memUsed / 1048576.0, g.memTotal / 1048576.0, g.tempC, g.fanPct, g.fanRpm, g.coreClockMHz,
                    g.memClockMHz, g.powerW);
    for (const Sensor& x : s.sensors)
        std::printf("SENS  %-12s %-16s %.1f %s\n", x.chip.c_str(), x.label.c_str(), x.value,
                    x.kind == Sensor::Fan ? "rpm" : "C");

    std::vector<const ProcStats*> procs;
    int netReadable = 0, ioReadable = 0, kthreads = 0;
    for (const ProcStats& p : s.procs) {
        procs.push_back(&p);
        netReadable += has(p.netRxBps);
        ioReadable += has(p.diskReadBps);
        kthreads += p.kernelThread;
    }
    std::sort(procs.begin(), procs.end(), [](const ProcStats* a, const ProcStats* b) {
        const double ka = a->cpuPct + (has(a->netRxBps) ? a->netRxBps + a->netTxBps : 0) / 1e3;
        const double kb = b->cpuPct + (has(b->netRxBps) ? b->netRxBps + b->netTxBps : 0) / 1e3;
        return ka > kb;
    });
    std::printf("PROCS %zu total, %d kernel threads, net readable for %d, io readable for %d\n", s.procs.size(),
                kthreads, netReadable, ioReadable);
    std::printf("  %7s %-20s %-8s S %6s %9s %9s %9s %9s %9s %3s %4s %2s  %s\n", "PID", "NAME", "USER", "CPU%",
                "MEM(M)", "NETRX", "NETTX", "DISKR", "DISKW", "NI", "PRI", "SC", "CMD");
    for (size_t i = 0; i < procs.size() && i < 8; ++i) {
        const ProcStats& p = *procs[i];
        std::printf("  %7d %-20.20s %-8.8s %c %6.1f %9.1f %9.0f %9.0f %9.0f %9.0f %3d %4d %2d  %.50s\n", p.pid,
                    p.name.c_str(), p.user.c_str(), p.state, p.cpuPct, p.memBytes / 1048576.0, p.netRxBps,
                    p.netTxBps, p.diskReadBps, p.diskWriteBps, p.nice, p.priority, p.policy, p.cmdline.c_str());
    }
}

int main(int argc, char** argv)
{
    const int samples = argc > 1 ? std::atoi(argv[1]) : 3;
    Sampler sampler;
    for (int i = 0; i < samples; ++i) {
        if (i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        print(*sampler.sample());
    }

    // Self-check of the DMI parser with a canned example.
    int slotCount = 0;
    const std::vector<Dimm> dimms = parseUdevDimms(
        "MEMORY_ARRAY_NUM_DEVICES=4\n"
        "MEMORY_DEVICE_0_PRESENT=0\n"
        "MEMORY_DEVICE_1_SIZE=17179869184\nMEMORY_DEVICE_1_TYPE=DDR4\nMEMORY_DEVICE_1_SPEED_MTS=3600\n"
        "MEMORY_DEVICE_1_CONFIGURED_SPEED_MTS=3200\nMEMORY_DEVICE_1_LOCATOR=DIMM_A2\n"
        "MEMORY_DEVICE_1_FORM_FACTOR=DIMM\nMEMORY_DEVICE_1_MANUFACTURER=ACME\nMEMORY_DEVICE_1_PART_NUMBER=X1\n"
        "MEMORY_DEVICE_2_PRESENT=0\n"
        "MEMORY_DEVICE_3_SIZE=17179869184\nMEMORY_DEVICE_3_TYPE=DDR4\nMEMORY_DEVICE_3_SPEED_MTS=3600\n",
        &slotCount);
    const bool ok = slotCount == 4 && dimms.size() == 2 && dimms[0].locator == "DIMM_A2" && dimms[0].speedMTs == 3600
                 && dimms[0].configuredSpeedMTs == 3200 && dimms[0].sizeBytes == 17179869184ULL;
    std::printf("DMI parser self-check: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
