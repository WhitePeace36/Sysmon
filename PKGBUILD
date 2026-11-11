# Maintainer: WhitePeace36 <WhitePeace36@users.noreply.github.com>
#
# Builds an Arch package from the checkout this file sits in:
#
#     makepkg -si
#
# The build happens in build/makepkg/ inside the checkout. The finished
# package is written next to this file.

# By default makepkg works in ./src and ./pkg beside the PKGBUILD. Send it to
# build/makepkg instead, unless BUILDDIR was chosen on purpose (environment
# or makepkg.conf).
if [[ -z ${BUILDDIR:-} || ${BUILDDIR} -ef ${startdir:-$PWD} ]]; then
    BUILDDIR="${startdir:-$PWD}/build/makepkg"
fi

pkgname=sysmon
# Keep pkgver in step with project(... VERSION ...) in CMakeLists.txt.
pkgver=1.0.0
pkgrel=1
pkgdesc='System monitor with performance graphs, process lists and systemd services'
arch=('x86_64' 'aarch64')
url='https://github.com/WhitePeace36/Sysmon'
license=('GPL-2.0-only')
depends=('qt6-base' 'gcc-libs' 'glibc' 'hicolor-icon-theme')
makedepends=('cmake')
optdepends=(
    'polkit: signal processes and manage services that need administrator rights'
    'hwdata: GPU model names'
    'nvidia-utils: NVIDIA GPU statistics'
)
# No downloads: the sources are the checkout itself ($startdir).
source=()

build() {
    cmake -S "$startdir" -B "$srcdir/build" \
        -DCMAKE_BUILD_TYPE=None \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -Wno-dev
    cmake --build "$srcdir/build"
}

package() {
    DESTDIR="$pkgdir" cmake --install "$srcdir/build"
}
