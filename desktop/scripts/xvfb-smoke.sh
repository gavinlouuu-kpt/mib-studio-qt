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
SMOKE_PROFILE="$(mktemp -d)"
trap 'rm -f "$LOG"; rm -rf "$SMOKE_PROFILE"' EXIT
# Never restore an operator profile or remembered hardware during a smoke test.
export XDG_CONFIG_HOME="$SMOKE_PROFILE/config"
export XDG_DATA_HOME="$SMOKE_PROFILE/data"
export MIB_CAMERA_MODE=mock
export GSETTINGS_BACKEND=memory

# HDF5 shared libs live in a versioned subdir on Ubuntu.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/usr/lib/x86_64-linux-gnu/hdf5/serial"
# Offline: avoid the network LUT-manifest fetch at backend startup.
export MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL="${MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL:-file:///nonexistent/mib-lut-manifest.json}"

# GNU timeout owns a process group: terminate the wrapper, application and
# descendants together. Killing only xvfb-run leaves the executable mapped,
# which prevents the subsequent Tauri bundle patch (ETXTBSY).
set +e
timeout --signal=TERM --kill-after=5s "${ALIVE}s" \
  xvfb-run -a --server-args="-screen 0 1024x768x24" \
  env WEBKIT_DISABLE_DMABUF_RENDERER=1 \
      WEBKIT_DISABLE_COMPOSITING_MODE=1 \
      LIBGL_ALWAYS_SOFTWARE=1 \
  "$BIN" >"$LOG" 2>&1
STATUS=$?
set -e

if [[ "$STATUS" -eq 124 ]]; then
  echo "smoke: OK — GUI alive after ${ALIVE}s (owned process group stopped)"
  exit 0
fi

echo "smoke: FAIL — process exited within ${ALIVE}s" >&2
echo "--- log ---" >&2
cat "$LOG" >&2
exit 1
