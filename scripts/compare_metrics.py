#!/usr/bin/env python3
"""
Compare two gold-standard JSON metric files.

Diffs two JSON metric files (e.g. Qt pipeline output vs gold standard) with
configurable per-field tolerances and prints a summary report: frame counts,
max/mean deltas per numeric field, and per-field failure counts.

Usage:
    python scripts/compare_metrics.py gold_standard.json qt_output.json
    python scripts/compare_metrics.py qt_output.json
        (gold path from GOLD_STANDARD_JSON env or scripts/gold_standard_dataset.json)
    python scripts/compare_metrics.py gold.json qt.json --tolerance deformability 0.001
    python scripts/compare_metrics.py gold.json qt.json -o report.txt

The committed conformance reference also carries exact SHA-256 digests for
masks and multi-image series plus target/tracking metadata. Those optional
fields are enforced whenever the reference contains them, while older
metrics-only documents remain comparable.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Gold-standard frame keys that are numeric (use tolerance)
REQUIRED_NUMERIC_KEYS = [
    "deformability", "area", "area_um2", "area_ratio",
    "brightness_q1", "brightness_q2", "brightness_q3", "brightness_q4",
]

# Focus metric per processing contract (ADR 0006): Contract 1 records always
# carry ring_ratio; Contract 2 records never do and carry laplacian_variance
# (omitted when NaN, i.e. no detection).
CONTRACT_REQUIRED_NUMERIC_KEYS = {1: ["ring_ratio"], 2: []}

OPTIONAL_NUMERIC_KEYS = ["youngs_modulus", "laplacian_variance"]
NUMERIC_KEYS = REQUIRED_NUMERIC_KEYS + ["ring_ratio"] + OPTIONAL_NUMERIC_KEYS

# Keys that must match exactly (boolean or integer)
REQUIRED_EXACT_KEYS = [
    "frame_type", "index", "timestamp_ns",
    "object_id", "object_count", "is_valid", "touches_border",
    "has_single_inner_contour", "in_range", "inner_contour_count",
]

OPTIONAL_EXACT_KEYS = [
    "is_target_group", "track_id", "track_first_frame", "track_last_frame",
    "track_observation_count", "mask_sha256", "series_images_sha256",
]
EXACT_KEYS = REQUIRED_EXACT_KEYS + OPTIONAL_EXACT_KEYS

# Default absolute tolerance for numeric fields (conservative)
DEFAULT_NUMERIC_TOLERANCE = 1e-6

# Keys to compare (subset of frame that we diff)
REQUIRED_COMPARE_KEYS = set(REQUIRED_NUMERIC_KEYS + REQUIRED_EXACT_KEYS)
OPTIONAL_COMPARE_KEYS = set(OPTIONAL_NUMERIC_KEYS + OPTIONAL_EXACT_KEYS)
COMPARE_KEYS = REQUIRED_COMPARE_KEYS | OPTIONAL_COMPARE_KEYS


def get_default_gold_path() -> Optional[Path]:
    """
    Resolve default gold-standard JSON path from GOLD_STANDARD_JSON env or
    scripts/gold_standard_dataset.json. The committed file is a reference
    document; for backward compatibility a local object containing
    ``reference_json_path`` is also accepted. Paths in that config form are
    relative to repo root (parent of script dir).
    """
    env_path = os.environ.get("GOLD_STANDARD_JSON")
    if env_path:
        p = Path(env_path)
        if p.is_absolute() or p.exists():
            return p.resolve()
        script_dir = Path(__file__).resolve().parent
        repo_root = script_dir.parent
        return (repo_root / p).resolve() if (repo_root / p).exists() else p.resolve()
    config_path = Path(__file__).resolve().parent / "gold_standard_dataset.json"
    if not config_path.exists():
        return None
    try:
        with open(config_path, encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict) and isinstance(data.get("frames"), list):
            return config_path.resolve()
        ref = data.get("reference_json_path")
        if not ref:
            return None
        script_dir = Path(__file__).resolve().parent
        repo_root = script_dir.parent
        return (repo_root / ref).resolve()
    except (json.JSONDecodeError, OSError):
        return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare two gold-standard JSON metric files.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "gold",
        type=str,
        nargs="?",
        default=None,
        help="Path to gold-standard JSON (reference). If omitted, use GOLD_STANDARD_JSON or scripts/gold_standard_dataset.json.",
    )
    parser.add_argument(
        "candidate",
        type=str,
        nargs="?",
        default=None,
        help="Path to candidate JSON (e.g. Qt pipeline output). If only one positional given, it is the candidate.",
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default=None,
        help="Write report to file instead of stdout",
    )
    parser.add_argument(
        "--tolerance", "-t",
        type=str,
        nargs=2,
        action="append",
        metavar=("FIELD", "VALUE"),
        default=[],
        help="Per-field absolute tolerance for numeric fields (e.g. -t deformability 0.001). Can be repeated.",
    )
    parser.add_argument(
        "--default-tolerance",
        type=float,
        default=DEFAULT_NUMERIC_TOLERANCE,
        help=f"Default absolute tolerance for numeric fields. Default: {DEFAULT_NUMERIC_TOLERANCE}",
    )
    parser.add_argument(
        "--match-by",
        type=str,
        choices=["index", "index_and_type", "index_type_object"],
        default="index_type_object",
        help=("Match records by index only, index + frame_type, or index + "
              "frame_type + object_id. Default: index_type_object"),
    )
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def frame_key(frame: dict, match_by: str) -> Tuple[Any, ...]:
    """Return the configured stable identity for one output record."""
    if match_by == "index":
        return (frame["index"],)
    if match_by == "index_and_type":
        return (frame["index"], frame["frame_type"])
    return (frame["index"], frame["frame_type"], frame.get("object_id", -1))


def build_frame_index(frames: List[dict], match_by: str) -> Dict[Tuple[Any, ...], dict]:
    """Index records and reject ambiguous duplicate identities."""
    out: Dict[Tuple[Any, ...], dict] = {}
    for fr in frames:
        key = frame_key(fr, match_by)
        if key in out:
            raise ValueError(
                f"duplicate record identity {key!r}; use --match-by "
                "index_type_object or fix duplicate object_id values"
            )
        out[key] = fr
    return out


def required_compare_keys(contract_version: int = 1) -> set:
    """Keys every record of a document with this contract_version must carry."""
    return REQUIRED_COMPARE_KEYS | set(CONTRACT_REQUIRED_NUMERIC_KEYS.get(contract_version, []))


def compare_frames(
    gold_frame: dict,
    cand_frame: dict,
    tolerances: Dict[str, float],
    default_tol: float,
    contract_version: int = 1,
) -> Tuple[bool, Dict[str, Any]]:
    """
    Compare one gold vs one candidate frame. Return (all_match, details).
    details: per-field status and numeric deltas where applicable.
    """
    details: Dict[str, Any] = {}
    all_ok = True

    keys_to_compare = required_compare_keys(contract_version) | {
        key for key in OPTIONAL_COMPARE_KEYS if key in gold_frame
    }

    for key in sorted(keys_to_compare):
        if key not in gold_frame:
            details[key] = {"match": False, "reason": "missing_in_reference"}
            all_ok = False
            continue
        if key not in cand_frame:
            details[key] = {"match": False, "reason": "missing_in_candidate"}
            all_ok = False
            continue

        g, c = gold_frame[key], cand_frame[key]

        if key in EXACT_KEYS:
            if key == "timestamp_ns":
                # integer
                match = g == c
                details[key] = {"match": match, "gold": g, "candidate": c}
                if not match:
                    details[key]["delta"] = c - g
            else:
                match = g == c
                details[key] = {"match": match, "gold": g, "candidate": c}
            if not match:
                all_ok = False
            continue

        # Numeric
        try:
            gv, cv = float(g), float(c)
        except (TypeError, ValueError):
            details[key] = {"match": False, "reason": "not_numeric", "gold": g, "candidate": c}
            all_ok = False
            continue

        tol = tolerances.get(key, default_tol)
        delta = abs(cv - gv)
        match = delta <= tol
        details[key] = {
            "match": match,
            "gold": gv,
            "candidate": cv,
            "delta": delta,
            "tolerance": tol,
        }
        if not match:
            all_ok = False

    return all_ok, details


def run_comparison(
    gold_path: Path,
    cand_path: Path,
    tolerances: Dict[str, float],
    default_tol: float,
    match_by: str,
) -> Tuple[int, int, List[Tuple[dict, dict, bool, Dict[str, Any]]]]:
    """
    Load both files, match frames, compare. Return (matched_count, total_gold, list of (gold, cand, ok, details)).
    """
    gold_data = load_json(gold_path)
    cand_data = load_json(cand_path)

    gold_frames = gold_data.get("frames", [])
    contract_version = int(gold_data.get("contract_version", 1))
    build_frame_index(gold_frames, match_by)  # validate reference identities too
    cand_index = build_frame_index(cand_data.get("frames", []), match_by)
    used_keys = set()

    results: List[Tuple[dict, dict, bool, Dict[str, Any]]] = []
    matched_count = 0

    for gf in gold_frames:
        key = frame_key(gf, match_by)
        cf = cand_index.get(key)
        if cf is None:
            results.append((gf, {}, False, {"_reason": "no_matching_candidate_frame"}))
            continue
        matched_count += 1
        used_keys.add(key)
        ok, details = compare_frames(gf, cf, tolerances, default_tol, contract_version)
        results.append((gf, cf, ok, details))

    for key, candidate_only in cand_index.items():
        if key not in used_keys:
            results.append(({}, candidate_only, False, {"_reason": "candidate_only_record"}))

    document_details: Dict[str, Any] = {}
    for key in ("version", "contract_version", "fixture", "input_frame_count"):
        if key in gold_data and gold_data.get(key) != cand_data.get(key):
            document_details[key] = {
                "match": False,
                "gold": gold_data.get(key),
                "candidate": cand_data.get(key),
            }
    if "pixel_to_micron" in gold_data:
        gold_pixel = float(gold_data["pixel_to_micron"])
        candidate_pixel = cand_data.get("pixel_to_micron")
        if candidate_pixel is None or abs(float(candidate_pixel) - gold_pixel) > default_tol:
            document_details["pixel_to_micron"] = {
                "match": False,
                "gold": gold_pixel,
                "candidate": candidate_pixel,
            }
    if document_details:
        results.append(({}, {}, False, {"_reason": "document_metadata_mismatch", **document_details}))

    return matched_count, len(gold_frames), results


def format_report(
    gold_path: Path,
    cand_path: Path,
    matched_count: int,
    total_gold: int,
    results: List[Tuple[dict, dict, bool, Dict[str, Any]]],
    tolerances: Dict[str, float],
    default_tol: float,
) -> str:
    """Produce a human-readable summary report."""
    lines: List[str] = []
    lines.append("Gold standard: " + str(gold_path))
    lines.append("Candidate:     " + str(cand_path))
    lines.append("")
    lines.append(f"Frames in gold: {total_gold}")
    lines.append(f"Frames matched: {matched_count}")
    missing = total_gold - matched_count
    if missing:
        lines.append(f"Frames missing in candidate: {missing}")
    candidate_only = sum(1 for _g, _c, _ok, details in results
                         if details.get("_reason") == "candidate_only_record")
    if candidate_only:
        lines.append(f"Extra records in candidate: {candidate_only}")
    lines.append("")

    failed = [r for r in results if not r[2]]
    lines.append(f"Frames with differences: {len(failed)}")
    if failed:
        lines.append("")

    # Per-field failure counts and numeric stats
    field_failures: Dict[str, int] = {k: 0 for k in COMPARE_KEYS}
    field_deltas: Dict[str, List[float]] = {k: [] for k in NUMERIC_KEYS}

    for _gf, _cf, ok, details in results:
        if "_reason" in details:
            continue
        for key, d in details.items():
            if not isinstance(d, dict) or d.get("match", True):
                continue
            if key in field_failures:
                field_failures[key] += 1
            if key in field_deltas and "delta" in d:
                field_deltas[key].append(d["delta"])

    lines.append("Per-field failure count:")
    for k in sorted(COMPARE_KEYS):
        n = field_failures.get(k, 0)
        if n > 0:
            lines.append(f"  {k}: {n}")
    lines.append("")

    lines.append("Numeric fields - max delta / mean delta (where differences exist):")
    for k in NUMERIC_KEYS:
        deltas = field_deltas.get(k, [])
        if not deltas:
            continue
        lines.append(f"  {k}: max={max(deltas):.6g}, mean={sum(deltas) / len(deltas):.6g} (tolerance={tolerances.get(k, default_tol):.6g})")
    lines.append("")

    if failed and len(failed) <= 20:
        lines.append("First few differing frames (index, gold vs candidate):")
        for gf, cf, _ok, details in failed[:10]:
            idx = gf.get("index", cf.get("index", "?"))
            if "_reason" in details:
                lines.append(f"  index {idx}: {details['_reason']}")
                continue
            diffs = [k for k, d in details.items() if isinstance(d, dict) and not d.get("match", True)]
            lines.append(f"  index {idx}: diffs in {diffs}")
    elif failed:
        lines.append("(More than 20 differing frames; omit per-frame list.)")

    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    # If only one positional given, it is the candidate; gold from config/env
    if args.candidate is None and args.gold is not None:
        cand_path = Path(args.gold)
        gold_path = get_default_gold_path()
        if gold_path is None:
            print("ERROR: Gold path required. Pass gold and candidate, or set GOLD_STANDARD_JSON or scripts/gold_standard_dataset.json.", file=sys.stderr)
            return 1
        gold_path = Path(gold_path)
    else:
        gold_path = Path(args.gold) if args.gold else get_default_gold_path()
        cand_path = Path(args.candidate) if args.candidate else None
        if gold_path is None:
            print("ERROR: Gold path required. Pass gold and candidate, or set GOLD_STANDARD_JSON or scripts/gold_standard_dataset.json.", file=sys.stderr)
            return 1
        gold_path = Path(gold_path)
        if cand_path is None:
            print("ERROR: Candidate path required.", file=sys.stderr)
            return 1

    if not gold_path.exists():
        print(f"ERROR: Gold file not found: {gold_path}", file=sys.stderr)
        return 1
    if not cand_path.exists():
        print(f"ERROR: Candidate file not found: {cand_path}", file=sys.stderr)
        return 1

    tolerances: Dict[str, float] = {}
    for field, val in args.tolerance:
        if field not in NUMERIC_KEYS:
            print(f"ERROR: Unknown numeric tolerance field: {field}", file=sys.stderr)
            return 1
        try:
            tolerances[field] = float(val)
        except ValueError:
            print(f"ERROR: Invalid tolerance value for {field}: {val}", file=sys.stderr)
            return 1

    try:
        matched_count, total_gold, results = run_comparison(
            gold_path,
            cand_path,
            tolerances,
            args.default_tolerance,
            args.match_by,
        )
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        print(f"ERROR: Cannot compare documents: {exc}", file=sys.stderr)
        return 1

    report = format_report(
        gold_path,
        cand_path,
        matched_count,
        total_gold,
        results,
        tolerances,
        args.default_tolerance,
    )

    if args.output:
        Path(args.output).write_text(report, encoding="utf-8")
        print(f"Report written to {args.output}")
    else:
        print(report)

    # Exit 0 only if all frames matched and no differences
    failed = sum(1 for r in results if not r[2])
    return 0 if matched_count == total_gold and failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
