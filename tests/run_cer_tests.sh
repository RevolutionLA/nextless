#!/usr/bin/env bash
# tests/meson.build registration for the issue #8 CER tooling (python part).
set -euo pipefail
cd "$(dirname "$0")"
command -v python3 >/dev/null 2>&1 || { echo "python3 missing"; exit 1; }
python3 -m unittest -q test_cer
