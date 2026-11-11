// CPU, memory, disks, network interfaces and hwmon sensors, plus the
// orchestration that assembles a Snapshot.
#include "internal.h"
#include "sysinfo.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace sys {

namespace {

using Clock = std::chrono::steady_clock;

struct CpuTimes {
    uint64_t idle = 0, total = 0;
};

struct DiskPrev {
    uint64_t readSectors = 0, writeSectors = 0, busyMs = 0;
    bool valid = false;
};

struct DiskStatic {
    std::string model;
    uint64_t sizeBytes = 0;
    bool rotational = false, removable = false;
};

struct NetPrev {
    uint64_t rx = 0, tx = 0;
    bool valid = false;
};

struct NetAddr {
    std::string ipv4, ipv6;
};

struct HwSensor {
    std::string chip, label, path;
    Sensor::Kind kind = Sensor::Temperature;
    int cpuScore = 0;  // how likely this is "the" CPU temperature / CPU fan
};

bool isGpuChip(const std::string& chip)
{
    return chip == "amdgpu" || chip == "radeon" || chip == "nouveau" || chip == "nvidia"
        || chip == "i915" || chip == "xe";
}

int cpuTempScore(const std::string& chip, const std::string& label)
{
    const std::string l = toLower(label);
    if (chip == "coretemp")
        return startsWith(l, "package id") ? 100 : 50;
    if (chip == "k10temp" || chip == "zenpower") {
        if (l == "tdie")
            return 100;
        if (l == "tctl")
            return 95;
        return 40;
    }
    if (chip.find("cpu") != std::string::npos || chip == "soc_thermal")
        return 90;
    if (isGpuChip(chip))
        return 0;
    if (l.find("cpu") != std::string::npos)
        return 80;
    if (chip == "acpitz")
        return 20;
    return 0;
}

int cpuFanScore(const std::string& chip, const std::string& label)
{
    if (isGpuChip(chip))
        return 0;
    const std::string l = toLower(label);
    if (l.find("cpu") != std::string::npos)
        return 100;
    if (l.find("gpu") != std::string::npos || l.find("pump") != std::string::npos)
        return 10;
    return 50;
}

// Number following a sysfs attribute prefix: "temp12_input" -> 12, else -1.
int attrIndex(const std::string& name, const char* prefix, const char* suffix)
{
    const size_t pl = std::strlen(prefix), sl = std::strlen(suffix);
    if (name.size() <= pl + sl || name.compare(0, pl, prefix) != 0
        || name.compare(name.size() - sl, sl, suffix) != 0)
        return -1;
    const std::string mid = name.substr(pl, name.size() - pl - sl);
    for (char c : mid)
        if (c < '0' || c > '9')
            return -1;
    return std::atoi(mid.c_str());
}

std::string unescapeMount(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3') {
            out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string runCommand(const char* command)
{
    std::string out;
    FILE* pipe = ::popen(command, "r");
    if (!pipe)
        return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, pipe)) > 0)
        out.append(buf, n);
    ::pclose(pipe);
    return out;
}

}  // namespace

std::vector<Dimm> parseUdevDimms(const std::string& text, int* slotCount)
{
    // systemd's dmi_memory_id udev builtin publishes one group of
    // MEMORY_DEVICE_<n>_<FIELD> properties per slot, readable without root.
    std::map<int, std::map<std::string, std::string>> devices;
    static const char kPrefix[] = "MEMORY_DEVICE_";
    for (const std::string& raw : splitLines(text)) {
        const std::string line = trim(raw);
        if (!startsWith(line, kPrefix))
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(sizeof kPrefix - 1, eq - (sizeof kPrefix - 1));
        const size_t us = key.find('_');
        if (us == std::string::npos || us == 0)
            continue;
        devices[std::atoi(key.substr(0, us).c_str())][key.substr(us + 1)] = trim(line.substr(eq + 1));
    }

    std::vector<Dimm> out;
    for (const auto& entry : devices) {
        const auto& f = entry.second;
        const auto get = [&f](const char* k) {
            const auto it = f.find(k);
            return it == f.end() ? std::string() : it->second;
        };
        if (get("PRESENT") == "0" || get("SIZE").empty())
            continue;
        Dimm m;
        m.sizeBytes = std::strtoull(get("SIZE").c_str(), nullptr, 10);
        if (m.sizeBytes == 0)
            continue;
        m.locator = get("LOCATOR");
        m.type = get("TYPE");
        m.formFactor = get("FORM_FACTOR");
        m.manufacturer = get("MANUFACTURER");
        m.partNumber = get("PART_NUMBER");
        m.speedMTs = std::atoi(get("SPEED_MTS").c_str());
        m.configuredSpeedMTs = std::atoi(get("CONFIGURED_SPEED_MTS").c_str());
        m.rank = std::atoi(get("RANK").c_str());
        out.push_back(m);
    }
    if (slotCount)
        *slotCount = static_cast<int>(devices.size());
    return out;
}

struct Sampler::Impl {
    Clock::time_point last{};
    bool first = true;
    unsigned tick = 0;

    // CPU
    std::string cpuModel, cpuCaches;
    int cores = 0, sockets = 0;
    double maxFreqMHz = NA;
    CpuTimes prevTotal;
    std::vector<CpuTimes> prevCores;

    // Memory hardware
    std::vector<Dimm> dimms;
    int dimmSlots = 0;

    // Disks, network, mounts
    std::map<std::string, DiskPrev> diskPrev;
    std::map<std::string, DiskStatic> diskStatic;
    std::map<std::string, NetPrev> netPrev;
    std::map<std::string, NetAddr> netAddr;
    std::vector<MountStats> mounts;

    // Sensors
    std::vector<HwSensor> hwSensors;
    std::string cpuThermalZone;  // fallback when hwmon has no CPU temperature

    ProcScanner procs;
    GpuMonitor gpu;

    void initCpu();
    void sampleCpu(CpuStats& c);
    void sampleMem(MemStats& m);
    void sampleDisks(double dt, std::vector<DiskStats>& out);
    void sampleNets(double dt, std::vector<NetStats>& out);
    void buildSensors();
    void sampleSensors(Snapshot& s);
    void refreshMounts();
    void refreshAddresses();
};

void Sampler::Impl::initCpu()
{
    std::string text;
    readFile("/proc/cpuinfo", text);
    std::set<std::pair<int, int>> coreIds;
    std::set<int> socketIds;
    int physical = 0;
    for (const std::string& line : splitLines(text)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (cpuModel.empty() && (key == "model name" || key == "Model" || key == "Hardware" || key == "cpu"))
            cpuModel = value;
        else if (key == "physical id") {
            physical = std::atoi(value.c_str());
            socketIds.insert(physical);
        } else if (key == "core id") {
            coreIds.insert({physical, std::atoi(value.c_str())});
        }
    }
    cores = static_cast<int>(coreIds.size());
    sockets = static_cast<int>(socketIds.size());
    if (cpuModel.empty()) {
        struct utsname u;
        if (::uname(&u) == 0)
            cpuModel = u.machine;
    }

    const std::string cacheDir = "/sys/devices/system/cpu/cpu0/cache/";
    for (const std::string& idx : listDir(cacheDir)) {
        if (!startsWith(idx, "index"))
            continue;
        const std::string level = readLine(cacheDir + idx + "/level");
        const std::string type = readLine(cacheDir + idx + "/type");
        const std::string size = readLine(cacheDir + idx + "/size");
        if (level.empty() || size.empty())
            continue;
        if (!cpuCaches.empty())
            cpuCaches += ", ";
        cpuCaches += "L" + level;
        if (type == "Data")
            cpuCaches += "d";
        else if (type == "Instruction")
            cpuCaches += "i";
        cpuCaches += " " + size;
    }

    const long n = ::sysconf(_SC_NPROCESSORS_CONF);
    for (long i = 0; i < n; ++i) {
        long long khz = 0;
        if (readNumber("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq", khz)) {
            const double mhz = khz / 1000.0;
            if (!has(maxFreqMHz) || mhz > maxFreqMHz)
                maxFreqMHz = mhz;
        }
    }
}

void Sampler::Impl::sampleCpu(CpuStats& c)
{
    c.model = cpuModel;
    c.caches = cpuCaches;
    c.maxFreqMHz = maxFreqMHz;

    std::string text;
    readFile("/proc/stat", text);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        if (text.compare(pos, 3, "cpu") != 0)
            break;  // the cpu lines come first
        const char* p = text.c_str() + pos + 3;
        char* end = nullptr;
        int index = -1;
        if (*p != ' ') {
            index = static_cast<int>(std::strtol(p, &end, 10));
            p = end;
        }
        unsigned long long v[8] = {};
        for (int i = 0; i < 8; ++i) {
            v[i] = std::strtoull(p, &end, 10);
            if (end == p)
                break;
            p = end;
        }
        CpuTimes cur;
        cur.idle = v[3] + v[4];  // idle + iowait
        for (int i = 0; i < 8; ++i)
            cur.total += v[i];

        if (index >= 0 && static_cast<size_t>(index) >= prevCores.size())
            prevCores.resize(static_cast<size_t>(index) + 1);
        CpuTimes& prev = index < 0 ? prevTotal : prevCores[static_cast<size_t>(index)];
        double usage = 0;
        if (!first && cur.total > prev.total) {
            const double idle = cur.idle > prev.idle ? double(cur.idle - prev.idle) : 0.0;
            usage = 100.0 * (1.0 - idle / double(cur.total - prev.total));
            usage = std::min(100.0, std::max(0.0, usage));
        }
        prev = cur;
        if (index < 0) {
            c.usage = usage;
        } else {
            if (static_cast<size_t>(index) >= c.coreUsage.size())
                c.coreUsage.resize(static_cast<size_t>(index) + 1, 0.0);
            c.coreUsage[static_cast<size_t>(index)] = usage;
        }
        pos = eol + 1;
    }
    c.logical = static_cast<int>(c.coreUsage.size());
    c.cores = cores > 0 ? cores : c.logical;
    c.sockets = sockets > 0 ? sockets : 1;

    // Current frequency: cpufreq when present, /proc/cpuinfo otherwise.
    c.coreFreqMHz.assign(c.coreUsage.size(), NA);
    bool any = false;
    for (size_t i = 0; i < c.coreFreqMHz.size(); ++i) {
        long long khz = 0;
        if (readNumber("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq", khz)) {
            c.coreFreqMHz[i] = khz / 1000.0;
            any = true;
        }
    }
    if (!any) {
        std::string info;
        readFile("/proc/cpuinfo", info);
        size_t i = 0, at = 0;
        while ((at = info.find("cpu MHz", at)) != std::string::npos && i < c.coreFreqMHz.size()) {
            const size_t colon = info.find(':', at);
            if (colon == std::string::npos)
                break;
            c.coreFreqMHz[i++] = std::atof(info.c_str() + colon + 1);
            at = colon;
        }
    }
    double sum = 0;
    int count = 0;
    for (double f : c.coreFreqMHz) {
        if (has(f) && f > 0) {
            sum += f;
            ++count;
        }
    }
    if (count > 0)
        c.freqMHz = sum / count;

    const std::string load = readLine("/proc/loadavg");
    std::sscanf(load.c_str(), "%lf %lf %lf", &c.load1, &c.load5, &c.load15);
    c.uptimeSec = std::atof(readLine("/proc/uptime").c_str());
}

void Sampler::Impl::sampleMem(MemStats& m)
{
    std::string text;
    readFile("/proc/meminfo", text);
    uint64_t sreclaimable = 0;
    for (const std::string& line : splitLines(text)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = line.substr(0, colon);
        const uint64_t bytes = std::strtoull(line.c_str() + colon + 1, nullptr, 10) * 1024ULL;
        if (key == "MemTotal") m.total = bytes;
        else if (key == "MemAvailable") m.available = bytes;
        else if (key == "MemFree") m.free = bytes;
        else if (key == "Buffers") m.buffers = bytes;
        else if (key == "Cached") m.cached = bytes;
        else if (key == "SReclaimable") sreclaimable = bytes;
        else if (key == "Shmem") m.shared = bytes;
        else if (key == "Dirty") m.dirty = bytes;
        else if (key == "SwapTotal") m.swapTotal = bytes;
        else if (key == "SwapFree") m.swapFree = bytes;
    }
    m.cached += sreclaimable;
    m.dimms = dimms;
    m.slotCount = dimmSlots;
}

void Sampler::Impl::sampleDisks(double dt, std::vector<DiskStats>& out)
{
    std::string text;
    readFile("/proc/diskstats", text);
    std::set<std::string> seen;
    for (const std::string& line : splitLines(text)) {
        const std::vector<std::string> f = splitWs(line);
        if (f.size() < 14)
            continue;
        const std::string& name = f[2];
        if (startsWith(name, "loop") || startsWith(name, "ram") || startsWith(name, "zram"))
            continue;
        const std::string sysDir = "/sys/block/" + name;

        auto st = diskStatic.find(name);
        if (st == diskStatic.end()) {
            if (!pathExists(sysDir))
                continue;  // a partition, not a whole disk
            DiskStatic info;
            long long v = 0;
            if (readNumber(sysDir + "/size", v))
                info.sizeBytes = static_cast<uint64_t>(v) * 512ULL;
            if (readNumber(sysDir + "/queue/rotational", v))
                info.rotational = v != 0;
            if (readNumber(sysDir + "/removable", v))
                info.removable = v != 0;
            info.model = readLine(sysDir + "/device/model");
            if (info.model.empty())
                info.model = readLine(sysDir + "/device/name");
            if (info.model.empty()) {
                const std::string dm = readLine(sysDir + "/dm/name");
                if (!dm.empty())
                    info.model = "Device mapper: " + dm;
                else if (startsWith(name, "md"))
                    info.model = "Software RAID";
            }
            st = diskStatic.emplace(name, info).first;
        }
        if (st->second.sizeBytes == 0) {
            // Empty card reader or optical drive: look again next time.
            diskStatic.erase(st);
            continue;
        }
        seen.insert(name);

        DiskStats d;
        d.name = name;
        d.model = st->second.model;
        d.sizeBytes = st->second.sizeBytes;
        d.rotational = st->second.rotational;
        d.removable = st->second.removable;
        const uint64_t rd = std::strtoull(f[5].c_str(), nullptr, 10);
        const uint64_t wr = std::strtoull(f[9].c_str(), nullptr, 10);
        const uint64_t busy = std::strtoull(f[12].c_str(), nullptr, 10);
        d.totalRead = rd * 512ULL;
        d.totalWritten = wr * 512ULL;
        DiskPrev& prev = diskPrev[name];
        if (prev.valid && dt > 0) {
            if (rd >= prev.readSectors)
                d.readBps = double(rd - prev.readSectors) * 512.0 / dt;
            if (wr >= prev.writeSectors)
                d.writeBps = double(wr - prev.writeSectors) * 512.0 / dt;
            if (busy >= prev.busyMs)
                d.busyPct = std::min(100.0, double(busy - prev.busyMs) / (dt * 10.0));
        }
        prev.readSectors = rd;
        prev.writeSectors = wr;
        prev.busyMs = busy;
        prev.valid = true;
        out.push_back(d);
    }
    for (auto it = diskPrev.begin(); it != diskPrev.end();) {
        if (seen.count(it->first)) {
            ++it;
        } else {
            diskStatic.erase(it->first);
            it = diskPrev.erase(it);
        }
    }
}

void Sampler::Impl::sampleNets(double dt, std::vector<NetStats>& out)
{
    std::string text;
    readFile("/proc/net/dev", text);
    std::set<std::string> seen;
    for (const std::string& line : splitLines(text)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = trim(line.substr(0, colon));
        if (name.empty() || name == "lo")
            continue;
        const std::vector<std::string> f = splitWs(line.substr(colon + 1));
        if (f.size() < 16)
            continue;
        seen.insert(name);

        NetStats n;
        n.name = name;
        n.rxTotal = std::strtoull(f[0].c_str(), nullptr, 10);
        n.txTotal = std::strtoull(f[8].c_str(), nullptr, 10);
        NetPrev& prev = netPrev[name];
        if (prev.valid && dt > 0) {
            if (n.rxTotal >= prev.rx)
                n.rxBps = double(n.rxTotal - prev.rx) / dt;
            if (n.txTotal >= prev.tx)
                n.txBps = double(n.txTotal - prev.tx) / dt;
        }
        prev.rx = n.rxTotal;
        prev.tx = n.txTotal;
        prev.valid = true;

        const std::string sysDir = "/sys/class/net/" + name;
        n.state = readLine(sysDir + "/operstate");
        n.mac = readLine(sysDir + "/address");
        long long speed = 0;
        if (readNumber(sysDir + "/speed", speed) && speed > 0)
            n.speedMbps = static_cast<int>(speed);
        if (pathExists(sysDir + "/wireless") || pathExists(sysDir + "/phy80211"))
            n.type = "Wi-Fi";
        else if (pathExists(sysDir + "/device"))
            n.type = "Ethernet";
        else
            n.type = "Virtual";
        const auto addr = netAddr.find(name);
        if (addr != netAddr.end()) {
            n.ipv4 = addr->second.ipv4;
            n.ipv6 = addr->second.ipv6;
        }
        out.push_back(n);
    }
    for (auto it = netPrev.begin(); it != netPrev.end();)
        it = seen.count(it->first) ? std::next(it) : netPrev.erase(it);
}

void Sampler::Impl::buildSensors()
{
    hwSensors.clear();
    const std::string root = "/sys/class/hwmon/";
    for (const std::string& hw : listDir(root)) {
        const std::string dir = root + hw + "/";
        const std::string chip = readLine(dir + "name");
        std::vector<std::pair<int, HwSensor>> found;
        for (const std::string& file : listDir(dir)) {
            int idx = attrIndex(file, "temp", "_input");
            Sensor::Kind kind = Sensor::Temperature;
            const char* prefix = "temp";
            if (idx < 0) {
                idx = attrIndex(file, "fan", "_input");
                kind = Sensor::Fan;
                prefix = "fan";
            }
            if (idx < 0)
                continue;
            HwSensor s;
            s.chip = chip.empty() ? hw : chip;
            s.kind = kind;
            s.path = dir + file;
            s.label = readLine(dir + prefix + std::to_string(idx) + "_label");
            if (s.label.empty())
                s.label = std::string(prefix) + std::to_string(idx);
            s.cpuScore = kind == Sensor::Temperature ? cpuTempScore(s.chip, s.label)
                                                     : cpuFanScore(s.chip, s.label);
            found.push_back({(kind == Sensor::Fan ? 1000 : 0) + idx, s});
        }
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& f : found)
            hwSensors.push_back(std::move(f.second));
    }

    // Fallback CPU temperature for machines without a CPU hwmon driver.
    cpuThermalZone.clear();
    const std::string tz = "/sys/class/thermal/";
    std::string firstZone;
    for (const std::string& zone : listDir(tz)) {
        if (!startsWith(zone, "thermal_zone"))
            continue;
        const std::string type = toLower(readLine(tz + zone + "/type"));
        if (firstZone.empty())
            firstZone = tz + zone + "/temp";
        if (type.find("x86_pkg_temp") != std::string::npos || type.find("cpu") != std::string::npos
            || type.find("soc") != std::string::npos) {
            cpuThermalZone = tz + zone + "/temp";
            break;
        }
    }
    if (cpuThermalZone.empty())
        cpuThermalZone = firstZone;
}

void Sampler::Impl::sampleSensors(Snapshot& s)
{
    if (tick % 30 == 0)
        buildSensors();

    int bestTemp = 0, bestFan = -1;
    for (const HwSensor& hw : hwSensors) {
        long long raw = 0;
        if (!readNumber(hw.path, raw))
            continue;
        Sensor r;
        r.chip = hw.chip;
        r.label = hw.label;
        r.kind = hw.kind;
        if (hw.kind == Sensor::Temperature) {
            r.value = raw / 1000.0;
            if (r.value < -40.0 || r.value > 200.0)
                continue;  // disconnected or bogus probe
            if (hw.cpuScore > bestTemp) {
                bestTemp = hw.cpuScore;
                s.cpu.tempC = r.value;
                s.cpu.tempSource = hw.chip + " / " + hw.label;
            }
        } else {
            r.value = static_cast<double>(raw);
            // Prefer a fan labelled CPU, then any fan that is actually spinning.
            const int score = hw.cpuScore * 2 + (raw > 0 ? 1 : 0);
            if (hw.cpuScore > 0 && score > bestFan) {
                bestFan = score;
                s.cpu.fanRpm = r.value;
                s.cpu.fanSource = hw.chip + " / " + hw.label;
            }
        }
        s.sensors.push_back(r);
    }
    if (!has(s.cpu.tempC) && !cpuThermalZone.empty()) {
        long long raw = 0;
        if (readNumber(cpuThermalZone, raw)) {
            s.cpu.tempC = raw / 1000.0;
            s.cpu.tempSource = "thermal zone";
        }
    }
}

void Sampler::Impl::refreshMounts()
{
    mounts.clear();
    std::string text;
    readFile("/proc/mounts", text);
    std::set<std::string> seen;
    for (const std::string& line : splitLines(text)) {
        const std::vector<std::string> f = splitWs(line);
        if (f.size() < 3 || !startsWith(f[0], "/dev/") || f[2] == "squashfs")
            continue;
        if (!seen.insert(f[0]).second)
            continue;  // same device mounted more than once (bind mounts, subvolumes)
        MountStats m;
        m.device = f[0];
        m.mountPoint = unescapeMount(f[1]);
        m.fsType = f[2];
        struct statvfs vfs;
        if (::statvfs(m.mountPoint.c_str(), &vfs) != 0 || vfs.f_blocks == 0)
            continue;
        m.total = uint64_t(vfs.f_blocks) * vfs.f_frsize;
        m.avail = uint64_t(vfs.f_bavail) * vfs.f_frsize;
        m.used = m.total - uint64_t(vfs.f_bfree) * vfs.f_frsize;

        // /dev/mapper/x -> dm-0; a partition belongs to its parent directory.
        const std::string real = realPath(m.device);
        const std::string node = baseName(real.empty() ? m.device : real);
        const std::string sysPath = realPath("/sys/class/block/" + node);
        if (!sysPath.empty() && pathExists(sysPath + "/partition"))
            m.disk = baseName(dirName(sysPath));
        else
            m.disk = node;
        mounts.push_back(m);
    }
}

void Sampler::Impl::refreshAddresses()
{
    netAddr.clear();
    struct ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
        return;
    for (struct ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || !a->ifa_name)
            continue;
        const int family = a->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6)
            continue;
        char host[NI_MAXHOST];
        const socklen_t len = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        if (::getnameinfo(a->ifa_addr, len, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0)
            continue;
        std::string text = host;
        const size_t scope = text.find('%');
        if (scope != std::string::npos)
            text.resize(scope);
        NetAddr& entry = netAddr[a->ifa_name];
        if (family == AF_INET) {
            if (entry.ipv4.empty())
                entry.ipv4 = text;
        } else if (entry.ipv6.empty() || (startsWith(entry.ipv6, "fe80") && !startsWith(text, "fe80"))) {
            entry.ipv6 = text;  // prefer a routable address over link-local
        }
    }
    ::freeifaddrs(list);
}

Sampler::Sampler() : d(new Impl)
{
    d->initCpu();
    d->dimms = parseUdevDimms(
        runCommand("udevadm info --query=property --path=/sys/devices/virtual/dmi/id 2>/dev/null"),
        &d->dimmSlots);
}

Sampler::~Sampler() = default;

std::shared_ptr<Snapshot> Sampler::sample()
{
    auto snap = std::make_shared<Snapshot>();
    const Clock::time_point now = Clock::now();
    const double dt = d->first ? 0.0 : std::chrono::duration<double>(now - d->last).count();
    d->last = now;
    snap->intervalSec = dt;

    if (d->tick % 5 == 0) {
        d->refreshMounts();
        d->refreshAddresses();
    }

    d->sampleCpu(snap->cpu);
    d->sampleMem(snap->mem);
    d->sampleDisks(dt, snap->disks);
    d->sampleNets(dt, snap->nets);
    d->sampleSensors(*snap);
    snap->mounts = d->mounts;

    d->procs.scan(dt, d->gpu.wantsFdinfo(), snap->procs);
    d->gpu.sample(d->procs.drmUsageByPdev(), snap->gpus);

    const auto& gpuUse = d->gpu.processUse();
    int threads = 0;
    for (ProcStats& p : snap->procs) {
        threads += p.threads;
        if (gpuUse.empty())
            continue;
        const auto it = gpuUse.find(p.pid);
        if (it != gpuUse.end()) {
            p.gpuPct = std::min(100.0, (has(p.gpuPct) ? p.gpuPct : 0.0) + it->second.pct);
            p.gpuMemBytes += it->second.memBytes;
        }
    }
    snap->cpu.processes = static_cast<int>(snap->procs.size());
    snap->cpu.threads = threads;

    d->first = false;
    ++d->tick;
    return snap;
}

}  // namespace sys
