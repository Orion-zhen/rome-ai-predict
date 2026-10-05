pkgname=rome-ai-predict
pkgver=0.1.0
pkgrel=1
pkgdesc="Rome AI Predict"
arch=("x86_64")
url="https://github.com/Orion-zhen/rome-ai-predict"
license=("AGPL-3.0-only")
depends=("fcitx5" "fcitx5-rime" "curl" "jsoncpp" "yaml-cpp")
makedepends=("cmake" "ninja" "pkgconf")
source=()

# 在本地仓库根目录运行 makepkg，直接构建当前源码。
_repo="$(pwd -P)"
# 隔离默认工作目录，避免 makepkg -C/-c 删除仓库的 src/。
if [[ "$BUILDDIR" -ef "$_repo" ]]; then
  BUILDDIR="$_repo/build-package"
fi

build() {
  cmake -S "$_repo" -B "$srcdir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_TESTING=OFF \
    -DBUILD_FCITX5=ON
  cmake --build "$srcdir/build"
}

package() {
  DESTDIR="$pkgdir" cmake --install "$srcdir/build"
}