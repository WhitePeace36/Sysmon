<p align="center">
  <img src="data/icons/sysmon.svg" alt="Sysmon logo" width="128" height="128">
</p>

<h1 align="center">Sysmon</h1>

A system monitor for Linux written in C++17 with Qt Widgets (Qt 6, or Qt 5.15).
No dependencies besides Qt: the graphs are drawn with QPainter and all data
comes straight from `/proc`, `/sys`, netlink and `systemctl`.

## Build

Install a compiler, CMake and the Qt base development package:

| Distribution     | Command |
|------------------|---------|
| Debian / Ubuntu  | `sudo apt install build-essential cmake qt6-base-dev` (Qt 5: `qtbase5-dev`) |
| Fedora           | `sudo dnf install gcc-c++ cmake qt6-qtbase-devel` |
| Arch             | `sudo pacman -S base-devel cmake qt6-base` |
| openSUSE         | `sudo zypper install gcc-c++ cmake qt6-base-devel` |

```sh
cmake -S . -B build
cmake --build build -j
./build/sysmon
```

Optional: `sudo cmake --install build` installs the binary, a menu entry and
the icon.

## Packages

**Arch Linux** (needs `base-devel` and `cmake`), from the project root:

```sh
makepkg -si
```

The build happens in `build/makepkg/` inside the checkout; the finished
`sysmon-*.pkg.tar.zst` is written next to the PKGBUILD.

**Debian / Ubuntu** (needs `dpkg-dev`, part of `build-essential`):

```sh
cmake -S . -B build && cmake --build build -j
cd build && cpack -G DEB
sudo apt install ./sysmon_*.deb
```

**Fedora** (needs `rpm-build`):

```sh
cmake -S . -B build && cmake --build build -j
cd build && cpack -G RPM
sudo dnf install ./sysmon-*.rpm
```

Library dependencies are worked out from the built binary, so a package fits
the distribution release it was built on. The package settings are in
`cmake/Packaging.cmake`.

## Tabs

### Performance

A device list on the left, graphs and figures for the selected device on the right.

- **CPU**: utilization graph, switchable between one overall graph and one
  graph per logical processor; temperature, fan speed and clock speed graphs;
  load average, up time, process and thread count, topology, caches.
- **Memory**: usage and swap graphs, breakdown (available, cached, buffers,
  shared, dirty), and the installed modules: slot, size, type, form factor,
  the speed they run at, their rated speed, rank, manufacturer, part number.
- **Disk** (one page per disk): read/write rate, active time, totals since
  boot, type, capacity, mounted file systems with usage.
- **Network** (one page per adapter): receive/send rate, totals, state, link
  speed, MAC, IPv4 and IPv6 address.
- **GPU** (one page per GPU): utilization, memory, temperature, fan speed, GPU
  clock, memory clock, power draw. Only what the GPU reports gets a graph.
- **Sensors**: every temperature and fan sensor the kernel exposes.

Disks, adapters and GPUs appear and disappear as they are plugged in.

### Processes

Programs grouped by name with their summed CPU, GPU, memory, network and disk
use. A program with several processes can be expanded.

### Details

One row per PID with the same columns plus nice, PRIO, scheduler and the full
command line.

### Both process tabs

- **Del** kills the selection with SIGKILL (asks first; can be turned off in *View*).
- **Right-click > Send Signal** lists every signal. Choose SIGTERM there to
  ask a process to quit. Real-time signals are in a submenu.
- Right-click also offers *Change Priority* (nice) and copy PID / command line.
- Click a column header to sort. Right-click the header for more columns:
  state, GPU memory, resident memory, separate network and disk directions,
  threads, PPID.
- Click a row and type: the selection jumps to the first name that starts
  with what you typed (keys typed quickly extend the search).
- **Ctrl+F** filters by name, PID, user or command line.
- If a process belongs to another user, the program offers to repeat the
  action through `pkexec`.

### Services

All systemd service units, for the system or for your user session, with
their active state, sub-state and whether they start at boot. Start, Stop,
Restart, Enable, Disable, Mask and Unmask are in the toolbar and the context
menu; the lower pane shows `systemctl status` of the selected service. The
list refreshes every five seconds while the tab is open (**F5** forces it).

System services are changed through `systemctl`, which asks polkit: your
desktop's password dialog appears when an action needs it.

## Where the numbers come from

| Data | Source |
|------|--------|
| CPU load, frequency | `/proc/stat`, cpufreq in sysfs |
| Temperatures, fans | hwmon (`/sys/class/hwmon`), thermal zones as fallback |
| Memory modules | udev properties of the DMI device (`udevadm info`), no root needed |
| Disks, network adapters | `/proc/diskstats`, `/proc/net/dev`, sysfs |
| NVIDIA GPUs | NVML (`libnvidia-ml.so`, loaded at run time if present) |
| AMD GPUs | amdgpu sysfs and hwmon attributes |
| Intel GPUs | sysfs for the clock, DRM fdinfo for load |
| Per-process CPU, memory, disk | `/proc/<pid>/stat`, `statm`, `io` |
| Per-process network | per-socket TCP counters from the kernel (sock_diag) matched to the sockets each process holds |
| Per-process GPU | NVML on NVIDIA, DRM fdinfo on AMD and Intel |

## Things to know

- **Other users' processes**: the kernel only lets you read the I/O counters
  and sockets of your own processes. Disk and Network stay empty for the rest
  unless you run the program as root.
- **Per-process network is TCP only** and excludes loopback. The kernel keeps
  no per-socket byte counters for UDP. Connections that open and close
  between two samples are missed.
- **Disk of a process** includes the I/O of child processes it has already
  waited for; that is how the kernel accounts it.
- **CPU fan** is a best guess among the motherboard's fan sensors (one
  labelled "CPU" wins). The Sensors page lists all of them.
- **Memory module details** need systemd 248 or newer and real hardware.
- **Intel GPU load** is the sum over the processes you may inspect, so it can
  read low unless run as root.
- CPU % in the process lists is relative to the whole machine by default;
  *View* has a switch for top-style percentages where 100 % is one core.

## Checking a machine without the GUI

```sh
cmake -S . -B build -DSYSMON_BUILD_DUMP=ON
cmake --build build --target sysmon-dump
./build/sysmon-dump 3
```

prints three samples of everything the backend reads. Add
`-DSYSMON_BUILD_GUI=OFF` to build only this tool, without Qt.

## Layout

```
source/sys/         backend, plain C++ without Qt
  sampler.cpp       CPU, memory, disks, adapters, sensors
  procs.cpp         process table, per-process network and GPU
  gpu.cpp           NVML and DRM sysfs
source/             Qt interface
  graphwidget       scrolling line graph
  performancetab    device list and pages
  processmodel      models for Processes and Details, sorting and filtering
  processtab        the two process tabs, signals, priorities
  typeaheadview     list view with type-ahead and Del
  servicestab       systemd services
  samplerworker     runs the backend on its own thread
tests/backend_dump  console tool
data/icons/         logo: sysmon.svg is the source, the PNGs are rendered from it
```
