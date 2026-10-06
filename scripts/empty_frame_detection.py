#!/usr/bin/env python3
"""
Empty frame detection algorithm using the MIB Studio processing pipeline.

Loads the HuggingFace dataset gavinlouuu/dc_ds, applies background subtraction
and contour detection on the middle horizontal band of each image, and classifies
frames as empty (no cell) or non-empty. Evaluates against ground truth labels
where empty + debris = empty, and small + medium + large + blob = non-empty.

Usage:
    python scripts/empty_frame_detection.py
    python scripts/empty_frame_detection.py --output-dir data/empty_detection_results
    python scripts/empty_frame_detection.py --band-fraction 0.33 --threshold 12
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np
from datasets import load_dataset
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
    f1_score,
    precision_score,
    recall_score,
)

# ---------------------------------------------------------------------------
# Processing defaults (match ProcessingService / reanalyse_hdf5.py)
# ---------------------------------------------------------------------------
DEFAULT_BLUR = 3
DEFAULT_THRESHOLD = 8
DEFAULT_MORPH_KERNEL = 3
DEFAULT_MORPH_ITERATIONS = 1
MIN_NOISE_AREA = 100.0
DEFAULT_BAND_FRACTION = 0.5

# Label mapping: dataset class names → binary (0 = empty, 1 = non-empty)
EMPTY_LABELS = {"empty", "debris"}


@dataclass
class ProcessingConfig:
    gaussian_blur_size: int = DEFAULT_BLUR
    bg_subtract_threshold: int = DEFAULT_THRESHOLD
    morph_kernel_size: int = DEFAULT_MORPH_KERNEL
    morph_iterations: int = DEFAULT_MORPH_ITERATIONS
    min_contour_area: float = MIN_NOISE_AREA
    band_fraction: float = DEFAULT_BAND_FRACTION


@dataclass
class FrameResult:
    """Holds all intermediate images from the processing pipeline."""
    is_empty: bool
    contours: list
    gray: np.ndarray        # original grayscale
    diff: np.ndarray        # background-subtracted
    thresh: np.ndarray      # binary threshold (full frame)
    mask: np.ndarray        # morphology output (full frame)
    mask_band: np.ndarray   # band-cropped mask


def _to_odd(v: int) -> int:
    if v < 1:
        v = 1
    if (v % 2) == 0:
        v += 1
    return v


def ensure_grayscale(img: np.ndarray) -> np.ndarray:
    """Convert image to single-channel uint8 grayscale."""
    if img.dtype != np.uint8:
        if img.size == 0:
            return img.astype(np.uint8)
        mx = img.max()
        if mx > 255:
            img = (img.astype(np.float64) / mx * 255).astype(np.uint8)
        else:
            img = img.astype(np.uint8)
    if img.ndim == 2:
        return np.ascontiguousarray(img)
    if img.ndim == 3 and img.shape[2] == 1:
        return np.ascontiguousarray(img[:, :, 0])
    if img.ndim == 3:
        return cv2.cvtColor(img, cv2.COLOR_RGB2GRAY)
    return img


def pil_to_numpy(pil_img) -> np.ndarray:
    """Convert a PIL Image to a numpy array (RGB or grayscale)."""
    return np.array(pil_img)


def build_background(images: list[np.ndarray]) -> np.ndarray:
    """Build background by averaging a list of grayscale uint8 images."""
    if not images:
        raise ValueError("No images provided to build background")
    h, w = images[0].shape[:2]
    acc = np.zeros((h, w), dtype=np.float64)
    count = 0
    for img in images:
        gray = ensure_grayscale(img)
        if gray.shape[0] != h or gray.shape[1] != w:
            continue
        acc += gray.astype(np.float64)
        count += 1
    if count == 0:
        raise ValueError("No valid images for background (shape mismatch)")
    return np.clip(acc / count, 0, 255).astype(np.uint8)


def process_frame(
    image: np.ndarray,
    background: np.ndarray,
    config: ProcessingConfig,
) -> FrameResult:
    """
    Process a single frame through the pipeline.

    Returns a FrameResult with all intermediate images and classification.
    """
    gray = ensure_grayscale(image)
    h, w = gray.shape[:2]
    blur_k = _to_odd(config.gaussian_blur_size)
    morph_k = _to_odd(config.morph_kernel_size)

    # 1. Gaussian blur
    blurred = cv2.GaussianBlur(gray, (blur_k, blur_k), 0)
    blurred_bg = cv2.GaussianBlur(background, (blur_k, blur_k), 0)

    # 2. Background subtraction
    diff = cv2.subtract(blurred, blurred_bg)

    # 3. Binary threshold
    _, thresh = cv2.threshold(
        diff, config.bg_subtract_threshold, 255, cv2.THRESH_BINARY
    )

    # 4. Morphology: close then open
    kernel = cv2.getStructuringElement(cv2.MORPH_CROSS, (morph_k, morph_k))
    mask = cv2.morphologyEx(
        thresh, cv2.MORPH_CLOSE, kernel, iterations=config.morph_iterations
    )
    mask = cv2.morphologyEx(
        mask, cv2.MORPH_OPEN, kernel, iterations=config.morph_iterations
    )

    # 5. Crop to middle horizontal band
    band_margin = int(h * (1.0 - config.band_fraction) / 2.0)
    y_start = band_margin
    y_end = h - band_margin
    mask_band = mask[y_start:y_end, :]

    # 6. Contour detection on the band
    contours_raw, _ = cv2.findContours(
        mask_band, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE
    )

    # 7. Filter by minimum area
    contours = [c for c in contours_raw if cv2.contourArea(c) >= config.min_contour_area]

    # 8. Classify
    is_empty = len(contours) == 0

    return FrameResult(
        is_empty=is_empty,
        contours=contours,
        gray=gray,
        diff=diff,
        thresh=thresh,
        mask=mask,
        mask_band=mask_band,
    )


def save_annotated_image(
    output_path: Path,
    image: np.ndarray,
    contours: list,
    is_empty_pred: bool,
    gt_label: str,
    config: ProcessingConfig,
    index: int,
) -> None:
    """Save an annotated image with band boundaries, contours, and labels."""
    gray = ensure_grayscale(image)
    h, w = gray.shape[:2]

    # Convert to BGR for colored annotations
    vis = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)

    # Draw middle band boundaries
    band_margin = int(h * (1.0 - config.band_fraction) / 2.0)
    y_start = band_margin
    y_end = h - band_margin
    cv2.line(vis, (0, y_start), (w, y_start), (255, 255, 0), 1)  # cyan top line
    cv2.line(vis, (0, y_end), (w, y_end), (255, 255, 0), 1)  # cyan bottom line

    # Draw contours (shifted to full-image coords)
    color = (0, 0, 255) if is_empty_pred else (0, 255, 0)  # red=empty, green=non-empty
    shifted_contours = []
    for c in contours:
        shifted = c.copy()
        shifted[:, :, 1] += y_start
        shifted_contours.append(shifted)
    cv2.drawContours(vis, shifted_contours, -1, color, 1)

    # Overlay text
    pred_label = "empty" if is_empty_pred else "non-empty"
    gt_binary = "empty" if gt_label in EMPTY_LABELS else "non-empty"
    correct = pred_label == gt_binary
    status = "OK" if correct else "WRONG"

    text_lines = [
        f"GT: {gt_label} ({gt_binary})",
        f"Pred: {pred_label} | Contours: {len(contours)}",
        f"[{status}]",
    ]
    for i, line in enumerate(text_lines):
        y_pos = 20 + i * 20
        cv2.putText(vis, line, (5, y_pos), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 255), 1)

    filename = f"{index:03d}_{gt_label}_{pred_label}.png"
    cv2.imwrite(str(output_path / filename), vis)
    return output_path / filename


def save_intermediate_images(
    intermediates_dir: Path,
    result: FrameResult,
    annotated_path: Path,
    gt_label: str,
    index: int,
) -> list[Path]:
    """Save all 6 pipeline stage images for a single frame."""
    subdir = intermediates_dir / f"{index:03d}_{gt_label}"
    subdir.mkdir(parents=True, exist_ok=True)

    saved: list[Path] = []
    stages = [
        ("1_grayscale.png", result.gray),
        ("2_bg_subtracted.png", result.diff),
        ("3_threshold.png", result.thresh),
        ("4_morphology.png", result.mask),
        ("5_band_mask.png", result.mask_band),
    ]
    for name, img in stages:
        path = subdir / name
        cv2.imwrite(str(path), img)
        saved.append(path)

    # Copy the annotated image as the 6th stage
    ann_dest = subdir / "6_annotated.png"
    shutil.copy2(str(annotated_path), str(ann_dest))
    saved.append(ann_dest)

    return saved


def evaluate(
    predictions: list[int],
    ground_truth: list[int],
    label_names: list[str],
) -> dict:
    """Compute and print evaluation metrics."""
    acc = accuracy_score(ground_truth, predictions)
    prec = precision_score(ground_truth, predictions, zero_division=0)
    rec = recall_score(ground_truth, predictions, zero_division=0)
    f1 = f1_score(ground_truth, predictions, zero_division=0)

    print("\n=== Binary Classification: empty (0) vs non-empty (1) ===")
    print(classification_report(
        ground_truth, predictions,
        target_names=["empty", "non-empty"],
        zero_division=0,
    ))

    cm = confusion_matrix(ground_truth, predictions)
    print("Confusion Matrix (rows=true, cols=pred):")
    print(f"  {'':>12s} pred_empty  pred_non-empty")
    for i, name in enumerate(["true_empty", "true_non-empty"]):
        row = cm[i] if i < len(cm) else [0, 0]
        print(f"  {name:>14s}  {row[0]:>6d}  {row[1]:>6d}")

    # Per-class breakdown
    print("\n=== Per-Class Breakdown ===")
    print(f"  {'Class':<10s} {'Count':>5s} {'Pred Empty':>10s} {'Pred Non-Empty':>14s}")
    for label_name in sorted(set(label_names)):
        indices = [i for i, ln in enumerate(label_names) if ln == label_name]
        count = len(indices)
        pred_empty = sum(1 for i in indices if predictions[i] == 0)
        pred_nonempty = sum(1 for i in indices if predictions[i] == 1)
        print(f"  {label_name:<10s} {count:>5d} {pred_empty:>10d} {pred_nonempty:>14d}")

    return {
        "accuracy": acc,
        "precision": prec,
        "recall": rec,
        "f1": f1,
        "confusion_matrix": cm,
    }


def generate_summary(
    config: ProcessingConfig,
    metrics: dict,
    label_names: list[str],
    predictions: list[int],
    ground_truth: list[int],
    output_path: Path,
) -> Path:
    """Generate a markdown experiment summary."""
    cm = metrics["confusion_matrix"]
    report = classification_report(
        ground_truth, predictions,
        target_names=["empty", "non-empty"],
        zero_division=0,
    )

    # Per-class breakdown
    class_rows = []
    for label_name in sorted(set(label_names)):
        indices = [i for i, ln in enumerate(label_names) if ln == label_name]
        count = len(indices)
        pred_empty = sum(1 for i in indices if predictions[i] == 0)
        pred_nonempty = sum(1 for i in indices if predictions[i] == 1)
        class_rows.append(f"| {label_name} | {count} | {pred_empty} | {pred_nonempty} |")

    summary = f"""# Empty Frame Detection - Experiment Summary

## Date
{datetime.now().isoformat()}

## Dataset
- Source: gavinlouuu/dc_ds (HuggingFace)
- Total images: {len(label_names)}
- Classes: {', '.join(sorted(set(label_names)))}

## Method
Middle-band contour count with background subtraction

## Parameters
| Parameter | Value |
|---|---|
| gaussian_blur_size | {config.gaussian_blur_size} |
| bg_subtract_threshold | {config.bg_subtract_threshold} |
| morph_kernel_size | {config.morph_kernel_size} |
| morph_iterations | {config.morph_iterations} |
| min_contour_area | {config.min_contour_area} |
| band_fraction | {config.band_fraction} |

## Metrics
| Metric | Value |
|---|---|
| Accuracy | {metrics['accuracy']:.4f} |
| Precision | {metrics['precision']:.4f} |
| Recall | {metrics['recall']:.4f} |
| F1 Score | {metrics['f1']:.4f} |

## Confusion Matrix
|  | Pred Empty | Pred Non-Empty |
|---|---|---|
| True Empty | {cm[0][0]} | {cm[0][1]} |
| True Non-Empty | {cm[1][0]} | {cm[1][1]} |

## Per-Class Breakdown
| Class | Count | Pred Empty | Pred Non-Empty |
|---|---|---|---|
{chr(10).join(class_rows)}

## Classification Report
```
{report}
```
"""
    summary_path = output_path / "experiment_summary.md"
    summary_path.write_text(summary, encoding="utf-8")
    return summary_path


def _test_mlflow_client_artifacts(tracking_uri: str) -> bool:
    """Quick test: can the mlflow client upload artifacts without S3 credentials?"""
    import tempfile
    try:
        import mlflow
        mlflow.set_tracking_uri(tracking_uri)
        client = mlflow.MlflowClient()
        run = client.create_run("0")  # default experiment
        rid = run.info.run_id
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
            f.write("test")
            tmp = f.name
        try:
            client.log_artifact(rid, tmp)
            client.set_terminated(rid, "FINISHED")
            client.delete_run(rid)
            return True
        except Exception:
            client.set_terminated(rid, "FAILED")
            client.delete_run(rid)
            return False
        finally:
            os.unlink(tmp)
    except Exception:
        return False


def log_to_mlflow(
    config: ProcessingConfig,
    metrics: dict,
    output_dir: Path,
    mlflow_uri: str | None = None,
) -> None:
    """Log parameters, metrics, and artifacts to MLflow.

    Tests mlflow client artifact support first. If the server proxies to S3,
    uses the client. Otherwise falls back to REST API with proxy PUT.
    """
    base_url = mlflow_uri or os.environ.get("MLFLOW_TRACKING_URI", "http://100.81.210.49:5000")

    # Save confusion matrix text file
    cm_path = output_dir / "confusion_matrix.txt"
    np.savetxt(str(cm_path), metrics["confusion_matrix"], fmt="%d")

    # Test if mlflow client can handle artifacts (avoids creating a duplicate run)
    if _test_mlflow_client_artifacts(base_url):
        print("\nUsing MLflow client for logging ...")
        _log_with_mlflow_client(config, metrics, output_dir, base_url)
    else:
        print("\nMLflow client cannot upload artifacts (S3 proxy not available).")
        print("Using REST API with artifact proxy ...")
        _log_with_rest_api(config, metrics, output_dir, base_url)


def _log_with_mlflow_client(
    config: ProcessingConfig,
    metrics: dict,
    output_dir: Path,
    tracking_uri: str,
) -> None:
    """Log experiment using the mlflow Python client."""
    import mlflow

    mlflow.set_tracking_uri(tracking_uri)
    mlflow.set_experiment("empty-frame-detection")

    with mlflow.start_run(run_name="middle-band-contour-count") as run:
        # Log parameters
        mlflow.log_params({
            "gaussian_blur_size": config.gaussian_blur_size,
            "bg_subtract_threshold": config.bg_subtract_threshold,
            "morph_kernel_size": config.morph_kernel_size,
            "morph_iterations": config.morph_iterations,
            "min_contour_area": config.min_contour_area,
            "band_fraction": config.band_fraction,
            "dataset": "gavinlouuu/dc_ds",
            "method": "middle_band_contour_count",
        })

        # Log metrics
        mlflow.log_metrics({
            "accuracy": metrics["accuracy"],
            "precision": metrics["precision"],
            "recall": metrics["recall"],
            "f1": metrics["f1"],
        })

        # Log top-level artifacts (summary, confusion matrix)
        for f in output_dir.iterdir():
            if f.is_file() and f.suffix in (".md", ".txt"):
                mlflow.log_artifact(str(f))

        # Log annotated images
        annotated_files = sorted(output_dir.glob("*.png"))
        if annotated_files:
            print(f"\nUploading {len(annotated_files)} annotated images ...")
            for i, img_path in enumerate(annotated_files):
                mlflow.log_artifact(str(img_path), artifact_path="annotated_images")
                if (i + 1) % 20 == 0 or (i + 1) == len(annotated_files):
                    print(f"  {i + 1}/{len(annotated_files)}")

        # Log intermediate images
        intermediates_dir = output_dir / "intermediates"
        if intermediates_dir.is_dir():
            frame_dirs = sorted(d for d in intermediates_dir.iterdir() if d.is_dir())
            total = sum(len(list(d.glob("*.png"))) for d in frame_dirs)
            print(f"Uploading {total} intermediate images ...")
            uploaded = 0
            for frame_dir in frame_dirs:
                for img_path in sorted(frame_dir.glob("*.png")):
                    mlflow.log_artifact(
                        str(img_path),
                        artifact_path=f"intermediates/{frame_dir.name}",
                    )
                    uploaded += 1
                    if uploaded % 50 == 0 or uploaded == total:
                        print(f"  {uploaded}/{total}")

        run_url = f"{tracking_uri}/#/experiments/{run.info.experiment_id}/runs/{run.info.run_id}"
        print(f"\nMLflow run logged: {run_url}")

        # Verify artifacts
        client = mlflow.MlflowClient()
        arts = client.list_artifacts(run.info.run_id)
        if arts:
            print(f"Artifact verification: {len(arts)} top-level entries visible")
            for a in arts:
                print(f"  {'[dir]' if a.is_dir else '[file]'} {a.path}")
        else:
            print("WARNING: No artifacts visible in MLflow. Server may need --serve-artifacts flag.")


def _log_with_rest_api(
    config: ProcessingConfig,
    metrics: dict,
    output_dir: Path,
    base_url: str,
) -> None:
    """Fallback: log experiment using raw REST API."""
    import time

    import requests

    def api(method, endpoint, **kwargs):
        url = f"{base_url}/api/2.0/mlflow/{endpoint}"
        resp = getattr(requests, method)(url, **kwargs)
        resp.raise_for_status()
        return resp.json()

    def upload_artifact(
        experiment_id: str, run_id: str, local_path: Path, subdir: str = "",
    ) -> None:
        """Upload via artifact proxy. Path must match s3://mlflow/{exp}/{run}/artifacts/..."""
        artifact_path = f"{subdir}/{local_path.name}" if subdir else local_path.name
        url = (
            f"{base_url}/api/2.0/mlflow-artifacts/artifacts/"
            f"{experiment_id}/{run_id}/artifacts/{artifact_path}"
        )
        with open(local_path, "rb") as f:
            resp = requests.put(
                url, data=f,
                headers={"Content-Type": "application/octet-stream"},
            )
            resp.raise_for_status()

    # Get or create experiment
    try:
        result = api("get", "experiments/get-by-name", params={"experiment_name": "empty-frame-detection"})
        experiment_id = result["experiment"]["experiment_id"]
    except requests.HTTPError:
        result = api("post", "experiments/create", json={"name": "empty-frame-detection"})
        experiment_id = result["experiment_id"]

    # Create run
    run_data = api("post", "runs/create", json={
        "experiment_id": experiment_id,
        "run_name": "middle-band-contour-count",
        "start_time": int(time.time() * 1000),
    })
    run_id = run_data["run"]["info"]["run_id"]

    # Log parameters and metrics
    ts = int(time.time() * 1000)
    api("post", "runs/log-batch", json={
        "run_id": run_id,
        "params": [
            {"key": "gaussian_blur_size", "value": str(config.gaussian_blur_size)},
            {"key": "bg_subtract_threshold", "value": str(config.bg_subtract_threshold)},
            {"key": "morph_kernel_size", "value": str(config.morph_kernel_size)},
            {"key": "morph_iterations", "value": str(config.morph_iterations)},
            {"key": "min_contour_area", "value": str(config.min_contour_area)},
            {"key": "band_fraction", "value": str(config.band_fraction)},
            {"key": "dataset", "value": "gavinlouuu/dc_ds"},
            {"key": "method", "value": "middle_band_contour_count"},
        ],
        "metrics": [
            {"key": "accuracy", "value": metrics["accuracy"], "timestamp": ts, "step": 0},
            {"key": "precision", "value": metrics["precision"], "timestamp": ts, "step": 0},
            {"key": "recall", "value": metrics["recall"], "timestamp": ts, "step": 0},
            {"key": "f1", "value": metrics["f1"], "timestamp": ts, "step": 0},
        ],
    })

    # Upload artifacts
    print("\nUploading artifacts to MLflow (REST) ...")

    # Top-level files
    for f in output_dir.iterdir():
        if f.is_file() and f.suffix in (".md", ".txt"):
            upload_artifact(experiment_id, run_id, f)

    # Annotated images
    annotated_files = sorted(output_dir.glob("*.png"))
    for i, img_path in enumerate(annotated_files):
        upload_artifact(experiment_id, run_id, img_path, subdir="annotated_images")
        if (i + 1) % 20 == 0 or (i + 1) == len(annotated_files):
            print(f"  Annotated: {i + 1}/{len(annotated_files)}")

    # Intermediate images
    intermediates_dir = output_dir / "intermediates"
    if intermediates_dir.is_dir():
        frame_dirs = sorted(d for d in intermediates_dir.iterdir() if d.is_dir())
        total = sum(len(list(d.glob("*.png"))) for d in frame_dirs)
        uploaded = 0
        for frame_dir in frame_dirs:
            for img_path in sorted(frame_dir.glob("*.png")):
                upload_artifact(experiment_id, run_id, img_path, subdir=f"intermediates/{frame_dir.name}")
                uploaded += 1
                if uploaded % 50 == 0 or uploaded == total:
                    print(f"  Intermediates: {uploaded}/{total}")

    # End run
    api("post", "runs/update", json={
        "run_id": run_id,
        "status": "FINISHED",
        "end_time": int(time.time() * 1000),
    })

    run_url = f"{base_url}/#/experiments/{experiment_id}/runs/{run_id}"
    print(f"\nMLflow run logged: {run_url}")

    # Verify
    resp = requests.get(
        f"{base_url}/api/2.0/mlflow/artifacts/list",
        params={"run_id": run_id},
    )
    if resp.status_code == 200:
        data = resp.json()
        files = data.get("files", [])
        dirs = data.get("root_uri", "")
        print(f"Artifact verification: root_uri={dirs}, {len(files)} entries")
    else:
        print(f"WARNING: Could not verify artifacts (HTTP {resp.status_code})")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Empty frame detection on gavinlouuu/dc_ds dataset.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--output-dir", "-o",
        type=str,
        default="data/empty_detection_results",
        help="Directory for annotated images and results. Default: data/empty_detection_results",
    )
    parser.add_argument("--blur", type=int, default=DEFAULT_BLUR, help=f"Gaussian blur kernel size. Default: {DEFAULT_BLUR}")
    parser.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD, help=f"Binary threshold. Default: {DEFAULT_THRESHOLD}")
    parser.add_argument("--morph-kernel", type=int, default=DEFAULT_MORPH_KERNEL, help=f"Morphology kernel size. Default: {DEFAULT_MORPH_KERNEL}")
    parser.add_argument("--morph-iterations", type=int, default=DEFAULT_MORPH_ITERATIONS, help=f"Morphology iterations. Default: {DEFAULT_MORPH_ITERATIONS}")
    parser.add_argument("--min-area", type=float, default=MIN_NOISE_AREA, help=f"Min contour area filter. Default: {MIN_NOISE_AREA}")
    parser.add_argument("--band-fraction", type=float, default=DEFAULT_BAND_FRACTION, help=f"Middle band height fraction. Default: {DEFAULT_BAND_FRACTION}")
    parser.add_argument("--no-mlflow", action="store_true", help="Skip MLflow logging.")
    parser.add_argument("--mlflow-uri", type=str, default=None, help="MLflow tracking URI. Default: http://100.81.210.49:5000")
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    config = ProcessingConfig(
        gaussian_blur_size=args.blur,
        bg_subtract_threshold=args.threshold,
        morph_kernel_size=args.morph_kernel,
        morph_iterations=args.morph_iterations,
        min_contour_area=args.min_area,
        band_fraction=args.band_fraction,
    )

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # --- MLflow flag ---
    use_mlflow = not args.no_mlflow

    # --- Load dataset ---
    # Corpus id, split and pinned revision live in env/assets.json (asset dc-ds).
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from assets_manifest import get_asset

    dc_ds = get_asset("dc-ds")
    print(f"Loading dataset {dc_ds.repo}@{dc_ds.revision[:12]} ...")
    dataset = load_dataset(dc_ds.repo, split=dc_ds.hf_datasets["split"], revision=dc_ds.revision)
    label_feature = dataset.features["label"]
    label_id_to_name = {i: name for i, name in enumerate(label_feature.names)}

    print(f"  {len(dataset)} images, classes: {label_feature.names}")

    # --- Separate images by label ---
    images_by_label: dict[str, list[np.ndarray]] = {}
    all_images: list[np.ndarray] = []
    all_labels: list[str] = []

    for sample in dataset:
        img = pil_to_numpy(sample["image"])
        label_name = label_id_to_name[sample["label"]]
        all_images.append(img)
        all_labels.append(label_name)
        images_by_label.setdefault(label_name, []).append(img)

    print("  Class distribution:")
    for name in label_feature.names:
        count = len(images_by_label.get(name, []))
        print(f"    {name}: {count}")

    # --- Build background from empty frames ---
    empty_frames = [ensure_grayscale(img) for img in images_by_label.get("empty", [])]
    if empty_frames:
        print(f"\nBuilding background from {len(empty_frames)} empty frames ...")
        background = build_background(empty_frames)
    else:
        print("\nNo 'empty' frames found, building background from all frames ...")
        background = build_background([ensure_grayscale(img) for img in all_images])

    # --- Process each frame ---
    print(f"\nProcessing {len(all_images)} frames ...")
    print(f"  Config: blur={config.gaussian_blur_size}, threshold={config.bg_subtract_threshold}, "
          f"morph={config.morph_kernel_size}x{config.morph_iterations}, "
          f"min_area={config.min_contour_area}, band={config.band_fraction}")

    intermediates_dir = output_dir / "intermediates"
    intermediates_dir.mkdir(parents=True, exist_ok=True)

    predictions: list[int] = []
    ground_truth: list[int] = []

    for i, (img, label_name) in enumerate(zip(all_images, all_labels)):
        result = process_frame(img, background, config)

        pred = 0 if result.is_empty else 1  # 0=empty, 1=non-empty
        gt = 0 if label_name in EMPTY_LABELS else 1

        predictions.append(pred)
        ground_truth.append(gt)

        # Save annotated image
        ann_path = save_annotated_image(
            output_dir, img, result.contours, result.is_empty, label_name, config, i,
        )

        # Save intermediate pipeline images
        save_intermediate_images(intermediates_dir, result, ann_path, label_name, i)

    # --- Evaluate ---
    metrics = evaluate(predictions, ground_truth, all_labels)

    print(f"\nAnnotated images saved to: {output_dir}")

    # --- Generate experiment summary ---
    summary_path = generate_summary(config, metrics, all_labels, predictions, ground_truth, output_dir)
    print(f"Experiment summary: {summary_path}")

    # --- MLflow ---
    if use_mlflow:
        log_to_mlflow(config, metrics, output_dir, mlflow_uri=args.mlflow_uri)


if __name__ == "__main__":
    main()
