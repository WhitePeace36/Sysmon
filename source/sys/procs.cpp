// Process table.
//
// Cheap, every sample: /proc/<pid>/{stat,statm,io,cmdline}.
// Expensive, at most every two seconds: the open file descriptors of every
// process we may inspect. Those give us
//   * the sockets a process holds, matched against the kernel's per-socket
//     TCP byte counters (sock_diag) for per-process network traffic, and
//   * its DRM clients, whose fdinfo carries per-process GPU time and memory
//     on AMD and Intel drivers.
#include "internal.h"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>

namespace sys {

namespace {

using Clock = std::chrono::steady_clock;

constexpr unsigned long kPfKthread = 0x00200000;
constexpr double kHeavyIntervalSec = 1.9;

struct SockCount {
    uint64_t tx = 0, rx = 0;
};

struct PidState {
    uint64_t seen = 0;
    uint64_t startTicks = 0;
    uint64_t cpuTicks = 0;
    bool fresh = true;

    bool ioOk = false;
    bool ioDenied = false;  // /proc/<pid>/io could not be read last time
    uint64_t ioRead = 0, ioWrite = 0;

    std::string comm, name, cmdline, user;
    int refreshIn = 0;

    // Results of the last file-descriptor scan.
    bool fdOk = false;
    uint64_t netRxAcc = 0, netTxAcc = 0;
    double netRxBps = 0, netTxBps = 0;
    bool hasDrm = false;
    double gpuPct = 0;
    uint64_t gpuMem = 0;
    std::map<std::string, uint64_t> drmNs;                           // "pdev/engine" -> busy ns
    std::map<std::string, std::pair<uint64_t, uint64_t>> drmCycles;  // "pdev/class" -> cycles, total
};

struct StatFields {
    std::string comm;
    char state = '?';
    int ppid = 0;
    unsigned long flags = 0;
    uint64_t utime = 0, stime = 0;
    long priority = 0, nice = 0, threads = 0;
    uint64_t start = 0;
    int policy = 0;
};

// The command name may itself contain spaces and parentheses, so the fixed
// fields are counted from the last ')'.
bool parseStat(const std::string& s, StatFields& f)
{
    const size_t open = s.find('(');
    const size_t close = s.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open)
        return false;
    f.comm = s.substr(open + 1, close - open - 1);

    const char* p = s.c_str() + close + 1;
    const char* const end = s.c_str() + s.size();
    int idx = 0;  // field number minus 3: idx 0 is the state
    while (p < end) {
        while (p < end && *p == ' ')
            ++p;
        if (p >= end || *p == '\n')
            break;
        const char* tok = p;
        while (p < end && *p != ' ' && *p != '\n')
            ++p;
        switch (idx) {
        case 0: f.state = *tok; break;
        case 1: f.ppid = std::atoi(tok); break;
        case 6: f.flags = std::strtoul(tok, nullptr, 10); break;
        case 11: f.utime = std::strtoull(tok, nullptr, 10); break;
        case 12: f.stime = std::strtoull(tok, nullptr, 10); break;
        case 15: f.priority = std::strtol(tok, nullptr, 10); break;
        case 16: f.nice = std::strtol(tok, nullptr, 10); break;
        case 17: f.threads = std::strtol(tok, nullptr, 10); break;
        case 19: f.start = std::strtoull(tok, nullptr, 10); break;
        case 38: f.policy = std::atoi(tok); break;
        default: break;
        }
        ++idx;
    }
    return idx > 19;
}

uint64_t fieldAfter(const std::string& text, const char* key)
{
    const size_t at = text.find(key);
    if (at == std::string::npos)
        return 0;
    return std::strtoull(text.c_str() + at + std::strlen(key), nullptr, 10);
}

uint64_t toKiB(const std::string& value)
{
    char* end = nullptr;
    const uint64_t n = std::strtoull(value.c_str(), &end, 10);
    const std::string unit = trim(end ? std::string(end) : std::string());
    if (unit == "KiB")
        return n;
    if (unit == "MiB")
        return n * 1024ULL;
    if (unit == "GiB")
        return n * 1024ULL * 1024ULL;
    return n / 1024ULL;  // plain bytes
}

struct DrmClient {
    std::string pdev, id;
    std::map<std::string, uint64_t> ns;
    std::map<std::string, std::pair<uint64_t, uint64_t>> cycles;
    uint64_t vramKiB = 0, totalKiB = 0;
    bool hasVram = false;
};

// See Documentation/gpu/drm-usage-stats.rst in the kernel tree.
bool parseFdinfo(const std::string& text, DrmClient& c)
{
    for (const std::string& line : splitLines(text)) {
        if (!startsWith(line, "drm-"))
            continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = line.substr(0, colon);
        const std::string value = trim(line.substr(colon + 1));
        if (key == "drm-pdev")
            c.pdev = value;
        else if (key == "drm-client-id")
            c.id = value;
        else if (startsWith(key, "drm-engine-capacity-"))
            continue;
        else if (startsWith(key, "drm-engine-"))
            c.ns[key.substr(11)] = std::strtoull(value.c_str(), nullptr, 10);
        else if (startsWith(key, "drm-total-cycles-"))
            c.cycles[key.substr(17)].second = std::strtoull(value.c_str(), nullptr, 10);
        else if (startsWith(key, "drm-cycles-"))
            c.cycles[key.substr(11)].first = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "drm-memory-vram") {
            c.hasVram = true;
            c.vramKiB = toKiB(value);
        } else if (startsWith(key, "drm-total-"))
            c.totalKiB += toKiB(value);
    }
    return !c.id.empty();
}

bool isLoopback(const inet_diag_msg* msg)
{
    const unsigned char* dst = reinterpret_cast<const unsigned char*>(msg->id.idiag_dst);
    if (msg->idiag_family == AF_INET)
        return dst[0] == 127;
    static const unsigned char v6Loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const unsigned char v4Mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(dst, v6Loopback, 16) == 0)
        return true;
    return std::memcmp(dst, v4Mapped, 12) == 0 && dst[12] == 127;
}

// Dumps every TCP socket of one address family with its tcp_info. The kernel
// allows this without privileges; it is what `ss -ti` does.
bool dumpTcpSockets(int family, std::unordered_map<uint32_t, SockCount>& out)
{
    const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return false;

    struct {
        nlmsghdr nlh;
        inet_diag_req_v2 req;
    } request;
    std::memset(&request, 0, sizeof request);
    request.nlh.nlmsg_len = sizeof request;
    request.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.req.sdiag_family = static_cast<__u8>(family);
    request.req.sdiag_protocol = IPPROTO_TCP;
    request.req.idiag_states = ~0U;
    request.req.idiag_ext = 1 << (INET_DIAG_INFO - 1);

    if (::send(fd, &request, sizeof request, 0) < 0) {
        ::close(fd);
        return false;
    }

    const size_t needed = offsetof(tcp_info, tcpi_bytes_received) + sizeof(__u64);
    std::vector<char> buf(64 * 1024);
    bool ok = true, done = false;
    while (!done) {
        const ssize_t got = ::recv(fd, buf.data(), buf.size(), 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (got == 0)
            break;
        int len = static_cast<int>(got);
        for (nlmsghdr* h = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(h, len); h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_type == NLMSG_DONE) {
                done = true;
                break;
            }
            if (h->nlmsg_type == NLMSG_ERROR) {
                ok = false;
                done = true;
                break;
            }
            if (h->nlmsg_type != SOCK_DIAG_BY_FAMILY)
                continue;
            const inet_diag_msg* msg = static_cast<const inet_diag_msg*>(NLMSG_DATA(h));
            if (msg->idiag_inode == 0 || isLoopback(msg))
                continue;
            int attrLen = static_cast<int>(h->nlmsg_len) - static_cast<int>(NLMSG_LENGTH(sizeof *msg));
            for (rtattr* a = reinterpret_cast<rtattr*>(const_cast<inet_diag_msg*>(msg) + 1); RTA_OK(a, attrLen);
                 a = RTA_NEXT(a, attrLen)) {
                if (a->rta_type != INET_DIAG_INFO || RTA_PAYLOAD(a) < needed)
                    continue;
                tcp_info info;
                std::memset(&info, 0, sizeof info);
                std::memcpy(&info, RTA_DATA(a), std::min<size_t>(RTA_PAYLOAD(a), sizeof info));
                SockCount& c = out[msg->idiag_inode];
                c.tx += info.tcpi_bytes_acked;
                c.rx += info.tcpi_bytes_received;
            }
        }
    }
    ::close(fd);
    return ok;
}

}  // namespace

struct ProcScanner::Impl {
    const long clkTck = ::sysconf(_SC_CLK_TCK);
    const long pageSize = ::sysconf(_SC_PAGESIZE);

    uint64_t generation = 0;
    std::unordered_map<int, PidState> states;
    std::unordered_map<unsigned, std::string> users;

    Clock::time_point lastHeavy{};
    bool heavyDone = false;
    bool netSupported = true;
    bool sockBaseline = false;
    std::unordered_map<uint32_t, SockCount> sockPrev;
    std::map<std::string, double> pdevUsage;

    const std::string& userName(unsigned uid);
    bool readProcess(int pid, double dt, ProcStats& p, PidState*& stateOut);
    void refreshIdentity(int pid, PidState& st);
    void collectSocketDeltas(std::unordered_map<uint32_t, SockCount>& deltas);
    void scanFds(int pid, PidState& st, double dt, bool wantDrm,
                 const std::unordered_map<uint32_t, SockCount>& deltas,
                 std::unordered_map<uint32_t, int>& owners);
};

const std::string& ProcScanner::Impl::userName(unsigned uid)
{
    const auto it = users.find(uid);
    if (it != users.end())
        return it->second;
    std::string name = std::to_string(uid);
    struct passwd pw;
    struct passwd* result = nullptr;
    char buf[4096];
    if (::getpwuid_r(uid, &pw, buf, sizeof buf, &result) == 0 && result)
        name = pw.pw_name;
    return users.emplace(uid, name).first->second;
}

// Owner, command line and display name. These rarely change, so they are
// re-read only now and then, and whenever the kernel-side name changes (exec).
void ProcScanner::Impl::refreshIdentity(int pid, PidState& st)
{
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d", pid);
    struct stat sb;
    if (::stat(path, &sb) == 0)
        st.user = userName(sb.st_uid);

    std::string raw;
    std::snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    readFile(path, raw, 8192);
    while (!raw.empty() && raw.back() == '\0')
        raw.pop_back();

    // The kernel truncates comm to 15 characters; recover the full name from
    // argv[0] when that is clearly the same program.
    st.name = st.comm;
    if (st.comm.size() >= 15 && !raw.empty()) {
        const std::string argv0 = raw.substr(0, raw.find('\0'));
        const std::string base = baseName(argv0);
        if (base.compare(0, st.comm.size(), st.comm) == 0 && base.find(' ') == std::string::npos)
            st.name = base;
    }
    std::replace(raw.begin(), raw.end(), '\0', ' ');
    std::replace(raw.begin(), raw.end(), '\n', ' ');
    st.cmdline = raw;
    st.refreshIn = 10;
}

bool ProcScanner::Impl::readProcess(int pid, double dt, ProcStats& p, PidState*& stateOut)
{
    char path[64];
    std::string text;
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (!readFile(path, text, 4096))
        return false;
    StatFields f;
    if (!parseStat(text, f))
        return false;

    PidState& st = states[pid];
    if (st.startTicks != f.start || st.seen == 0) {
        st = PidState();  // new process, or a recycled pid
        st.startTicks = f.start;
    }
    st.seen = generation;
    stateOut = &st;

    const uint64_t ticks = f.utime + f.stime;
    if (!st.fresh && dt > 0 && ticks >= st.cpuTicks && clkTck > 0)
        p.cpuPct = double(ticks - st.cpuTicks) / double(clkTck) / dt * 100.0;
    st.cpuTicks = ticks;

    bool identityRefreshed = false;
    if (st.comm != f.comm || --st.refreshIn <= 0) {
        st.comm = f.comm;
        refreshIdentity(pid, st);
        identityRefreshed = true;
    }

    p.pid = pid;
    p.ppid = f.ppid;
    p.name = st.name;
    p.cmdline = st.cmdline;
    p.user = st.user;
    p.state = f.state;
    p.kernelThread = (f.flags & kPfKthread) != 0;
    p.nice = static_cast<int>(f.nice);
    p.priority = static_cast<int>(f.priority);
    p.policy = f.policy;
    p.threads = static_cast<int>(f.threads);

    if (!p.kernelThread) {
        std::snprintf(path, sizeof path, "/proc/%d/statm", pid);
        if (readFile(path, text, 256)) {
            unsigned long long size = 0, resident = 0, shared = 0;
            if (std::sscanf(text.c_str(), "%llu %llu %llu", &size, &resident, &shared) == 3) {
                p.rssBytes = resident * static_cast<unsigned long long>(pageSize);
                p.memBytes = (resident > shared ? resident - shared : 0) * static_cast<unsigned long long>(pageSize);
            }
        }
    }

    // Only readable for our own processes (or as root). Most processes of a
    // desktop belong to other users, so after a refusal the file is tried
    // again only when the identity is re-read (exec, or every few samples).
    std::snprintf(path, sizeof path, "/proc/%d/io", pid);
    const bool tryIo = !st.ioDenied || identityRefreshed;
    if (tryIo && readFile(path, text, 1024) && !text.empty()) {
        st.ioDenied = false;
        const uint64_t rd = fieldAfter(text, "\nread_bytes:");
        const uint64_t wr = fieldAfter(text, "\nwrite_bytes:");
        if (st.ioOk && !st.fresh && dt > 0) {
            p.diskReadBps = rd >= st.ioRead ? double(rd - st.ioRead) / dt : 0.0;
            p.diskWriteBps = wr >= st.ioWrite ? double(wr - st.ioWrite) / dt : 0.0;
        } else {
            p.diskReadBps = p.diskWriteBps = 0.0;
        }
        st.ioRead = rd;
        st.ioWrite = wr;
        st.ioOk = true;
    } else {
        if (tryIo)
            st.ioDenied = true;
        st.ioOk = false;
    }

    st.fresh = false;
    return true;
}

void ProcScanner::Impl::collectSocketDeltas(std::unordered_map<uint32_t, SockCount>& deltas)
{
    std::unordered_map<uint32_t, SockCount> current;
    const bool v4 = dumpTcpSockets(AF_INET, current);
    const bool v6 = dumpTcpSockets(AF_INET6, current);
    if (!v4 && !v6) {
        netSupported = false;
        return;
    }
    netSupported = true;
    if (sockBaseline) {
        for (const auto& entry : current) {
            uint64_t prevTx = 0, prevRx = 0;
            const auto it = sockPrev.find(entry.first);
            if (it != sockPrev.end()) {
                prevTx = it->second.tx;
                prevRx = it->second.rx;
            }
            if (entry.second.tx < prevTx || entry.second.rx < prevRx)
                prevTx = prevRx = 0;  // the inode number was reused by a new socket
            SockCount delta;
            delta.tx = entry.second.tx - prevTx;
            delta.rx = entry.second.rx - prevRx;
            if (delta.tx || delta.rx)
                deltas[entry.first] = delta;
        }
    }
    sockPrev.swap(current);
    sockBaseline = true;
}

void ProcScanner::Impl::scanFds(int pid, PidState& st, double dt, bool wantDrm,
                                const std::unordered_map<uint32_t, SockCount>& deltas,
                                std::unordered_map<uint32_t, int>& owners)
{
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/fd", pid);
    DIR* dir = ::opendir(path);
    if (!dir) {
        st.fdOk = false;
        st.hasDrm = false;
        return;
    }
    st.fdOk = true;
    const int dfd = ::dirfd(dir);

    std::map<std::string, uint64_t> ns;
    std::map<std::string, std::pair<uint64_t, uint64_t>> cycles;
    std::set<std::string> clients;
    uint64_t memKiB = 0;
    bool anyDrm = false;

    char link[256];
    while (const dirent* e = ::readdir(dir)) {
        if (e->d_name[0] == '.')
            continue;
        const ssize_t n = ::readlinkat(dfd, e->d_name, link, sizeof link - 1);
        if (n <= 0)
            continue;
        link[n] = '\0';
        if (n > 8 && std::memcmp(link, "socket:[", 8) == 0) {
            const uint32_t inode = static_cast<uint32_t>(std::strtoul(link + 8, nullptr, 10));
            if (deltas.count(inode)) {
                // A socket inherited across fork() shows up in several
                // processes; credit the youngest, which is usually the worker.
                int& owner = owners[inode];
                if (pid > owner)
                    owner = pid;
            }
        } else if (wantDrm && n > 9 && std::memcmp(link, "/dev/dri/", 9) == 0) {
            const std::string info = "/proc/" + std::to_string(pid) + "/fdinfo/" + e->d_name;
            std::string text;
            DrmClient c;
            if (!readFile(info, text, 16384) || !parseFdinfo(text, c))
                continue;
            if (!clients.insert(c.pdev + "#" + c.id).second)
                continue;  // same client through a duplicated descriptor
            anyDrm = true;
            for (const auto& en : c.ns)
                ns[c.pdev + "/" + en.first] += en.second;
            for (const auto& cy : c.cycles) {
                auto& slot = cycles[c.pdev + "/" + cy.first];
                slot.first += cy.second.first;
                slot.second = std::max(slot.second, cy.second.second);
            }
            memKiB += c.hasVram ? c.vramKiB : c.totalKiB;
        }
    }
    ::closedir(dir);

    double pidPct = 0;
    if (anyDrm && st.hasDrm && dt > 0) {
        std::map<std::string, double> perPdev;
        for (const auto& en : ns) {
            const auto prev = st.drmNs.find(en.first);
            if (prev == st.drmNs.end() || en.second < prev->second)
                continue;
            const double pct = double(en.second - prev->second) / (dt * 1e9) * 100.0;
            double& slot = perPdev[en.first.substr(0, en.first.find('/'))];
            slot = std::max(slot, pct);
        }
        for (const auto& cy : cycles) {
            const auto prev = st.drmCycles.find(cy.first);
            if (prev == st.drmCycles.end() || cy.second.first < prev->second.first
                || cy.second.second <= prev->second.second)
                continue;
            const double pct = double(cy.second.first - prev->second.first)
                             / double(cy.second.second - prev->second.second) * 100.0;
            double& slot = perPdev[cy.first.substr(0, cy.first.find('/'))];
            slot = std::max(slot, pct);
        }
        for (const auto& pp : perPdev) {
            const double pct = std::min(100.0, pp.second);
            pdevUsage[pp.first] += pct;
            pidPct = std::max(pidPct, pct);
        }
    }
    st.hasDrm = anyDrm;
    st.gpuPct = pidPct;
    st.gpuMem = memKiB * 1024ULL;
    st.drmNs.swap(ns);
    st.drmCycles.swap(cycles);
}

ProcScanner::ProcScanner() : d(new Impl) {}
ProcScanner::~ProcScanner() = default;

const std::map<std::string, double>& ProcScanner::drmUsageByPdev() const
{
    return d->pdevUsage;
}

void ProcScanner::scan(double dt, bool wantDrm, std::vector<ProcStats>& out)
{
    const Clock::time_point now = Clock::now();
    const double sinceHeavy = d->heavyDone ? std::chrono::duration<double>(now - d->lastHeavy).count() : 0.0;
    const bool heavy = !d->heavyDone || sinceHeavy >= kHeavyIntervalSec;

    std::unordered_map<uint32_t, SockCount> deltas;
    std::unordered_map<uint32_t, int> owners;
    if (heavy) {
        d->collectSocketDeltas(deltas);
        d->pdevUsage.clear();
    }

    ++d->generation;
    DIR* dir = ::opendir("/proc");
    if (!dir)
        return;
    out.reserve(d->states.size() + 16);
    while (const dirent* e = ::readdir(dir)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        const int pid = std::atoi(e->d_name);
        ProcStats p;
        PidState* st = nullptr;
        if (!d->readProcess(pid, dt, p, st))
            continue;
        if (heavy) {
            if (p.kernelThread)
                st->fdOk = false;
            else
                d->scanFds(pid, *st, sinceHeavy, wantDrm, deltas, owners);
        }
        out.push_back(std::move(p));
    }
    ::closedir(dir);

    // Forget processes that are gone.
    for (auto it = d->states.begin(); it != d->states.end();)
        it = it->second.seen == d->generation ? std::next(it) : d->states.erase(it);

    if (heavy) {
        for (auto& entry : d->states)
            entry.second.netRxAcc = entry.second.netTxAcc = 0;
        for (const auto& owner : owners) {
            const auto st = d->states.find(owner.second);
            const auto delta = deltas.find(owner.first);
            if (st == d->states.end() || delta == deltas.end())
                continue;
            st->second.netTxAcc += delta->second.tx;
            st->second.netRxAcc += delta->second.rx;
        }
        for (auto& entry : d->states) {
            PidState& st = entry.second;
            st.netRxBps = sinceHeavy > 0 ? double(st.netRxAcc) / sinceHeavy : 0.0;
            st.netTxBps = sinceHeavy > 0 ? double(st.netTxAcc) / sinceHeavy : 0.0;
        }
        for (auto& usage : d->pdevUsage)
            usage.second = std::min(100.0, usage.second);
        d->lastHeavy = now;
        d->heavyDone = true;
    }

    for (ProcStats& p : out) {
        const auto it = d->states.find(p.pid);
        if (it == d->states.end())
            continue;
        const PidState& st = it->second;
        if (st.fdOk && d->netSupported) {
            p.netRxBps = st.netRxBps;
            p.netTxBps = st.netTxBps;
        }
        if (st.hasDrm) {
            p.gpuPct = st.gpuPct;
            p.gpuMemBytes = st.gpuMem;
        }
    }
}

}  // namespace sys
