# Binary packages with CPack.
#
#   Debian / Ubuntu:   cd build && cpack -G DEB    ->  sysmon_<version>-1_amd64.deb
#   Fedora:            cd build && cpack -G RPM    ->  sysmon-<version>-1.fc<N>.x86_64.rpm
#
# Plain `cpack` (or `cmake --build build --target package`) picks the format
# of the distribution it runs on. The Arch package is built by the PKGBUILD in
# the project root instead.
#
# The version comes from project(... VERSION ...) in the top-level CMakeLists.txt.

set(CPACK_PACKAGE_NAME "sysmon")
set(CPACK_PACKAGE_VENDOR "WhitePeace36")
set(CPACK_PACKAGE_CONTACT "WhitePeace36 <WhitePeace36@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/WhitePeace36/Sysmon")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "System monitor with performance graphs, process lists and systemd services")
set(CPACK_PACKAGE_DESCRIPTION
    "Qt system monitor for Linux. Graphs for CPU, memory, every disk, network adapter and GPU, a process list grouped by program, a detailed list of all PIDs with signals and priorities, and a tab to start, stop, enable and mask systemd services.")

# Install below /usr, like every distribution package.
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(NOT CPACK_GENERATOR)
    if(EXISTS "/etc/debian_version")
        set(CPACK_GENERATOR "DEB")
    elseif(EXISTS "/etc/fedora-release" OR EXISTS "/etc/redhat-release")
        set(CPACK_GENERATOR "RPM")
    else()
        set(CPACK_GENERATOR "TGZ")
    endif()
endif()

# ---- Debian / Ubuntu ---------------------------------------------------------
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")  # sysmon_1.0.0-1_amd64.deb
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# Let dpkg-shlibdeps work out the Qt and C++ runtime packages from the binary,
# so the same rule is right for Qt 5 and Qt 6 builds and for every release.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# pkexec for acting on other users' processes, pci.ids for GPU model names.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "polkitd | policykit-1, pci.ids")

# ---- Fedora ------------------------------------------------------------------
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")     # sysmon-1.0.0-1.fc41.x86_64.rpm
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-only")
# Library dependencies are found by rpmbuild itself (AutoReq is on by default).
set(CPACK_RPM_PACKAGE_SUGGESTS "polkit, hwdata")
# These directories belong to the "filesystem" package. Without this the RPM
# would claim them too and fail to install with a file conflict.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications
    /usr/share/licenses)

include(CPack)
