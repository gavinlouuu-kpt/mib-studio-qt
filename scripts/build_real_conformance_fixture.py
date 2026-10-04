#!/usr/bin/env python3
"""Build the real-frame conformance fixture (Contract 2 rollout T0.3).

Selects a small, deterministic set of real 1184x240 MIB frames from the
cells-in-different-focus experiment at the nominal 50 V focus, one
recording per cell line, and writes them with each recording's background and
Contract-1 processing config to a compressed .npz consumed by
``run_processing_conformance.py --fixture-npz``.

Per recording it samples 50-frame blocks every 1500 frames, classifies each
sampled frame with the current wheel under Contract 1, and keeps:

* two frames with valid cells, preferring frames that also hold invalid
  objects (area/border rejects) and more objects;
* one frame whose objects are all invalid, when the sample has one;
* one empty frame.

Backgrounds and configs come from the dataset's provenance files, pinned as
asset ``cells-different-focus-50v-provenance`` in env/assets.json. The raw
frames come from the NAS recordings.

The fixture is a frozen input. Rebuild it only in a PR labelled
``gold-reference-change``; the gold references that use it must be
regenerated in the same PR.

Usage (on the machine holding the raw recordings):

    python scripts/build_real_conformance_fixture.py \\
        --raw-root /mnt/hdd/developer-data/mib-cells-different-focus/raw \\
        --provenance-root /mnt/hdd/shared/exports/huggingface/mib-cells-different-focus/provenance/recordings \\
        --out scripts/conformance/focus-50v-real.npz
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

# (cell line, provenance stem, raw recording path relative to --raw-root)
RECORDINGS = (
    ("C2C12", "50v", "C2C12_in different focus/50v.h5"),
    ("HEK293", "50v_in focus", "HEK293_in different focus/50v_in focus.h5"),
    ("HeLa", "50v", "Hela_in different focus/50v.h5"),
    ("PANC-1", "50_in focus", "PANC-1/50_in focus.h5"),
)
SAMPLE_STRIDE = 1500
SAMPLE_BLOCK = 50
IMAGE_DATASET = "recorded_frames/images"


def classify(mp, frame, background, config, pixel_to_micron):
    height, width = frame.shape
    objects = mp.compute_processed_objects(
        frame, background, config, (0, 0, width, height), 0, 0, pixel_to_micron, False
    )
    count = int(objects[0]["object_count"])
    valid = sum(1 for o in objects if o["is_valid"])
    return count, valid


def select_frames(mp, images, background, config, pixel_to_micron):
    with_valid, all_invalid, empty = [], [], []
    for start in range(0, images.shape[0], SAMPLE_STRIDE):
        block = np.asarray(images[start:start + SAMPLE_BLOCK])
        for offset, frame in enumerate(block):
            index = start + offset
            count, valid = classify(mp, frame, background, config, pixel_to_micron)
            if count == 0:
                empty.append(index)
            elif valid == 0:
                all_invalid.append(index)
            else:
                with_valid.append((-(count - valid), -count, index))
    picks = [index for _, _, index in sorted(with_valid)[:2]]
    picks += all_invalid[:1]
    picks += empty[:1]
    return picks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--raw-root", type=Path, required=True)
    parser.add_argument("--provenance-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    import cv2
    import h5py
    import mib_processing as mp

    frames, frame_background, backgrounds, provenance = [], [], [], []
    config = None
    pixel_to_micron = None
    for cell_line, stem, relative in RECORDINGS:
        recording_dir = args.provenance_root / cell_line / stem
        results = json.loads((recording_dir / "mib_processing_results.json").read_text())
        recording_config = dict(mp.DEFAULT_PROCESSING_CONFIG)
        recording_config.update(results["processing_config"])
        recording_config["processing_contract_version"] = 1
        if config is None:
            config, pixel_to_micron = recording_config, float(results["pixel_to_micron"])
        elif recording_config != config or float(results["pixel_to_micron"]) != pixel_to_micron:
            raise SystemExit(f"{cell_line}/{stem}: config differs from the first recording")
        background = cv2.imread(str(recording_dir / "contract2_background.png"), cv2.IMREAD_GRAYSCALE)
        if background is None:
            raise SystemExit(f"{cell_line}/{stem}: background image missing")
        with h5py.File(args.raw_root / relative, "r", rdcc_nbytes=64 * 1024 ** 2) as h5_file:
            images = h5_file[IMAGE_DATASET]
            picks = select_frames(mp, images, background, config, pixel_to_micron)
            for index in picks:
                frames.append(np.asarray(images[index]))
                frame_background.append(len(backgrounds))
        backgrounds.append(background)
        provenance.append({
            "cell_line": cell_line,
            "recording": relative,
            "frame_indices": picks,
            "background": f"{cell_line}/{stem}/contract2_background.png "
                          "(mean of up to 128 YOLO-negative frames)",
        })
        print(f"{cell_line}: frames {picks}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        args.out,
        frames=np.stack(frames).astype(np.uint8),
        backgrounds=np.stack(backgrounds).astype(np.uint8),
        frame_background=np.asarray(frame_background, dtype=np.int16),
        config_json=np.asarray(json.dumps(config, sort_keys=True)),
        pixel_to_micron=np.asarray(pixel_to_micron, dtype=np.float64),
        provenance_json=np.asarray(json.dumps({
            "asset": "cells-different-focus-50v-provenance (env/assets.json)",
            "focus_voltage_v": 50,
            "selection": f"stride {SAMPLE_STRIDE}, block {SAMPLE_BLOCK}, classified under Contract 1",
            "recordings": provenance,
        }, sort_keys=True)),
    )
    print(f"wrote {args.out} ({args.out.stat().st_size / 1e6:.2f} MB, {len(frames)} frames)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
