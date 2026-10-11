# Host rehearsal of the #693 slot

A Studio on this host runs the whole slot chain without a board: **Run, an SSD run, Stop (the ring is held), a read of every ring frame, the export, a re-read, `ring_vs_ssd`**.
It is for rehearsing the slot driver and the comparison before the board is used; it is **not** a qualification of anything on the board and never runs there.

| Piece | What it is |
|---|---|
| `MIB_EXECUTION_PROVIDER=rehearsal:<fps>[:<ring frames>]` + `MIB_REHEARSAL_DIR` | `RehearsalProvider` (`include/backend/processing/pz/RehearsalProvider.h`): a frame ring with no thread. Frame ids are a pure function of the monotonic clock (CLOCK_MONOTONIC, shared by the two processes) since the ARM (`floor(elapsed * fps)`), one epoch per ARM, frozen by STOP, a window of the newest N frames; pixels, mask and cells are a pure function of the id. Brings register stubs, so Run, the LED and the D3 ring hold work. Writes `emu.json` in `MIB_REHEARSAL_DIR`. The board's unit uses the PL-science default; this spec is rejected unless `MIB_REHEARSAL_DIR` names a directory. |
| `fake_pzrec.py` (set as `MIB_PZREC`) | the verbs Studio uses (`status`, `runs`, `start`, `stop`, `snapshot --window-only`, `read`), a run table in `MIB_REHEARSAL_DIR/ssd.json`, records built from the same ids as the ring (59,392 B, the vendored encoder). Knobs: `REHEARSAL_STOP_S` (drain time), `REHEARSAL_DROP_EVERY=N` (+ `REHEARSAL_DROP_UNTIL=M`) drops frame ids like a drain that cannot keep up, `REHEARSAL_CORRUPT_ID=N` flips one pixel byte (a mutant), `REHEARSAL_READ_MBPS`. |
| `../slot_driver.py` | one cycle through Studio's own commands (WebSocket) and the export route; the same script drives the board slot. `--dl DIR` writes the export in the board owner's layout (`run<N>.bin/.headers/.time/.md5`), `--h5-dir DIR` places the experiment's HDF5 file. Only Studio commands are sent; every wait is bounded; Ctrl-C/SIGTERM/failure while its experiment runs sends one `experiment_stop` and waits (bounded) for the run to close, leaving the ring held (checked here: stopped 18 s into a 120 s run, closed 2 s later). |
| `run_rehearsal.sh OUT FPS[:SECONDS[:DROP_EVERY]]...` | starts a host Studio per cycle (127.0.0.1:18427, never the tunnel's 8427, systemd user unit with a memory limit), runs the driver, stops Studio. |

| `camera_document_cycle.py` (`BEFORE_DRIVER` / `AFTER_DRIVER` hooks of `run_rehearsal.sh`) | the PZ7035 camera document (bridge ABI 38) through the real server path: `apply` writes `<data>/camera/camera.json`, checks it, proves the server refuses a path outside that folder and the desktop-only `json` kind, applies it; `verify` (after the driver, Studio still up) checks that the Run entry applied the setting (`fetch_run_fps`, nothing pending) and that the HDF5 run snapshot records `run_timing` {fps, hmax, vmax, shs}. Evidence in `camera-document-{apply,verify}.json`. A host Studio has no Aravis: the camera and the producer are the board's. **`--remote --url URL --out DIR` is the board's form** (`apply`, `verify`, `restore`): it writes nothing locally except the evidence files and makes the document the way the UI does, through the server (`camera_document` default → edit → create, or save with the revision read first, as kind `pz7035` under the server's `<data>/camera/` → `check_pz_camera_document` → `apply_pz_camera_document`); `restore` puts 5000 / auto back the same way so the instrument's data directory is left at the standing setting; `--acknowledge-discard-ring` agrees to the discard of a held ring. Tests: `CameraDocumentRemote` (an in-memory board: no local files, call order, create then save, restore, failed apply fails). |

```
cmake --preset linux-backend-only ... && cmake --build build/linux-backend --target mib_backend
(cd crates/mib-bridge-server && MIB_BRIDGE_NO_CMAKE=1 cargo build)          # the host server
tools/ring_vs_ssd/rehearsal/run_rehearsal.sh /tmp/reh 5000:8 1000:10 5000:6:97
# a 1 kHz cycle with the camera document applied first (REH_H5DIR is where the HDF5 file goes, for the snapshot check):
mkdir -p /tmp/reh/h5; REH_H5DIR=/tmp/reh/h5 DRIVER_ARGS="--h5-dir /tmp/reh/h5" \
  BEFORE_DRIVER="python3 tools/ring_vs_ssd/rehearsal/camera_document_cycle.py apply --fps 1000" \
  AFTER_DRIVER="python3 tools/ring_vs_ssd/rehearsal/camera_document_cycle.py verify --fps 1000" \
  tools/ring_vs_ssd/rehearsal/run_rehearsal.sh /tmp/reh 1000:12
```

What a rehearsal shows (and does not). It exercises Studio's real coordinator, ring status and frame reads, D3 hold, the acknowledged Start, the export route with a verifying reader, and the driver and `ring_vs_ssd` end to end, at any rate you give.
It cannot say anything about the PL, the drain's throughput, the SSD, or the 5 kHz timing: the rate here is a number in the clock formula, and Studio's own Run is still 5 kHz.
A drop-free ring window passes; drops that run to the end of the run (`REHEARSAL_DROP_EVERY` alone) make the SSD tail shorter than the ring and `ring_vs_ssd` fails criteria a and d on purpose (the comparison assumes a drop-free tail); a run whose drops stop earlier (`REHEARSAL_DROP_UNTIL`) passes.

Tests: `processing.rehearsal_provider` (C++: the content equals `testdata/frames.json`, which `test_rehearsal.py --write` regenerates from the fake's formulas, and the ring behaviour) and `tools.ring_vs_ssd_rehearsal` (the fake's run table, drops, records through the tool's own decoder, the corrupt knob).
