#!/usr/bin/env python3
"""Strictly compare an Ultra96 direct-DDR run with the MIB reference."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Any


def sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def equivalent(left: Any, right: Any, path: str, differences: list[str]) -> None:
    if isinstance(left, bool) or isinstance(right, bool):
        if left is not right:
            differences.append(f"{path}: {left!r} != {right!r}")
        return
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        if not math.isclose(float(left), float(right), rel_tol=1e-12, abs_tol=1e-12):
            differences.append(f"{path}: {left!r} != {right!r}")
        return
    if type(left) is not type(right):
        differences.append(
            f"{path}: type {type(left).__name__} != {type(right).__name__}"
        )
        return
    if isinstance(left, dict):
        if left.keys() != right.keys():
            differences.append(
                f"{path}: keys {sorted(left)} != {sorted(right)}"
            )
            return
        for key in left:
            equivalent(left[key], right[key], f"{path}.{key}", differences)
        return
    if isinstance(left, list):
        if len(left) != len(right):
            differences.append(f"{path}: length {len(left)} != {len(right)}")
            return
        for index, (left_item, right_item) in enumerate(zip(left, right, strict=True)):
            equivalent(left_item, right_item, f"{path}[{index}]", differences)
        return
    if left != right:
        differences.append(f"{path}: {left!r} != {right!r}")


def normalized_frames(document: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        {
            "frame_index": frame["frame_index"],
            "white_pixels": frame["white_pixels"],
            "contours": frame["contours"],
        }
        for frame in document["frames"]
    ]


def normalized_records(document: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        {
            "frame_index": record["frame_index"],
            "validation": record["validation"],
        }
        for record in document["records"]
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-masks", type=Path, required=True)
    parser.add_argument("--reference-json", type=Path, required=True)
    parser.add_argument("--board-masks", type=Path, required=True)
    parser.add_argument("--board-json", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    reference_masks = args.reference_masks.read_bytes()
    board_masks = args.board_masks.read_bytes()
    reference = json.loads(args.reference_json.read_text())
    board = json.loads(args.board_json.read_text())

    mask_mismatch_count = sum(
        left != right for left, right in zip(reference_masks, board_masks)
    ) + abs(len(reference_masks) - len(board_masks))

    outline_differences: list[str] = []
    equivalent(
        normalized_frames(reference),
        normalized_frames(board),
        "frames",
        outline_differences,
    )
    record_differences: list[str] = []
    equivalent(
        normalized_records(reference),
        normalized_records(board),
        "records",
        record_differences,
    )
    summary_differences: list[str] = []
    equivalent(reference["summary"], board["summary"], "summary", summary_differences)

    expected_config = {
        "frame_count": 171,
        "geometry": [512, 96],
        "background_enabled": True,
        "threshold": 8,
        "pixels_per_clock": 4,
        "pl_clock_hz": 250_000_000,
        "transport": "XRT CMA buffer objects over S_AXI_HP0_FPD",
    }
    config_differences: list[str] = []
    for key, expected in expected_config.items():
        if board.get(key) != expected:
            config_differences.append(
                f"{key}: {board.get(key)!r} != {expected!r}"
            )

    exact = not any(
        (
            mask_mismatch_count,
            outline_differences,
            record_differences,
            summary_differences,
            config_differences,
        )
    )
    result = {
        "exact": exact,
        "mask_bytes": len(board_masks),
        "mask_mismatch_pixels": mask_mismatch_count,
        "reference_mask_sha256": sha256(reference_masks),
        "board_mask_sha256": sha256(board_masks),
        "exact_outline_match": not outline_differences,
        "outline_differences": outline_differences[:100],
        "equivalent_records": not record_differences,
        "record_differences": record_differences[:100],
        "summary_differences": summary_differences,
        "config_differences": config_differences,
        "board_summary": board.get("summary"),
        "board_timing": {
            "correctness_pass_timing_us": board.get(
                "correctness_pass_timing_us"
            ),
            "steady_state": board.get("steady_state"),
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")

    if exact:
        print(
            "MIB_DIRECT_DDR_EXACT "
            f"frames={board['frame_count']} mask_pixels={len(board_masks)} "
            f"contours={board['summary']['total_contours']} "
            f"records={board['summary']['batch_records']}"
        )
        return 0
    print("MIB_DIRECT_DDR_MISMATCH")
    print(json.dumps(result, indent=2))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
