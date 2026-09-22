#!/usr/bin/env python3
"""Compare native subtract/absdiff on the exact frozen Parquet samples.

No focus threshold is fitted and no scientific ground truth is inferred.
Timing excludes I/O, Python conversion, and warm-up; not sorting latency.
"""
from __future__ import annotations
import argparse
from collections import defaultdict
import csv
import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess

import numpy as np
from PIL import Image
import pyarrow.parquet as pq
import mib_processing as core

CONFIG = dict(gaussian_blur_size=3, bg_subtract_threshold=8,
              morph_kernel_size=3, morph_iterations=1,
              enable_border_check=True, enable_area_range_check=False,
              enable_ring_ratio_check=False, enable_deformability_range_check=False,
              enable_area_ratio_check=False, require_single_inner_contour=False,
              enable_target_group=False, multi_image_enabled=False)
VARIANTS = [("subtract_ring", "subtract", False), ("absdiff_ring", "absdiff", False),
            ("subtract_laplacian", "subtract", True), ("absdiff_laplacian", "absdiff", True)]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, q):
    return float(np.percentile(values, q)) if values else None


def iou(a, b):
    x, y, w, h = a
    xx, yy, ww, hh = b
    overlap = max(0, min(x+w, xx+ww)-max(x, xx)) * max(0, min(y+h, yy+hh)-max(y, yy))
    union = w*h+ww*hh-overlap
    return overlap/union if union else 0


def matched_pairs(left, right):
    # Deterministic greedy IoU; describes correspondence, not accuracy/ground truth.
    candidates = sorted((-iou(a["bbox"], b["bbox"]), i, j)
                        for i, a in enumerate(left) for j, b in enumerate(right)
                        if iou(a["bbox"], b["bbox"]) >= .3)
    used_left, used_right, pairs = set(), set(), []
    for negative_overlap, i, j in candidates:
        if i not in used_left and j not in used_right:
            used_left.add(i); used_right.add(j)
            pairs.append((left[i], right[j], -negative_overlap))
    return pairs


def write_csv(path, rows):
    if not rows:
        path.write_text("")
        return
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)


def source_provenance():
    root = Path(__file__).resolve().parent.parent
    try:
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
        dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True))
    except (OSError, subprocess.CalledProcessError):
        revision, dirty = None, None
    return dict(source_revision=revision, source_worktree_dirty=dirty,
                runner_sha256=digest(Path(__file__)),
                extractor_sha256=digest(Path(__file__).with_name("prepare_focus_benchmark.py")))


def run(dataset, output):
    dataset, output = dataset.resolve(), output.resolve()
    manifest = json.loads((dataset / "manifest.json").read_text())
    if manifest["status"] != "complete":
        raise ValueError("dataset extraction is not complete")
    output.mkdir(parents=True, exist_ok=False)
    core.set_opencv_threads(1)
    buckets = defaultdict(list)
    paired_rows, frame_count = [], 0
    details_path = output / "per_frame.jsonl"
    with details_path.open("w") as details:
        for recording in manifest["recordings"]:
            shard = dataset / recording["parquet"]
            bg_path = dataset / recording["background"]
            if digest(shard) != recording["parquet_sha256"] or digest(bg_path) != recording["background_png_sha256"]:
                raise ValueError("dataset artifact hash mismatch")
            background = np.array(Image.open(bg_path))
            table = pq.read_table(shard)
            if table.num_rows != recording["retained"]:
                raise ValueError("retained frame count mismatch")
            for row_index, row in enumerate(table.to_pylist()):
                image = np.array(Image.open(io.BytesIO(row["image"]["bytes"])))
                if hashlib.sha256(image.tobytes()).hexdigest() != row["image_sha256"]:
                    raise ValueError("decoded frame pixel hash mismatch")
                if row_index == 0:
                    for _, policy, lap in VARIANTS:
                        for _ in range(3): core.benchmark_frame(image, background, CONFIG, policy, lap)
                results = {}
                # Rotate order so the same variant is not always first.
                offset = frame_count % len(VARIANTS)
                for variant, policy, lap in VARIANTS[offset:] + VARIANTS[:offset]:
                    result = core.benchmark_frame(image, background, CONFIG, policy, lap)
                    result["mask_nonzero"] = int(np.count_nonzero(result.pop("mask")))
                    result.update(variant=variant, sample_id=row["sample_id"],
                                  recording=row["recording"], cell_line=row["cell_line"],
                                  focus_setting_v=row["focus_setting_v"])
                    buckets[(row["recording"], variant)].append(result)
                    buckets[("ALL", variant)].append(result)
                    results[variant] = result
                    details.write(json.dumps(result, allow_nan=False) + "\n")
                left, right = results["subtract_laplacian"]["objects"], results["absdiff_laplacian"]["objects"]
                for a, b, overlap in matched_pairs(left, right):
                    paired_rows.append(dict(sample_id=row["sample_id"], recording=row["recording"],
                        subtract_object_id=a["object_id"], absdiff_object_id=b["object_id"], iou=overlap,
                        both_valid=a["is_valid"] and b["is_valid"],
                        area_delta_px2=(b["area"]-a["area"] if a["area"] is not None and b["area"] is not None else None),
                        ring_delta=(b["ring_ratio"]-a["ring_ratio"] if a["ring_ratio"] is not None and b["ring_ratio"] is not None else None),
                        laplacian_delta=(b["laplacian_variance"]-a["laplacian_variance"] if a["laplacian_variance"] is not None and b["laplacian_variance"] is not None else None)))
                frame_count += 1
            print(f"Benchmarked {recording['recording']}: {table.num_rows} frames", flush=True)
    summaries = []
    for (recording, variant), frames in sorted(buckets.items()):
        objects = [obj for frame in frames for obj in frame["objects"]]
        ring = [obj["ring_ratio"] for obj in objects if obj["ring_ratio"] is not None]
        lap = [obj["laplacian_variance"] for obj in objects if obj["laplacian_variance"] is not None]
        times = [frame["pipeline_us"] for frame in frames]
        summaries.append(dict(recording=recording, variant=variant, frames=len(frames),
            frames_with_candidates=sum(bool(frame["objects"]) for frame in frames),
            object_candidates=len(objects), valid_objects=sum(obj["is_valid"] for obj in objects),
            ring_available=len(ring), ring_median=percentile(ring,50),
            laplacian_available=len(lap), laplacian_median=percentile(lap,50),
            pipeline_us_p50=percentile(times,50), pipeline_us_p95=percentile(times,95),
            pipeline_us_p99=percentile(times,99)))
    write_csv(output / "metrics.csv", summaries)
    write_csv(output / "paired_objects.csv", paired_rows)
    import mib_processing._mib_processing as native
    provenance = dict(schema_version=1, dataset_manifest_sha256=digest(dataset / "manifest.json"),
        native_extension_sha256=digest(Path(native.__file__)), config=CONFIG,
        core_version=core.__version__, python=platform.python_version(), platform=platform.platform(),
        opencv_version=next(iter(buckets.values()))[0]["opencv_version"], opencv_threads=1,
        total_input_frames=frame_count, timing="steady_clock mask+science, excludes conversion/I/O; 3 warmups/variant/recording; rotating variant order",
        laplacian_definition="Gray8 raw; filled selected inner/top-level contour; unmasked context crop; CV_64F ksize=1 scale=1 delta=0 REFLECT_101; population masked variance",
        limitations=["Laplacian variants retain ring diagnostics: additive metric overhead, not ring-free successor timing",
                     "No independent focus/segmentation labels; accuracy and false-accept/reject unavailable",
                     "No calibrated pixel-to-micron factor; image-space measurements only",
                     "Shared legacy science selects nested contours when present, outer fallback otherwise; not Contract 2",
                     "Voltage labels are filename-derived and not calibrated focus ground truth",
                     "Some sources filtered empty frames during acquisition; see upstream counts in manifest",
                     "Full-frame analysis: no recorded channel ROI is available; channel-wall artifacts may be candidates"],
        summary=summaries, matching="greedy IoU>=0.3; not accuracy ground truth", matched_pairs=len(paired_rows))
    provenance.update(source_provenance())
    (output / "results.json").write_text(json.dumps(provenance, indent=2, allow_nan=False)+"\n")
    headers = ["Variant", "Frames", "Candidates", "Valid*", "Ring available", "Median ring", "Median Laplacian", "p50 µs", "p95 µs"]
    lines = ["# Native core screening results", "", "| " + " | ".join(headers) + " |", "|"+"---|"*len(headers)]
    def fmt(value): return "N/A" if value is None else (f"{value:.3f}" if isinstance(value,float) else str(value))
    for row in summaries:
        if row["recording"] == "ALL":
            fields = [row[k] for k in ["variant","frames","object_candidates","valid_objects","ring_available","ring_median","laplacian_median","pipeline_us_p50","pipeline_us_p95"]]
            lines.append("| " + " | ".join(map(fmt,fields)) + " |")
    lines += ["", "*Valid means the configured core checks passed, not independently confirmed cells or good focus.",
              "Area/focus/deformability gates are disabled for this exploratory comparison; border checks remain enabled.",
              "", "## Interpretation limits", ""] + ["- "+item for item in provenance["limitations"]]
    lines += ["", "`metrics.csv` includes each recording separately; `paired_objects.csv` contains matched-object deltas.",
              "`per_frame.jsonl` records every candidate, missing metric, native timing, and sample identity.",
              "No winner or production threshold is selected from this unlabeled screening dataset."]
    (output / "README.md").write_text("\n".join(lines)+"\n")
    return provenance


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    run(args.dataset, args.output)
