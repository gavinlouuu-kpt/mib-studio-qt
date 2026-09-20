#!/usr/bin/env python3
"""Regenerate the labelled supervisor scenario fixtures (issue #422).

Each fixture under tests/supervisor/ is one deterministic decision point:
a frozen ExperimentSnapshot (schema 1) plus the explicit expected answers
under decision contract 1. Labels never appear inside the snapshot, so the
snapshot alone is what any provider sees.

Edit the SCENARIOS table, run `python3 tools/supervisor_gen_fixtures.py`,
review the diff, and bump DATASET_VERSION when a label or input changes.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path

DATASET_VERSION = "fixtures/2026-09-20.1"
OUT_DIR = Path(__file__).resolve().parent.parent / "tests" / "supervisor"


def base_snapshot() -> dict:
    """A healthy 60 s acquisition; scenarios override what differs."""
    return {
        "schema_version": 1,
        "sequence": 12,
        "run_id": "fixture-run",
        "start_generation": 1,
        "experiment_state": "active",
        "host_time_us": 60_000_000,
        "wall_clock_ns": 1_758_326_400_000_000_000,
        "elapsed_seconds": 60.0,
        "objective": "collect valid cells",
        "target_valid_objects": None,
        "configuration": {
            "camera_source": "mock",
            "simulated": True,
            "delivery_mode": "everyFrame",
            "requested_fps": 500.0,
            "exposure_us": 300.0,
            "roi_width": 512,
            "roi_height": 96,
            "detection_threshold": 8,
            "min_area_um2": 60,
            "max_area_um2": 290,
            "area_range_check": True,
            "trigger_enabled": False,
            "processing_core_version": "0.2.1",
            "config_sha256": "0" * 64,
        },
        "acquisition": {
            "capture_state": "running",
            "camera_ready": True,
            "last_failure": "none",
            "measured_fps": 500.0,
            "frames_delivered": 30_000.0,
            "transport_lost_frames": 0.0,
            "intentionally_discarded_frames": 0.0,
            "buffer_underruns": 0.0,
            "sdk_queue_depth": 1.0,
            "sdk_input_buffers": 19.0,
            "frame_age_us": 900.0,
            "publish_latency_us": 40.0,
        },
        "detection": {
            "frames_processed": 30_000.0,
            "valid_objects": 900.0,
            "invalid_objects": 300.0,
            "dropped_valid": 0.0,
            "dropped_invalid": 0.0,
            "processing_failures": 0.0,
            "algo_fps": 500.0,
            "valid_per_second": 15.0,
            "invalid_per_second": 5.0,
            "processing_time_us": 800.0,
            "window_objects": 256,
            "area_mean_um2": 150.0,
            "area_min_um2": 70.0,
            "area_max_um2": 260.0,
            "contrast_mean": 60.0,
            "brightness_median_mean": 120.0,
            "brightness_max_mean": 200.0,
            "laplacian_variance_mean": None,
            "rejections": {"border": 120, "area": 100, "inner_contour": 40, "ring_ratio": 30, "other": 10},
        },
        "trigger": {
            "camera_bound": False,
            "eligible_objects": 0.0,
            "triggers_issued": 0.0,
            "suppressed_requests": 0.0,
            "dropped_pulses": 0.0,
            "stale_requests": 0.0,
            "last_onset_us": None,
        },
        "recording": {
            "state": "active",
            "storage_ready": True,
            "output_path": "data/fixture-run.h5",
            "persistence_admitted": 1100.0,
            "persistence_committed": 1100.0,
            "persistence_failed": 0.0,
            "valid_buffered": 80.0,
            "invalid_buffered": 20.0,
            "fault_code": "",
            "fault_message": "",
        },
        "prior_recommendations": [],
    }


def expected(quality, problem, action, target="NONE", direction="KEEP") -> dict:
    return {
        "run_quality": quality,
        "primary_problem": problem,
        "next_action": action,
        "adjustment_target": target,
        "adjustment_direction": direction,
    }


def deep_update(d: dict, path: str, value) -> None:
    keys = path.split(".")
    for k in keys[:-1]:
        d = d[k]
    d[keys[-1]] = value


# id, scenario, notes, overrides, expected, expect_policy
SCENARIOS = [
    ("01_normal", "normal healthy acquisition",
     "Stable 500 fps, 25% rejection, good contrast: nothing to change.",
     {}, expected("GOOD", "NONE", "CONTINUE"), False),
    ("02_low_contrast", "low contrast",
     "Objects barely separate from background (Q3-Q1 = 12); brighten.",
     {"detection.contrast_mean": 12.0, "detection.brightness_median_mean": 90.0,
      "detection.brightness_max_mean": 110.0},
     expected("MARGINAL", "LOW_CONTRAST", "ADJUST", "EXPOSURE", "INCREASE"), False),
    ("03_underexposed", "underexposure",
     "Median brightness 15/255 inside the mask.",
     {"detection.brightness_median_mean": 15.0, "detection.brightness_max_mean": 60.0,
      "detection.contrast_mean": 25.0},
     expected("BAD", "UNDEREXPOSURE", "ADJUST", "EXPOSURE", "INCREASE"), False),
    ("04_overexposed", "overexposure / saturation",
     "Q4 pinned at 255: saturated pixels inside every object.",
     {"detection.brightness_max_mean": 255.0, "detection.brightness_median_mean": 230.0,
      "detection.contrast_mean": 30.0},
     expected("BAD", "OVEREXPOSURE", "ADJUST", "EXPOSURE", "DECREASE"), False),
    ("05_threshold_too_low", "threshold too low / excessive false detections",
     "800 objects/s with 95% rejected: the background threshold admits noise.",
     {"detection.valid_per_second": 40.0, "detection.invalid_per_second": 760.0,
      "detection.valid_objects": 2400.0, "detection.invalid_objects": 45600.0,
      "detection.frames_processed": 48000.0},
     expected("BAD", "EXCESS_FALSE_DETECTIONS", "ADJUST", "DETECTION_THRESHOLD", "INCREASE"), False),
    ("06_threshold_too_high", "threshold too high / excessive rejection",
     "Only 15% of normally sized objects survive; loosen the threshold.",
     {"detection.valid_objects": 180.0, "detection.invalid_objects": 1020.0,
      "detection.valid_per_second": 3.0, "detection.invalid_per_second": 17.0},
     expected("MARGINAL", "EXCESS_REJECTIONS", "ADJUST", "DETECTION_THRESHOLD", "DECREASE"), False),
    ("07_excessive_debris", "many small debris-like blobs",
     "Mean accepted-window area 20 um2 (< half of min area) with 90% rejection.",
     {"detection.area_mean_um2": 20.0, "detection.area_min_um2": 5.0, "detection.area_max_um2": 65.0,
      "detection.valid_objects": 120.0, "detection.invalid_objects": 1080.0,
      "detection.valid_per_second": 2.0, "detection.invalid_per_second": 18.0},
     expected("MARGINAL", "EXCESS_FALSE_DETECTIONS", "ADJUST", "MIN_AREA", "INCREASE"), False),
    ("08_no_objects", "no objects",
     "30 s, 15 000 frames, nothing detected at all.",
     {"elapsed_seconds": 30.0, "detection.frames_processed": 15000.0, "detection.valid_objects": 0.0,
      "detection.invalid_objects": 0.0, "detection.valid_per_second": 0.0,
      "detection.invalid_per_second": 0.0, "detection.window_objects": 0,
      "detection.area_mean_um2": None, "detection.area_min_um2": None, "detection.area_max_um2": None,
      "detection.contrast_mean": None, "detection.brightness_median_mean": None,
      "detection.brightness_max_mean": None, "detection.rejections": {},
      "acquisition.frames_delivered": 15000.0, "recording.persistence_admitted": 0.0,
      "recording.persistence_committed": 0.0, "recording.valid_buffered": 0.0,
      "recording.invalid_buffered": 0.0},
     expected("BAD", "INSUFFICIENT_DATA", "HUMAN_REVIEW"), False),
    ("09_high_concentration", "high object concentration",
     "200 objects/s with otherwise good quality: risk of overlaps; a human decides on dilution.",
     {"detection.valid_per_second": 140.0, "detection.invalid_per_second": 60.0,
      "detection.valid_objects": 8400.0, "detection.invalid_objects": 3600.0},
     expected("MARGINAL", "OTHER", "HUMAN_REVIEW"), False),
    ("10_frame_loss_severe", "increasing frame loss (hard limit)",
     "Transport loss 25% of frames: deterministic policy stops the run, the model is not asked.",
     {"acquisition.transport_lost_frames": 10000.0},
     expected("BAD", "FRAME_LOSS", "STOP_FAILURE"), True),
    ("11_camera_stall", "camera stall / failure",
     "Capture reports deviceHealthLost while the experiment is active.",
     {"acquisition.camera_ready": False, "acquisition.capture_state": "stopping",
      "acquisition.last_failure": "deviceHealthLost", "acquisition.measured_fps": 0.0},
     expected("BAD", "FRAME_LOSS", "STOP_FAILURE"), True),
    ("12_trigger_inconsistency", "trigger/detection count inconsistency",
     "130 pulses for 100 eligible objects: accounting disagrees; never auto-fix.",
     {"configuration.trigger_enabled": True, "trigger.camera_bound": True,
      "trigger.eligible_objects": 100.0, "trigger.triggers_issued": 130.0, "trigger.last_onset_us": 1200.0},
     expected("BAD", "TRIGGER_FAILURE", "HUMAN_REVIEW"), False),
    ("13_trigger_failure", "trigger failures",
     "40% of eligible objects never got a pulse (queue eviction / late).",
     {"configuration.trigger_enabled": True, "trigger.camera_bound": True,
      "trigger.eligible_objects": 100.0, "trigger.triggers_issued": 60.0,
      "trigger.suppressed_requests": 30.0, "trigger.dropped_pulses": 10.0, "trigger.last_onset_us": 4800.0},
     expected("BAD", "TRIGGER_FAILURE", "ADJUST", "TRIGGER_TIMING", "INCREASE"), False),
    ("14_contradictory_state", "conflicting state: good detection + severe frame loss",
     "8% transport loss (below the hard limit) while detection quality is excellent.",
     {"acquisition.transport_lost_frames": 2600.0},
     expected("MARGINAL", "FRAME_LOSS", "HUMAN_REVIEW"), False),
    ("15_target_complete", "target object count reached",
     "Objective 500 valid cells; 900 collected: deterministic completion.",
     {"target_valid_objects": 500, "objective": "collect 500 valid cells"},
     expected("GOOD", "NONE", "STOP_SUCCESS"), True),
    ("16_insufficient_early", "insufficient observations early in a run",
     "1 s in, 40 frames: too early for any model decision; keep going.",
     {"elapsed_seconds": 1.0, "detection.frames_processed": 40.0, "detection.valid_objects": 1.0,
      "detection.invalid_objects": 0.0, "acquisition.frames_delivered": 40.0,
      "detection.window_objects": 1, "recording.persistence_admitted": 0.0,
      "recording.persistence_committed": 0.0, "recording.valid_buffered": 1.0,
      "recording.invalid_buffered": 0.0},
     expected("MARGINAL", "INSUFFICIENT_DATA", "CONTINUE"), True),
    ("17_storage_failure", "storage / persistence failure",
     "The writer reported failed frames: existing deterministic error path.",
     {"recording.persistence_failed": 12.0, "recording.persistence_committed": 1088.0},
     expected("BAD", "OTHER", "STOP_FAILURE"), True),
    ("18_unresolved_fault", "unresolved lifecycle fault",
     "Coordinator holds an unresolved fault; nothing autonomous may run.",
     {"recording.fault_code": "save.fatal", "recording.fault_message": "HDF5 write failed"},
     expected("BAD", "OTHER", "STOP_FAILURE"), True),
]


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for old in OUT_DIR.glob("*.json"):
        old.unlink()
    for cid, scenario, notes, overrides, exp, expect_policy in SCENARIOS:
        snap = base_snapshot()
        for path, value in overrides.items():
            deep_update(snap, path, value)
        case = {
            "case_schema": 1,
            "id": cid,
            "scenario": scenario,
            "notes": notes,
            "dataset_version": DATASET_VERSION,
            "expect_policy": expect_policy,
            "expected": exp,
            "snapshot": snap,
        }
        (OUT_DIR / f"{cid}.json").write_text(json.dumps(case, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {len(SCENARIOS)} fixtures to {OUT_DIR}")


if __name__ == "__main__":
    main()
