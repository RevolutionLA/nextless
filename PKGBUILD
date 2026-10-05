# Maintainer: RevolutionLA <https://github.com/RevolutionLA>

pkgname=fcitx5-nextless-git
_pkgname=fcitx5-nextless
pkgver=0.2.0.r0.0000000
pkgrel=1
pkgdesc="Push-to-talk voice input addon for fcitx5 with a configurable hotkey (offline-first, Linux Typeless)"
arch=('x86_64' 'aarch64')
url="https://github.com/RevolutionLA/nextless"
license=('MIT')
depends=('fcitx5' 'libebur128' 'libpulse' 'curl' 'speexdsp' 'libsoxr')
makedepends=('git' 'meson' 'ninja')
optdepends=('sherpa-onnx: local offline ASR backends (zipformer / fire-red)'
            'deepfilternet: the optional DeepFilterNet3 denoiser (provides deep-filter)'
            'pulseaudio: paplay for the notification sounds (pw-play from pipewire works too)')
provides=("$_pkgname")
conflicts=("$_pkgname")
install=PKGBUILD.install
source=("$_pkgname::git+https://github.com/RevolutionLA/nextless.git")
sha256sums=('SKIP')
backup=(
    'etc/nextless/advanced.json'
    'etc/nextless/audio.json'
    'etc/nextless/doubao.json'
    'etc/nextless/qwen.json'
    'etc/nextless/nextless.json'
)

pkgver() {
    cd "$_pkgname"
    printf "0.2.0.r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cd "$_pkgname"
    # --sysconfdir=/etc：默认配置装到 /etc/nextless（meson 规则），
    # 并由同一路径编译进 loader；backup=(...) 仍然对 pacman 生效。
    meson setup build --prefix=/usr --sysconfdir=/etc --buildtype=plain \
        -Dcpp_args='-O2 -march=native'
    meson compile -C build
}

package() {
    cd "$_pkgname"
    DESTDIR="$pkgdir" meson install -C build
    install -Dm644 README.md "$pkgdir/usr/share/doc/$_pkgname/README.md"
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$_pkgname/LICENSE"
    for f in config/*.json.example; do
        install -Dm644 "$f" "$pkgdir/usr/share/doc/$_pkgname/$f"
    done
}
