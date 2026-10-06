#!/usr/bin/env bash
# get_models.sh — first-run model downloader for Nextless local ASR backends.
#
# Installs the sherpa-onnx runtime and the ASR models the built-in defaults
# (config/advanced.json.example, and the same paths compiled into the
# providers) expect, under ~/.local/share/nextless/. Nothing is written
# outside $NEXTLESS_DATA_HOME (default ~/.local/share/nextless).
#
# Re-running is cheap: pieces already on disk are skipped unless --force.
# Downloads land in a staging dir inside the data home and are only moved
# into place after the archive extracted with the expected top-level
# directory; on any failure the staging dir is removed, so a half-extracted
# model never shadows a working one.
#
# Asset names were checked against the v1.13.8 release (issue #5):
#   x86_64  -> sherpa-onnx-v1.13.8-linux-x64-shared.tar.bz2        (curl -IL -> 200)
#   aarch64 -> sherpa-onnx-v1.13.8-linux-aarch64-shared-cpu.tar.bz2 (200)
#              plain linux-aarch64-shared is a 404, so the mapping cannot
#              be a sed over the x64 name.
set -euo pipefail

VERSION="1.13.8"
# Test hook (also used by tests/test_get_models.sh): point the two release
# bases at a local file:// tree to exercise install/idempotency/failure paths
# without downloading ~1.8 GB from GitHub.
RELEASE_BASE="${NEXTLESS_RELEASE_BASE:-https://github.com/k2-fsa/sherpa-onnx/releases/download/v${VERSION}}"
MODEL_BASE="${NEXTLESS_MODEL_BASE:-https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models}"

# Directory names the providers look for (see advanced.json.example).
ZIPFORMER_DIR="sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20"
FIRERED_DIR="sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26"
# Local punctuation model (issue #4 post-processing; the punctuator's default
# model_dir must match this name). Opt-in: ~62 MB, adds ~0.2 s per utterance.
PUNCT_BASE="${NEXTLESS_PUNCT_BASE:-https://github.com/k2-fsa/sherpa-onnx/releases/download/punctuation-models}"
PUNCT_DIR="sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8"

DATA_HOME="${NEXTLESS_DATA_HOME:-$HOME/.local/share/nextless}"
RUNTIME_DIR="$DATA_HOME/sherpa-onnx"
MODELS_DIR="$DATA_HOME/models"

usage() {
    cat <<'EOF'
Usage: nextless-get-models [options]   (in the repo: tools/get_models.sh)

Downloads the sherpa-onnx runtime and ASR models into ~/.local/share/nextless
so the Zipformer / FireRed local backends work. Re-running skips what is
already installed.

Options:
  --backend=zipformer|firered|both   which model(s) to install
                                     (without it: asks on a terminal,
                                      defaults to "both" elsewhere)
  --arch=x86_64|aarch64              override `uname -m` (for testing the
                                     asset-name mapping without ARM hardware)
  --dry-run                          print exactly what would happen; no
                                     network, no writes
  --punctuation                      also install the local punctuation
                                     model (opt-in, ~62 MB; on a terminal
                                     the script asks, elsewhere this flag
                                     is the only way to get it)
  --no-punctuation                   never install it, even interactively
  --force                            reinstall pieces that are already present
  -y, --yes                          skip the confirmation prompt
  -h, --help                         this help
EOF
}

backend=""
arch_override=""
dry_run=false
force=false
assume_yes=false
want_punct=false
punct_specified=false

for arg in "$@"; do
    case "$arg" in
        --backend=*) backend="${arg#*=}" ;;
        --arch=*)    arch_override="${arg#*=}" ;;
        --dry-run)   dry_run=true ;;
        --force)     force=true ;;
        -y|--yes)    assume_yes=true ;;
        --punctuation)    want_punct=true;  punct_specified=true ;;
        --no-punctuation) want_punct=false; punct_specified=true ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "get_models: unknown option '$arg' (see --help)" >&2; exit 2 ;;
    esac
done

case "$backend" in
    ""|zipformer|firered|both) ;;
    *) echo "get_models: --backend must be zipformer, firered or both" >&2; exit 2 ;;
esac

machine="$(uname -m)"
arch="${arch_override:-$machine}"
case "$arch" in
    x86_64)
        RUNTIME_ASSET="sherpa-onnx-v${VERSION}-linux-x64-shared.tar.bz2"
        RUNTIME_TOP="sherpa-onnx-v${VERSION}-linux-x64-shared"
        ;;
    aarch64)
        # -shared-cpu suffix is part of the real asset name; the x64-style
        # "linux-aarch64-shared" name 404s upstream (verified 2026-10).
        RUNTIME_ASSET="sherpa-onnx-v${VERSION}-linux-aarch64-shared-cpu.tar.bz2"
        RUNTIME_TOP="sherpa-onnx-v${VERSION}-linux-aarch64-shared-cpu"
        ;;
    *)
        echo "get_models: unsupported architecture '$arch' ($machine)." >&2
        echo "Only x86_64 and aarch64 runtime assets are handled here;" >&2
        echo "see README section \"Get models (local backends)\" for the manual path." >&2
        exit 1
        ;;
esac

RUNTIME_URL="$RELEASE_BASE/$RUNTIME_ASSET"
zipformer_url="$MODEL_BASE/$ZIPFORMER_DIR.tar.bz2"
firered_url="$MODEL_BASE/$FIRERED_DIR.tar.bz2"
punct_url="$PUNCT_BASE/$PUNCT_DIR.tar.bz2"

runtime_present() {
    [ -x "$RUNTIME_DIR/bin/sherpa-onnx" ] &&
    [ -x "$RUNTIME_DIR/bin/sherpa-onnx-offline" ] &&
    ls "$RUNTIME_DIR"/lib/libonnxruntime*.so >/dev/null 2>&1
}

# A model counts as present when its expected files are all there — a
# directory left by an interrupted --force run must not pass.
model_present() { # $1=dir name, rest=files
    local name="$1"; shift
    [ -d "$MODELS_DIR/$name" ] || return 1
    local f
    for f in "$@"; do
        [ -f "$MODELS_DIR/$name/$f" ] || return 1
    done
    return 0
}

human_size() { # bytes -> "xxx MiB"
    awk -v b="$1" 'BEGIN{
        if (b >= 1073741824) printf "%.1f GiB", b/1073741824;
        else printf "%.0f MiB", b/1048576 }'
}

# HEAD the asset and print its size, or "unknown" — never fail the run.
size_of() {
    local url="$1" bytes
    bytes=$(curl -sIL --max-time 15 "$url" 2>/dev/null \
            | awk 'tolower($1)=="content-length:"{n=$2} END{print n+0}') || bytes=0
    if [ "${bytes:-0}" -gt 0 ]; then human_size "$bytes"; else echo "unknown"; fi
}

want_zipformer=false
want_firered=false
case "$backend" in
    "")            want_zipformer=true; want_firered=true ;;
    both)          want_zipformer=true; want_firered=true ;;
    zipformer)     want_zipformer=true ;;
    firered)       want_firered=true ;;
esac

# Non-interactive without an explicit --backend defaults to both; state it.
if [ -z "$backend" ] && [ ! -t 0 ]; then
    echo "get_models: no terminal and no --backend, installing both models."
fi

# Interactive pick, unless --backend was given or stdin is not a terminal.
if [ -z "$backend" ] && [ -t 0 ] && ! $dry_run; then
    echo "Which local backend do you want?"
    echo "  [1] Zipformer  bilingual zh-en, streaming, ~360 MB (smaller, faster)"
    echo "  [2] FireRed    ASR2 int8 zh-en, offline,   ~800 MB download (more accurate, slower)"
    echo "  [3] both       (default)"
    read -r -p "Choice [1/2/3]: " reply
    case "$reply" in
        1) want_zipformer=true;  want_firered=false ;;
        2) want_zipformer=false; want_firered=true ;;
        *) ;;
    esac
fi

# Punctuation is an opt-in add-on: ask on a terminal (clear size notice,
# issue #4), elsewhere only --punctuation enables it. Needs the runtime,
# which is being installed anyway whenever anything else is fetched.
if ! $punct_specified && [ -t 0 ] && ! $dry_run; then
    if $want_zipformer || $want_firered; then
        read -r -p "Also install the local punctuation model (~62 MB, adds ~0.2s per utterance)? [Y/n] " reply
        case "$reply" in [nN]*) want_punct=false ;; *) want_punct=true ;; esac
    fi
fi

# What actually needs fetching, decided once and reused by plan + actions.
do_runtime=false
if ! runtime_present || $force; then do_runtime=true; fi
do_zipformer=false
if $want_zipformer && { ! model_present "$ZIPFORMER_DIR" encoder-epoch-99-avg-1.onnx \
        decoder-epoch-99-avg-1.onnx joiner-epoch-99-avg-1.onnx tokens.txt || $force; }; then
    do_zipformer=true
fi
do_firered=false
if $want_firered && { ! model_present "$FIRERED_DIR" encoder.int8.onnx decoder.int8.onnx \
        tokens.txt || $force; }; then
    do_firered=true
fi
do_punct=false
if $want_punct && { ! model_present "$PUNCT_DIR" model.int8.onnx tokens.json \
        config.yaml || $force; }; then
    do_punct=true
fi

# ---- plan ------------------------------------------------------------------
echo "Nextless model helper — arch: $arch"
echo "  data home : $DATA_HOME"
echo "  runtime   : $RUNTIME_URL  [$( $do_runtime && echo install || echo skip )]"
if $want_zipformer; then
    echo "  Zipformer : $zipformer_url  [$( $do_zipformer && echo install || echo skip )]"
fi
if $want_firered; then
    echo "  FireRed   : $firered_url  [$( $do_firered && echo install || echo skip )]"
fi
if $want_punct; then
    echo "  Punct     : $punct_url  [$( $do_punct && echo install || echo skip )]"
fi

if $dry_run; then
    echo "--dry-run: nothing downloaded, nothing written."
    exit 0
fi

# Show sizes before downloading (only for what we will actually fetch).
total_note=""
if $do_runtime; then total_note+="runtime $(size_of "$RUNTIME_URL")"; fi
if $do_zipformer; then total_note+="${total_note:+, }Zipformer $(size_of "$zipformer_url")"; fi
if $do_firered; then total_note+="${total_note:+, }FireRed $(size_of "$firered_url")"; fi
if $do_punct; then total_note+="${total_note:+, }punctuation $(size_of "$punct_url")"; fi

if [ -n "$total_note" ]; then
    echo "  to download: $total_note"
    if ! $assume_yes && [ -t 0 ]; then
        read -r -p "  Proceed? [Y/n] " reply
        case "$reply" in [nN]*) echo "get_models: aborted, nothing changed."; exit 0 ;; esac
    fi
else
    echo "  everything you asked for is already installed (--force to redo)."
    exit 0
fi

# ---- staging ----------------------------------------------------------------
# Kept inside the data home: cross-device /tmp writes would both violate the
# "stay under ~/.local/share/nextless" rule and break the atomic mv below.
STAGING="$DATA_HOME/.get-models-tmp.$$"
cleanup() {
    [ -n "${STAGING:-}" ] && rm -rf "$STAGING"
}
trap cleanup EXIT
mkdir -p "$DATA_HOME" "$STAGING" "$MODELS_DIR"

fail() {
    echo "get_models: $*" >&2
    echo "Nothing was moved into place; staging files removed by cleanup." >&2
    exit 1
}

fetch_and_extract() { # $1=url $2=expected top dir
    local url="$1" expect="$2" archive="$STAGING/$(basename "$1")"
    # -C - resume is deliberately off: a truncated retry into a tarball we
    # are about to fully extract adds a corruption path for no real gain.
    if ! curl -fL --retry 3 --connect-timeout 10 --progress-bar -o "$archive" "$url"; then
        fail "download failed: $url — check your network, then re-run; already-installed pieces are skipped."
    fi
    tar -xjf "$archive" -C "$STAGING" || fail "could not extract $archive (corrupt download?)"
    [ -d "$STAGING/$expect" ] || fail "archive extracted a directory other than '$expect'; README paths would not match — aborting."
    rm -f "$archive"
}

install_runtime() {
    if runtime_present && ! $force; then
        echo "runtime: already installed, skipping."
        return
    fi
    echo "runtime: fetching sherpa-onnx v$VERSION ($arch)..."
    fetch_and_extract "$RUNTIME_URL" "$RUNTIME_TOP"
    # bin/ and lib/ both required: the binaries carry rpath $ORIGIN/../lib.
    [ -d "$STAGING/$RUNTIME_TOP/bin" ] && [ -d "$STAGING/$RUNTIME_TOP/lib" ] \
        || fail "runtime archive lacks bin/ or lib/."
    if $force; then rm -rf "$RUNTIME_DIR"; fi
    mkdir -p "$RUNTIME_DIR"
    cp -a "$STAGING/$RUNTIME_TOP/bin" "$STAGING/$RUNTIME_TOP/lib" "$RUNTIME_DIR/" \
        || fail "could not copy the runtime into $RUNTIME_DIR."
    rm -rf "$STAGING/$RUNTIME_TOP"
    echo "runtime: installed to $RUNTIME_DIR/{bin,lib}"
}

install_model() { # $1=url $2=dir name $3=label, rest=expected files
    local url="$1" name="$2" label="$3"; shift 3
    if model_present "$name" "$@" && ! $force; then
        echo "$label: already installed, skipping."
        return
    fi
    echo "$label: fetching (this is the big one, be patient)..."
    fetch_and_extract "$url" "$name"
    local f
    for f in "$@"; do
        [ -f "$STAGING/$name/$f" ] || fail "$name extracted without expected file '$f'."
    done
    # We only get here when the dir is absent, incomplete (--force), so
    # dropping it before the rename is safe; without this, mv would bury the
    # fresh copy inside the old partial dir.
    rm -rf "$MODELS_DIR/$name"
    # same filesystem as staging -> rename(2), effectively atomic
    mv -T "$STAGING/$name" "$MODELS_DIR/$name" \
        || fail "could not move $name into $MODELS_DIR."
    echo "$label: installed to $MODELS_DIR/$name"
}

install_runtime
if $want_zipformer; then
    install_model "$zipformer_url" "$ZIPFORMER_DIR" "Zipformer" \
        encoder-epoch-99-avg-1.onnx decoder-epoch-99-avg-1.onnx \
        joiner-epoch-99-avg-1.onnx tokens.txt
fi
if $want_firered; then
    install_model "$firered_url" "$FIRERED_DIR" "FireRed" \
        encoder.int8.onnx decoder.int8.onnx tokens.txt
fi
if $want_punct; then
    install_model "$punct_url" "$PUNCT_DIR" "Punctuation" \
        model.int8.onnx tokens.json config.yaml
fi

echo
echo "Done. Restart fcitx5 to pick everything up:  fcitx5 -r -d"
echo "Switch backend with Shift+trigger (see README)."
