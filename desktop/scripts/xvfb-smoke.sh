#!/usr/bin/env bash
# Headless GUI smoke test for the MIB Studio desktop app (Phase 3, epic #246).
#
# Launches the built Tauri binary under a virtual framebuffer (Xvfb) and
# asserts it initializes its GTK/WebKit event loop and stays alive — i.e. the
# window + webview came up without crashing. The webview env vars are the
# standard container/headless WebKitGTK workarounds (no GPU/dmabuf).
#
# Usage: desktop/scripts/xvfb-smoke.sh <path-to-binary> [alive_seconds]
set -euo pipefail

BIN="${1:?usage: xvfb-smoke.sh <binary> [alive_seconds]}"
ALIVE="${2:-10}"

if [[ ! -x "$BIN" ]]; then
  echo "smoke: binary not found or not executable: $BIN" >&2
  exit 2
fi

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

# HDF5 shared libs live in a versioned subdir on Ubuntu.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/usr/lib/x86_64-linux-gnu/hdf5/serial"
# Offline: avoid the network LUT-manifest fetch at backend startup.
export MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL="${MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL:-file:///nonexistent/mib-lut-manifest.json}"

xvfb-run -a --server-args="-screen 0 1024x768x24" \
  env WEBKIT_DISABLE_DMABUF_RENDERER=1 \
      WEBKIT_DISABLE_COMPOSITING_MODE=1 \
      LIBGL_ALWAYS_SOFTWARE=1 \
  "$BIN" >"$LOG" 2>&1 &
PID=$!

sleep "$ALIVE"

if kill -0 "$PID" 2>/dev/null; then
  echo "smoke: OK — GUI alive after ${ALIVE}s (window + webview initialized)"
  kill "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  exit 0
fi

echo "smoke: FAIL — process exited within ${ALIVE}s" >&2
echo "--- log ---" >&2
cat "$LOG" >&2
exit 1
