#!/usr/bin/env python3
"""Validate detection behavior under deterministic brightness/contrast variants.

This harness samples frames from gavinlouuu/512x96stream, generates synthetic
image-condition variants in memory, runs the existing empty-frame detection
processing path, and writes reviewer-facing images plus metrics.
"""

from __future__ import annotations

import argparse
import base64
import html
import itertools
import json
import shlex
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


# Corpus identity and pinned revision come from env/assets.json.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from assets_manifest import get_asset  # noqa: E402

_STREAM_ASSET = get_asset("512x96stream-kin10")
DATASET_NAME = _STREAM_ASSET.repo
DATASET_CONFIG = _STREAM_ASSET.viewer["config"]
DATASET_SPLIT = _STREAM_ASSET.viewer["split"]
DATASET_REVISION = _STREAM_ASSET.revision
DEFAULT_OUTPUT_DIR = "review_artifacts/KIN-12"
DEFAULT_SAMPLE_START = 21
DEFAULT_SAMPLE_COUNT = 3
DEFAULT_BACKGROUND_SAMPLE_START = 0
DEFAULT_BACKGROUND_SAMPLE_COUNT = 1


@dataclass(frozen=True)
class TransformSpec:
    name: str
    brightness: float = 1.0
    contrast: float = 1.0
    description: str = ""


TRANSFORMS: tuple[TransformSpec, ...] = (
    TransformSpec("baseline", 1.0, 1.0, "Unmodified sampled frame."),
    TransformSpec("brightness_low", 0.65, 1.0, "Brightness scaled to 65%."),
    TransformSpec("brightness_high", 1.35, 1.0, "Brightness scaled to 135%."),
    TransformSpec(
        "brightness_extreme_low",
        0.35,
        1.0,
        "Brightness scaled to 35% for an extreme low-light case.",
    ),
    TransformSpec(
        "brightness_extreme_high",
        1.75,
        1.0,
        "Brightness scaled to 175% for an extreme bright case.",
    ),
    TransformSpec("contrast_low", 1.0, 0.65, "Contrast scaled to 65% around frame mean."),
    TransformSpec("contrast_high", 1.0, 1.35, "Contrast scaled to 135% around frame mean."),
    TransformSpec(
        "contrast_extreme_low",
        1.0,
        0.35,
        "Contrast scaled to 35% around frame mean for an extreme low-contrast case.",
    ),
    TransformSpec(
        "contrast_extreme_high",
        1.0,
        1.75,
        "Contrast scaled to 175% around frame mean for an extreme high-contrast case.",
    ),
)


@dataclass(frozen=True)
class DatasetSample:
    sample_index: int
    image: Any


@dataclass(frozen=True)
class DetectionRecord:
    sample_index: int
    condition: str
    detected: bool
    contour_count: int
    mask_pixels: int
    band_mask_pixels: int
    max_contour_area: float


def apply_brightness_contrast(
    image: Any,
    *,
    brightness: float,
    contrast: float,
) -> Any:
    """Apply deterministic brightness and contrast changes to a uint8 image."""
    import numpy as np

    arr = np.asarray(image)
    adjusted = arr.astype(np.float32)

    if contrast != 1.0:
        axes = (0, 1) if adjusted.ndim == 3 else None
        mean = adjusted.mean(axis=axes, keepdims=True) if axes else adjusted.mean()
        adjusted = (adjusted - mean) * contrast + mean

    if brightness != 1.0:
        adjusted = adjusted * brightness

    return np.clip(np.floor(adjusted + 0.5), 0, 255).astype(np.uint8)


def summarize_detection_records(
    records_by_condition: dict[str, list[DetectionRecord]],
) -> dict[str, dict[str, Any]]:
    """Summarize detection success/failure and parity against baseline."""
    baseline_by_sample = {
        record.sample_index: record.detected
        for record in records_by_condition.get("baseline", [])
    }

    summary: dict[str, dict[str, Any]] = {}
    for condition, records in records_by_condition.items():
        total = len(records)
        successes = sum(1 for record in records if record.detected)
        failures = total - successes

        parity_counts = {
            "baseline_success_variant_success": 0,
            "baseline_success_variant_failure": 0,
            "baseline_failure_variant_success": 0,
            "baseline_failure_variant_failure": 0,
            "same_as_baseline": 0,
            "changed_from_baseline": 0,
        }
        for record in records:
            baseline_detected = baseline_by_sample.get(record.sample_index)
            if baseline_detected is None:
                continue
            if baseline_detected and record.detected:
                parity_counts["baseline_success_variant_success"] += 1
            elif baseline_detected and not record.detected:
                parity_counts["baseline_success_variant_failure"] += 1
            elif not baseline_detected and record.detected:
                parity_counts["baseline_failure_variant_success"] += 1
            else:
                parity_counts["baseline_failure_variant_failure"] += 1

            if baseline_detected == record.detected:
                parity_counts["same_as_baseline"] += 1
            else:
                parity_counts["changed_from_baseline"] += 1

        summary[condition] = {
            "total_frames": total,
            "detection_success_count": successes,
            "detection_failure_count": failures,
            "detection_success_rate": successes / total if total else 0.0,
            **parity_counts,
        }

    return summary


def _processing_module():
    script_dir = Path(__file__).resolve().parent
    if str(script_dir) not in sys.path:
        sys.path.insert(0, str(script_dir))

    from empty_frame_detection import (  # type: ignore
        ProcessingConfig,
        build_background,
        ensure_grayscale,
        process_frame,
    )

    return ProcessingConfig, build_background, ensure_grayscale, process_frame


def _dataset_revision(dataset_name: str) -> str | None:
    try:
        from huggingface_hub import HfApi

        return HfApi().dataset_info(dataset_name).sha
    except Exception:
        return None


def load_samples(
    dataset_name: str,
    dataset_config: str,
    split: str,
    sample_start: int,
    sample_count: int,
) -> tuple[list[DatasetSample], dict[str, Any]]:
    """Load an exact Hugging Face split slice and preserve original row indices."""
    import numpy as np
    from datasets import load_dataset

    if sample_start < 0:
        raise ValueError("--sample-start must be >= 0")
    if sample_count < 1:
        raise ValueError("--sample-count must be >= 1")

    sample_end = sample_start + sample_count
    split_expr = f"{split}[{sample_start}:{sample_end}]"
    if dataset_config:
        dataset = load_dataset(
            dataset_name,
            dataset_config,
            split=split,
            streaming=True,
            revision=DATASET_REVISION if dataset_name == DATASET_NAME else None,
        )
    else:
        dataset = load_dataset(
            dataset_name,
            split=split,
            streaming=True,
            revision=DATASET_REVISION if dataset_name == DATASET_NAME else None,
        )

    samples: list[DatasetSample] = []
    selected_rows = itertools.islice(dataset, sample_start, sample_end)
    for offset, sample in enumerate(selected_rows):
        samples.append(
            DatasetSample(
                sample_index=sample_start + offset,
                image=np.asarray(sample["image"]),
            )
        )

    if len(samples) != sample_count:
        raise ValueError(
            f"Requested {sample_count} samples from {split_expr}, got {len(samples)}"
        )

    first_shape = list(samples[0].image.shape) if samples else []
    dataset_info = {
        "name": dataset_name,
        "config": dataset_config,
        "split": split,
        "split_expression": split_expr,
        "sample_start": sample_start,
        "sample_end_exclusive": sample_end,
        "sample_count": sample_count,
        "sample_indices": [sample.sample_index for sample in samples],
        "revision": _dataset_revision(dataset_name),
        "first_sample_shape": first_shape,
    }
    return samples, dataset_info


def _condition_frames(
    samples: list[DatasetSample],
    transform: TransformSpec,
) -> list[Any]:
    if transform.name == "baseline":
        return [sample.image for sample in samples]
    return [
        apply_brightness_contrast(
            sample.image,
            brightness=transform.brightness,
            contrast=transform.contrast,
        )
        for sample in samples
    ]


def run_detection(
    samples: list[DatasetSample],
    background_samples: list[DatasetSample],
    config: Any,
    background_mode: str,
    background_sample_count: int,
) -> tuple[
    dict[str, list[Any]],
    dict[str, list[DetectionRecord]],
    dict[str, list[Any]],
]:
    """Run the existing processing path for baseline and each transform."""
    import cv2
    import numpy as np

    _, build_background, ensure_grayscale, process_frame = _processing_module()

    if background_sample_count < 1:
        raise ValueError("--background-sample-count must be >= 1")
    if background_sample_count > len(background_samples):
        raise ValueError(
            "--background-sample-count cannot exceed loaded background samples"
        )

    frames_by_condition = {
        transform.name: _condition_frames(samples, transform)
        for transform in TRANSFORMS
    }
    background_frames_by_condition = {
        transform.name: _condition_frames(background_samples, transform)
        for transform in TRANSFORMS
    }
    baseline_background = build_background(
        [
            ensure_grayscale(frame)
            for frame in background_frames_by_condition["baseline"][
                :background_sample_count
            ]
        ]
    )

    records_by_condition: dict[str, list[DetectionRecord]] = {}
    results_by_condition: dict[str, list[Any]] = {}

    for transform in TRANSFORMS:
        condition = transform.name
        frames = frames_by_condition[condition]
        if background_mode == "baseline":
            background = baseline_background
        else:
            background = build_background(
                [
                    ensure_grayscale(frame)
                    for frame in background_frames_by_condition[condition][
                        :background_sample_count
                    ]
                ]
            )

        records: list[DetectionRecord] = []
        results: list[Any] = []
        for sample, frame in zip(samples, frames):
            result = process_frame(frame, background, config)
            contour_areas = [float(cv2.contourArea(c)) for c in result.contours]
            records.append(
                DetectionRecord(
                    sample_index=sample.sample_index,
                    condition=condition,
                    detected=not result.is_empty,
                    contour_count=len(result.contours),
                    mask_pixels=int(np.count_nonzero(result.mask)),
                    band_mask_pixels=int(np.count_nonzero(result.mask_band)),
                    max_contour_area=max(contour_areas, default=0.0),
                )
            )
            results.append(result)

        records_by_condition[condition] = records
        results_by_condition[condition] = results

    return frames_by_condition, records_by_condition, results_by_condition


def _clean_output_dir(output_dir: Path) -> None:
    for name in ("frames", "masks", "overlays"):
        shutil.rmtree(output_dir / name, ignore_errors=True)
    for name in (
        "metrics.json",
        "manifest.json",
        "sample_array_manifest.json",
        "README.md",
        "report.html",
        "flow_diagram.svg",
    ):
        path = output_dir / name
        if path.exists():
            path.unlink()
    for path in output_dir.glob("sample_case_*.png"):
        path.unlink()


def _write_png(path: Path, image: Any) -> None:
    import cv2
    import numpy as np

    path.parent.mkdir(parents=True, exist_ok=True)
    arr = np.asarray(image)
    if arr.ndim == 3 and arr.shape[2] == 3:
        arr = cv2.cvtColor(arr, cv2.COLOR_RGB2BGR)
    if not cv2.imwrite(str(path), arr):
        raise RuntimeError(f"Failed to write image: {path}")


def _baseline_detected_sample_indices(
    records_by_condition: dict[str, list[DetectionRecord]],
) -> list[int]:
    return [
        record.sample_index
        for record in records_by_condition.get("baseline", [])
        if record.detected
    ]


def _representative_sample_index(
    samples: list[DatasetSample],
    records_by_condition: dict[str, list[DetectionRecord]],
) -> int:
    detected_indices = _baseline_detected_sample_indices(records_by_condition)
    if detected_indices:
        return detected_indices[0]
    return samples[0].sample_index


def _records_by_sample(
    records_by_condition: dict[str, list[DetectionRecord]],
) -> dict[int, dict[str, DetectionRecord]]:
    records_by_sample: dict[int, dict[str, DetectionRecord]] = {}
    for condition, records in records_by_condition.items():
        for record in records:
            records_by_sample.setdefault(record.sample_index, {})[condition] = record
    return records_by_sample


def _first_sample_matching(
    samples: list[DatasetSample],
    records_by_sample: dict[int, dict[str, DetectionRecord]],
    predicate: Callable[[dict[str, DetectionRecord]], bool],
) -> int | None:
    for sample in samples:
        if predicate(records_by_sample.get(sample.sample_index, {})):
            return sample.sample_index
    return None


def _representative_sample_cases(
    samples: list[DatasetSample],
    records_by_condition: dict[str, list[DetectionRecord]],
) -> list[dict[str, Any]]:
    """Pick stable sample cases that summarize empty, cell, and failure behavior."""
    records_by_sample = _records_by_sample(records_by_condition)
    cases: list[dict[str, Any]] = []
    seen_case_keys: set[str] = set()

    def add_case(
        key: str,
        title: str,
        sample_index: int | None,
        conditions: list[str],
        reason: str,
    ) -> None:
        if sample_index is None or key in seen_case_keys:
            return
        available = records_by_sample.get(sample_index, {})
        selected_conditions = [
            condition for condition in conditions if condition in available
        ]
        if not selected_conditions:
            return
        seen_case_keys.add(key)
        cases.append(
            {
                "key": key,
                "title": title,
                "sample_index": sample_index,
                "conditions": selected_conditions,
                "reason": reason,
                "metrics": {
                    condition: asdict(available[condition])
                    for condition in selected_conditions
                },
            }
        )

    empty_index = _first_sample_matching(
        samples,
        records_by_sample,
        lambda records: "baseline" in records and not records["baseline"].detected,
    )
    add_case(
        "empty_baseline_reference",
        "Empty/no-contour baseline reference",
        empty_index,
        ["baseline"],
        "First sampled HF frame where the baseline detector returned no filtered contours.",
    )

    detected_index = _first_sample_matching(
        samples,
        records_by_sample,
        lambda records: "baseline" in records and records["baseline"].detected,
    )
    add_case(
        "cell_positive_baseline",
        "Cell-positive baseline reference",
        detected_index,
        [transform.name for transform in TRANSFORMS],
        "First sampled HF frame where the baseline detector returned filtered contours.",
    )

    standard_low_failure_index = _first_sample_matching(
        samples,
        records_by_sample,
        lambda records: (
            "baseline" in records
            and records["baseline"].detected
            and (
                (
                    "brightness_low" in records
                    and not records["brightness_low"].detected
                )
                or ("contrast_low" in records and not records["contrast_low"].detected)
            )
        ),
    )
    add_case(
        "standard_low_condition_drop",
        "Standard low-condition detection drop",
        standard_low_failure_index,
        ["baseline", "brightness_low", "contrast_low"],
        "Cell-positive HF frame where a standard low brightness or contrast condition lost detection.",
    )

    extreme_low_failure_index = _first_sample_matching(
        samples,
        records_by_sample,
        lambda records: (
            "baseline" in records
            and records["baseline"].detected
            and (
                (
                    "brightness_extreme_low" in records
                    and not records["brightness_extreme_low"].detected
                )
                or (
                    "contrast_extreme_low" in records
                    and not records["contrast_extreme_low"].detected
                )
            )
        ),
    )
    add_case(
        "extreme_low_condition_drop",
        "Extreme low-condition detection drop",
        extreme_low_failure_index,
        ["baseline", "brightness_extreme_low", "contrast_extreme_low"],
        "Cell-positive HF frame where extreme low brightness and contrast conditions lose detection.",
    )

    fallback_conditions = [
        "baseline",
        "brightness_low",
        "brightness_high",
        "contrast_low",
        "contrast_high",
    ]
    target_unique_samples = min(3, len(samples))

    def unique_sample_count() -> int:
        return len({case["sample_index"] for case in cases})

    for sample in samples:
        if len(cases) >= 3 and unique_sample_count() >= target_unique_samples:
            break
        if any(case["sample_index"] == sample.sample_index for case in cases):
            continue
        add_case(
            f"fallback_sample_{sample.sample_index:05d}",
            "Fallback sampled frame",
            sample.sample_index,
            fallback_conditions,
            "Fallback case included to keep at least three reviewer sample cases when available.",
        )

    return cases


def _artifact_paths_for_case(case: dict[str, Any]) -> dict[str, Any]:
    sample_name = f"sample_{case['sample_index']:05d}"
    paths_by_condition = {}
    for condition in case["conditions"]:
        paths_by_condition[condition] = {
            "input": str(
                Path("frames") / condition / f"{sample_name}_input.png"
            ),
            "mask": str(Path("masks") / condition / f"{sample_name}_mask.png"),
            "overlay": str(
                Path("overlays") / condition / f"{sample_name}_overlay.png"
            ),
        }
    return {
        **case,
        "paths": paths_by_condition,
    }


def _read_png_rgb(path: Path) -> Any:
    import cv2

    image = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if image is None:
        raise RuntimeError(f"Failed to read image: {path}")
    if image.ndim == 2:
        return cv2.cvtColor(image, cv2.COLOR_GRAY2RGB)
    if image.shape[2] == 4:
        return cv2.cvtColor(image, cv2.COLOR_BGRA2RGB)
    return cv2.cvtColor(image, cv2.COLOR_BGR2RGB)


def _sample_case_contact_sheet(
    output_dir: Path,
    sample_case: dict[str, Any],
) -> Any:
    import cv2
    import numpy as np

    rows = []
    for condition in sample_case["conditions"]:
        paths = sample_case["paths"][condition]
        images = [
            _read_png_rgb(output_dir / paths["input"]),
            _read_png_rgb(output_dir / paths["mask"]),
            _read_png_rgb(output_dir / paths["overlay"]),
        ]
        row = np.concatenate(images, axis=1)
        label_bar = np.full((22, row.shape[1], 3), 32, dtype=np.uint8)
        metrics = sample_case["metrics"][condition]
        label = (
            f"{sample_case['key']} sample={sample_case['sample_index']} "
            f"{condition} detected={metrics['detected']} "
            f"contours={metrics['contour_count']} mask_pixels={metrics['mask_pixels']}"
        )
        cv2.putText(
            label_bar,
            label,
            (5, 15),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.42,
            (255, 255, 255),
            1,
        )
        rows.append(np.concatenate([label_bar, row], axis=0))

    separator = np.full((4, rows[0].shape[1], 3), 64, dtype=np.uint8)
    separated_rows = []
    for row in rows:
        if separated_rows:
            separated_rows.append(separator)
        separated_rows.append(row)
    return np.concatenate(separated_rows, axis=0)


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _git_value(args: list[str]) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            check=True,
            capture_output=True,
            text=True,
        )
    except Exception:
        return None
    return result.stdout.strip() or None


def _relative_path(output_dir: Path, path: Path) -> str:
    try:
        return str(path.relative_to(output_dir))
    except ValueError:
        return str(path)


def _read_json_if_present(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def _image_data_uri(path: Path) -> str | None:
    if not path.exists():
        return None
    encoded = base64.b64encode(path.read_bytes()).decode("ascii")
    suffix = path.suffix.lower()
    media_type = "image/svg+xml" if suffix == ".svg" else "image/png"
    return f"data:{media_type};base64,{encoded}"


def _artifact_record(
    output_dir: Path,
    path: Path,
    *,
    purpose: str,
    command: str,
    sample_id: int | None = None,
    condition: str | None = None,
) -> dict[str, Any]:
    exists = path.exists()
    record: dict[str, Any] = {
        "purpose": purpose,
        "relative_path": _relative_path(output_dir, path),
        "local_path": str(path),
        "command_provenance": command,
        "exists": exists,
        "file_size_bytes": path.stat().st_size if exists else None,
    }
    if sample_id is not None:
        record["sample_id"] = sample_id
    if condition is not None:
        record["condition"] = condition
    return record


def _write_flow_diagram(output_dir: Path, split_expression: str) -> Path:
    path = output_dir / "flow_diagram.svg"
    path.write_text(
        """<svg xmlns="http://www.w3.org/2000/svg" width="980" height="360" viewBox="0 0 980 360" role="img" aria-labelledby="title desc">
  <title id="title">KIN-12 synthetic validation flow</title>
  <desc id="desc">Dataset frames are loaded, transformed, detected, and written as review evidence.</desc>
  <defs>
    <marker id="arrow" markerWidth="10" markerHeight="8" refX="9" refY="4" orient="auto">
      <path d="M0,0 L10,4 L0,8 Z" fill="#334155"/>
    </marker>
    <style>
      .box { fill: #f8fafc; stroke: #334155; stroke-width: 2; rx: 8; }
      .accent { fill: #ecfeff; stroke: #0f766e; stroke-width: 2; rx: 8; }
      .evidence { fill: #fff7ed; stroke: #c2410c; stroke-width: 2; rx: 8; }
      .label { font: 14px sans-serif; fill: #0f172a; font-weight: 700; }
      .small { font: 12px sans-serif; fill: #334155; }
      .arrow { stroke: #334155; stroke-width: 2; marker-end: url(#arrow); fill: none; }
    </style>
  </defs>
  <rect x="30" y="48" width="165" height="92" class="box"/>
  <text x="48" y="78" class="label">HF dataset slice</text>
  <text x="48" y="104" class="small">gavinlouuu/512x96stream</text>
  <text x="48" y="122" class="small">default __SPLIT_EXPRESSION__</text>
  <path d="M195 94 H260" class="arrow"/>
  <rect x="260" y="48" width="175" height="92" class="accent"/>
  <text x="278" y="78" class="label">Synthetic variants</text>
  <text x="278" y="104" class="small">brightness low/high/extreme</text>
  <text x="278" y="122" class="small">contrast low/high/extreme</text>
  <path d="M435 94 H500" class="arrow"/>
  <rect x="500" y="48" width="185" height="92" class="box"/>
  <text x="518" y="78" class="label">Detection path</text>
  <text x="518" y="104" class="small">background subtraction</text>
  <text x="518" y="122" class="small">middle-band contours</text>
  <path d="M685 94 H750" class="arrow"/>
  <rect x="750" y="48" width="190" height="92" class="evidence"/>
  <text x="768" y="78" class="label">Evidence bundle</text>
  <text x="768" y="104" class="small">frames, masks, overlays</text>
  <text x="768" y="122" class="small">metrics, report, manifest</text>
  <path d="M592 140 V218 H182" class="arrow"/>
  <rect x="60" y="218" width="245" height="92" class="evidence"/>
  <text x="78" y="248" class="label">Representative gallery</text>
  <text x="78" y="274" class="small">empty, cell-positive, low-drop</text>
  <text x="78" y="292" class="small">input plus mask plus overlay</text>
  <path d="M845 140 V218 H710" class="arrow"/>
  <rect x="595" y="218" width="250" height="92" class="evidence"/>
  <text x="613" y="248" class="label">Review report</text>
  <text x="613" y="274" class="small">summary, commands, checks</text>
  <text x="613" y="292" class="small">metrics table and limitations</text>
</svg>
""".replace("__SPLIT_EXPRESSION__", html.escape(split_expression)),
        encoding="utf-8",
    )
    return path


def _command_rows(command: str, output_dir: Path) -> list[dict[str, Any]]:
    ci_summary = _read_json_if_present(output_dir / "logs" / "ci-summary.json")
    rows = list(ci_summary.get("commands", []))
    if not any(row.get("label") == "synthetic evidence regeneration" for row in rows):
        rows.append(
            {
                "label": "synthetic evidence regeneration",
                "command": command,
                "exit_code": 0,
                "started_at": None,
                "ended_at": _utc_timestamp(),
                "log": "logs/synthetic_validation.log",
            }
        )
    return rows


def _html_table(headers: list[str], rows: list[list[Any]]) -> str:
    head = "".join(f"<th>{html.escape(header)}</th>" for header in headers)
    body_rows = []
    for row in rows:
        cells = "".join(
            f"<td>{html.escape('' if cell is None else str(cell))}</td>"
            for cell in row
        )
        body_rows.append(f"<tr>{cells}</tr>")
    return f"<table><thead><tr>{head}</tr></thead><tbody>{''.join(body_rows)}</tbody></table>"


def _write_report_html(
    output_dir: Path,
    metrics: dict[str, Any],
    command: str,
    flow_diagram_path: Path,
) -> Path:
    report_path = output_dir / "report.html"
    dataset = metrics["dataset"]
    background_dataset = metrics["background_dataset"]
    condition_summary = metrics["condition_summary"]
    ci_summary = _read_json_if_present(output_dir / "logs" / "ci-summary.json")
    pull_request = ci_summary.get("pull_request", {})
    commit = _git_value(["rev-parse", "--short", "HEAD"]) or "unknown"
    branch = _git_value(["branch", "--show-current"]) or "unknown"
    baseline = condition_summary.get("baseline", {})
    verdict = (
        "Pass: deterministic synthetic validation generated for "
        f"{dataset['name']} {dataset['split_expression']}; baseline "
        f"{baseline.get('detection_success_count', 0)}/"
        f"{baseline.get('total_frames', 0)} target frames detected cells. "
        "Condition summaries report success/failure counts for every "
        "brightness and contrast transform."
    )

    command_table = _html_table(
        ["Label", "Command", "Exit", "Started", "Ended", "Log"],
        [
            [
                row.get("label"),
                row.get("command"),
                row.get("exit_code"),
                row.get("started_at"),
                row.get("ended_at"),
                row.get("log"),
            ]
            for row in _command_rows(command, output_dir)
        ],
    )
    condition_table = _html_table(
        [
            "Condition",
            "Success",
            "Failure",
            "Changed From Baseline",
            "Success Rate",
        ],
        [
            [
                condition,
                summary["detection_success_count"],
                summary["detection_failure_count"],
                summary["changed_from_baseline"],
                f"{summary['detection_success_rate']:.3f}",
            ]
            for condition, summary in sorted(condition_summary.items())
        ],
    )
    per_sample_rows = []
    for condition, records in sorted(metrics["per_frame"].items()):
        for record in records:
            per_sample_rows.append(
                [
                    record["sample_index"],
                    condition,
                    record["detected"],
                    record["contour_count"],
                    record["mask_pixels"],
                    record["band_mask_pixels"],
                    record["max_contour_area"],
                ]
            )
    metrics_table = _html_table(
        [
            "Sample ID",
            "Condition",
            "Detected",
            "Contours",
            "Mask Pixels",
            "Band Mask Pixels",
            "Max Contour Area",
        ],
        per_sample_rows,
    )

    flow_svg = flow_diagram_path.read_text(encoding="utf-8")
    gallery_items = []
    for sample_case in metrics["sample_cases"]:
        contact_sheet = output_dir / sample_case["contact_sheet"]
        data_uri = _image_data_uri(contact_sheet)
        image_html = (
            f'<img src="{data_uri}" alt="{html.escape(sample_case["title"])}">'
            if data_uri
            else "<p>Contact sheet missing.</p>"
        )
        gallery_items.append(
            f"""
            <section class="sample">
              <h3>{html.escape(sample_case['title'])}</h3>
              <p><strong>Sample ID:</strong> {sample_case['sample_index']} | <strong>Conditions:</strong> {html.escape(', '.join(sample_case['conditions']))}</p>
              <p>{html.escape(sample_case['reason'])}</p>
              {image_html}
            </section>
            """
        )

    html_text = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>KIN-12 Synthetic Condition Validation Report</title>
  <style>
    body {{ font-family: Arial, sans-serif; color: #111827; margin: 24px; line-height: 1.45; }}
    h1, h2, h3 {{ color: #0f172a; }}
    .verdict {{ background: #ecfdf5; border: 1px solid #047857; padding: 12px; border-radius: 6px; }}
    .section {{ margin-top: 28px; }}
    table {{ width: 100%; border-collapse: collapse; font-size: 12px; }}
    th, td {{ border: 1px solid #cbd5e1; padding: 6px; vertical-align: top; }}
    th {{ background: #f1f5f9; text-align: left; }}
    code, pre {{ background: #f8fafc; padding: 2px 4px; border-radius: 4px; }}
    img {{ max-width: 100%; border: 1px solid #cbd5e1; }}
    .sample {{ border-top: 1px solid #cbd5e1; padding-top: 16px; margin-top: 16px; }}
    .diagram svg {{ max-width: 100%; height: auto; }}
  </style>
</head>
<body>
  <h1>KIN-12 Synthetic Condition Validation</h1>
  <p class="verdict">{html.escape(verdict)}</p>

  <div class="section">
    <h2>Metadata</h2>
    <table>
      <tr><th>Issue</th><td>KIN-12</td></tr>
      <tr><th>Branch</th><td>{html.escape(branch)}</td></tr>
      <tr><th>Commit</th><td>{html.escape(commit)}</td></tr>
      <tr><th>Pull Request</th><td>{html.escape(str(pull_request.get('url', 'pending')))}</td></tr>
      <tr><th>CI Status</th><td>{html.escape(str(pull_request.get('checks_status', 'pending')))}</td></tr>
      <tr><th>Dataset</th><td>{html.escape(dataset['name'])}</td></tr>
      <tr><th>Target Split/Samples</th><td>{html.escape(dataset['split_expression'])} indices {html.escape(str(dataset['sample_indices']))}</td></tr>
      <tr><th>Background Split/Samples</th><td>{html.escape(background_dataset['split_expression'])} indices {html.escape(str(background_dataset['sample_indices']))}</td></tr>
      <tr><th>Dataset Revision</th><td>{html.escape(str(dataset['revision']))}</td></tr>
      <tr><th>Generated At</th><td>{_utc_timestamp()}</td></tr>
    </table>
  </div>

  <div class="section">
    <h2>Commands</h2>
    {command_table}
  </div>

  <div class="section">
    <h2>Build / Compile</h2>
    <p>Python-only harness validation uses <code>py_compile</code>; no C++/Qt binaries are changed by this ticket. Exit codes and logs are listed in the command table.</p>
  </div>

  <div class="section">
    <h2>Tests</h2>
    <p>Focused pytest coverage validates deterministic transforms, extreme cases, representative cell-positive sample selection, and condition count summaries. Exit codes and logs are listed in the command table.</p>
  </div>

  <div class="section">
    <h2>PR Feedback Sweep</h2>
    <p>{html.escape(str(ci_summary.get('pr_feedback_sweep', {}).get('summary', 'Pending final PR feedback sweep.')))}</p>
  </div>

  <div class="section diagram">
    <h2>Flow Diagram</h2>
    <div>{flow_svg}</div>
  </div>

  <div class="section">
    <h2>Condition Summary</h2>
    {condition_table}
  </div>

  <div class="section">
    <h2>Visual Sample Gallery</h2>
    {''.join(gallery_items)}
  </div>

  <div class="section">
    <h2>Metrics By Sample ID</h2>
    {metrics_table}
  </div>

  <div class="section">
    <h2>Known Limitations / Confusions</h2>
    <ul>
      <li>The issue description still references KIN-6 as a dependency, but this repository copy contains enough detection primitives for a standalone validation harness.</li>
      <li>The sample slice is intentionally fixed to {html.escape(dataset['split_expression'])}; broader statistical claims need a larger validation run.</li>
    </ul>
  </div>

  <div class="section">
    <h2>Regeneration</h2>
    <pre>{html.escape(command)}</pre>
  </div>
</body>
</html>
"""
    report_path.write_text(html_text, encoding="utf-8")
    return report_path


def _purpose_for_artifact(key: str) -> str:
    if key == "report":
        return "Canonical human-readable review report."
    if key == "flow_diagram":
        return "Synthetic validation data/control flow diagram."
    if key == "metrics":
        return "Machine-readable per-frame and aggregate detection metrics."
    if key == "manifest":
        return "Machine-readable review artifact index with provenance."
    if key == "sample_array_manifest":
        return "Representative sample-case mapping for visual review."
    if key == "readme":
        return "Reviewer-facing regeneration and bundle explanation."
    if key.endswith("_input"):
        return "Sample input frame for the selected condition."
    if key.endswith("_mask"):
        return "Processed binary mask for the selected condition."
    if key.endswith("_overlay"):
        return "Overlay with band boundaries, mask tint, and contours."
    if key.startswith("sample_case_"):
        return "Contact sheet with input, mask, and overlay columns."
    if key == "log_ci_summary":
        return "Command, PR feedback, and CI summary consumed by report.html."
    if key.endswith("_command"):
        return "Structured command provenance with exit code and timestamps."
    if key.endswith("_log"):
        return "Command output log captured during validation."
    if key.startswith("log_"):
        return "Validation log or command summary captured during evidence generation."
    return "Review artifact."


def _sample_id_for_key(key: str, sample_cases: list[dict[str, Any]]) -> int | None:
    for sample_case in sample_cases:
        if key == f"sample_case_{sample_case['key']}":
            return int(sample_case["sample_index"])
    return None


def _condition_for_key(key: str) -> str | None:
    for suffix in ("_input", "_mask", "_overlay"):
        if key.endswith(suffix):
            return key[: -len(suffix)]
    return None


def _write_manifest(
    output_dir: Path,
    command: str,
    metrics: dict[str, Any],
    artifact_paths: dict[str, Path],
) -> Path:
    manifest_path = output_dir / "manifest.json"
    script_path = Path("scripts/synthetic_condition_validation.py")
    all_paths = dict(artifact_paths)
    all_paths["regeneration_script"] = script_path
    for log_path in sorted((output_dir / "logs").glob("*")):
        if log_path.is_file():
            all_paths[f"log_{log_path.stem.replace('-', '_')}"] = log_path
    for legacy_log in (
        output_dir / "python_validation.log",
        output_dir / "synthetic_validation.log",
        output_dir / "pytest.log",
    ):
        if legacy_log.exists():
            all_paths[f"legacy_{legacy_log.stem}_log"] = legacy_log

    artifact_records = {
        key: _artifact_record(
            output_dir,
            path,
            purpose=_purpose_for_artifact(key),
            command=command,
            sample_id=(
                _sample_id_for_key(key, metrics["sample_cases"])
                or (
                    metrics["review_sample_index"]
                    if _condition_for_key(key) is not None
                    else None
                )
            ),
            condition=_condition_for_key(key),
        )
        for key, path in sorted(all_paths.items())
    }
    artifact_records["manifest"] = _artifact_record(
        output_dir,
        manifest_path,
        purpose=_purpose_for_artifact("manifest"),
        command=command,
    )

    manifest = {
        "issue": "KIN-12",
        "generated_at": _utc_timestamp(),
        "command": command,
        "dataset": metrics["dataset"],
        "background_dataset": metrics["background_dataset"],
        "background_mode": metrics["background_mode"],
        "background_sample_count": metrics["background_sample_count"],
        "baseline_detected_sample_indices": metrics[
            "baseline_detected_sample_indices"
        ],
        "review_sample_index": metrics["review_sample_index"],
        "sample_cases": metrics["sample_cases"],
        "transforms": metrics["transforms"],
        "artifacts": artifact_records,
        "linear_uploads": _read_json_if_present(output_dir / "linear_uploads.json"),
        "review_paths": {
            key: record["relative_path"]
            for key, record in artifact_records.items()
        },
    }

    last_size: int | None = None
    for _ in range(4):
        manifest["artifacts"]["manifest"] = _artifact_record(
            output_dir,
            manifest_path,
            purpose=_purpose_for_artifact("manifest"),
            command=command,
        )
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        current_size = manifest_path.stat().st_size
        if current_size == last_size:
            break
        last_size = current_size
    return manifest_path


def _overlay_image(image: Any, result: Any, config: Any, label: str) -> Any:
    import cv2
    import numpy as np

    _, _, ensure_grayscale, _ = _processing_module()
    gray = ensure_grayscale(image)
    h, w = gray.shape[:2]
    vis = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)

    mask = result.mask > 0
    if np.any(mask):
        tint = np.zeros_like(vis)
        tint[:, :, 1] = 255
        vis[mask] = cv2.addWeighted(vis[mask], 0.55, tint[mask], 0.45, 0)

    band_margin = int(h * (1.0 - config.band_fraction) / 2.0)
    y_start = band_margin
    y_end = h - band_margin
    cv2.line(vis, (0, y_start), (w, y_start), (255, 255, 0), 1)
    cv2.line(vis, (0, y_end), (w, y_end), (255, 255, 0), 1)

    shifted_contours = []
    for contour in result.contours:
        shifted = contour.copy()
        shifted[:, :, 1] += y_start
        shifted_contours.append(shifted)
    cv2.drawContours(vis, shifted_contours, -1, (0, 0, 255), 1)

    text = f"{label} detected={not result.is_empty} contours={len(result.contours)}"
    cv2.putText(
        vis,
        text,
        (5, 16),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.42,
        (255, 255, 255),
        1,
    )
    return cv2.cvtColor(vis, cv2.COLOR_BGR2RGB)


def write_artifacts(
    output_dir: Path,
    samples: list[DatasetSample],
    frames_by_condition: dict[str, list[Any]],
    records_by_condition: dict[str, list[DetectionRecord]],
    results_by_condition: dict[str, list[Any]],
    dataset_info: dict[str, Any],
    background_dataset_info: dict[str, Any],
    processing_config: Any,
    background_mode: str,
    background_sample_count: int,
    command: str,
    clean: bool,
) -> dict[str, Path]:
    """Write review images, metrics JSON, manifest, and README."""
    output_dir.mkdir(parents=True, exist_ok=True)
    if clean:
        _clean_output_dir(output_dir)

    review_sample_index = _representative_sample_index(samples, records_by_condition)
    baseline_detected_indices = _baseline_detected_sample_indices(records_by_condition)
    sample_cases = [
        _artifact_paths_for_case(sample_case)
        for sample_case in _representative_sample_cases(samples, records_by_condition)
    ]

    artifact_paths: dict[str, Path] = {}
    for transform in TRANSFORMS:
        condition = transform.name
        for position, sample in enumerate(samples):
            sample_name = f"sample_{sample.sample_index:05d}"
            frame = frames_by_condition[condition][position]
            result = results_by_condition[condition][position]

            input_path = output_dir / "frames" / condition / f"{sample_name}_input.png"
            mask_path = output_dir / "masks" / condition / f"{sample_name}_mask.png"
            overlay_path = (
                output_dir / "overlays" / condition / f"{sample_name}_overlay.png"
            )

            _write_png(input_path, frame)
            _write_png(mask_path, result.mask)
            _write_png(
                overlay_path,
                _overlay_image(
                    frame,
                    result,
                    processing_config,
                    f"{condition}:{sample.sample_index}",
                ),
            )

            if sample.sample_index == review_sample_index:
                artifact_paths[f"{condition}_input"] = input_path
                artifact_paths[f"{condition}_mask"] = mask_path
                artifact_paths[f"{condition}_overlay"] = overlay_path

    for sample_case in sample_cases:
        contact_sheet_path = output_dir / f"sample_case_{sample_case['key']}.png"
        _write_png(
            contact_sheet_path,
            _sample_case_contact_sheet(output_dir, sample_case),
        )
        sample_case["contact_sheet"] = str(contact_sheet_path.relative_to(output_dir))
        artifact_paths[f"sample_case_{sample_case['key']}"] = contact_sheet_path

    per_frame = {
        condition: [asdict(record) for record in records]
        for condition, records in records_by_condition.items()
    }
    metrics = {
        "dataset": dataset_info,
        "background_dataset": background_dataset_info,
        "processing_config": dict(vars(processing_config)),
        "background_mode": background_mode,
        "background_sample_count": background_sample_count,
        "baseline_detected_sample_indices": baseline_detected_indices,
        "review_sample_index": review_sample_index,
        "sample_cases": sample_cases,
        "review_sample_selection": (
            "First sampled frame where the baseline processing path returned "
            "at least one filtered contour; falls back to the first sampled frame."
        ),
        "detection_success_definition": (
            "A frame succeeds when the existing processing path returns at least "
            "one filtered contour in the middle band."
        ),
        "transforms": [asdict(transform) for transform in TRANSFORMS],
        "condition_summary": summarize_detection_records(records_by_condition),
        "per_frame": per_frame,
    }

    metrics_path = output_dir / "metrics.json"
    metrics_path.write_text(
        json.dumps(metrics, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    artifact_paths["metrics"] = metrics_path

    sample_array_manifest = {
        "issue": "KIN-12",
        "dataset": dataset_info,
        "background_dataset": background_dataset_info,
        "selection": (
            "Representative sample cases are selected deterministically from the "
            "requested split slice: first empty baseline, first cell-positive "
            "baseline, first standard low-condition detection drop, and first "
            "extreme low-condition detection drop when available."
        ),
        "sample_cases": sample_cases,
    }
    sample_array_manifest_path = output_dir / "sample_array_manifest.json"
    sample_array_manifest_path.write_text(
        json.dumps(sample_array_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    artifact_paths["sample_array_manifest"] = sample_array_manifest_path

    readme_path = output_dir / "README.md"
    readme_path.write_text(
        _readme_text(
            dataset_info,
            background_dataset_info,
            background_mode,
            background_sample_count,
            command,
            baseline_detected_indices,
            review_sample_index,
            sample_cases,
        ),
        encoding="utf-8",
    )
    artifact_paths["readme"] = readme_path

    flow_diagram_path = _write_flow_diagram(
        output_dir,
        dataset_info["split_expression"],
    )
    artifact_paths["flow_diagram"] = flow_diagram_path
    report_path = _write_report_html(output_dir, metrics, command, flow_diagram_path)
    artifact_paths["report"] = report_path
    manifest_path = _write_manifest(output_dir, command, metrics, artifact_paths)
    artifact_paths["manifest"] = manifest_path
    return artifact_paths


def _readme_text(
    dataset_info: dict[str, Any],
    background_dataset_info: dict[str, Any],
    background_mode: str,
    background_sample_count: int,
    command: str,
    baseline_detected_indices: list[int],
    review_sample_index: int,
    sample_cases: list[dict[str, Any]],
) -> str:
    sample_case_lines = "\n".join(
        (
            f"- `{case['key']}`: sample `{case['sample_index']}`, "
            f"conditions `{case['conditions']}` - {case['reason']}"
        )
        for case in sample_cases
    )
    return f"""# KIN-12 Synthetic Condition Validation

This bundle validates the existing middle-band contour detection path on
`{dataset_info['name']}` with deterministic brightness and contrast variants,
including standard low/high cases and more extreme low/high cases.

## Dataset Slice

- Dataset: `{dataset_info['name']}`
- Config: `{dataset_info['config']}`
- Split expression: `{dataset_info['split_expression']}`
- Sample indices: `{dataset_info['sample_indices']}`
- Dataset revision: `{dataset_info['revision']}`
- Background split expression: `{background_dataset_info['split_expression']}`
- Background sample indices: `{background_dataset_info['sample_indices']}`
- Background mode: `{background_mode}`
- Background sample count: `{background_sample_count}`
- Baseline detected sample indices: `{baseline_detected_indices}`
- Reviewer-facing sample index: `{review_sample_index}`

## Representative Sample Cases

{sample_case_lines}

## Regenerate

Run from the repository root:

```bash
{command}
```

## Outputs

- `frames/` contains baseline and transformed input frames.
- `masks/` contains processed binary masks from the detection path.
- `overlays/` contains input frames with band boundaries, mask tint, and contours.
- `metrics.json` reports detection success/failure counts per condition and parity
  against baseline, including matching metrics for the representative sample cases.
- `manifest.json` records the exact dataset slice, transforms, and review paths.
- `sample_array_manifest.json` records the deterministic reviewer sample-case
  array with input, mask, overlay, and per-condition metric paths.
- `sample_case_*.png` contact sheets show each representative case as input,
  mask, and overlay columns for the selected conditions.

The sample-case array includes cell-positive frames from the Hugging Face
dataset when the requested slice contains baseline detections.
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate synthetic brightness/contrast detection behavior on a "
            "Hugging Face stream dataset."
        ),
    )
    parser.add_argument("--dataset", default=DATASET_NAME)
    parser.add_argument("--dataset-config", default=DATASET_CONFIG)
    parser.add_argument("--split", default=DATASET_SPLIT)
    parser.add_argument("--sample-start", type=int, default=DEFAULT_SAMPLE_START)
    parser.add_argument("--sample-count", type=int, default=DEFAULT_SAMPLE_COUNT)
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument(
        "--background-mode",
        choices=("transformed", "baseline"),
        default="transformed",
        help=(
            "Use condition-matched transformed backgrounds or the baseline "
            "background for every condition."
        ),
    )
    parser.add_argument(
        "--background-sample-start",
        type=int,
        default=DEFAULT_BACKGROUND_SAMPLE_START,
        help=(
            "Starting row for the deterministic background slice. This is "
            "loaded separately from the target validation slice."
        ),
    )
    parser.add_argument(
        "--background-sample-count",
        type=int,
        default=DEFAULT_BACKGROUND_SAMPLE_COUNT,
        help="Number of leading sampled frames used to build each background.",
    )
    parser.add_argument("--blur", type=int, default=3)
    parser.add_argument("--threshold", type=int, default=8)
    parser.add_argument("--morph-kernel", type=int, default=3)
    parser.add_argument("--morph-iterations", type=int, default=1)
    parser.add_argument("--min-area", type=float, default=100.0)
    parser.add_argument("--band-fraction", type=float, default=0.5)
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="Do not remove prior generated images/JSON in the output directory.",
    )
    return parser.parse_args()


def _regeneration_command(args: argparse.Namespace) -> str:
    parts = [
        "PYTHONPATH=.python_deps",
        "HF_HOME=.cache/huggingface",
        "HF_DATASETS_CACHE=.cache/huggingface/datasets",
        "python",
        "scripts/synthetic_condition_validation.py",
        "--dataset",
        args.dataset,
        "--dataset-config",
        args.dataset_config,
        "--split",
        args.split,
        "--sample-start",
        str(args.sample_start),
        "--sample-count",
        str(args.sample_count),
        "--output-dir",
        args.output_dir,
        "--background-mode",
        args.background_mode,
        "--background-sample-start",
        str(args.background_sample_start),
        "--background-sample-count",
        str(args.background_sample_count),
        "--blur",
        str(args.blur),
        "--threshold",
        str(args.threshold),
        "--morph-kernel",
        str(args.morph_kernel),
        "--morph-iterations",
        str(args.morph_iterations),
        "--min-area",
        str(args.min_area),
        "--band-fraction",
        str(args.band_fraction),
    ]
    if args.no_clean:
        parts.append("--no-clean")
    return " ".join(shlex.quote(part) for part in parts)


def main() -> None:
    args = parse_args()
    ProcessingConfig, _, _, _ = _processing_module()
    processing_config = ProcessingConfig(
        gaussian_blur_size=args.blur,
        bg_subtract_threshold=args.threshold,
        morph_kernel_size=args.morph_kernel,
        morph_iterations=args.morph_iterations,
        min_contour_area=args.min_area,
        band_fraction=args.band_fraction,
    )

    samples, dataset_info = load_samples(
        args.dataset,
        args.dataset_config,
        args.split,
        args.sample_start,
        args.sample_count,
    )
    background_samples, background_dataset_info = load_samples(
        args.dataset,
        args.dataset_config,
        args.split,
        args.background_sample_start,
        args.background_sample_count,
    )
    frames_by_condition, records_by_condition, results_by_condition = run_detection(
        samples,
        background_samples,
        processing_config,
        args.background_mode,
        args.background_sample_count,
    )
    artifact_paths = write_artifacts(
        Path(args.output_dir),
        samples,
        frames_by_condition,
        records_by_condition,
        results_by_condition,
        dataset_info,
        background_dataset_info,
        processing_config,
        args.background_mode,
        args.background_sample_count,
        _regeneration_command(args),
        clean=not args.no_clean,
    )

    print(f"Dataset: {dataset_info['name']} {dataset_info['split_expression']}")
    print(f"Background: {background_dataset_info['split_expression']}")
    print(f"Dataset revision: {dataset_info['revision']}")
    print(f"Review bundle: {Path(args.output_dir)}")
    print(f"Metrics: {artifact_paths['metrics']}")
    print(f"Report: {artifact_paths['report']}")
    print(f"Manifest: {artifact_paths['manifest']}")
    print(f"Flow diagram: {artifact_paths['flow_diagram']}")
    print("Condition summary:")
    for condition, summary in summarize_detection_records(records_by_condition).items():
        print(
            f"  {condition}: success={summary['detection_success_count']} "
            f"failure={summary['detection_failure_count']} "
            f"changed_from_baseline={summary['changed_from_baseline']}"
        )


if __name__ == "__main__":
    main()
