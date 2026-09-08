#!/usr/bin/env python3
"""Repeated-export soak harness for the HDF5 exporter (issue #344).

Runs N consecutive exports in ONE process and records, per round: duration,
RSS before/peak/after, live Python threads, created/finished/destroyed
worker+QThread counts, output file count and SHA-256 manifest, and the source
file hash. Emits a machine-readable JSON report and applies the acceptance
gates from #344:

* round N duration <= 1.25 x median(rounds 2-5)
* post-job RSS plateaus: median of the last window <= median of the early
  window + allowance (relative and absolute)
* no per-round linear RSS growth (least-squares slope over the steady rounds)
* worker/thread create/finish/destroy counts reconcile (GUI mode)
* output manifests identical across rounds; source hash unchanged

Modes: ``gui`` drives the real ExportWindow/ExportWorker/QThread path
offscreen; ``engine`` calls ``run_export_job`` directly (no Qt).

Exit codes: 0 pass, 1 gate failure, 2 setup error, 99 watchdog.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Dict, List

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))


def rss_bytes() -> int:
    try:
        import psutil  # type: ignore

        return int(psutil.Process().memory_info().rss)
    except Exception:  # noqa: BLE001
        try:
            with open("/proc/self/statm", "r", encoding="ascii") as f:
                pages = int(f.read().split()[1])
            return pages * os.sysconf("SC_PAGE_SIZE")
        except Exception:  # noqa: BLE001
            return 0


class _PeakRss:
    """Background sampler for the peak RSS during a round."""

    def __init__(self, interval_s: float = 0.02) -> None:
        self._interval = interval_s
        self._stop = threading.Event()
        self.peak = 0
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        while not self._stop.is_set():
            self.peak = max(self.peak, rss_bytes())
            self._stop.wait(self._interval)

    def __enter__(self):
        self.peak = rss_bytes()
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._thread.join(timeout=2)
        self.peak = max(self.peak, rss_bytes())


def _slope(values: List[float]) -> float:
    n = len(values)
    if n < 2:
        return 0.0
    xs = list(range(n))
    mx = sum(xs) / n
    my = sum(values) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, values))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den else 0.0


def run_soak(args: argparse.Namespace) -> int:
    import export_test_fixture as fixture
    from hdf_export_engine import ExportJob, ExportState, run_export_job

    def watchdog():
        time.sleep(args.watchdog)
        sys.stderr.write("exporter_soak watchdog fired\n")
        os._exit(99)

    threading.Thread(target=watchdog, daemon=True).start()

    work = Path(args.workdir) if args.workdir else Path(tempfile.mkdtemp(prefix="exporter_soak_"))
    work.mkdir(parents=True, exist_ok=True)
    source = fixture.write_fixture(work / "soak_fixture.h5", valid_frames=args.valid, invalid_frames=args.invalid,
                                   series_count=args.series, height=args.height, width=args.width)
    source_sha = fixture.sha256_of_file(source)
    out_root = work / "out"
    out_root.mkdir(exist_ok=True)

    window = None
    app = None
    if args.mode == "gui":
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtCore import QCoreApplication, QDeadlineTimer, QEventLoop
        from PySide6.QtWidgets import QApplication

        from hdf5_export_app import ExportWindow

        app = QApplication.instance() or QApplication(sys.argv[:1])
        window = ExportWindow(interactive=False)
        window.input_file_edit.setText(str(source))
        window.output_dir_edit.setText(str(out_root))
        window.format_combo.setCurrentIndex({"csv": 0, "images": 1, "all": 2}[args.format])
        window.show()

        def wait_idle(timeout_s: float) -> bool:
            deadline = QDeadlineTimer(int(timeout_s * 1000))
            while not deadline.hasExpired():
                QCoreApplication.processEvents(QEventLoop.AllEvents, 20)
                QCoreApplication.sendPostedEvents(None, 0)
                if window.active is None:
                    return True
                time.sleep(0.002)
            return window.active is None

    rounds: List[Dict] = []
    manifest0 = None
    failures: List[str] = []
    baseline_rss = rss_bytes()
    print(f"soak: mode={args.mode} cycles={args.cycles} fixture={source} baseline_rss={baseline_rss/1e6:.1f}MB")
    for n in range(1, args.cycles + 1):
        before = rss_bytes()
        t0 = time.monotonic()
        with _PeakRss() as peak:
            if args.mode == "gui":
                assert window is not None
                if not window.start_export():
                    failures.append(f"round {n}: start refused")
                    break
                if not wait_idle(args.round_timeout):
                    failures.append(f"round {n}: export did not finish within {args.round_timeout}s")
                    break
                result = window.last_result
            else:
                job = ExportJob(source, out_root, args.format)
                result = run_export_job(job)
        duration = time.monotonic() - t0
        after = rss_bytes()
        if result is None or result.state != ExportState.COMPLETED:
            failures.append(f"round {n}: {result.state.value if result else 'no result'} {result.error if result else ''}")
            break
        manifest = fixture.output_manifest(result.final_path)
        if manifest0 is None:
            manifest0 = manifest
        elif manifest != manifest0:
            failures.append(f"round {n}: output manifest differs from round 1")
        record = {
            "round": n,
            "job_id": result.job_id,
            "duration_s": round(duration, 4),
            "engine_duration_s": round(result.duration_s, 4),
            "rss_before": before,
            "rss_peak": peak.peak,
            "rss_after": after,
            "python_threads": threading.active_count(),
            "output_files": len(manifest),
            "images": result.images_exported + result.series_exported,
            "final_path": str(result.final_path),
        }
        if window is not None:
            record.update({
                "exports_started": window.exports_started,
                "exports_finished": window.exports_finished,
                "workers_destroyed": window.workers_destroyed,
                "threads_destroyed": window.threads_destroyed,
            })
        rounds.append(record)
        print(f"round {n:3d}: {duration:6.3f}s rss after={after/1e6:7.1f}MB peak={peak.peak/1e6:7.1f}MB "
              f"files={len(manifest)} threads={threading.active_count()}")
        if not args.keep_outputs:
            import shutil

            shutil.rmtree(result.final_path, ignore_errors=True) if result.final_path.is_dir() else result.final_path.unlink(missing_ok=True)

    if window is not None:
        # Let deferred deletes run, then reconcile lifecycle counts.
        from PySide6.QtCore import QCoreApplication, QEventLoop

        for _ in range(20):
            QCoreApplication.sendPostedEvents(None, 0)
            QCoreApplication.processEvents(QEventLoop.AllEvents, 10)
        if not (window.exports_started == window.exports_finished == window.workers_destroyed ==
                window.threads_destroyed == len(rounds)):
            failures.append(
                f"lifecycle counts do not reconcile: started={window.exports_started} finished={window.exports_finished} "
                f"workers_destroyed={window.workers_destroyed} threads_destroyed={window.threads_destroyed} rounds={len(rounds)}")
        window.close()

    if fixture.sha256_of_file(source) != source_sha:
        failures.append("source HDF5 hash changed")

    # ---- gates ------------------------------------------------------------
    gates: Dict[str, Dict] = {}
    if len(rounds) >= 6:
        early = [r["duration_s"] for r in rounds[1:5]]
        median_early = statistics.median(early)
        last = rounds[-1]["duration_s"]
        ratio = last / median_early if median_early > 0 else 0.0
        gates["duration_ratio"] = {"last": last, "median_rounds_2_5": median_early, "ratio": round(ratio, 3),
                                   "limit": args.duration_ratio, "pass": ratio <= args.duration_ratio}
        window_n = max(3, len(rounds) // 5)
        early_rss = statistics.median(r["rss_after"] for r in rounds[1:1 + window_n])
        late_rss = statistics.median(r["rss_after"] for r in rounds[-window_n:])
        allowance = max(args.rss_abs_mb * 1e6, early_rss * args.rss_rel)
        gates["rss_plateau"] = {"early_median": early_rss, "late_median": late_rss,
                                "allowance_bytes": allowance, "pass": late_rss <= early_rss + allowance}
        steady = [r["rss_after"] for r in rounds[1 + window_n:]]
        slope = _slope(steady)
        per_round_limit = args.rss_slope_mb * 1e6
        gates["rss_slope"] = {"bytes_per_round": round(slope, 1), "limit_bytes_per_round": per_round_limit,
                              "pass": slope <= per_round_limit}
        peak_over_after = max(r["rss_peak"] - r["rss_after"] for r in rounds)
        gates["peak_headroom"] = {"max_peak_minus_after_bytes": peak_over_after}
        for name, g in gates.items():
            if "pass" in g and not g["pass"]:
                failures.append(f"gate {name} failed: {g}")
    else:
        gates["note"] = "fewer than 6 rounds: timing/RSS gates not evaluated"

    report = {
        "mode": args.mode,
        "format": args.format,
        "cycles_requested": args.cycles,
        "cycles_completed": len(rounds),
        "fixture": {"path": str(source), "valid": args.valid, "invalid": args.invalid, "series": args.series,
                    "height": args.height, "width": args.width, "sha256": source_sha},
        "baseline_rss": baseline_rss,
        "rounds": rounds,
        "gates": gates,
        "failures": failures,
        "pass": not failures and len(rounds) == args.cycles,
        "python": sys.version.split()[0],
        "platform": sys.platform,
    }
    if args.report:
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"report: {args.report}")
    print(json.dumps({"gates": gates, "failures": failures, "pass": report["pass"]}, indent=2))
    return 0 if report["pass"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cycles", type=int, default=50)
    parser.add_argument("--mode", choices=["gui", "engine"], default="gui")
    parser.add_argument("--format", choices=["csv", "images", "all"], default="all")
    parser.add_argument("--valid", type=int, default=60)
    parser.add_argument("--invalid", type=int, default=30)
    parser.add_argument("--series", type=int, default=3)
    parser.add_argument("--height", type=int, default=96)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--report", type=str, default="")
    parser.add_argument("--workdir", type=str, default="")
    parser.add_argument("--keep-outputs", action="store_true")
    parser.add_argument("--round-timeout", type=float, default=120.0)
    parser.add_argument("--watchdog", type=float, default=1800.0)
    parser.add_argument("--duration-ratio", type=float, default=1.25)
    parser.add_argument("--rss-rel", type=float, default=0.10, help="relative RSS allowance (fraction)")
    parser.add_argument("--rss-abs-mb", type=float, default=48.0, help="absolute RSS allowance (MB)")
    parser.add_argument("--rss-slope-mb", type=float, default=0.5, help="max steady-state RSS growth per round (MB)")
    args = parser.parse_args()
    try:
        return run_soak(args)
    except ImportError as exc:
        print(f"setup error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
