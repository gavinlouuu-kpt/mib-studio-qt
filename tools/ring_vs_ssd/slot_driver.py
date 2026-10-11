#!/usr/bin/env python3
"""One cycle of the #693 slot, driven through Studio's own interface (WebSocket + the export route), then judged by ring_vs_ssd.

  python3 slot_driver.py http://HOST:8427 OUT --label R3-5000fps [--duration 10] [--idle-s 600] [--reread 50] [--sample-hz 0] [--resume-at-end]

What it does (Studio owns the window: nothing here touches pzrec, the registers or the board other than through Studio's commands):
  1. take_control, read the instrument / SSD / ring status (pre-*.json); switch to Run if the instrument is not there (set_instrument_mode run)
  2. experiment_start (acknowledgeDiscardRing true: a held ring from an earlier cycle is discarded on purpose), the filter is the unit's (ALL in the export bundle)
  3. run for --duration seconds; optional --sample-hz polls fetch_ssd_status into ssd-samples.jsonl (t, seen/written/dropped/bytes_written/drain_kbps)
  4. experiment_stop; wait until the run is closed (SSD READY, experiment terminal) and the ring is held (run_frozen)
  5. fetch_ssd_runs (the closed run), fetch_ring_status (ring-status-1.json), then fetch_ring_frame for EVERY sequence first_seq..last_seq (ring/seq-N.mibr)
  6. GET /ssd/runs/ID/records (DL/runID.bin, headers saved), fetch_ssd_runs again (post.runs.json)
  7. fetch_ring_status (ring-status-2.json) and ~--reread spread sequences again (ring2/), wait --idle-s, fetch_ring_status (ring-status-3.json)
  8. ring_vs_ssd.py with all of it -> OUT/ring-vs-ssd/{summary.txt,ring-vs-ssd.json}; exit 0 only when it passes
The rate is not set here (it is whatever Studio's Run is): the label only names the output. --resume-at-end sends ring_resume afterwards (default: the ring stays held).
"""
import argparse
import json
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

from websockets.sync.client import connect

HERE = Path(__file__).resolve().parent


class Studio:
    def __init__(self, base, log):
        self.base = base.rstrip("/")
        self.log = log
        self.seq = 810000
        self.lock = threading.Lock()
        self.ws = connect("ws" + self.base[4:] + "/ws", open_timeout=15, max_size=None)

    def call(self, cmd, args=None, wait=30, binary=False):
        """The reply (the `ok` object, or {'error': ...}); with binary=True the payload bytes of a binary reply."""
        with self.lock:
            self.seq += 1
            rid = self.seq
            self.ws.send(json.dumps({"request_id": rid, "cmd": cmd, "args": args}))
            end = time.time() + wait
            while time.time() < end:
                try:
                    m = self.ws.recv(timeout=max(0.05, end - time.time()))
                except TimeoutError:
                    break
                if isinstance(m, bytes):
                    if int.from_bytes(m[:8], "little") == rid:
                        return m[8:]
                    continue
                try:
                    j = json.loads(m)
                except ValueError:
                    continue
                if j.get("request_id") == rid:
                    return j["ok"] if "ok" in j else {"error": j.get("error", "no ok in reply")}
            return {"error": f"no reply to {cmd} within {wait} s"}

    def get(self, path, timeout=30):
        return json.loads(urllib.request.urlopen(self.base + path, timeout=timeout).read())


def save(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=1, sort_keys=True) if not isinstance(obj, str) else obj)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("out")
    ap.add_argument("--label", required=True, help="names the cycle in every output (put the rate in it: R1-1000fps)")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--idle-s", type=float, default=600.0)
    ap.add_argument("--reread", type=int, default=50)
    ap.add_argument("--sample-hz", type=float, default=0.0)
    ap.add_argument("--stop-timeout", type=float, default=240.0)
    ap.add_argument("--mode-xy", default="152,200", help="Run ROI offset x,y if the instrument must be switched to Run")
    ap.add_argument("--resume-at-end", action="store_true")
    ap.add_argument("--no-judge", action="store_true", help="collect only; do not run ring_vs_ssd")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    logf = open(out / "driver.log", "a")

    def log(msg):
        line = f"{time.strftime('%H:%M:%S')} [{a.label}] {msg}"
        print(line, flush=True)
        logf.write(line + "\n")
        logf.flush()

    st = Studio(a.base, log)
    log(f"connected; healthz {json.dumps(st.get('/healthz'))}")
    save(out / "auth.json", st.get("/auth"))
    log("take_control: " + json.dumps(st.call("take_control")))
    if not st.get("/healthz").get("initialized"):
        r = st.call("init", {"dataDir": ""}, wait=300)  # as the UI does on its first connect: the backend starts here
        log("init: " + json.dumps(r)[:200])
        if r is not True:
            sys.exit("the backend did not initialize")
    inst = st.call("fetch_instrument_status")
    save(out / "pre-instrument.json", inst)
    save(out / "pre-ssd.json", st.call("fetch_ssd_status"))
    save(out / "pre-ring.json", st.call("fetch_ring_status"))
    mode = ((inst or {}).get("mode") or {}).get("mode") if isinstance(inst, dict) else None
    log(f"instrument mode before: {mode!r}")
    if mode != "run":
        x, y = (int(v) for v in a.mode_xy.split(","))
        r = st.call("set_instrument_mode", {"mode": "run", "x": x, "y": y}, wait=60)
        log("set_instrument_mode run: " + json.dumps(r)[:300])
        if isinstance(r, dict) and (r.get("error") or r.get("ok") is False):
            sys.exit("could not switch to Run")

    name = f"slot693_{a.label}.h5"
    ready = st.call("fetch_experiment_readiness", {"outputPath": name})
    save(out / "pre-readiness.json", ready)
    blockers = [g for g in (ready or {}).get("gates", []) if g.get("status") in (2, 3, "fail", "block")] if isinstance(ready, dict) else []
    log("readiness blockers: " + json.dumps([f"{g.get('id')}: {g.get('reason')}" for g in blockers]))

    samples = []
    stop_sampler = threading.Event()

    def sampler():
        f = open(out / "ssd-samples.jsonl", "w")
        period = 1.0 / a.sample_hz
        while not stop_sampler.is_set():
            t = time.time()
            r = st.call("fetch_ssd_status", wait=5)
            oc = (r or {}).get("open_run") if isinstance(r, dict) else None
            f.write(json.dumps({"t": t, "state": (r or {}).get("state") if isinstance(r, dict) else None, "counters": (oc or {}).get("counters", oc)}) + "\n")
            f.flush()
            stop_sampler.wait(max(0.0, period - (time.time() - t)))
        f.close()

    t_start = time.time()
    start = st.call("experiment_start", {"outputPath": name, "acknowledgeDiscardRing": True}, wait=60)
    save(out / "experiment-start.json", start)
    log("experiment_start: " + json.dumps(start)[:400])
    if not (isinstance(start, dict) and start.get("ok") is True and not start.get("error")):
        sys.exit("experiment_start refused")
    th = None
    if a.sample_hz > 0:
        th = threading.Thread(target=sampler, daemon=True)
        th.start()
    time.sleep(a.duration)
    during = st.call("fetch_ssd_status")
    save(out / "during-ssd.json", during)
    t_stop = time.time()
    log("experiment_stop: " + json.dumps(st.call("experiment_stop", wait=60))[:300])
    deadline = time.time() + a.stop_timeout
    state = {}
    while time.time() < deadline:
        es = st.call("fetch_experiment_status")
        ssd = st.call("fetch_ssd_status")
        ring = st.call("fetch_ring_status")
        state = {"experiment_terminal": bool(isinstance(es, dict) and es.get("terminal")), "ssd_state": (ssd or {}).get("state") if isinstance(ssd, dict) else None,
                 "run_frozen": bool(isinstance(ring, dict) and ring.get("run_frozen"))}
        if state["experiment_terminal"] and state["ssd_state"] == "READY" and state["run_frozen"]:
            break
        time.sleep(0.5)
    stop_took = time.time() - t_stop
    stop_sampler.set()
    if th:
        th.join(timeout=5)
    save(out / "post-experiment-status.json", st.call("fetch_experiment_status"))
    log(f"closed after {stop_took:.1f} s: {state}")
    if not all([state.get("experiment_terminal"), state.get("ssd_state") == "READY", state.get("run_frozen")]):
        sys.exit(f"the run did not close and hold the ring: {state}")

    runs = st.call("fetch_ssd_runs")
    save(out / "runs-after-stop.json", runs)
    rows = (runs or {}).get("runs", []) if isinstance(runs, dict) else []
    row = max(rows, key=lambda r: r.get("id", r.get("run_id", 0))) if rows else None
    if not row or row.get("open"):
        sys.exit("no closed run in the run table")
    rid = int(row.get("id", row.get("run_id")))
    log(f"run {rid}: seen {row.get('seen')} written {row.get('written')} dropped {row.get('dropped')} failed {row.get('failed')} frames {row.get('first_frame_id')}..{row.get('last_frame_id')} tick_hz {row.get('tick_hz')}")

    status1 = st.call("fetch_ring_status")
    save(out / "ring-status-1.json", status1)
    first, last = int(status1["first_seq"]), int(status1["last_seq"])
    log(f"ring held: seq {first}..{last} ({status1.get('count')} frames, capacity {status1.get('capacity_frames')}, epoch {status1.get('epoch')}, frozen {status1.get('frozen')}, run_frozen {status1.get('run_frozen')})")
    (out / "ring").mkdir(exist_ok=True)
    t0 = time.time()
    for s in range(first, last + 1):
        r = st.call("fetch_ring_frame", {"seq": s}, wait=30)
        if not isinstance(r, bytes):
            sys.exit(f"fetch_ring_frame {s}: {r}")
        (out / "ring" / f"seq-{s}.mibr").write_bytes(r)
    log(f"read {last - first + 1} ring frames in {time.time() - t0:.1f} s")

    (out / "DL").mkdir(exist_ok=True)
    t0 = time.time()
    try:
        resp = urllib.request.urlopen(f"{st.base}/ssd/runs/{rid}/records", timeout=3600)
    except urllib.error.HTTPError as e:
        save(out / "DL" / f"run{rid}.error", f"{e.code} {e.reason}\n{e.read().decode(errors='replace')}\n")
        sys.exit(f"export refused: {e.code} {e.reason}: {e.read()[:300]!r}")
    heads = {k: v for k, v in resp.getheaders()}
    n = 0
    with open(out / "DL" / f"run{rid}.bin", "wb") as f:
        while chunk := resp.read(1 << 20):
            f.write(chunk)
            n += len(chunk)
    save(out / "DL" / f"run{rid}.headers", heads)
    log(f"export: {n} bytes in {time.time() - t0:.1f} s ({n / max(time.time() - t0, 1e-9) / 1e6:.1f} MB/s), headers {json.dumps({k: v for k, v in heads.items() if k.lower().startswith('x-')})}")
    post = st.call("fetch_ssd_runs")
    save(out / "post.runs.json", post)

    status2 = st.call("fetch_ring_status")
    save(out / "ring-status-2.json", status2)
    (out / "ring2").mkdir(exist_ok=True)
    count = last - first + 1
    picks = sorted({first + (i * (count - 1)) // max(1, a.reread - 1) for i in range(a.reread)}) if count > 1 else [first]
    for s in picks:
        r = st.call("fetch_ring_frame", {"seq": s}, wait=30)
        if not isinstance(r, bytes):
            sys.exit(f"re-read fetch_ring_frame {s}: {r}")
        (out / "ring2" / f"seq-{s}.mibr").write_bytes(r)
    log(f"re-read {len(picks)} frames after the export; idle {a.idle_s:.0f} s")
    time.sleep(a.idle_s)
    status3 = st.call("fetch_ring_status")
    save(out / "ring-status-3.json", status3)
    save(out / "post-ssd.json", st.call("fetch_ssd_status"))

    rc = 0
    if not a.no_judge:
        cmd = [sys.executable, str(HERE / "ring_vs_ssd.py"), "--ring", str(out / "ring"), "--status", str(out / "ring-status-1.json"), "--ring2", str(out / "ring2"),
               "--status2", str(out / "ring-status-2.json"), "--status3", str(out / "ring-status-3.json"), "--run", str(out / "DL" / f"run{rid}.bin"), "--runs",
               str(out / "post.runs.json"), "--run-id", str(rid), "--out", str(out / "ring-vs-ssd")]
        log("judge: " + " ".join(cmd))
        p = subprocess.run(cmd, capture_output=True, text=True)
        (out / "ring-vs-ssd.stdout").write_text(p.stdout + p.stderr)
        rc = p.returncode
        log(f"ring_vs_ssd exit {rc}: {p.stdout.strip().splitlines()[-1] if p.stdout.strip() else p.stderr.strip()[-200:]}")
    if a.resume_at_end:
        log("ring_resume: " + json.dumps(st.call("ring_resume", wait=60))[:300])
    save(out / "summary.json", {"label": a.label, "run_id": rid, "duration_s": a.duration, "stop_s": stop_took, "wall_start": t_start, "ring_vs_ssd_exit": rc, "run_row": row})
    sys.exit(rc)


if __name__ == "__main__":
    main()
