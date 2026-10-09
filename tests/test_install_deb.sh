#!/usr/bin/env bash
# Issue #43: tools/install-deb.sh must fail CLOSED on a digest it cannot check.
# "sha256sums.txt unreachable" and "this .deb is not listed in it" are exactly
# what a tampered or sloppily re-published release looks like, so they have to
# stop the install unless the user opts in with --allow-unverified /
# ALLOW_UNVERIFIED=1.
#
# The installer is exercised as shipped: root, apt and the network are stubbed
# on PATH, so the assertions cover the real file - including its `set -euo
# pipefail` control flow, which silently killed several of its own error
# messages before this test existed - without needing root or a real release.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
script="$here/../tools/install-deb.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f "$script" ] || fail "installer moved: $script"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
bin="$work/bin"
mkdir -p "$bin"

# --- fixtures: a release with one amd64 deb and its real digest --------------
DEB_NAME="fcitx5-nextless_9.9.9-1_amd64.deb"
TAG="v9.9.9"
BASE="https://github.com/RevolutionLA/nextless/releases/download/${TAG}"

printf 'this stands in for a .deb; only its bytes matter\n' > "$work/real.deb"
printf 'these bytes were swapped after the digest was cut\n' > "$work/tampered.deb"
sha_real=$(sha256sum "$work/real.deb" | awk '{print $1}')
sha_tampered=$(sha256sum "$work/tampered.deb" | awk '{print $1}')

printf '%s  %s\n' "$sha_real" "$DEB_NAME" > "$work/sums.txt"
printf '%s  %s\n' "$sha_tampered" "$DEB_NAME" > "$work/sums-bad.txt"
printf '%s  %s\n' "$sha_real" "fcitx5-nextless_9.9.9-1_arm64.deb" > "$work/sums-unlisted.txt"

APT_LOG="$work/apt.log"

# --- stubs ------------------------------------------------------------------
cat > "$bin/curl" <<STUB
#!/usr/bin/env bash
# Serves the URLs install-deb.sh asks for: the release API, the .deb asset,
# sha256sums.txt, and the /releases/latest redirect used when the API 403s.
out=""; url=""
argv=("\$@"); i=0
while [ \$i -lt \${#argv[@]} ]; do
  a="\${argv[\$i]}"
  case "\$a" in
    -o) out="\${argv[\$((i+1))]}"; i=\$((i+2)); continue ;;
    -w) i=\$((i+2)); continue ;;
    -*) i=\$((i+1)); continue ;;
  esac
  url="\$a"; i=\$((i+1))
done
case "\$url" in
  *api.github.com/*)
    [ "\${STUB_API:-ok}" = "403" ] && exit 22
    printf '{"tag_name": "%s",\n' "\$STUB_TAG"
    if [ "\${STUB_ASSET_LIST:-both}" != "none" ]; then
      printf ' "browser_download_url": "%s/%s",\n' "\$STUB_BASE" "\$STUB_DEB_NAME"
    fi
    if [ "\${STUB_ASSET_LIST:-both}" = "both" ]; then
      printf ' "browser_download_url": "%s/sha256sums.txt"\n' "\$STUB_BASE"
    fi
    printf '}\n'
    ;;
  *releases/latest*)
    printf 'https://github.com/RevolutionLA/nextless/releases/tag/%s\n' "\$STUB_TAG"
    ;;
  *sha256sums.txt)
    case "\${STUB_SUMS:-ok}" in
      404) exit 22 ;;
      unlisted) cp "$work/sums-unlisted.txt" "\$out" ;;
      bad) cp "$work/sums-bad.txt" "\$out" ;;
      *) cp "$work/sums.txt" "\$out" ;;
    esac
    ;;
  *.deb)
    if [ "\${STUB_DEB:-real}" = "tampered" ]; then
      cp "$work/tampered.deb" "\$out"
    else
      cp "$work/real.deb" "\$out"
    fi
    ;;
  *) exit 22 ;;
esac
STUB

cat > "$bin/id" <<STUB
#!/usr/bin/env bash
if [ "\${1-}" = "-u" ]; then printf '%s\n' "\${STUB_UID:-0}"; fi
STUB

cat > "$bin/apt-get" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "\${APT_LOG}"
STUB

cat > "$bin/dpkg-query" <<STUB
#!/usr/bin/env bash
printf '9.9.9-1\n'
STUB

chmod +x "$bin/curl" "$bin/id" "$bin/apt-get" "$bin/dpkg-query"

# --- runner -----------------------------------------------------------------
# run <label> <expected-rc> <expected-substring> [installer args...]
# Knobs are set as ordinary variables by the caller: STUB_API, STUB_SUMS,
# STUB_DEB, STUB_ASSET_LIST, STUB_UID, ALLOW_UNVERIFIED.
ran=0
run() {
  local label="$1" exp_rc="$2" exp_sub="$3"; shift 3
  local o rc
  : > "$APT_LOG"
  set +e
  o=$(PATH="$bin:$PATH" APT_LOG="$APT_LOG" \
      STUB_TAG="$TAG" STUB_BASE="$BASE" STUB_DEB_NAME="$DEB_NAME" \
      STUB_API="${STUB_API:-ok}" STUB_SUMS="${STUB_SUMS:-ok}" \
      STUB_DEB="${STUB_DEB:-real}" STUB_ASSET_LIST="${STUB_ASSET_LIST:-both}" \
      STUB_UID="${STUB_UID:-0}" ALLOW_UNVERIFIED="${ALLOW_UNVERIFIED:-}" \
      bash "$script" "$@" 2>&1)
  rc=$?
  set -e
  ran=$((ran + 1))
  if [ "$rc" != "$exp_rc" ]; then
    echo "--- $label (rc=$rc) ---" >&2
    printf '%s\n' "$o" >&2
    fail "$label: exit $rc, expected $exp_rc"
  fi
  if [ -n "$exp_sub" ] && ! grep -qF -- "$exp_sub" <<<"$o"; then
    echo "--- $label output ---" >&2
    printf '%s\n' "$o" >&2
    fail "$label: expected to find '$exp_sub'"
  fi
  if [ "$exp_rc" = "0" ]; then
    if [ ! -s "$APT_LOG" ]; then fail "$label: rc 0 but apt-get was never called"; fi
    grep -qF "install.deb" "$APT_LOG" || fail "$label: apt-get did not get the downloaded deb"
  else
    if [ -s "$APT_LOG" ]; then fail "$label: refused (rc $rc) but installed anyway"; fi
  fi
  LAST_OUTPUT="$o"
}

# 1. happy path: digest matches the published sums
run "verified asset" 0 "sha256 verified (${sha_real:0:12}"

# 2. the asset was replaced after publishing
STUB_DEB=tampered run "tampered asset" 1 "sha256 MISMATCH"

# 3. the sums list a digest the published asset does not have
STUB_SUMS=bad run "digest mismatch" 1 "sha256 MISMATCH"

# 4. this .deb is not in sha256sums.txt. Two regressions live here: `set -e`
#    used to abort inside the grep assignment before any message, and the old
#    warning path still printed "sha256 verified" for an unverified asset.
STUB_SUMS=unlisted run "asset not listed" 1 "refusing to install unverified"
if grep -q "sha256 verified" <<<"$LAST_OUTPUT"; then
  fail "asset not listed claimed to be verified"
fi

# 5. sums file gone; the refusal has to name the opt-in or the user is stuck
STUB_SUMS=404 run "sums unreachable" 1 "refusing to install unverified"
grep -q -- "--allow-unverified" <<<"$LAST_OUTPUT" || fail "refusal must say how to opt in"

# 6. the release ships no sums asset at all
STUB_ASSET_LIST=deb run "no sums asset" 1 "refusing to install unverified"

# 7+8. both documented opt-ins
STUB_SUMS=404 run "opt-in via argv" 0 "continuing only because you asked" --allow-unverified
STUB_SUMS=404 ALLOW_UNVERIFIED=1 run "opt-in via env" 0 "continuing only because you asked"

# 9. anonymous api.github.com 403s are normal behind shared NAT: the redirect
#    fallback resolves the tag and rebuilds the asset name, and must still end
#    up verified - the fail-closed change may not turn that route into a
#    permanent refusal.
STUB_API=403 run "403 fallback, verified" 0 "sha256 verified (${sha_real:0:12}"
STUB_API=403 STUB_SUMS=404 run "403 fallback, unverifiable" 1 "refusing to install unverified"

# 10. the API answers but lists no assets: same deterministic fallback
STUB_ASSET_LIST=none run "API without assets" 0 "sha256 verified (${sha_real:0:12}"

# 11. not root
STUB_UID=1000 run "non-root" 1 "not root"

echo "install-deb.sh: $ran checks, fail-closed verified"
