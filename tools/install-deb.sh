#!/usr/bin/env bash
# issue #7 follow-up / "one command" install route.
#
#   curl -fsSL https://raw.githubusercontent.com/RevolutionLA/nextless/main/tools/install-deb.sh | sudo bash
#
# Downloads the .deb from the latest GitHub Release, verifies its sha256,
# installs it with apt (apt pulls fcitx5 and all shared libs from the normal
# Ubuntu/Debian archive - nothing here bypasses the distro), and prints the
# two follow-up commands every first-run needs. amd64 only for now; source or
# PKGBUILD routes cover other architectures.
set -euo pipefail

REPO="RevolutionLA/nextless"
RAW="https://raw.githubusercontent.com/${REPO}/main/tools/install-deb.sh"

if [ "$(id -u)" -ne 0 ]; then
  echo "not root; rerun as:  curl -fsSL ${RAW} | sudo bash" >&2
  exit 1
fi
command -v apt-get >/dev/null || { echo "apt-get not found - this route is for Debian/Ubuntu; see README for Arch/source" >&2; exit 1; }
command -v curl >/dev/null || apt-get install -y curl >/dev/null

case "$(uname -m)" in
  x86_64) ARCH=amd64 ;;
  *) echo "install-deb.sh ships amd64 only; $(uname -m) users: build the .deb (README 'pick your route') or use the PKGBUILD" >&2; exit 1 ;;
esac

api="https://api.github.com/repos/${REPO}/releases/latest"
deb_url=""; sums_url=""; tag=""
# Every one of these greps exits 1 when the release JSON is not what we expect.
# Under `set -euo pipefail` a failed assignment aborts immediately, so without
# `|| true` the "could not resolve latest release" and "no ${ARCH} .deb asset"
# messages below would be unreachable - the script would just die silently.
if json=$(curl -fsSL "$api" 2>/dev/null); then
  tag=$(printf '%s\n' "$json" | grep -m1 '"tag_name"' | cut -d'"' -f4 || true)
  deb_url=$(printf '%s\n' "$json" | grep -oE '"browser_download_url": *"[^"]+"' | cut -d'"' -f4 | grep -E "_${ARCH}\.deb$" | head -1 || true)
  sums_url=$(printf '%s\n' "$json" | grep -oE '"browser_download_url": *"[^"]+"' | cut -d'"' -f4 | grep -E 'sha256sums\.txt$' | head -1 || true)
fi
if [ -z "$deb_url" ]; then
  # Anonymous api.github.com rate limits (403) are normal behind shared NAT.
  # /releases/latest 302-redirects to the tag page on github.com itself, and
  # the asset names are deterministic: the Release workflow refuses to publish
  # unless the tag matches debian/changelog at revision -1.
  tag=$(curl -fsSIL -o /dev/null -w '%{url_effective}' "https://github.com/${REPO}/releases/latest" | sed 's#.*/tag/##' || true)
  ver=${tag#v}
  if [ -z "$tag" ] || [ "$tag" = "$ver" ]; then
    echo "could not resolve latest release (API said $(curl -fsS -o /dev/null -w '%{http_code}' "$api" 2>/dev/null || echo '?'))" >&2
    exit 1
  fi
  base="https://github.com/${REPO}/releases/download/${tag}"
  deb_url="${base}/fcitx5-nextless_${ver}-1_${ARCH}.deb"
  sums_url="${base}/sha256sums.txt"
fi
[ -n "$deb_url" ] || { echo "no ${ARCH} .deb asset on release ${tag}" >&2; exit 1; }

dir=$(mktemp -d)
chmod 700 "$dir"
trap 'rm -rf "$dir"' EXIT
echo "fetching ${deb_url##*/} (${tag})..." >&2
curl -fsSL -o "$dir/install.deb" "$deb_url"
verified=0
if [ -n "$sums_url" ] && curl -fsSL -o "$dir/sha256sums.txt" "$sums_url" 2>/dev/null; then
  # The asset is stored locally as install.deb, so `sha256sum -c` (which looks
  # files up by the name inside the list) is the wrong tool - compare digests
  # directly against the line for the published filename.
  # `|| true` is load-bearing: grep exits 1 when the asset is not listed, and
  # under `set -euo pipefail` a plain failed assignment would kill the script
  # before the fail-closed branch below could print its message.
  expected=$(grep -F "${deb_url##*/}" "$dir/sha256sums.txt" | awk '{print $1}' || true)
  actual=$(sha256sum "$dir/install.deb" | awk '{print $1}')
  if [ -n "$expected" ] && [ "$expected" = "$actual" ]; then
    verified=1
    echo "sha256 verified (${actual:0:12}...) for ${deb_url##*/}" >&2
  elif [ -n "$expected" ]; then
    echo "sha256 MISMATCH: expected $expected got $actual" >&2
    exit 1
  fi
fi
if [ "$verified" = 0 ]; then
  # Fail closed (issue #43): the degraded paths - sums unreachable, or the deb
  # not listed in them - are exactly what a mutated release asset would look
  # like, so they need an explicit opt-in, not a shrug.
  if [ "${1:-}" = "--allow-unverified" ] || [ "${ALLOW_UNVERIFIED:-0}" = "1" ]; then
    echo "WARNING: sha256 could not be verified; continuing only because you asked (--allow-unverified / ALLOW_UNVERIFIED=1)" >&2
  else
    {
      echo "refusing to install unverified: release digest unavailable or this .deb is not listed in sha256sums.txt."
      echo "Note the checksums ship on the same GitHub Release as the deb (TOFU): they catch corruption and"
      echo "sloppy publishes, not a compromised release. If you still want to proceed:"
      echo "  curl -fsSL ${RAW} | sudo bash -s -- --allow-unverified"
    } >&2
    exit 1
  fi
fi

DEBIAN_FRONTEND=noninteractive apt-get install -y "$dir/install.deb"

echo
echo "installed $(dpkg-query -W -f='${Version}' fcitx5-nextless)"
cat >&2 <<EOF

Two steps remain, and they are deliberately yours:

  1. fetch the offline models (~360 MB zipformer, add --punctuation for
     local commas/periods) - run as your desktop user, not root:

       nextless-get-models --zipformer --punctuation

  2. load the add-on into the running fcitx5:

       fcitx5 -r -d

Then hold Right Ctrl and talk. Everything (trigger key, backend, denoise)
is configurable in ~/.config/nextless/ - the panel hint appears if a model
is still missing.
EOF
