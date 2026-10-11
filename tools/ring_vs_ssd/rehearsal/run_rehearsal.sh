#!/usr/bin/env bash
# Host rehearsal of the #693 slot: a host Studio with the rehearsal provider and the fake pzrec, driven by slot_driver.py, judged by ring_vs_ssd.
# NOT the board. See README.md in this directory.
#
#   run_rehearsal.sh OUT FPS[:DURATION_S[:DROP_EVERY]] ...        e.g.  run_rehearsal.sh /tmp/reh 5000:8 1000:12 5000:8:97
#   BEFORE_DRIVER / AFTER_DRIVER: a command run (sh -c) once Studio is up, before the driver, and after it while Studio still runs, with REH_URL, REH_WORK, REH_DATA,
#   REH_LABEL and REH_FPS in its environment (camera_document_cycle.py is made for them); a failing hook fails the cycle.
#   DRIVER_ARGS="--dl DIR --h5-dir DIR" adds options to every slot_driver.py call; REHEARSAL_CORRUPT_ID / REHEARSAL_DROP_UNTIL pass to the fake pzrec.
#
# Needs: the host backend built (cmake --preset linux-backend-only; mib_backend), crates/mib-bridge-server built with MIB_BRIDGE_NO_CMAKE=1
# (target/debug/yofo-studio-server), python3 with numpy and websockets. Each cycle starts a fresh Studio on 127.0.0.1:$PORT (default 18427, never the
# tunnel's 8427), a fresh data dir and a fresh fake SSD, and stops it afterwards. Exit 0 only when every cycle's ring_vs_ssd passes.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
out=${1:?usage: run_rehearsal.sh OUT FPS[:DURATION_S[:DROP_EVERY]]...}; shift
[ $# -gt 0 ] || { echo "no cycle given" >&2; exit 2; }
port=${PORT:-18427}
server=${SERVER:-$repo/crates/mib-bridge-server/target/debug/yofo-studio-server}
[ -x "$server" ] || { echo "no server at $server (build crates/mib-bridge-server with MIB_BRIDGE_NO_CMAKE=1)" >&2; exit 2; }
mkdir -p "$out"
frames=$out/mock-frames
python3 - "$frames" <<'P'
import struct, sys, zlib
from pathlib import Path
d = Path(sys.argv[1]); d.mkdir(parents=True, exist_ok=True)
def png(path, w, h, seed):
    rows = b"".join(b"\0" + bytes(((x * 3 + y * 5 + seed * 7) & 0xFF) for x in range(w)) for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
for i in range(8): png(d / f"frame_{i:05d}.png", 512, 96, i)
P
overall=0
for cycle in "$@"; do
    IFS=: read -r fps dur drop <<<"$cycle"
    dur=${dur:-8}; drop=${drop:-0}
    label="R-${fps}fps${drop:+-drop$drop}"; [ "$drop" = 0 ] && label="R-${fps}fps"
    work=$out/$label; rm -rf "$work"; mkdir -p "$work/emu" "$work/data"
    unit=rehearsal-studio-$$-$fps
    # Studio runs the fake from a systemd unit whose PATH may lack numpy: pin the interpreter that is on this shell's PATH
    printf '#!/bin/sh\nexec %s %s "$@"\n' "$(command -v python3)" "$here/fake_pzrec.py" > "$work/pzrec"; chmod +x "$work/pzrec"
    echo "== $label: Studio on 127.0.0.1:$port, fake SSD in $work/emu (duration $dur s, drop every $drop)"
    systemd-run --user --unit "$unit" --collect -p MemoryHigh=8G -p MemoryMax=12G \
        -E MIB_PL_SCIENCE=1 -E MIB_CAMERA_MODE=mock -E "MIB_MOCK_CAMERA_DIR=$frames" -E MIB_DISABLED_SERVICES=sqlite,autofocus,trigger,playback \
        -E "MIB_EXECUTION_PROVIDER=rehearsal:$fps:5000" -E "MIB_REHEARSAL_DIR=$work/emu" -E "MIB_PZREC=$work/pzrec" -E MIB_SSD_IMAGE=disk -E MIB_SSD_FILTER=all \
        -E "REHEARSAL_DROP_EVERY=$drop" -E "REHEARSAL_CORRUPT_ID=${REHEARSAL_CORRUPT_ID:-0}" -E "REHEARSAL_DROP_UNTIL=${REHEARSAL_DROP_UNTIL:-0}" -E REHEARSAL_STOP_S=1.5 -E LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/hdf5/serial \
        -E MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL=file:///nonexistent/mib-lut-manifest.json \
        "$server" --listen "127.0.0.1:$port" --data-dir "$work/data" --resource-dir "$repo/resources" --no-token >/dev/null
    trap 'systemctl --user stop "$unit" 2>/dev/null || true' EXIT
    for _ in $(seq 1 120); do curl -sf "http://127.0.0.1:$port/healthz" >/dev/null && break; sleep 1; done
    curl -sf "http://127.0.0.1:$port/healthz" >/dev/null || { echo "Studio did not come up" >&2; journalctl --user -u "$unit" --no-pager | tail -20 >&2; exit 1; }
    rc=0
    export REH_URL="http://127.0.0.1:$port" REH_WORK="$work" REH_DATA="$work/data" REH_LABEL="$label" REH_FPS="$fps"
    if [ -n "${BEFORE_DRIVER:-}" ]; then sh -c "$BEFORE_DRIVER" || rc=$?; fi
    [ "$rc" != 0 ] || python3 "$here/../slot_driver.py" "http://127.0.0.1:$port" "$work/out" --label "$label" --duration "$dur" --idle-s 3 --reread 20 --sample-hz 2 --resume-at-end --mode-xy 152,200 --fps "$fps" ${DRIVER_ARGS:-} || rc=$?
    if [ -n "${AFTER_DRIVER:-}" ] && [ "$rc" = 0 ]; then sh -c "$AFTER_DRIVER" || rc=$?; fi
    journalctl --user -u "$unit" --no-pager > "$work/studio.log" 2>&1 || true
    systemctl --user stop "$unit" 2>/dev/null || true
    trap - EXIT
    echo "== $label: slot_driver exit $rc"
    [ "$rc" = 0 ] || overall=1
done
exit $overall
