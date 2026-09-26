#!/usr/bin/env python3
"""End-to-end compression headroom check (ADR 0006, PR 0 step 3), scripted.

For each save mode (experiment, recording) it runs:
  1. a baseline soak: mock camera -> full production pipeline -> HDF5, no extra load;
  2. the lossless ratio of the frames that baseline actually stored;
  3. one soak per pool size T, with scripts/bench_hdf5_compression.py compressing
     continuously on T below-normal-priority threads for the whole run. That is
     the CPU the planned compression pool would take.

The soaks are `mib_backend_tests mock_experiment_soak_run`
(tests/tools/mock_experiment_soak_run.cpp). A pool size T passes when:
  * headroom: gzip MB/s sustained on T threads *during the run* is at least
    1.3 x the rate the baseline actually wrote (the worst case, fps x W x H, is
    reported alongside);
  * no harm: the loaded run's completion state equals the baseline's, capture fps
    stays within 1 %, and its loss terms (ring overwrites, persistence
    failed/pending, processing drops, camera discards/loss) do not exceed the
    baseline's by more than 1 % of admitted frames.

Usage:
    python3 scripts/run_compression_headroom_e2e.py [--runner PATH] [--frames DIR]
        [--modes experiment,recording] [--threads 1,2,3] [--duration 300] [--fps 1000]
        [--gates default] [--level 1] [--chunk-frames 10] [--work-dir DIR]
        [--json report.json] [--keep-files]

Needs numpy and h5py (for the benchmark) and a built mib_backend_tests.
"""

import argparse
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BENCH = REPO / "scripts" / "bench_hdf5_compression.py"
DEFAULT_FRAMES = REPO / "build" / "vendor" / "assets" / "datasets" / "512x96stream-mock-frames"
RUNNER_CANDIDATES = [
    "build/linux-backend/Release/mib_backend_tests",
    "build/linux-backend/mib_backend_tests",
    "build-ninja/mib_backend_tests.exe",
    "build/Release/mib_backend_tests.exe",
    "build-ninja/Release/mib_backend_tests.exe",
]
LOSS_TERMS = ("store_overwritten", "store_not_committed", "store_malformed", "persistence_failed",
              "persistence_pending_at_stop", "pending_at_stop")


def ntfs_compressed(path):
    """True when Windows reports FILE_ATTRIBUTE_COMPRESSED on path (new files inherit it)."""
    attrs = getattr(os.stat(path), "st_file_attributes", 0)
    return bool(attrs & 0x800)


def find_runner(explicit):
    if explicit:
        return Path(explicit)
    for rel in RUNNER_CANDIDATES:
        p = REPO / rel
        if p.exists():
            return p
    sys.exit("mib_backend_tests not found; build it or pass --runner")


def soak(args, runner, mode, tag):
    out_h5 = Path(args.work_dir) / f"{mode}-{tag}.h5"
    out_json = Path(args.work_dir) / f"{mode}-{tag}.soak.json"
    log = Path(args.work_dir) / f"{mode}-{tag}.soak.log"
    cmd = [str(runner), "mock_experiment_soak_run", "--frames", str(args.frames), "--out", str(out_h5),
           "--mode", mode, "--gates", args.gates, "--fps", str(args.fps), "--duration", str(args.duration),
           "--json", str(out_json)]
    with open(log, "w") as lf:
        rc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=lf).returncode
    if not out_json.exists():
        sys.exit(f"soak {mode}/{tag} produced no report (exit {rc}); see {log}")
    report = json.loads(out_json.read_text())
    report["exit_code"] = rc
    return report, out_h5


def bench_cmd(args, extra, json_path):
    return [sys.executable, str(BENCH), "--frames-dir", str(args.frames), "--levels", str(args.level),
            "--chunk-frames", str(args.chunk_frames), "--json", str(json_path)] + extra


def stored_ratio(args, h5_path, mode):
    dataset = "/valid_frames/images" if mode == "experiment" else "/recorded_frames/images"
    jp = Path(args.work_dir) / f"{mode}-ratio.bench.json"
    cmd = [sys.executable, str(BENCH), "--h5", str(h5_path), "--dataset", dataset, "--levels", str(args.level),
           "--threads", "1", "--chunk-frames", str(args.chunk_frames), "--repeat", "1", "--max-frames", "3000",
           "--no-nice", "--json", str(jp)]
    if subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0 or not jp.exists():
        return None
    rows = json.loads(jp.read_text()).get("codec", [])
    return rows[0]["ratio"] if rows else None


def loss(r):
    a = r.get("stored_accounting") or {}
    p = r.get("processing", {})
    c = r.get("capture", {})
    return (sum(a.get(k, 0) for k in LOSS_TERMS) + p.get("dropped_valid", 0) + p.get("dropped_invalid", 0)
            + c.get("intentionally_discarded", 0) + c.get("transport_lost", 0))


def completion(r):
    return (r.get("stored_accounting") or {}).get("completion", "missing")


def run_mode(args, runner, mode):
    print(f"\n=== {mode}: baseline ({args.duration:.0f} s @ {args.fps:.0f} fps, gates={args.gates}) ===", flush=True)
    base, base_h5 = soak(args, runner, mode, "baseline")
    write_mb_s = base["file_bytes"] / 1e6 / max(base["duration_s"], 1e-9)
    worst_mb_s = args.fps * base["frame_width"] * base["frame_height"] / 1e6
    ratio = stored_ratio(args, base_h5, mode)
    if not args.keep_files:
        base_h5.unlink(missing_ok=True)
    print(f"baseline: completion={completion(base)} capture={base['capture']['fps_measured']:.1f} fps "
          f"wrote {write_mb_s:.1f} MB/s (worst case {worst_mb_s:.1f}) loss={loss(base)} "
          f"cpu={base['process_cpu_cores']:.2f} cores stored-frame ratio={ratio}", flush=True)

    rows = []
    for t in args.threads:
        print(f"--- {mode}: soak with a {t}-thread gzip-{args.level} load ---", flush=True)
        bj = Path(args.work_dir) / f"{mode}-t{t}.bench.json"
        bench = subprocess.Popen(bench_cmd(args, ["--threads", str(t), "--seconds", str(args.duration + 8)], bj),
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2)
        loaded, loaded_h5 = soak(args, runner, mode, f"t{t}")
        bench.wait()
        if not args.keep_files:
            loaded_h5.unlink(missing_ok=True)
        mbs = json.loads(bj.read_text())["codec"][0]["compress_mb_s"] if bj.exists() else 0.0
        admitted = max((loaded.get("stored_accounting") or {}).get("admitted", 0), 1)
        fps_delta = abs(loaded["capture"]["fps_measured"] / base["capture"]["fps_measured"] - 1) * 100
        extra_loss = loss(loaded) - loss(base)
        headroom_ok = mbs >= 1.3 * write_mb_s
        # A failed baseline makes "same completion as baseline" meaningless, so
        # a loaded run only passes when it actually completed.
        harm_ok = (completion(loaded) == "complete" and completion(loaded) == completion(base)
                   and fps_delta <= 1.0 and extra_loss <= 0.01 * admitted)
        row = {"threads": t, "compress_mb_s_under_load": mbs, "required_mb_s": round(1.3 * write_mb_s, 1),
               "worst_case_mb_s": round(worst_mb_s, 1), "covers_worst_case": mbs >= worst_mb_s,
               "completion": completion(loaded), "capture_fps": loaded["capture"]["fps_measured"],
               "capture_fps_delta_pct": round(fps_delta, 2), "loss": loss(loaded), "extra_loss": extra_loss,
               "stop_ms": loaded["stop_ms"], "process_cpu_cores": loaded["process_cpu_cores"],
               "headroom_ok": headroom_ok, "no_harm_ok": harm_ok, "pass": headroom_ok and harm_ok,
               "soak": loaded}
        rows.append(row)
        print(f"T={t}: {mbs:.0f} MB/s under load (need {1.3 * write_mb_s:.1f}, worst case {worst_mb_s:.1f}) "
              f"completion={row['completion']} capture Δ{fps_delta:.2f}% extra loss={extra_loss} "
              f"-> {'PASS' if row['pass'] else 'FAIL'}", flush=True)
    passing = [r["threads"] for r in rows if r["pass"]]
    return {"baseline": base, "write_mb_s": round(write_mb_s, 2), "worst_case_mb_s": round(worst_mb_s, 1),
            "stored_frame_ratio": ratio, "loaded": rows, "smallest_passing_threads": min(passing) if passing else None}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runner")
    ap.add_argument("--frames", default=os.environ.get("MIB_MOCK_CAMERA_DIR") or str(DEFAULT_FRAMES))
    ap.add_argument("--modes", default="experiment,recording")
    ap.add_argument("--threads", default="1,2,3")
    ap.add_argument("--duration", type=float, default=300.0)
    ap.add_argument("--fps", type=float, default=1000.0)
    ap.add_argument("--gates", default="default", choices=("default", "open"))
    ap.add_argument("--level", type=int, default=1)
    ap.add_argument("--chunk-frames", type=int, default=10)
    ap.add_argument("--work-dir", default=str(REPO / "build" / "compression-e2e"))
    ap.add_argument("--json")
    ap.add_argument("--keep-files", action="store_true")
    args = ap.parse_args()
    args.threads = [int(t) for t in args.threads.split(",") if t]
    Path(args.work_dir).mkdir(parents=True, exist_ok=True)
    if ntfs_compressed(args.work_dir):
        print(f"WARNING: {args.work_dir} is NTFS-compressed; files written there are compressed "
              "synchronously by Windows and a 1000 fps recording overflows its write queue. "
              "Use an uncompressed folder (compact /u) to measure the drive.", flush=True)
    runner = find_runner(args.runner)

    report = {"host": {"platform": platform.platform(), "cpu_count": os.cpu_count(),
                       "processor": platform.processor()},
              "settings": {k: v for k, v in vars(args).items() if k not in ("runner",)},
              "modes": {}}
    for mode in [m for m in args.modes.split(",") if m]:
        report["modes"][mode] = run_mode(args, runner, mode)

    print("\n=== summary ===")
    ok = True
    for mode, r in report["modes"].items():
        print(f"{mode}: baseline {completion(r['baseline'])}, wrote {r['write_mb_s']} MB/s, stored-frame "
              f"ratio {r['stored_frame_ratio']}, smallest passing pool = {r['smallest_passing_threads']}")
        ok = ok and completion(r["baseline"]) == "complete" and r["smallest_passing_threads"] is not None
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2))
        print(f"report written to {args.json}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
