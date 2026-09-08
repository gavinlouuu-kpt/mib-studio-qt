#!/usr/bin/env python3
"""Analyze pipeline latency CSVs dumped by PipelineTimingRecorder.

Usage:
    python3 scripts/analyze_pipeline_timing.py <dump_dir>

<dump_dir> is the directory containing pipeline_frames.csv,
pipeline_triggers.csv and pipeline_skips.csv (by default
<dataDir>/pipeline_timing, dumped on capture stop / shutdown when
MIB_PIPELINE_TIMING=1 — see docs/howto/pipeline-latency-diagnosis.md).

Prints per-stage latency percentiles (all in milliseconds):
    grab -> algo start     time a frame waited between acquisition and the
                           realtime algorithm picking it up (queueing delay)
    algo                   algorithm duration
    algo end -> dispatch   callback dispatch delay for trigger frames
    request -> fire        trigger-thread wake + output-line latency
    grab -> fire           END-TO-END acquisition-to-trigger-pulse latency
plus inter-frame gap statistics and the frame-accounting summary
(records + skips vs. what the capture pushed), which shows where frames
were dropped and why. Stdlib only; no third-party dependencies.
"""

import csv
import sys
from pathlib import Path


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    idx = (p / 100.0) * (len(sorted_values) - 1)
    lo = int(idx)
    hi = min(lo + 1, len(sorted_values) - 1)
    frac = idx - lo
    return sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac


def stats_line(name, values_us):
    """Format one metric row; values are microseconds, displayed as ms."""
    if not values_us:
        return f"  {name:<24} (no samples)"
    vals = sorted(v / 1000.0 for v in values_us)  # -> ms
    return (
        f"  {name:<24} n={len(vals):<7} "
        f"mean={sum(vals) / len(vals):8.3f}  p50={percentile(vals, 50):8.3f}  "
        f"p95={percentile(vals, 95):8.3f}  p99={percentile(vals, 99):8.3f}  "
        f"max={vals[-1]:8.3f}"
    )


def read_csv(path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    dump_dir = Path(sys.argv[1])
    frames = read_csv(dump_dir / "pipeline_frames.csv")
    triggers = read_csv(dump_dir / "pipeline_triggers.csv")
    skips_path = dump_dir / "pipeline_skips.csv"

    print(f"== Pipeline timing analysis: {dump_dir} ==")
    print(f"frame records:   {len(frames)}")
    print(f"trigger records: {len(triggers)}")

    # --- per-stage frame latencies (all values are host-monotonic us) ---
    grab_to_algo = [
        f["algo_start_us"] - f["grab_us"]
        for f in frames
        if f["grab_us"] and f["algo_start_us"]
    ]
    algo = [
        f["algo_end_us"] - f["algo_start_us"]
        for f in frames
        if f["algo_start_us"] and f["algo_end_us"]
    ]
    dispatch = [
        f["trigger_dispatch_us"] - f["algo_end_us"]
        for f in frames
        if f["trigger_dispatch_us"] and f["algo_end_us"]
    ]
    print("\n-- Frame stage latencies (ms) --")
    print(stats_line("grab -> algo start", grab_to_algo))
    print(stats_line("algo duration", algo))
    print(stats_line("algo end -> tg dispatch", dispatch))

    # --- inter-frame gaps: host clock vs device ticks ---
    # A growing host gap with a steady device gap means frames are queueing in
    # the grabber/SDK before the capture thread sees them.
    host_gaps = []
    device_gaps = []
    for prev, cur in zip(frames, frames[1:]):
        if cur["frame_index"] == prev["frame_index"] + 1:  # adjacent frames only
            if prev["grab_us"] and cur["grab_us"]:
                host_gaps.append(cur["grab_us"] - prev["grab_us"])
            if prev["device_timestamp"] and cur["device_timestamp"]:
                device_gaps.append(cur["device_timestamp"] - prev["device_timestamp"])
    print("\n-- Adjacent-frame gaps --")
    print(stats_line("host grab gap (ms)", host_gaps))
    if device_gaps:
        vals = sorted(device_gaps)
        print(
            f"  {'device tick gap (raw)':<24} n={len(vals):<7} "
            f"mean={sum(vals) / len(vals):8.1f}  p50={percentile(vals, 50):8.1f}  "
            f"max={vals[-1]:8.1f}   (device units, not host us)"
        )

    # --- trigger latencies ---
    req_to_wake = [t["wake_us"] - t["request_us"] for t in triggers if t["request_us"]]
    wake_to_fire = [t["fire_us"] - t["wake_us"] for t in triggers if t["wake_us"]]
    req_to_fire = [t["fire_us"] - t["request_us"] for t in triggers if t["request_us"]]
    grab_to_fire = [
        t["fire_us"] - t["grab_us"] for t in triggers if t["grab_us"] and t["fire_us"]
    ]
    coalesced = sum(t["coalesced"] for t in triggers)
    print("\n-- Trigger latencies (ms) --")
    print(stats_line("request -> wake", req_to_wake))
    print(stats_line("wake -> fire", wake_to_fire))
    print(stats_line("request -> fire", req_to_fire))
    print(stats_line("grab -> fire (END2END)", grab_to_fire))
    if coalesced:
        print(f"  NOTE: {coalesced} trigger request(s) coalesced into earlier pulses")

    # --- frame accounting ---
    print("\n-- Frame accounting --")
    accounted = len(frames)
    if skips_path.exists():
        with skips_path.open(newline="") as f:
            for row in csv.DictReader(f):
                print(f"  {row['reason']:<24} {row['count']}")
                if row["reason"] not in ("frame_records", "trigger_records"):
                    accounted += int(row["count"])
        print(f"  accounted total (records in ring + skips): {accounted}")
        print("  Compare against capture framesProcessed for the same session; a")
        print("  mismatch beyond the ring capacity means unexplained frame loss.")
    else:
        print(f"  (missing {skips_path})")

    # --- identification funnel + loss ---
    # Quantifies "loss of target identification": how the objects the detector
    # produced narrowed down to sort pulses, and where identified frames were
    # lost before reaching the algorithm at all (the skip reasons).
    valid_objs = sum(f["valid_count"] for f in frames)
    invalid_objs = sum(f["invalid_count"] for f in frames)
    tg_frames = [f for f in frames if f["is_target_group"]]
    total_objs = valid_objs + invalid_objs
    print("\n-- Identification funnel --")
    print(f"  objects detected:       {total_objs}")
    if total_objs:
        print(f"  valid (passed gates):   {valid_objs} ({100.0 * valid_objs / total_objs:.1f}%)")
        print(f"  invalid (gated out):    {invalid_objs} "
              f"({100.0 * invalid_objs / total_objs:.1f}%)")
    print(f"  frames requesting sort: {len(tg_frames)} / {len(frames)}")

    # Frames lost before the algorithm (from the skip file) are identification
    # opportunities that never happened. ring_behind / dropped_to_latest are the
    # ones that indicate the realtime consumer could not keep up.
    if skips_path.exists():
        loss = {}
        with skips_path.open(newline="") as f:
            for row in csv.DictReader(f):
                if row["reason"] in ("ring_behind", "dropped_to_latest",
                                     "kernel_error", "batch_queue_rejected"):
                    loss[row["reason"]] = int(row["count"])
        lost = sum(loss.values())
        if lost:
            detail = ", ".join(f"{k}={v}" for k, v in loss.items() if v)
            print(f"  frames lost pre-algo:   {lost}  ({detail})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
