#!/usr/bin/env bash
# YOFO Review parity sign-off (plan 2026-10-01-standalone-review-app, PR 8).
#
#   tools/review_parity/run.sh <out-dir> <file.h5> [<file.h5> …]
#
# For each file: the Qt tab dump (ctest parity.review_qt_dump, build/linux-system),
# the YOFO Review dump through the review bridge (cargo example review_parity,
# build/linux-backend archives), the TS chart numbers (vitest
# desktop/scripts/review-parity-charts.test.ts), then
# compare.py → <out-dir>/<stem>/report.md. Exit 1 if any file has an
# unaccepted difference. Needs: linux-system-release (mib_frontend_tests) and
# linux-backend-only (mib_review_core) builds, desktop/node_modules.
# Inputs used for the sign-off (README.md): the z-adjustment-50v corpus and the
# 512x96 run kept by integration.review_scatter_e2e (MIB_REVIEW_E2E_KEEP_H5).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
[[ $# -ge 2 ]] || { sed -n '2,14p' "$0"; exit 2; }
out="$(realpath -m "$1")"; shift
mkdir -p "$out"

export MIB_BRIDGE_NO_CMAKE=1
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu/hdf5/serial${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
(cd "$repo/crates/mib-bridge" && cargo build -q --features review-only --example review_parity)
yofo_bin="$repo/crates/mib-bridge/target/debug/examples/review_parity"
[[ -x "$yofo_bin" ]] || yofo_bin="$(find "$repo" -path '*/debug/examples/review_parity' -type f | head -1)"

status=0
for f in "$@"; do
  f="$(realpath "$f")"
  d="$out/$(basename "${f%.*}")"
  mkdir -p "$d"
  echo "== $f"
  MIB_REVIEW_PARITY_FILE="$f" MIB_REVIEW_PARITY_OUT="$d" \
    ctest --test-dir "$repo/build/linux-system" -R '^parity\.review_qt_dump$' --output-on-failure > "$d/qt.log" 2>&1 \
    || { echo "Qt dump failed (see $d/qt.log)"; status=1; continue; }
  "$yofo_bin" "$f" "$d" > "$d/yofo.log" 2>&1 || { echo "YOFO dump failed (see $d/yofo.log)"; status=1; continue; }
  (cd "$repo/desktop" && MIB_REVIEW_PARITY_DIR="$d" npx vitest run scripts/review-parity-charts.test.ts > "$d/ts.log" 2>&1) \
    || { echo "TS chart dump failed (see $d/ts.log)"; status=1; continue; }
  python3 "$here/compare.py" "$d" || status=1
done
exit $status
