"""
Synthetic MIB Studio HDF5 fixture for exporter tests and soak runs (issue #344).

Writes ``/valid_frames/{metadata,images,series_images}``,
``/invalid_frames/{metadata,images}`` and ``/experiment_info`` with the same
compound metadata fields the C++ ``Hdf5Service`` produces. Pixel values are a
deterministic function of the frame index so exported TIFF hashes are stable
across rounds.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Dict, Iterable, Optional

import h5py
import numpy as np

METADATA_DTYPE = np.dtype([
    ("index", "<u8"),
    ("timestampNs", "<u8"),
    ("objectId", "<i4"),
    ("objectCount", "<i4"),
    ("deformability", "<f8"),
    ("area", "<f8"),
    ("areaRatio", "<f8"),
    ("ringRatio", "<f8"),
    ("isValid", "u1"),
    ("touchesBorder", "u1"),
    ("hasSingleInnerContour", "u1"),
    ("inRange", "u1"),
    ("innerContourCount", "<i4"),
    ("brightness_q1", "<f8"),
    ("brightness_q2", "<f8"),
    ("brightness_q3", "<f8"),
    ("brightness_q4", "<f8"),
    ("youngsModulus", "<f8"),
])


def frame_pixels(index: int, height: int, width: int, offset: int = 0) -> np.ndarray:
    """Deterministic 8-bit pattern derived from the frame index."""
    yy, xx = np.mgrid[0:height, 0:width]
    return ((xx * 3 + yy * 5 + index * 7 + offset) % 251).astype(np.uint8)


def _metadata(indices: Iterable[int], valid: bool) -> np.ndarray:
    rows = []
    for i in indices:
        rows.append((
            i, (i + 1) * 1_000_000, i, 1,
            0.10 + (i % 7) * 0.05, 100.0 + i * 2.5, 0.8, 1.2,
            1 if valid else 0, 0, 1, 1 if valid else 0, 1,
            10.0 + i, 20.0 + i, 30.0 + i, 40.0 + i, 1.5 + i * 0.01,
        ))
    return np.array(rows, dtype=METADATA_DTYPE)


def write_fixture(path: Path, *, valid_frames: int = 24, invalid_frames: int = 12,
                  series_count: int = 3, height: int = 96, width: int = 128,
                  chunk_frames: int = 8) -> Path:
    """Create the fixture file and return its path."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    valid_idx = list(range(0, valid_frames * 2, 2))
    invalid_idx = list(range(1, invalid_frames * 2, 2))
    with h5py.File(path, "w") as f:
        vg = f.create_group("valid_frames")
        vg.create_dataset("metadata", data=_metadata(valid_idx, True))
        images = vg.create_dataset("images", shape=(valid_frames, height, width), dtype=np.uint8,
                                   chunks=(min(chunk_frames, max(valid_frames, 1)), height, width))
        for n, i in enumerate(valid_idx):
            images[n] = frame_pixels(i, height, width)
        if series_count > 0:
            series = vg.create_dataset("series_images", shape=(valid_frames, series_count, height, width),
                                       dtype=np.uint8,
                                       chunks=(1, series_count, height, width))
            for n, i in enumerate(valid_idx):
                for s in range(series_count):
                    series[n, s] = frame_pixels(i, height, width, offset=100 + s * 17)
        ig = f.create_group("invalid_frames")
        ig.create_dataset("metadata", data=_metadata(invalid_idx, False))
        inv_images = ig.create_dataset("images", shape=(invalid_frames, height, width), dtype=np.uint8,
                                       chunks=(min(chunk_frames, max(invalid_frames, 1)), height, width))
        for n, i in enumerate(invalid_idx):
            inv_images[n] = frame_pixels(i, height, width, offset=50)
        info = f.create_group("experiment_info")
        info.attrs["start_time_ns"] = np.uint64(1_000)
        info.attrs["end_time_ns"] = np.uint64(2_000_000)
        info.attrs["total_valid_frames"] = np.uint64(valid_frames)
        info.attrs["total_invalid_frames"] = np.uint64(invalid_frames)
    return path


def sha256_of_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def output_manifest(root: Path) -> Dict[str, str]:
    """``{relative name: sha256}`` for every file below ``root``."""
    manifest: Dict[str, str] = {}
    root = Path(root)
    if root.is_file():
        return {root.name: sha256_of_file(root)}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            manifest[str(p.relative_to(root))] = sha256_of_file(p)
    return manifest


def expected_image_count(valid_frames: int, invalid_frames: int, series_count: int,
                         frame_selection: str = "both", series_range: Optional[tuple] = None) -> int:
    total = 0
    if frame_selection in ("valid", "both"):
        total += valid_frames
        per_record = series_count
        if series_range is not None and series_count > 0:
            per_record = min(series_range[1], series_count - 1) - min(series_range[0], series_count - 1) + 1
        total += valid_frames * per_record
    if frame_selection in ("invalid", "both"):
        total += invalid_frames
    return total
