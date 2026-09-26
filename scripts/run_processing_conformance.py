#!/usr/bin/env python3
"""Run the installed mib-processing wheel against a deterministic fixture.

The candidate contains the portable metrics plus exact SHA-256 digests of
every mask and multi-image-series frame. It is compared with
``scripts/gold_standard_dataset.json`` by default and exits non-zero on any
drift, making this entrypoint reusable from this repository's wheel CI and
from Biowork.

Use ``--update-reference`` only when an intentional algorithm/contract change
has been reviewed. A local ``.npz`` containing an ``N x H x W`` uint8
``frames`` array or a bounded grayscale HDF5 image dataset can replace the
built-in fixture.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import tempfile
from pathlib import Path
from typing import Any, Optional, Sequence

import numpy as np

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import compare_metrics  # noqa: E402

PIXEL_TO_MICRON = 0.4886
FIXTURE_ID = "synthetic-ring-series-v1"
DEFAULT_HDF5_DATASET = "/recorded_frames/images"
DEFAULT_HDF5_FRAME_LIMIT = 3
# Per-record wheel fields that are not gold-standard metrics: object geometry,
# the channel-band flag, and the per-record contract version (the document
# carries contract_version). Dropped by name so any other new field still
# fails the strict schema.
NON_GOLD_RECORD_KEYS = ("bbox_xywh", "centroid_xy", "in_channel", "processing_contract_version")


def make_ring_frame(center_x: int, center_y: int = 40) -> np.ndarray:
    """Generate the stable nested-contour pattern used by C++/binding tests."""
    yy, xx = np.ogrid[:80, :80]
    distance_squared = (xx - center_x) ** 2 + (yy - center_y) ** 2
    image = np.zeros((80, 80), dtype=np.uint8)
    image[distance_squared <= 20 ** 2] = 255
    image[distance_squared <= 8 ** 2] = 0
    return image


def built_in_frames() -> list[np.ndarray]:
    """Two observations of one track followed by one conserved empty frame."""
    return [make_ring_frame(36), make_ring_frame(40), np.zeros((80, 80), dtype=np.uint8)]


def load_hdf5_frames(
    hdf5_path: Path,
    dataset_path: str,
    frame_offset: int,
    frame_limit: int,
) -> tuple[list[np.ndarray], str]:
    """Read a bounded N x H x W uint8 window without loading the whole file."""
    if frame_offset < 0:
        raise ValueError("HDF5 frame offset must be non-negative")
    if frame_limit <= 0:
        raise ValueError("HDF5 frame limit must be positive")

    try:
        import h5py
    except ImportError as exc:
        raise RuntimeError(
            "h5py is required for --hdf5 inputs; install env/requirements-scripts.txt"
        ) from exc

    normalized_path = f"/{dataset_path.lstrip('/')}"
    with h5py.File(hdf5_path, "r") as h5_file:
        if normalized_path not in h5_file:
            raise ValueError(
                f"{hdf5_path}: HDF5 dataset not found: {normalized_path}"
            )
        dataset = h5_file[normalized_path]
        if len(dataset.shape) != 3 or np.dtype(dataset.dtype) != np.dtype(np.uint8):
            raise ValueError(
                f"{hdf5_path}:{normalized_path} must have shape (N,H,W) and "
                f"dtype uint8; got shape={dataset.shape}, dtype={dataset.dtype}"
            )
        frame_end = min(frame_offset + frame_limit, int(dataset.shape[0]))
        if frame_offset >= frame_end:
            raise ValueError(
                f"{hdf5_path}:{normalized_path} has no frames in requested window "
                f"[{frame_offset}:{frame_offset + frame_limit}]"
            )
        values = np.asarray(dataset[frame_offset:frame_end])

    frames = [np.ascontiguousarray(frame) for frame in values]
    fixture_id = (
        f"hdf5:{hdf5_path.name}:{normalized_path}[{frame_offset}:{frame_end}]"
    )
    return frames, fixture_id


def load_frames(
    npz_path: Optional[Path],
    hdf5_path: Optional[Path],
    hdf5_dataset: str,
    frame_offset: int,
    frame_limit: int,
    fixture_id: Optional[str],
) -> tuple[list[np.ndarray], str]:
    if hdf5_path is not None:
        frames, detected_fixture_id = load_hdf5_frames(
            hdf5_path, hdf5_dataset, frame_offset, frame_limit
        )
        return frames, fixture_id or detected_fixture_id
    if npz_path is None:
        return built_in_frames(), fixture_id or FIXTURE_ID
    with np.load(npz_path, allow_pickle=False) as archive:
        if "frames" not in archive:
            raise ValueError(f"{npz_path} has no 'frames' array")
        frames = np.asarray(archive["frames"])
    if frames.ndim != 3 or frames.dtype != np.uint8:
        raise ValueError(
            f"{npz_path}: frames must have shape (N,H,W) and dtype uint8; "
            f"got shape={frames.shape}, dtype={frames.dtype}"
        )
    detected_fixture_id = f"npz:{npz_path.name}"
    return (
        [np.ascontiguousarray(frame) for frame in frames],
        fixture_id or detected_fixture_id,
    )


def conformance_config(mp: Any) -> dict[str, Any]:
    config = dict(mp.DEFAULT_PROCESSING_CONFIG)
    config.update(
        gaussian_blur_size=1,
        bg_subtract_threshold=127,
        morph_kernel_size=1,
        morph_iterations=1,
        enable_area_range_check=False,
        enable_deformability_range_check=False,
        enable_ring_ratio_check=False,
        enable_area_ratio_check=False,
        enable_border_check=True,
        require_single_inner_contour=True,
        empty_frame_pixel_threshold=1,
        enable_target_group=True,
        target_group_area_min=0,
        target_group_area_max=100000,
        target_group_deformability_min=0.0,
        target_group_deformability_max=1.0,
        multi_image_enabled=True,
        multi_image_count=3,
    )
    return config


def array_sha256(value: np.ndarray) -> str:
    """Hash dtype + shape + contiguous payload so geometry drift cannot hide."""
    array = np.ascontiguousarray(value)
    header = json.dumps(
        {"dtype": array.dtype.str, "shape": list(array.shape)},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")
    digest = hashlib.sha256()
    digest.update(header)
    digest.update(b"\0")
    digest.update(array.tobytes(order="C"))
    return digest.hexdigest()


def load_real_fixture(
    npz_path: Path,
) -> tuple[list[tuple[list[np.ndarray], np.ndarray]], dict[str, Any], float, str]:
    """Load a real-frame fixture written by build_real_conformance_fixture.py.

    Returns one (frames, background) group per recording, in file order, plus
    the recorded processing config and pixel size.
    """
    with np.load(npz_path, allow_pickle=False) as archive:
        missing = {"frames", "backgrounds", "frame_background", "config_json",
                   "pixel_to_micron"} - set(archive.files)
        if missing:
            raise ValueError(f"{npz_path}: real fixture lacks {sorted(missing)}")
        frames = np.asarray(archive["frames"])
        backgrounds = np.asarray(archive["backgrounds"])
        owner = np.asarray(archive["frame_background"])
        config = json.loads(str(archive["config_json"]))
        pixel_to_micron = float(archive["pixel_to_micron"])
    if frames.ndim != 3 or frames.dtype != np.uint8 or backgrounds.dtype != np.uint8:
        raise ValueError(f"{npz_path}: frames/backgrounds must be uint8 N x H x W")
    if owner.shape != (frames.shape[0],) or np.any(np.diff(owner) < 0):
        raise ValueError(f"{npz_path}: frame_background must map frames to backgrounds in order")
    groups = []
    for background_index in range(backgrounds.shape[0]):
        selected = [np.ascontiguousarray(f) for f in frames[owner == background_index]]
        if selected:
            groups.append((selected, np.ascontiguousarray(backgrounds[background_index])))
    return groups, config, pixel_to_micron, f"npz-real:{npz_path.name}"


def config_for_contract(config: dict[str, Any], contract: int) -> dict[str, Any]:
    """The fixture's recorded config run under ``contract``. Contract 2 takes
    the same threshold through its canonical ``difference_threshold`` key."""
    config = dict(config)
    config["processing_contract_version"] = contract
    if contract == 2 and "bg_subtract_threshold" in config:
        config["difference_threshold"] = config.pop("bg_subtract_threshold")
    return config


def build_candidate(
    frames: Sequence[np.ndarray],
    fixture_id: str,
    config: Optional[dict[str, Any]] = None,
    pixel_to_micron: float = PIXEL_TO_MICRON,
    groups: Optional[Sequence[tuple[Sequence[np.ndarray], Optional[np.ndarray]]]] = None,
) -> dict[str, Any]:
    """Run the wheel over the fixture. ``groups`` pairs frames with their own
    background (real fixtures); record indices are made global across groups."""
    import mib_processing as mp

    if config is None:
        config = conformance_config(mp)
    if groups is None:
        groups = [(list(frames), None)]
    records: list[dict[str, Any]] = []
    offset = 0
    for group_frames, background in groups:
        raw_results = mp.process_batch(
            list(group_frames),
            config,
            background=background,
            pixel_to_micron=pixel_to_micron,
            include_masks=True,
            include_series_images=True,
        )
        for raw in raw_results:
            record = {key: value for key, value in raw.items() if key not in NON_GOLD_RECORD_KEYS}
            # Contract-1 documents omit laplacian_variance (schema); it is also
            # optional and NaN (no detection) is not valid JSON.
            laplacian = record.get("laplacian_variance")
            if raw.get("processing_contract_version", 1) == 1 or (
                laplacian is not None and math.isnan(laplacian)
            ):
                record.pop("laplacian_variance", None)
            mask = record.pop("mask", None)
            series = record.pop("series_images", None)
            if mask is None or series is None:
                raise RuntimeError("wheel did not return requested mask/series payloads")
            record["mask_sha256"] = array_sha256(mask)
            record["series_images_sha256"] = [array_sha256(image) for image in series]
            record["index"] = int(record["index"]) + offset
            records.append(record)
        offset += len(group_frames)

    return {
        "version": int(mp.CONTRACT_VERSION),
        "contract_version": int(config.get("processing_contract_version", mp.CONTRACT_VERSION)),
        "wheel_version": str(mp.__version__),
        "fixture": fixture_id,
        "input_frame_count": offset,
        "pixel_to_micron": pixel_to_micron,
        "source": "mib-processing-wheel-conformance",
        "frames": records,
    }


def write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def validate_document(document: dict[str, Any]) -> None:
    """Validate with the committed JSON Schema before comparing or updating."""
    try:
        import jsonschema
    except ImportError as exc:
        raise RuntimeError(
            "jsonschema is required for conformance validation; install "
            "bindings/python[test] or 'jsonschema>=4'"
        ) from exc
    schema_path = REPO_ROOT / "docs" / "gold_standard_metrics.schema.json"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    try:
        jsonschema.validate(document, schema)
    except jsonschema.ValidationError as exc:
        location = ".".join(str(part) for part in exc.absolute_path) or "<document>"
        raise ValueError(f"candidate violates schema at {location}: {exc.message}") from exc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--reference",
        type=Path,
        default=SCRIPT_DIR / "gold_standard_dataset.json",
        help="Reference JSON. Default: scripts/gold_standard_dataset.json",
    )
    parser.add_argument("--candidate-out", type=Path, default=None)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument("--frames-npz", type=Path, default=None)
    inputs.add_argument(
        "--fixture-npz",
        type=Path,
        default=None,
        help="Real-frame fixture (frames + per-recording backgrounds + config), "
        "see build_real_conformance_fixture.py.",
    )
    parser.add_argument(
        "--processing-contract",
        type=int,
        choices=(1, 2),
        default=None,
        help="Run the --fixture-npz config under this processing contract "
        "(default: the fixture's recorded contract).",
    )
    inputs.add_argument(
        "--hdf5",
        type=Path,
        default=None,
        help="HDF5 recording containing a grayscale N x H x W image dataset.",
    )
    parser.add_argument(
        "--hdf5-dataset",
        default=DEFAULT_HDF5_DATASET,
        help=f"Image dataset path. Default: {DEFAULT_HDF5_DATASET}",
    )
    parser.add_argument(
        "--frame-offset",
        type=int,
        default=0,
        help="First HDF5 frame to process. Default: 0",
    )
    parser.add_argument(
        "--frame-limit",
        type=int,
        default=DEFAULT_HDF5_FRAME_LIMIT,
        help=f"Maximum HDF5 frames to process. Default: {DEFAULT_HDF5_FRAME_LIMIT}",
    )
    parser.add_argument(
        "--fixture-id",
        default=None,
        help="Stable provenance label stored in the candidate/reference JSON.",
    )
    parser.add_argument(
        "--update-reference",
        action="store_true",
        help="Replace --reference with current wheel output after intentional review.",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.fixture_npz is not None:
            groups, config, pixel_to_micron, fixture_id = load_real_fixture(args.fixture_npz)
            if args.processing_contract is not None:
                config = config_for_contract(config, args.processing_contract)
            candidate = build_candidate(
                [], args.fixture_id or fixture_id, config=config,
                pixel_to_micron=pixel_to_micron, groups=groups,
            )
        else:
            frames, fixture_id = load_frames(
                args.frames_npz,
                args.hdf5,
                args.hdf5_dataset,
                args.frame_offset,
                args.frame_limit,
                args.fixture_id,
            )
            candidate = build_candidate(frames, fixture_id)
        validate_document(candidate)
    except (ImportError, OSError, RuntimeError, ValueError) as exc:
        print(f"ERROR: cannot generate conformance candidate: {exc}", file=sys.stderr)
        return 2

    if args.update_reference:
        write_json(args.reference, candidate)
        print(f"Updated conformance reference: {args.reference}")
        return 0
    if not args.reference.is_file():
        print(f"ERROR: conformance reference not found: {args.reference}", file=sys.stderr)
        return 2

    temporary: Optional[tempfile.TemporaryDirectory[str]] = None
    candidate_path = args.candidate_out
    if candidate_path is None:
        temporary = tempfile.TemporaryDirectory(prefix="mib-processing-conformance-")
        candidate_path = Path(temporary.name) / "candidate.json"
    write_json(candidate_path, candidate)

    try:
        matched, total, results = compare_metrics.run_comparison(
            args.reference,
            candidate_path,
            tolerances={},
            default_tol=compare_metrics.DEFAULT_NUMERIC_TOLERANCE,
            match_by="index_type_object",
        )
        report = compare_metrics.format_report(
            args.reference,
            candidate_path,
            matched,
            total,
            results,
            tolerances={},
            default_tol=compare_metrics.DEFAULT_NUMERIC_TOLERANCE,
        )
        print(report)
        failed = sum(1 for result in results if not result[2])
        return 0 if matched == total and failed == 0 else 1
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        print(f"ERROR: cannot compare conformance output: {exc}", file=sys.stderr)
        return 2
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    sys.exit(main())
