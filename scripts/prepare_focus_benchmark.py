#!/usr/bin/env python3
"""Create a bounded, reproducible, score-independent focus benchmark from HDF5.

Writes only a NEW output directory. Sources are opened read-only. No repair,
focus/cell-quality rejection, calibration inference, or upload is performed.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess

import h5py
import numpy as np
from PIL import Image
import pyarrow as pa
import pyarrow.parquet as pq


def sha(data):
    return hashlib.sha256(data).hexdigest()


def png(image):
    stream = io.BytesIO()
    Image.fromarray(image).save(stream, format="PNG")
    return stream.getvalue()


def indices(count, limit):
    if count < 1 or limit < 1:
        return []
    return np.unique(np.linspace(0, count - 1, min(count, limit), dtype=np.int64)).tolist()


def inspect_source(path):
    with h5py.File(path, "r") as source:
        ds = source["recorded_frames/images"]
        if ds.ndim != 3 or ds.dtype != np.uint8 or min(ds.shape) < 1:
            raise ValueError(f"expected nonempty Gray8 N,H,W, got {ds.shape} {ds.dtype}")
        return ds.shape


def add_source_filter_provenance(source_root, output):
    """Read acquisition-filter counts; never invent missing empty controls."""
    manifest_path = output / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    for recording in manifest["recordings"]:
        path = source_root / recording["recording"]
        stat = path.stat()
        if (stat.st_size, stat.st_mtime_ns) != (recording["source_bytes"], recording["source_mtime_ns"]):
            raise ValueError("source changed before provenance finalization")
        with h5py.File(path, "r") as source:
            info = source.get("recording_info")
            value = info.attrs.get("total_filtered_empty_frames") if info is not None else None
            recording["source_filtered_empty_frames"] = int(value) if value is not None else None
    manifest["upstream_filter_warning"] = (
        "Some sources discarded empty frames during acquisition; retained controls are not an unbiased sample. "
        "Discarded frames cannot be reconstructed. Counts are source-reported, not independently verified.")
    manifest_path.write_text(json.dumps(manifest, indent=2)+"\n")
    appendix = ["## Upstream acquisition filtering", "", manifest["upstream_filter_warning"], "",
                "| Recording | Source-reported discarded empty frames |", "|---|---:|"]
    for recording in manifest["recordings"]:
        value = recording["source_filtered_empty_frames"]
        appendix.append(f"| {recording['recording']} | {value if value is not None else 'unknown'} |")
    card = output / "README.md"
    # Idempotent metadata finalization; preserve the primary filtering table.
    content = card.read_text().split("## Upstream acquisition filtering")[0].rstrip()
    card.write_text(content+"\n\n"+"\n".join(appendix)+"\n")
    return manifest


def create(source_root, output, sample_count=256, background_count=65):
    if sample_count < 1 or background_count < 1:
        raise ValueError("sample and background counts must be positive")
    source_root = source_root.resolve()
    output = output.resolve()
    if output == source_root or source_root in output.parents:
        raise ValueError("output must be outside the source tree")
    output.mkdir(parents=True, exist_ok=False)
    (output / "data").mkdir()
    (output / "backgrounds").mkdir()
    manifest = dict(schema_version=1, status="building", sampling="uniform linspace including endpoints",
                    sample_count_per_recording=sample_count, background_sample_count=background_count,
                    filter_policy="all focus levels; no image-content or score rejection; exact pixel duplicates within recording excluded",
                    background_policy="pixel-wise temporal median of evenly spaced non-evaluation frames; estimated, not acquisition background",
                    split_policy="one screening split; no training/validation independence claimed",
                    recordings=[], exclusions=[])
    def checkpoint():
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    features = {
        "image": {"_type": "Image"},
        "sample_id": {"dtype": "string", "_type": "Value"},
        "recording": {"dtype": "string", "_type": "Value"},
        "cell_line": {"dtype": "string", "_type": "Value"},
        "focus_setting_v": {"dtype": "int32", "_type": "Value"},
        "frame_index": {"dtype": "int64", "_type": "Value"},
        "timestamp_ns": {"dtype": "uint64", "_type": "Value"},
        "image_sha256": {"dtype": "string", "_type": "Value"},
        "background": {"dtype": "string", "_type": "Value"},
    }
    schema = pa.schema([
        ("image", pa.struct([("bytes", pa.binary()), ("path", pa.string())])),
        ("sample_id", pa.string()), ("recording", pa.string()), ("cell_line", pa.string()),
        ("focus_setting_v", pa.int32()), ("frame_index", pa.int64()), ("timestamp_ns", pa.uint64()),
        ("image_sha256", pa.string()), ("background", pa.string()),
    ], metadata={b"huggingface": json.dumps({"info": {"features": features}}).encode()})
    for path in sorted(source_root.glob("*/*.h5")):
        relative = path.relative_to(source_root).as_posix()
        if any(word in path.stem.lower() for word in ("remasked", "processed")):
            manifest["exclusions"].append(dict(recording=relative, reason="derived_file"))
            checkpoint()
            continue
        before = path.stat()
        try:
            count, height, width = inspect_source(path)
            chosen = indices(count, sample_count)
            available = np.setdiff1d(np.arange(count), chosen)
            if len(available) < 1:
                raise ValueError("no disjoint frames available to estimate background")
            bg_indices = available[indices(len(available), background_count)].tolist()
            with h5py.File(path, "r") as source:
                ds = source["recorded_frames/images"]
                background = np.median(ds[bg_indices], axis=0).astype(np.uint8)
                identifier = sha(relative.encode())[:16]
                bg_name = f"backgrounds/{identifier}.png"
                bg_bytes = png(background)
                rows, duplicates, unreadable, seen = [], [], [], set()
                for index in chosen:
                    try:
                        frame = ds[index]
                        digest = sha(frame.tobytes())
                        timestamp = int(source["recorded_frames/metadata"][index]["timestampNs"])
                    except (OSError, ValueError, KeyError, IndexError, RuntimeError) as exc:
                        unreadable.append(dict(frame_index=index, reason=str(exc)))
                        continue
                    if digest in seen:
                        duplicates.append(index)
                        continue
                    seen.add(digest)
                    rows.append(dict(image={"bytes": png(frame), "path": None},
                                     sample_id=f"{identifier}-{index:08d}", recording=relative,
                                     cell_line=path.parent.name.split("_in ")[0],
                                     focus_setting_v=int(re.match(r"\d+", path.stem)[0]),
                                     frame_index=index, timestamp_ns=timestamp, image_sha256=digest,
                                     background=bg_name))
            after = path.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ValueError("source changed during extraction")
            if not rows:
                raise ValueError("no readable unique sampled frames")
            (output / bg_name).write_bytes(bg_bytes)
            parquet_name = f"data/{identifier}.parquet"
            pq.write_table(pa.Table.from_pylist(rows, schema=schema), output / parquet_name,
                           compression="zstd", row_group_size=32)
            manifest["recordings"].append(dict(recording=relative, cell_line=rows[0]["cell_line"],
                focus_setting_v=rows[0]["focus_setting_v"], source_frames=count,
                height=height, width=width, source_bytes=before.st_size,
                source_mtime_ns=before.st_mtime_ns, sampled=len(chosen), retained=len(rows),
                duplicate_frame_indices=duplicates, unreadable_frames=unreadable,
                background=bg_name, background_frame_indices=bg_indices,
                background_png_sha256=sha(bg_bytes), parquet=parquet_name,
                parquet_sha256=sha((output / parquet_name).read_bytes())))
            print(f"{relative}: {len(rows)}/{len(chosen)} retained", flush=True)
        except (OSError, ValueError, KeyError, IndexError, RuntimeError, TypeError) as exc:
            manifest["exclusions"].append(dict(recording=relative, reason=str(exc)))
            print(f"Excluded {relative}: {exc}", flush=True)
        checkpoint()
    manifest["status"] = "complete" if manifest["recordings"] else "empty"
    manifest["total_retained"] = sum(r["retained"] for r in manifest["recordings"])
    manifest["total_source_frames"] = sum(r["source_frames"] for r in manifest["recordings"])
    checkpoint()
    table = ["| Cell line | Recordings | Source frames | Sampled | Retained | Duplicates | Read failures |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for cell_line in sorted({r["cell_line"] for r in manifest["recordings"]}):
        subset = [r for r in manifest["recordings"] if r["cell_line"] == cell_line]
        values = [cell_line, len(subset)] + [sum(r[k] for r in subset) for k in ["source_frames", "sampled", "retained"]]
        values += [sum(len(r[k]) for r in subset) for k in ["duplicate_frame_indices", "unreadable_frames"]]
        table.append("| " + " | ".join(map(str, values)) + " |")
    (output / "README.md").write_text("""---
pretty_name: MIB Cells in Different Focus — Screening Benchmark
configs:
- config_name: default
  data_files:
  - split: screening
    path: data/*.parquet
---
# MIB cells in different focus

Reproducible **screening sample**, not a clinically or scientifically validated
held-out test set. Gray8 input is preserved losslessly. All available focus
settings are retained. No ring, Laplacian, or segmentation score selects images.
Empty frames remain controls; they are not independently annotated as empty.

## Dataset filtering metrics

""" + "\n".join(table) + """

## Provenance and limitations

See `manifest.json` for exact per-recording frame/background indices, source
size/mtime, frame pixel hashes (in Parquet), artifact hashes, duplicate counts,
and file/read exclusions. Only sampled and background frames were read: this
is **not a full-file integrity scan**. Source filenames are retained; absolute
NAS paths and credentials are not published. Backgrounds are temporal-median
estimates from disjoint frames, not measured acquisition backgrounds. Voltage
comes from filenames and is not calibrated z-position or an independent focus
label. No license grant is inferred; keep this dataset private pending review.

Do not randomly divide neighboring frames into training and test splits.
No independent specimen/session IDs are available, so no train/test split or
statistical independence is claimed. Read failures leave explicit coverage gaps.
Accuracy, focus false-accept/reject rates, and calibrated measurement error need
independent labels and calibration and are unavailable in this screening run.

Native-core comparison results are added under `benchmark/` when complete.
""")
    return add_source_filter_provenance(source_root, output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples-per-recording", type=int, default=256)
    parser.add_argument("--background-frames", type=int, default=65)
    args = parser.parse_args()
    create(args.source_root, args.output, args.samples_per_recording, args.background_frames)
