// GPU monitoring.
//
// NVIDIA: NVML, opened with dlopen() so neither the build nor the binary
//         depends on the NVIDIA driver being installed.
// AMD:    sysfs attributes of the amdgpu driver plus its hwmon device.
// Intel:  frequency from sysfs; load is the sum of the per-process DRM
//         fdinfo statistics gathered by the process scanner.
#include "internal.h"
#include "util.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>

namespace sys {

namespace {

// ---- Minimal NVML declarations (ABI is stable; see nvml.h) -----------------

typedef struct nvmlDevice_st* nvmlDevice_t;

struct nvmlUtilization_t {
    unsigned int gpu, memory;
};
struct nvmlMemory_t {
    unsigned long long total, free, used;
};
struct nvmlProcessInfo_t {
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
};
struct nvmlProcessUtilizationSample_t {
    unsigned int pid;
    unsigned long long timeStamp;
    unsigned int smUtil, memUtil, encUtil, decUtil;
};

constexpr int kNvmlSuccess = 0;
constexpr int kNvmlInsufficientSize = 7;
constexpr int kNvmlClockGraphics = 0;
constexpr int kNvmlClockMem = 2;

struct Nvml {
    void* lib = nullptr;
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*getCount)(unsigned int*) = nullptr;
    int (*getHandle)(unsigned int, nvmlDevice_t*) = nullptr;
    int (*getName)(nvmlDevice_t, char*, unsigned int) = nullptr;
    int (*getUtil)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
    int (*getTemp)(nvmlDevice_t, int, unsigned int*) = nullptr;
    int (*getFan)(nvmlDevice_t, unsigned int*) = nullptr;
    int (*getMem)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
    int (*getClock)(nvmlDevice_t, int, unsigned int*) = nullptr;
    int (*getPower)(nvmlDevice_t, unsigned int*) = nullptr;
    int (*getProcUtil)(nvmlDevice_t, nvmlProcessUtilizationSample_t*, unsigned int*, unsigned long long) = nullptr;
    int (*getCompute)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*) = nullptr;
    int (*getGraphics)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*) = nullptr;

    std::vector<nvmlDevice_t> devices;
    std::vector<std::string> names;
    std::vector<unsigned long long> lastStamp;

    template <typename T>
    void bind(T& fn, const char* name)
    {
        fn = reinterpret_cast<T>(::dlsym(lib, name));
    }

    bool load()
    {
        lib = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!lib)
            lib = ::dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
        if (!lib)
            return false;
        bind(init, "nvmlInit_v2");
        bind(shutdown, "nvmlShutdown");
        bind(getCount, "nvmlDeviceGetCount_v2");
        bind(getHandle, "nvmlDeviceGetHandleByIndex_v2");
        bind(getName, "nvmlDeviceGetName");
        bind(getUtil, "nvmlDeviceGetUtilizationRates");
        bind(getTemp, "nvmlDeviceGetTemperature");
        bind(getFan, "nvmlDeviceGetFanSpeed");
        bind(getMem, "nvmlDeviceGetMemoryInfo");
        bind(getClock, "nvmlDeviceGetClockInfo");
        bind(getPower, "nvmlDeviceGetPowerUsage");
        bind(getProcUtil, "nvmlDeviceGetProcessUtilization");
        // Only the _v2/_v3 entry points use the struct layout declared above.
        bind(getCompute, "nvmlDeviceGetComputeRunningProcesses_v3");
        if (!getCompute)
            bind(getCompute, "nvmlDeviceGetComputeRunningProcesses_v2");
        bind(getGraphics, "nvmlDeviceGetGraphicsRunningProcesses_v3");
        if (!getGraphics)
            bind(getGraphics, "nvmlDeviceGetGraphicsRunningProcesses_v2");

        unsigned int count = 0;
        if (!init || !getCount || !getHandle || init() != kNvmlSuccess) {
            ::dlclose(lib);
            lib = nullptr;
            return false;
        }
        if (getCount(&count) != kNvmlSuccess)
            count = 0;
        for (unsigned int i = 0; i < count; ++i) {
            nvmlDevice_t dev = nullptr;
            if (getHandle(i, &dev) != kNvmlSuccess)
                continue;
            char name[96] = "";
            if (!getName || getName(dev, name, sizeof name) != kNvmlSuccess)
                std::strcpy(name, "NVIDIA GPU");
            devices.push_back(dev);
            names.push_back(name);
            lastStamp.push_back(0);
        }
        return true;
    }

    void unload()
    {
        if (!lib)
            return;
        if (shutdown)
            shutdown();
        ::dlclose(lib);
        lib = nullptr;
    }
};

// ---- DRM cards (AMD, Intel, nouveau) ---------------------------------------

struct DrmCard {
    std::string card;     // card0
    std::string cardDir;  // /sys/class/drm/card0
    std::string devDir;   // /sys/class/drm/card0/device
    std::string hwmon;    // .../device/hwmon/hwmonN, may be empty
    std::string driver, pdev, name;
};

bool isCardName(const std::string& n)
{
    if (!startsWith(n, "card") || n.size() <= 4)
        return false;
    for (size_t i = 4; i < n.size(); ++i)
        if (n[i] < '0' || n[i] > '9')
            return false;
    return true;
}

// Looks a device up in the pci.ids database shipped by hwdata/pciutils.
std::string pciDeviceName(const std::string& vendor, const std::string& device)
{
    static const char* const kFiles[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
                                         "/usr/share/pci.ids"};
    for (const char* file : kFiles) {
        std::string text;
        if (!readFile(file, text, 4 << 20))
            continue;
        bool inVendor = false;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos)
                eol = text.size();
            const size_t len = eol - pos;
            if (len > 6 && text[pos] != '#') {
                if (text[pos] != '\t') {
                    if (inVendor)
                        return std::string();  // left our vendor's block
                    inVendor = text.compare(pos, 4, vendor) == 0;
                } else if (inVendor && text[pos + 1] != '\t' && text.compare(pos + 1, 4, device) == 0) {
                    return trim(text.substr(pos + 5, len - 5));
                }
            }
            pos = eol + 1;
        }
    }
    return std::string();
}

std::string hexId(const std::string& path)
{
    std::string v = toLower(readLine(path));
    if (startsWith(v, "0x"))
        v = v.substr(2);
    while (v.size() < 4)
        v.insert(v.begin(), '0');
    return v;
}

// Active level of an amdgpu pp_dpm_* table: "1: 1200Mhz *".
double activeDpmMHz(const std::string& path)
{
    std::string text;
    if (!readFile(path, text, 4096))
        return NA;
    for (const std::string& line : splitLines(text)) {
        if (line.find('*') == std::string::npos)
            continue;
        const size_t colon = line.find(':');
        if (colon != std::string::npos)
            return std::atof(line.c_str() + colon + 1);
    }
    return NA;
}

double readScaled(const std::string& path, double divisor)
{
    long long v = 0;
    return readNumber(path, v) ? double(v) / divisor : NA;
}

}  // namespace

struct GpuMonitor::Impl {
    Nvml nvml;
    bool nvmlActive = false;
    std::vector<DrmCard> cards;
    std::unordered_map<int, GpuProcUse> procUse;
    // NVML refreshes its per-process samples roughly once a second, so a
    // value is held for a few polls instead of flickering back to zero.
    struct Held {
        double pct = 0;
        int age = 0;
    };
    std::unordered_map<int, Held> heldUtil;

    void findCards();
    void sampleNvml(std::vector<GpuStats>& out);
    void sampleCard(const DrmCard& c, const std::map<std::string, double>& usage, GpuStats& g);
};

void GpuMonitor::Impl::findCards()
{
    const std::string root = "/sys/class/drm/";
    for (const std::string& entry : listDir(root)) {
        if (!isCardName(entry))
            continue;
        DrmCard c;
        c.card = entry;
        c.cardDir = root + entry;
        c.devDir = c.cardDir + "/device";
        c.driver = baseName(linkTarget(c.devDir + "/driver"));
        const std::string vendor = hexId(c.devDir + "/vendor");
        const bool known = vendor == "1002" || vendor == "8086" || vendor == "10de";
        if (!known || !pathExists(c.devDir + "/vendor"))
            continue;
        if (c.driver == "nvidia")
            continue;  // proprietary driver: NVML or nothing
        c.pdev = baseName(realPath(c.devDir));
        const std::vector<std::string> hw = listDir(c.devDir + "/hwmon");
        if (!hw.empty())
            c.hwmon = c.devDir + "/hwmon/" + hw.front();

        const std::string vendorName = vendor == "1002" ? "AMD" : vendor == "8086" ? "Intel" : "NVIDIA";
        std::string model = pciDeviceName(vendor, hexId(c.devDir + "/device"));
        const size_t open = model.rfind('[');
        const size_t close = model.rfind(']');
        if (open != std::string::npos && close != std::string::npos && close > open)
            model = model.substr(open + 1, close - open - 1);  // marketing name
        c.name = model.empty() ? vendorName + " GPU" : vendorName + " " + model;
        cards.push_back(c);
    }
}

void GpuMonitor::Impl::sampleNvml(std::vector<GpuStats>& out)
{
    std::unordered_map<int, double> freshUtil;
    for (size_t i = 0; i < nvml.devices.size(); ++i) {
        nvmlDevice_t dev = nvml.devices[i];
        GpuStats g;
        g.id = "nvidia" + std::to_string(i);
        g.name = nvml.names[i];
        g.driver = "nvidia";

        unsigned int v = 0;
        nvmlUtilization_t util{};
        if (nvml.getUtil && nvml.getUtil(dev, &util) == kNvmlSuccess)
            g.usage = util.gpu;
        if (nvml.getTemp && nvml.getTemp(dev, 0, &v) == kNvmlSuccess)
            g.tempC = v;
        if (nvml.getFan && nvml.getFan(dev, &v) == kNvmlSuccess)
            g.fanPct = v;
        nvmlMemory_t mem{};
        if (nvml.getMem && nvml.getMem(dev, &mem) == kNvmlSuccess) {
            g.memUsed = double(mem.used);
            g.memTotal = double(mem.total);
        }
        if (nvml.getClock && nvml.getClock(dev, kNvmlClockGraphics, &v) == kNvmlSuccess)
            g.coreClockMHz = v;
        if (nvml.getClock && nvml.getClock(dev, kNvmlClockMem, &v) == kNvmlSuccess)
            g.memClockMHz = v;
        if (nvml.getPower && nvml.getPower(dev, &v) == kNvmlSuccess)
            g.powerW = v / 1000.0;
        out.push_back(g);

        if (nvml.getProcUtil) {
            unsigned int count = 0;
            int rc = nvml.getProcUtil(dev, nullptr, &count, nvml.lastStamp[i]);
            if (rc == kNvmlInsufficientSize && count > 0) {
                std::vector<nvmlProcessUtilizationSample_t> samples(count + 8);
                count = static_cast<unsigned int>(samples.size());
                rc = nvml.getProcUtil(dev, samples.data(), &count, nvml.lastStamp[i]);
                if (rc == kNvmlSuccess) {
                    // The buffer holds every sample taken since lastStamp, so a
                    // process usually appears several times (and on the very
                    // first call, with the whole history). Average its samples
                    // on this device instead of adding them up.
                    std::unordered_map<int, std::pair<double, int>> perPid;  // sum, samples
                    for (unsigned int k = 0; k < count && k < samples.size(); ++k) {
                        auto& acc = perPid[static_cast<int>(samples[k].pid)];
                        acc.first += samples[k].smUtil;
                        ++acc.second;
                        nvml.lastStamp[i] = std::max(nvml.lastStamp[i], samples[k].timeStamp);
                    }
                    for (const auto& p : perPid)
                        freshUtil[p.first] += p.second.first / p.second.second;
                }
            }
        }

        // A process that does compute and graphics work ("C+G") is listed by
        // both calls with the same allocation, so take it once per device.
        std::unordered_map<int, unsigned long long> memOnDevice;
        typedef int (*ListFn)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*);
        const ListFn lists[] = {nvml.getCompute, nvml.getGraphics};
        for (ListFn fn : lists) {
            if (!fn)
                continue;
            unsigned int count = 0;
            int rc = fn(dev, &count, nullptr);
            if (rc != kNvmlInsufficientSize || count == 0)
                continue;
            std::vector<nvmlProcessInfo_t> infos(count + 8);
            count = static_cast<unsigned int>(infos.size());
            if (fn(dev, &count, infos.data()) != kNvmlSuccess)
                continue;
            for (unsigned int k = 0; k < count && k < infos.size(); ++k) {
                if (infos[k].usedGpuMemory == ~0ULL)
                    continue;  // "not available"
                unsigned long long& bytes = memOnDevice[static_cast<int>(infos[k].pid)];
                bytes = std::max(bytes, infos[k].usedGpuMemory);
            }
        }
        for (const auto& m : memOnDevice)
            procUse[m.first].memBytes += m.second;
    }

    for (const auto& u : freshUtil) {
        Held& h = heldUtil[u.first];
        h.pct = u.second;
        h.age = 0;
    }
    for (auto it = heldUtil.begin(); it != heldUtil.end();) {
        if (it->second.age++ > 3) {
            it = heldUtil.erase(it);
        } else {
            procUse[it->first].pct = std::min(100.0, it->second.pct);
            ++it;
        }
    }
}

void GpuMonitor::Impl::sampleCard(const DrmCard& c, const std::map<std::string, double>& usage, GpuStats& g)
{
    g.id = c.card;
    g.name = c.name;
    g.driver = c.driver;

    // A GPU in runtime suspend (typically the discrete GPU of a laptop) is
    // woken up by reading most of its sysfs and hwmon attributes, so polling
    // it every second would keep it powered forever. power/runtime_status is
    // kept by the PM core and can be read without waking the device.
    if (readLine(c.devDir + "/power/runtime_status") == "suspended") {
        g.usage = 0;
        return;
    }

    long long v = 0;
    if (readNumber(c.devDir + "/gpu_busy_percent", v)) {
        g.usage = double(v);
    } else {
        const auto it = usage.find(c.pdev);
        if (it != usage.end()) {
            g.usage = it->second;
            g.usageApprox = true;
        } else if (c.driver == "i915" || c.driver == "xe") {
            g.usage = 0;
            g.usageApprox = true;
        }
    }
    if (readNumber(c.devDir + "/mem_info_vram_total", v) && v > 0) {
        g.memTotal = double(v);
        if (readNumber(c.devDir + "/mem_info_vram_used", v))
            g.memUsed = double(v);
    }

    if (!c.hwmon.empty()) {
        g.tempC = readScaled(c.hwmon + "/temp1_input", 1000.0);
        g.fanRpm = readScaled(c.hwmon + "/fan1_input", 1.0);
        long long pwm = 0, pwmMax = 255;
        if (readNumber(c.hwmon + "/pwm1", pwm)) {
            readNumber(c.hwmon + "/pwm1_max", pwmMax);
            if (pwmMax > 0)
                g.fanPct = std::min(100.0, 100.0 * double(pwm) / double(pwmMax));
        }
        // amdgpu: freq1 = core clock (sclk), freq2 = memory clock (mclk).
        for (int i = 1; i <= 2; ++i) {
            const std::string base = c.hwmon + "/freq" + std::to_string(i);
            const double mhz = readScaled(base + "_input", 1e6);
            if (!has(mhz))
                continue;
            const std::string label = readLine(base + "_label");
            if (label == "mclk" || (label.empty() && i == 2))
                g.memClockMHz = mhz;
            else
                g.coreClockMHz = mhz;
        }
        g.powerW = readScaled(c.hwmon + "/power1_average", 1e6);
        if (!has(g.powerW))
            g.powerW = readScaled(c.hwmon + "/power1_input", 1e6);
    }
    if (!has(g.coreClockMHz))
        g.coreClockMHz = activeDpmMHz(c.devDir + "/pp_dpm_sclk");
    if (!has(g.memClockMHz))
        g.memClockMHz = activeDpmMHz(c.devDir + "/pp_dpm_mclk");
    if (!has(g.coreClockMHz)) {
        // Intel: i915 exposes the frequency on the card, xe below the device.
        static const char* const kIntel[] = {"/gt_act_freq_mhz", "/gt_cur_freq_mhz"};
        for (const char* f : kIntel) {
            g.coreClockMHz = readScaled(c.cardDir + f, 1.0);
            if (has(g.coreClockMHz) && g.coreClockMHz > 0)
                break;
        }
        if (!has(g.coreClockMHz))
            g.coreClockMHz = readScaled(c.devDir + "/tile0/gt0/freq0/act_freq", 1.0);
    }
}

GpuMonitor::GpuMonitor() : d(new Impl)
{
    d->nvmlActive = d->nvml.load();
    d->findCards();
}

GpuMonitor::~GpuMonitor()
{
    d->nvml.unload();
}

bool GpuMonitor::wantsFdinfo() const
{
    return !d->cards.empty();
}

const std::unordered_map<int, GpuProcUse>& GpuMonitor::processUse() const
{
    return d->procUse;
}

void GpuMonitor::sample(const std::map<std::string, double>& drmUsageByPdev, std::vector<GpuStats>& out)
{
    d->procUse.clear();
    if (d->nvmlActive)
        d->sampleNvml(out);
    for (const DrmCard& c : d->cards) {
        GpuStats g;
        d->sampleCard(c, drmUsageByPdev, g);
        out.push_back(g);
    }
}

}  // namespace sys
