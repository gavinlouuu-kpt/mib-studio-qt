#!/usr/bin/env python3
"""A fake `pzrec` for the host rehearsal of the #693 slot (tools/ring_vs_ssd/rehearsal/README.md). NOT the board's tool.

Studio runs it as the board's pzrec (MIB_PZREC) with the verbs it uses: status, runs, start, stop, snapshot --window-only and read. It keeps its run table in
$MIB_REHEARSAL_DIR/ssd.json and takes the frames from $MIB_REHEARSAL_DIR/emu.json, which the rehearsal execution provider writes at ARM and at STOP
(include/backend/processing/pz/RehearsalProvider.h): the frame ids are a pure function of the monotonic clock (CLOCK_MONOTONIC, shared by the two processes) since the ARM, and the pixels, mask and cells of a frame a
pure function of its id, so the ring (Studio) and the "SSD" (this script) hold the same frames the way the PL's two taps do. The formulas below mirror
RehearsalProvider::fillFrame; keep them in step.

Knobs (environment, all optional):
  REHEARSAL_STOP_S        seconds `stop` takes (the drain), default 1.5
  REHEARSAL_DROP_EVERY=N  the "SSD" drops every Nth frame id (not stored, counted in `dropped`): a drain that cannot keep up
  REHEARSAL_DROP_UNTIL=N  ...only for frame ids up to N (the drain recovers: the ring's window is then drop-free)
  REHEARSAL_READ_MBPS     throttle `read` to this rate (default: unthrottled)
  REHEARSAL_CORRUPT_ID=N  `read` flips one gray byte of frame id N (a mutant: ring_vs_ssd must FAIL)
Exit codes follow pzrec: 0 ok, 1 error with a JSON line on stderr.
"""
import fcntl
import json
import math
import os
import sys
import time
from pathlib import Path

REC = 59392
SECTORS = 116
HERE = Path(__file__).resolve().parent
D = Path(os.environ.get("MIB_REHEARSAL_DIR", "/nonexistent"))


def fail(msg, code=1, name="ARGS"):
    sys.stderr.write(json.dumps({"error": msg, "code": code, "name": name}) + "\n")
    sys.exit(1)


def load_emu():
    try:
        return json.loads((D / "emu.json").read_text())
    except (OSError, ValueError):
        return None


def head_at(emu, now_ns):
    end = now_ns if emu["running"] else emu["stop_ns"]
    return max(0, int(math.floor((end - emu["t0_ns"]) * 1e-9 * emu["fps"])))


class State:
    def __enter__(self):
        D.mkdir(parents=True, exist_ok=True)
        self.lock = open(D / "ssd.lock", "w")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        p = D / "ssd.json"
        self.s = json.loads(p.read_text()) if p.exists() else {"next_run_id": 1, "runs": [], "private": {}}
        return self.s

    def __exit__(self, *a):
        tmp = D / "ssd.json.tmp"
        tmp.write_text(json.dumps(self.s))
        tmp.replace(D / "ssd.json")
        fcntl.flock(self.lock, fcntl.LOCK_UN)
        self.lock.close()


def dropped(fid, every):
    until = int(os.environ.get("REHEARSAL_DROP_UNTIL", "0"))     # 0: all run long; N: only frame ids up to N (a drain that recovers)
    return every > 0 and fid % every == 0 and (until == 0 or fid <= until)


def counters_for(run, emu, now_ns, every):
    """seen / written / dropped of an open run so far."""
    last = head_at(emu, now_ns) if emu else run["first_frame_id"] - 1
    seen = max(0, last - run["first_frame_id"] + 1)
    drops = sum(1 for f in range(run["first_frame_id"], last + 1) if dropped(f, every)) if every else 0
    return seen, seen - drops, drops, last


def status_json(s):
    now = time.monotonic_ns()
    emu = load_emu()
    every = int(os.environ.get("REHEARSAL_DROP_EVERY", "0"))
    op = next((r for r in s["runs"] if r["open"]), None)
    c = {"seen": 0, "empty_filtered": 0, "invalid_not_sampled": 0, "passed": 0, "written": 0, "dropped": 0, "failed": 0, "bytes_written": 0, "drain_kbps": 0,
         "first_frame_id": 0, "last_frame_id": 0}
    if op and emu:
        seen, written, drops, last = counters_for(op, emu, now, every)
        c.update(seen=seen, passed=seen, written=written, dropped=drops, bytes_written=written * REC, drain_kbps=int(emu["fps"] * REC / 1000), first_frame_id=op["first_frame_id"],
                 last_frame_id=last)
    state = "READY" if not op else ("RECORDING" if (emu and emu["running"]) else "STOPPING")
    return {"state": state, "state_code": 3, "last_error": "OK", "raw_sectors": 417792, "head_lba": 2048, "tail_lba": 2048,
            "free_sectors": 415744, "min_run_sectors": 131072, "runs": len(s["runs"]), "next_run_id": s["next_run_id"], "table_entries": 256, "open_run": bool(op),
            "open_run_id": op["run_id"] if op else 0, "open_start_unix_ms": op["start_unix_ms"] if op else 0, "open_client_tag": op["client_tag"] if op else 0,
            "open_wall_source": op["wall_source"] if op else 0, "recovered_runs": 0, "skipped_bad_entries": 0, "recovered_ids": [0] * 8, "counters": c}


def arg(argv, name, default=None):
    return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else default


def verb_start(argv):
    with State() as s:
        if any(r["open"] for r in s["runs"]):
            fail("a run is already open", 1, "ACTIVE")
        emu = load_emu()
        if not emu or not emu["running"]:
            fail("the drain is not armed: start the provider first", 1, "BAD_STATE")
        flt = {"all": 0, "any": 1, "valid": 2}.get(arg(argv, "--filter", "valid"), 2)
        rid = s["next_run_id"]
        s["next_run_id"] = rid + 1
        row = {"run_id": rid, "flags": 0, "open": True, "deleted": False, "incomplete": 0, "start_lba": 2048 + (rid - 1) * 4000000, "end_lba": 2048 + (rid - 1) * 4000000,
               "start_unix_ms": int(arg(argv, "--start-ms", "0")), "wall_source": int(arg(argv, "--wall-source", "0")), "client_tag": int(arg(argv, "--tag", "0")), "filter": flt,
               "sampler_n": int(arg(argv, "--sampler", "0")), "rec_sectors": SECTORS, "record_sectors": SECTORS, "first_frame_id": 1, "last_frame_id": 0, "first_ticks": 0,
               "last_ticks": 0, "tick_hz": emu["tick_hz"], "seen": 0, "empty_filtered": 0, "invalid_not_sampled": 0, "passed": 0, "written": 0, "dropped": 0, "failed": 0,
               "recoveries": 0, "reason": 0, "counts_unknown": False}
        s["runs"].append(row)
        s["private"][str(rid)] = {"epoch": emu["epoch"], "fps": emu["fps"], "tick_base": emu["tick_base"]}
        print(json.dumps(status_json(s)))


def verb_stop(argv):
    delay = float(os.environ.get("REHEARSAL_STOP_S", "1.5"))
    every = int(os.environ.get("REHEARSAL_DROP_EVERY", "0"))
    abort = "--abort" in argv
    time.sleep(0 if abort else delay)          # the drain empties the ring; status reads STOPPING meanwhile and the lock is not held
    with State() as s:
        op = next((r for r in s["runs"] if r["open"]), None)
        if op is None:
            print(json.dumps(status_json(s)))
            return
        emu = load_emu()
        seen, written, drops, last = counters_for(op, emu, time.monotonic_ns(), every)
        pv = s["private"][str(op["run_id"])]
        tick = lambda f: ticks_of(f, pv["tick_base"], emu["tick_hz"], pv["fps"])
        op.update(open=False, last_frame_id=last, first_ticks=tick(op["first_frame_id"]), last_ticks=tick(max(last, 1)), seen=seen, passed=seen, written=written, dropped=drops,
                  reason=8 if abort else 0, end_lba=op["start_lba"] + written * SECTORS + 1)
        print(json.dumps(status_json(s)))


def frame_content(fid):
    """(gray 96x512 uint8, packed MASK1 bytes, objects) of frame `fid`: mirrors RehearsalProvider::fillFrame."""
    import numpy as np

    sys.path.insert(0, str(HERE.parents[1] / "pzrec_to_h5"))
    import synthetic_records as T

    y, x = np.mgrid[0:96, 0:512]
    gray = ((x * 3 + y * 5 + fid * 7) & 0xFF).astype(np.uint8)
    bits = (((x ^ y ^ fid) & 15) == 0).astype(np.uint8)
    objs = [T.cell(1), T.cell(2, reason=3)] if fid % 3 == 0 else ([T.cell(1)] if fid % 3 == 1 else [])
    return gray, np.packbits(bits, axis=1, bitorder="little").tobytes(), objs


def ticks_of(fid, tick_base, tick_hz, fps):
    return tick_base + int(math.floor(fid * tick_hz / fps + 0.5))


def cell_words(objs):
    """The 19 words of a cell in a MIBR packet: payload[0..14], y<<16|x, h<<16|w, cells<<24|index<<16|valid, payload validity."""
    out = []
    for i, o in enumerate(objs):
        x, y, w, h = o["bbox"]
        out.append(list(o["payload"]) + [x | y << 16, w | h << 16, (len(objs) & 0xFF) << 24 | (i & 0xFF) << 16 | (1 if o.get("flags", 8) & 8 else 0), o.get("validity", 0x7FFF)])
    return out


def record_for(fid, run, pv, drops_before):
    sys.path.insert(0, str(HERE.parents[1] / "pzrec_to_h5"))
    import synthetic_records as T

    gray, packed, objs = frame_content(fid)
    rec, _, _ = T.build_record(fid, ticks_of(fid, pv["tick_base"], run["tick_hz"], pv["fps"]), run_id=run["run_id"], drops=drops_before, epoch=pv["epoch"], objects=objs)
    buf = bytearray(rec)
    buf[4096:4096 + 49152] = gray.tobytes()
    buf[53248:53248 + 6144] = packed
    if fid == int(os.environ.get("REHEARSAL_CORRUPT_ID", "0")):
        buf[4096 + 1000] ^= 0x40
    return bytes(buf)


def verb_read(argv):
    every = int(os.environ.get("REHEARSAL_DROP_EVERY", "0"))
    rid = int(argv[1])
    with State() as s:
        run = next((r for r in s["runs"] if r["run_id"] == rid), None)
        if run is None:
            fail(f"no such run {rid}", 1, "NO_SUCH_RUN")
        if run["open"]:
            fail("the run is still open", 1, "ACTIVE")
        pv = dict(s["private"][str(rid)])
        run = dict(run)
    ids = [f for f in range(run["first_frame_id"], run["last_frame_id"] + 1) if not dropped(f, every)]
    frm = int(arg(argv, "--from", "0"))
    cnt = int(arg(argv, "--count", str(len(ids) - frm)))
    mbps = float(os.environ.get("REHEARSAL_READ_MBPS", "0"))
    out = sys.stdout.buffer
    t0 = time.time()
    sent = 0
    for n, fid in enumerate(ids[frm:frm + cnt]):
        prev = ids[frm + n - 1] if frm + n > 0 else None
        gap = 0 if prev is None else fid - prev - 1  # passing records the drain dropped since the previous stored record (pzrec_records.md: drops_before u8, saturating)
        out.write(record_for(fid, run, pv, min(gap, 255)))
        sent += 1
        if mbps > 0:
            ahead = sent * REC / (mbps * 1e6) - (time.time() - t0)
            if ahead > 0:
                time.sleep(ahead)
    out.flush()
    if "--summary" in argv:
        sys.stderr.write(json.dumps({"run_id": rid, "records": sent, "bytes": sent * REC, "retries": 0, "seconds": round(time.time() - t0, 3), "MBps": 0}) + "\n")


def main():
    argv = sys.argv[1:]
    if not argv:
        fail("usage: fake_pzrec VERB IMG ...")
    verb = argv[0]
    if verb == "status":
        with State() as s:
            print(json.dumps(status_json(s)))
    elif verb == "runs":
        with State() as s:
            rows = []
            for r in s["runs"]:
                r = dict(r)
                if r["open"]:
                    emu = load_emu()
                    seen, written, drops, last = counters_for(r, emu, time.monotonic_ns(), int(os.environ.get("REHEARSAL_DROP_EVERY", "0"))) if emu else (0, 0, 0, 0)
                    r.update(seen=seen, passed=seen, written=written, dropped=drops, last_frame_id=last)
                rows.append(r)
            print(json.dumps(rows))
    elif verb == "start":
        verb_start(argv)
    elif verb == "stop":
        verb_stop(argv)
    elif verb == "snapshot":
        with State() as s:
            recording = any(r["open"] for r in s["runs"])
        print(json.dumps({"state": "RUNNING" if recording else "IDLE", "state_code": 2 if recording else 0}))
    elif verb == "read":
        verb_read(argv)
    else:
        fail(f"unexpected verb {verb}")


if __name__ == "__main__":
    main()
