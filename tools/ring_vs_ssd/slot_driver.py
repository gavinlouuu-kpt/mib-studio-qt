#!/usr/bin/env python3
"""One cycle of the #693 slot, driven through Studio's own interface (WebSocket + the export route), then judged by ring_vs_ssd.

  python3 slot_driver.py http://HOST:8427 OUT --label R3-5000fps [--duration 10] [--idle-s 600] [--reread 50] [--sample-hz 0] [--resume-at-end] [--dl DIR] [--h5-dir DIR]

What it does (Studio owns the window: nothing here touches pzrec, the registers or the board other than through Studio's commands):
  1. take_control, read the instrument / SSD / ring status (pre-*.json); require Run (set_instrument_mode run only with an explicit --mode-xy; the operator's Run window is never moved by default)
  2. experiment_start (acknowledgeDiscardRing true: a held ring from an earlier cycle is discarded on purpose), the filter is the unit's (ALL in the export bundle)
  3. run for --duration seconds; optional --sample-hz polls fetch_ssd_status into ssd-samples.jsonl (t, seen/written/dropped/bytes_written/drain_kbps)
  4. experiment_stop; wait until the run is closed (SSD READY, experiment terminal) and the ring is held (run_frozen)
  5. fetch_ssd_runs (the closed run), fetch_ring_status (ring-status-1.json), then fetch_ring_frame for EVERY sequence first_seq..last_seq (ring/seq-N.mibr)
  6. GET /ssd/runs/ID/records (DL/runID.bin, headers saved), fetch_ssd_runs again (post.runs.json)
  7. fetch_ring_status (ring-status-2.json) and ~--reread spread sequences again (ring2/), wait --idle-s, fetch_ring_status (ring-status-3.json)
  8. ring_vs_ssd.py with all of it -> OUT/ring-vs-ssd/{summary.txt,ring-vs-ssd.json}; exit 0 only when it passes
--dl DIR writes the export exactly as the board owner's run_export.sh does (DIR/run<N>.bin, run<N>.headers in curl -D style with lowercase names, run<N>.time in seconds, run<N>.md5 written
after the file), so that script can skip its own fetch; default OUT/DL. --h5-dir DIR puts the experiment's HDF5 file there (default: relative to Studio's working directory, /var/lib/yofo-studio on the board).
Notes: take_control makes an operator's browser viewer-only until this driver disconnects; Ctrl-C during the download makes Studio journal 'download ended', which export_accept's BAD_JOURNAL check fails after: re-run the cycle. --h5-dir must not be RAM; the HDF5 destination is checked against fetch_instrument_status storage (not RAM, free >= duration*fps*3*240*2 B, --fps).
Safety: only Studio commands (take_control, init, set_instrument_mode run, fetch_*, experiment_start/stop, ring_resume with --resume-at-end) and the export GET are ever sent; every wait is bounded
(--stop-timeout, per-call waits, the download's socket timeout --dl-timeout). On Ctrl-C, SIGTERM or any failure while the experiment this driver started is still running, it sends experiment_stop once
and waits for the run to close (bounded), then leaves the ring held. It never sends experiment_cancel.
The rate is not set here (it is whatever Studio's Run is): the label only names the output. --resume-at-end sends ring_resume afterwards (default: the ring stays held).
"""
import argparse
import hashlib
import signal
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


def find_key(obj, key):
    """The first value stored under `key` anywhere in a JSON object (None when absent)."""
    if isinstance(obj, dict):
        if key in obj:
            return obj[key]
        for v in obj.values():
            r = find_key(v, key)
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = find_key(v, key)
            if r is not None:
                return r
    return None


def guard_path(label, path):
    """A path under /mnt/hdd must really be on the HDD: an unmounted /mnt/hdd is a directory on the system SSD (findmnt -T)."""
    p = Path(path).expanduser().resolve()
    if str(p) == "/mnt/hdd" or str(p).startswith("/mnt/hdd/"):
        try:
            target = subprocess.run(["findmnt", "-n", "-o", "TARGET", "-T", str(p)], capture_output=True, text=True, timeout=10).stdout.strip()
        except (OSError, subprocess.SubprocessError) as e:
            sys.exit(f"{label} {p}: cannot verify the mount with findmnt: {e}")
        if target in ("", "/"):
            sys.exit(f"{label} {p}: /mnt/hdd is not mounted (findmnt -T says {target or 'nothing'!r}): refusing to write to the system drive")
    return p


def acceptance(rc, report, require, judged=True):
    """The slot's verdict per part from ring_vs_ssd's exit code and ring-vs-ssd.json. Returns (dict, text).
    `judged` False: the comparison was not run (--no-judge). `report` None with rc 0: the comparison ran but its report cannot be read (FAIL).
    TRANSPORT passes only when the comparison passed AND the images it compared are real: every compared frame had its pixels compared (none flagged cut / mask incomplete /
    invalid: a cut frame is an image-transport symptom), no frame is constant, and any required part is non-vacuous (by default images NONTRIVIAL).
    MASKS / RESULTS: COVERED (non-empty and byte-equal: content coverage, not validity; on a bench the background objects make them COVERED), VACUOUS (empty, unproven),
    NOT ASSESSED when the comparison failed, was not run, or the report lacks the part."""
    report = report if isinstance(report, dict) else None
    content = report.get("content") if report else None
    content = content if isinstance(content, dict) else {}
    why = []
    if not judged:
        transport = "NOT JUDGED (the comparison was not run: --no-judge)"
        status = "not_judged"
    elif report is None:
        transport = "FAIL (report unreadable: ring-vs-ssd.json is missing or not an object)"
        status = "fail"
    elif rc != 0 or report.get("ok") is not True:
        transport = "FAIL (the comparison did not pass)"
        status = "fail"
    else:
        verdicts = {"images": "NONTRIVIAL", "masks": "NONEMPTY", "results": "NONEMPTY"}
        for part in sorted(require):
            if content.get(part) != verdicts.get(part):
                why.append(f"{part} {content.get(part, 'missing from the report')} (required {verdicts.get(part, '?')})")
        frames, compared, flagged = content.get("frames"), content.get("frames_compared"), content.get("frames_flagged")
        if not isinstance(frames, int) or not isinstance(compared, int) or not isinstance(flagged, int):
            why.append("the report lacks frames / frames_compared / frames_flagged")
        else:
            if frames == 0 or flagged != 0 or frames != compared:
                why.append(f"pixels compared on {frames} of {compared} compared frames, {flagged} flagged (cut / mask incomplete / invalid)")
            if content.get("constant_frames") != 0:
                why.append(f"{content.get('constant_frames')} constant frames (a real run has none)")
        status = "fail" if why else "pass"
        transport = "FAIL (" + "; ".join(why) + ")" if why else ("PASS" if require else "PASS (content not required)")

    def part(name):
        if status in ("not_judged", "fail") and (not judged or report is None or rc != 0 or report.get("ok") is not True):
            return "NOT ASSESSED (the comparison " + ("was not run" if not judged else "did not pass or its report is unreadable") + ")"
        value = content.get(name)
        if value not in ("NONEMPTY", "EMPTY"):
            return "NOT ASSESSED (missing from the report)"
        return "COVERED (non-empty and byte-equal: content coverage, not validity)" if value == "NONEMPTY" else "VACUOUS (empty, unproven: the equality proves nothing about it)"

    res = {
        "transport": transport,
        "transport_status": status,
        "masks": part("masks"),
        "results": part("results"),
        "real_cell_optical": "not assessed",
        "required_content": sorted(require),
        "images": content.get("images"),
        "mono8_mean": content.get("mono8_mean"),
        "mono8_std_mean": content.get("mono8_std_mean"),
        "mono8_distinct_values": content.get("mono8_distinct_values"),
        "constant_frames": content.get("constant_frames"),
        "constant_fraction": content.get("constant_fraction"),
        "identical_to_predecessor": content.get("identical_to_predecessor"),
        "frames": content.get("frames"),
        "frames_compared": content.get("frames_compared"),
        "frames_flagged": content.get("frames_flagged"),
    }
    text = "\n".join([
        f"TRANSPORT: {transport}",
        f"  (comparison criteria a-f, byte equality, accounting; required non-vacuous: {', '.join(sorted(require)) or 'nothing'}; images {res['images']}, MONO8 mean {res['mono8_mean']}, std {res['mono8_std_mean']}, "
        f"{res['mono8_distinct_values']} distinct values, constant frames {res['constant_frames']} ({res['constant_fraction']}), identical to predecessor {res['identical_to_predecessor']}, "
        f"pixels compared on {res['frames']} of {res['frames_compared']} compared frames, flagged {res['frames_flagged']})",
        f"MASKS: {res['masks']}",
        f"RESULTS: {res['results']}",
        "REAL-CELL / OPTICAL ACCEPTANCE: not assessed",
        "NOTE: NONTRIVIAL images mean non-constant bytes (sensor noise alone can exceed the std floor): enough to detect corruption, not evidence of a lit image. COVERED means content coverage, not real cells:",
        "NOTE: a bench without a sample still shows background objects (the no-sample runs 18/19 had about 2 objects per frame with mask bits), so MASKS and RESULTS will likely read COVERED there.",
    ]) + "\n"
    return res, text


def reconnect(st, log):
    """Studio's WebSocket dropped: connect again and take_control (the server hands control to the oldest client, an operator's browser may hold it)."""
    try:
        st.ws.close()
    except Exception:  # noqa: BLE001
        pass
    st.ws = connect("ws" + st.base[4:] + "/ws", open_timeout=15, max_size=None)
    log("reconnected; take_control: " + json.dumps(st.call("take_control")))


def stop_and_wait(st, log, budget=240.0):
    """experiment_stop until it is accepted (or nothing runs), then wait for the run to close; one reconnect if the socket dropped. Returns True when the experiment is terminal."""
    end = time.time() + budget
    reconnected = False
    accepted = False
    last_reply = None
    while time.time() < end:
        try:
            if not accepted:
                last_reply = st.call("experiment_stop", wait=30)
                msg = json.dumps(last_reply)[:200]
                if isinstance(last_reply, dict) and last_reply.get("ok") is True:
                    accepted = True
                    log(f"experiment_stop accepted: {msg}")
                else:
                    log(f"experiment_stop not accepted (will retry): {msg}")
            es = st.call("fetch_experiment_status", wait=10)
            if isinstance(es, dict) and es.get("terminal") and (accepted or "NotActive" in json.dumps(last_reply)):
                log(f"the experiment is closed (terminal) after the stop: {json.dumps(last_reply)[:120]}")
                return True
            if isinstance(es, dict) and es.get("terminal") and not accepted:
                log(f"the experiment is already terminal; the stop reply was {json.dumps(last_reply)[:120]}")
                return True
        except Exception as e:  # noqa: BLE001 - a dropped socket
            if reconnected:
                log(f"the connection is gone again: {e!r}: stop the run in Studio")
                return False
            reconnected = True
            try:
                reconnect(st, log)
            except Exception as e2:  # noqa: BLE001
                log(f"cannot reconnect: {e2!r}: stop the run in Studio")
                return False
            continue
        time.sleep(3)
    log(f"the experiment did NOT close within {budget:.0f} s: last stop reply {json.dumps(last_reply)[:160]}: check Studio")
    return False


def save(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=1, sort_keys=True) if not isinstance(obj, str) else obj)


def _main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("out")
    ap.add_argument("--label", required=True, help="names the cycle in every output (put the rate in it: R1-1000fps)")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--idle-s", type=float, default=600.0)
    ap.add_argument("--reread", type=int, default=50)
    ap.add_argument("--sample-hz", type=float, default=0.0)
    ap.add_argument("--stop-timeout", type=float, default=240.0)
    ap.add_argument("--mode-xy", default=None, help="Run ROI offset x,y: switch the instrument to Run there if it is not in Run (default: refuse and ask for Run in the UI; never moves the operator's window)")
    ap.add_argument("--fps", type=float, default=5000.0, help="the rate Studio's Run is at: only sizes the free-space check of the HDF5 destination")
    ap.add_argument("--resume-at-end", action="store_true")
    ap.add_argument("--dl", help="directory for the export in the board owner's layout (default OUT/DL)")
    ap.add_argument("--h5-dir", default="", help="directory for the experiment's HDF5 file (default: Studio's working directory)")
    ap.add_argument("--dl-timeout", type=float, default=300.0, help="socket timeout of the export download, seconds without a byte")
    ap.add_argument("--require-content", default="images", help="content parts ring_vs_ssd must find non-vacuous for the transport verdict: images,masks,results (default images; '' for none)")
    ap.add_argument("--no-judge", action="store_true", help="collect only; do not run ring_vs_ssd")
    a = ap.parse_args()
    out = guard_path("OUT", a.out)
    if a.dl:
        guard_path("--dl", a.dl)
    if a.h5_dir and (str(Path(a.h5_dir).resolve()).startswith(("/tmp", "/dev/shm", "/run", "/var/tmp")) or a.h5_dir.startswith(("/tmp", "/dev/shm", "/run"))):
        sys.exit("--h5-dir is in RAM (/tmp, /dev/shm, /run): the Studio unit's RAM root is small and a run with cells would exhaust it")
    out.mkdir(parents=True, exist_ok=True)
    logf = open(out / "driver.log", "a")

    def log(msg):
        line = f"{time.strftime('%H:%M:%S')} [{a.label}] {msg}"
        print(line, flush=True)
        logf.write(line + "\n")
        logf.flush()

    st = Studio(a.base, log)
    ACTIVE["st"], ACTIVE["log"] = st, log
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
    m = ((inst or {}).get("mode") or {}) if isinstance(inst, dict) else {}
    mode = m.get("name")
    log(f"instrument mode before: {mode!r} (Run window {m.get('run_x')},{m.get('run_y')}, set {m.get('run_set')})")
    if mode != "run":
        if a.mode_xy is None:
            sys.exit(f"the instrument is in {mode!r}, not Run: put Studio in Run from the UI (this driver does not move the operator's Run window; --mode-xy x,y switches it explicitly)")
        x, y = (int(v) for v in a.mode_xy.split(","))
        r = st.call("set_instrument_mode", {"mode": "run", "x": x, "y": y}, wait=60)
        log("set_instrument_mode run: " + json.dumps(r)[:300])
        if isinstance(r, dict) and (r.get("error") or r.get("ok") is False):
            sys.exit("could not switch to Run")

    storage = (inst or {}).get("storage") if isinstance(inst, dict) else None
    need = a.duration * a.fps * 3 * 240 * 2  # worst case: three cells per frame, 240 B each, doubled for headroom
    if not isinstance(storage, dict) or storage.get("ram") is True or not isinstance(storage.get("free_bytes"), (int, float)) or storage["free_bytes"] < need:
        sys.exit(f"the HDF5 destination is not safe for this run: storage {json.dumps(storage)[:300]}, need {need:.0f} B free and not RAM (set --fps/--duration, or free space)")
    log(f"storage ok: {storage.get('path')} {storage.get('filesystem')}, {storage['free_bytes']} B free, need {need:.0f}")

    name = str(Path(a.h5_dir) / f"slot693_{a.label}.h5") if a.h5_dir else f"slot693_{a.label}.h5"
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
    ACTIVE["running"] = True                     # before the start is sent: a start that times out or is interrupted mid-wait is stopped too (a stop with nothing running is NotActive, harmless)
    start = st.call("experiment_start", {"outputPath": name, "acknowledgeDiscardRing": True}, wait=60)
    save(out / "experiment-start.json", start)
    log("experiment_start: " + json.dumps(start)[:400])
    if not (isinstance(start, dict) and start.get("ok") is True and not start.get("error")):
        refused = isinstance(start, dict) and not str(start.get("error", "")).startswith("no reply")
        if refused:
            ACTIVE["running"] = False            # an explicit refusal: nothing started
        sys.exit("experiment_start refused" if refused else "experiment_start: no reply, stopping whatever started")
    ssd_now = st.call("fetch_ssd_status")
    ssd_run_id = ((ssd_now or {}).get("open_run") or {}).get("id") if isinstance(ssd_now, dict) else None
    log(f"the experiment records to SSD run {ssd_run_id}")
    th = None
    if a.sample_hz > 0:
        th = threading.Thread(target=sampler, daemon=True)
        th.start()
    time.sleep(a.duration)
    during = st.call("fetch_ssd_status")
    save(out / "during-ssd.json", during)
    t_stop = time.time()
    stop_reply = None
    for attempt in range(10):  # Busy while the experiment is still Starting: retry for ~30 s
        stop_reply = st.call("experiment_stop", wait=60)
        log("experiment_stop: " + json.dumps(stop_reply)[:300])
        if isinstance(stop_reply, dict) and stop_reply.get("ok") is True:
            ACTIVE["running"] = False
            break
        time.sleep(3)
    else:
        sys.exit("experiment_stop was not accepted in 30 s; the interrupt path keeps trying")
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
    if ssd_run_id is not None:
        row = next((r for r in rows if int(r.get("id", r.get("run_id", -1))) == int(ssd_run_id)), None)
        if row is None:
            sys.exit(f"the experiment's SSD run {ssd_run_id} is not in the run table")
    else:
        log("WARNING: the SSD status names no open run; judging the newest run")
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

    dl = Path(a.dl) if a.dl else out / "DL"
    dl.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    try:
        resp = urllib.request.urlopen(f"{st.base}/ssd/runs/{rid}/records", timeout=a.dl_timeout)
    except urllib.error.HTTPError as e:
        body = e.read()
        (dl / f"run{rid}.error").write_text(f"{e.code} {e.reason}\n{body.decode(errors='replace')}\n")
        sys.exit(f"export refused: {e.code} {e.reason}: {body[:300]!r}")
    # curl -D style, as received (lowercase names): the board owner's scripts read these
    head = f"HTTP/1.1 {resp.status} {resp.reason}\r\n" + "".join(f"{k.lower()}: {v}\r\n" for k, v in resp.getheaders()) + "\r\n"
    (dl / f"run{rid}.headers").write_bytes(head.encode())
    heads = {k.lower(): v for k, v in resp.getheaders()}
    n = 0
    md5 = hashlib.md5()
    win_t, win_n = time.time(), 0
    with open(dl / f"run{rid}.bin", "wb") as f:
        while chunk := resp.read(1 << 20):
            f.write(chunk)
            md5.update(chunk)
            n += len(chunk)
            now = time.time()
            if now - win_t >= 120.0:  # like curl --speed-limit 1000 --speed-time 120: a trickle is not progress
                if n - win_n < 120 * 1000:
                    sys.exit(f"the export crawls: {n - win_n} B in {now - win_t:.0f} s (limit 1000 B/s)")
                win_t, win_n = now, n
    took = time.time() - t0
    want = int(heads.get("content-length", "-1"))
    (dl / f"run{rid}.time").write_text(f"{took:.6f}\n")
    (dl / f"run{rid}.md5").write_text(f"{md5.hexdigest()}  run{rid}.bin\n")  # last: its mtime says the file is complete
    log(f"export: {n} bytes in {took:.1f} s ({n / max(took, 1e-9) / 1e6:.1f} MB/s), headers {json.dumps({k: v for k, v in heads.items() if k.startswith('x-')})}")
    if want >= 0 and n != want:
        sys.exit(f"the export is short: {n} of {want} bytes")
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
               "--status2", str(out / "ring-status-2.json"), "--status3", str(out / "ring-status-3.json"), "--run", str(dl / f"run{rid}.bin"), "--runs",
               str(out / "post.runs.json"), "--run-id", str(rid), "--out", str(out / "ring-vs-ssd")]
        if a.require_content:
            cmd += ["--require-content", a.require_content]
        log("judge: " + " ".join(cmd))
        p = subprocess.run(cmd, capture_output=True, text=True)
        (out / "ring-vs-ssd.stdout").write_text(p.stdout + p.stderr)
        rc = p.returncode
        for ln in p.stdout.splitlines():
            if ln.startswith("CONTENT"):
                log(ln)  # what was actually compared
        log(f"ring_vs_ssd exit {rc}: {p.stdout.strip().splitlines()[-1] if p.stdout.strip() else p.stderr.strip()[-200:]}")
    require = {t.strip() for t in a.require_content.split(",") if t.strip()}
    report = None
    if not a.no_judge:
        try:
            loaded = json.loads((out / "ring-vs-ssd" / "ring-vs-ssd.json").read_text())
            report = loaded if isinstance(loaded, dict) else None
        except (OSError, ValueError):
            report = None
    try:
        acc, acc_text = acceptance(rc, report, require, judged=not a.no_judge)
    except Exception as e:  # noqa: BLE001 - the acceptance must never stop the hand-back (ring_resume, summary)
        acc, acc_text = {"transport": f"FAIL (acceptance could not be computed: {e!r})", "transport_status": "fail"}, f"TRANSPORT: FAIL (acceptance could not be computed: {e!r})\n"
    save(out / "acceptance.json", {"label": a.label, "run_id": rid, **acc})
    (out / "acceptance.txt").write_text(f"{a.label} run {rid}\n" + acc_text)
    for ln in acc_text.splitlines():
        log(ln)
    if a.resume_at_end:
        log("ring_resume: " + json.dumps(st.call("ring_resume", wait=60))[:300])
    save(out / "summary.json", {"label": a.label, "run_id": rid, "duration_s": a.duration, "stop_s": stop_took, "wall_start": t_start, "ring_vs_ssd_exit": rc, "run_row": row})
    sys.exit(rc)


ACTIVE = {"st": None, "log": print, "running": False}


def main():
    """_main, plus the one safety rule: if the experiment this driver started is still running when it dies (Ctrl-C, SIGTERM, a failed step), send experiment_stop once and wait, bounded, for it to close."""
    def on_term(*_):
        raise KeyboardInterrupt()

    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)  # also when the shell started us with SIGINT ignored (a background job)
    try:
        _main()
    except BaseException:
        st = ACTIVE["st"]
        if ACTIVE["running"] and st is not None:
            ACTIVE["log"]("interrupted while the experiment may be running: experiment_stop (retried while Studio is Busy, bounded)")
            try:
                closed = stop_and_wait(st, ACTIVE["log"])
                ACTIVE["log"]("the experiment is closed; the ring is left held" if closed else "the experiment was NOT confirmed closed: stop it in Studio")
            except BaseException as e:  # noqa: BLE001 - nothing more can be done
                ACTIVE["log"](f"could not stop the experiment: {e!r}: stop it in Studio")
        raise


if __name__ == "__main__":
    main()
