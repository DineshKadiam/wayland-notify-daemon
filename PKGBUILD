pkgname=fnott
pkgver=0.0.0.r0.gb630264
pkgrel=1
pkgdesc="Lightweight notification daemon for Wayland"
arch=('x86_64')
url=https://gitlab.com/dnkl/fnott
license=(mit)
makedepends=('meson' 'ninja' 'scdoc')
depends=(
  'wayland' 'wlroots'
  'freetype2' 'fontconfig' 'pixman' 'libpng' 'dbus')
source=()

pkgver() {
  [ -d ../.git ] && git describe --tags --long | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g'
  [ ! -d ../.git ] && head -3 ../meson.build | grep version | cut -d "'" -f 2
}

build() {
  meson --prefix=/usr --buildtype=release -Db_lto=true ..
  ninja
}

package() {
  DESTDIR="${pkgdir}/" ninja install
}
