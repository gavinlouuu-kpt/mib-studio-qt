# Host rehearsal of the #693 slot

A Studio on this host runs the whole slot chain without a board: **Run, an SSD run, Stop (the ring is held), a read of every ring frame, the export, a re-read, `ring_vs_ssd`**.
It is for rehearsing the slot driver and the comparison before the board is used; it is **not** a qualification of anything on the board and never runs there.

| Piece | What it is |
|---|---|
| `MIB_EXECUTION_PROVIDER=rehearsal:<fps>[:<ring frames>]` + `MIB_REHEARSAL_DIR` | `RehearsalProvider` (`include/backend/processing/pz/RehearsalProvider.h`): a frame ring with no thread. Frame ids are a pure function of the monotonic clock (CLOCK_MONOTONIC, shared by the two processes) since the ARM (`floor(elapsed * fps)`), one epoch per ARM, frozen by STOP, a window of the newest N frames; pixels, mask and cells are a pure function of the id. Brings register stubs, so Run, the LED and the D3 ring hold work. Writes `emu.json` in `MIB_REHEARSAL_DIR`. The board's unit sets `pz`; this spec is rejected unless `MIB_REHEARSAL_DIR` names a directory. |
| `fake_pzrec.py` (set as `MIB_PZREC`) | the verbs Studio uses (`status`, `runs`, `start`, `stop`, `snapshot --window-only`, `read`), a run table in `MIB_REHEARSAL_DIR/ssd.json`, records built from the same ids as the ring (59,392 B, the vendored encoder). Knobs: `REHEARSAL_STOP_S` (drain time), `REHEARSAL_DROP_EVERY=N` (+ `REHEARSAL_DROP_UNTIL=M`) drops frame ids like a drain that cannot keep up, `REHEARSAL_CORRUPT_ID=N` flips one pixel byte (a mutant), `REHEARSAL_READ_MBPS`. |
| `../slot_driver.py` | one cycle through Studio's own commands (WebSocket) and the export route; the same script drives the board slot. |
| `run_rehearsal.sh OUT FPS[:SECONDS[:DROP_EVERY]]...` | starts a host Studio per cycle (127.0.0.1:18427, never the tunnel's 8427, systemd user unit with a memory limit), runs the driver, stops Studio. |

```
cmake --preset linux-backend-only ... && cmake --build build/linux-backend --target mib_backend
(cd crates/mib-bridge-server && MIB_BRIDGE_NO_CMAKE=1 cargo build)          # the host server
tools/ring_vs_ssd/rehearsal/run_rehearsal.sh /tmp/reh 5000:8 1000:10 5000:6:97
```

What a rehearsal shows (and does not). It exercises Studio's real coordinator, ring status and frame reads, D3 hold, the acknowledged Start, the export route with a verifying reader, and the driver and `ring_vs_ssd` end to end, at any rate you give.
It cannot say anything about the PL, the drain's throughput, the SSD, or the 5 kHz timing: the rate here is a number in the clock formula, and Studio's own Run is still 5 kHz.
A drop-free ring window passes; drops that run to the end of the run (`REHEARSAL_DROP_EVERY` alone) make the SSD tail shorter than the ring and `ring_vs_ssd` fails criteria a and d on purpose (the comparison assumes a drop-free tail); a run whose drops stop earlier (`REHEARSAL_DROP_UNTIL`) passes.

Tests: `processing.rehearsal_provider` (C++: the content equals `testdata/frames.json`, which `test_rehearsal.py --write` regenerates from the fake's formulas, and the ring behaviour) and `tools.ring_vs_ssd_rehearsal` (the fake's run table, drops, records through the tool's own decoder, the corrupt knob).
