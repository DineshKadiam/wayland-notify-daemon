pkgname=fnott
pkgver=1.1.2
pkgrel=1
pkgdesc="Lightweight notification daemon for Wayland"
arch=('x86_64' 'aarch64')
url=https://codeberg.org/dnkl/fnott
license=(mit)
makedepends=('meson' 'ninja' 'scdoc' 'tllist>=1.0.1')
depends=(
  'wayland' 'pixman' 'libpng' 'dbus'
  'fcft>=3.0.0' 'fcft<4.0.0')
source=()

pkgver() {
  cd ../.git &> /dev/null && git describe --tags --long | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g' ||
      head -3 ../meson.build | grep version | cut -d "'" -f 2
}

build() {
  meson --prefix=/usr --buildtype=release --wrap-mode=nofallback -Db_lto=true ..
  ninja
}

package() {
  DESTDIR="${pkgdir}/" ninja install
}
