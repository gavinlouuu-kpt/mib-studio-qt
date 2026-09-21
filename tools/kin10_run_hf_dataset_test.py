#!/usr/bin/env python3
"""Prepare Hugging Face sample input and run the KIN-10 C++ test binary."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Mapping, Optional


# Dataset identity comes from env/assets.json (asset id 512x96stream-kin10) so
# the manifest is the single place that names the corpus, split and sample rows.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
from assets_manifest import get_asset  # noqa: E402

_KIN10_ASSET = get_asset("512x96stream-kin10")
DEFAULT_DATASET = _KIN10_ASSET.repo
DEFAULT_CONFIG = _KIN10_ASSET.viewer["config"]
DEFAULT_SPLIT = _KIN10_ASSET.viewer["split"]
DEFAULT_ROWS = ",".join(str(row) for row in _KIN10_ASSET.viewer["rows"])
BASE_URL = "https://datasets-server.huggingface.co"
SKIP_EXIT_CODE = 77


class RemoteServiceUnavailable(RuntimeError):
    """The remote dataset service remained transiently unavailable."""


def api_url(path: str, params: Mapping[str, object]) -> str:
    return f"{BASE_URL}{path}?{urllib.parse.urlencode(params)}"


def is_transient_remote_error(error: Exception) -> bool:
    if isinstance(error, urllib.error.HTTPError):
        return error.code == 429 or 500 <= error.code < 600
    return isinstance(error, (urllib.error.URLError, TimeoutError))


def fetch_bytes(url: str, retries: int = 5) -> bytes:
    last_error: Optional[Exception] = None
    for attempt in range(retries):
        try:
            request = urllib.request.Request(
                url,
                headers={"User-Agent": "mib-studio-qt-test-runner/1.0"},
            )
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.read()
        except Exception as exc:  # noqa: BLE001 - final error includes URL context
            last_error = exc
            time.sleep(min(10, 2**attempt))
    detail = f"failed after {retries} attempts: {url}: {last_error}"
    if last_error is not None and is_transient_remote_error(last_error):
        raise RemoteServiceUnavailable(detail) from last_error
    raise RuntimeError(detail) from last_error


def fetch_json(url: str) -> Dict[str, Any]:
    return json.loads(fetch_bytes(url).decode("utf-8"))


def parse_row_indices(raw_rows: str) -> List[int]:
    rows: List[int] = []
    for raw in raw_rows.split(","):
        raw = raw.strip()
        if not raw:
            continue
        value = int(raw)
        if value < 0:
            raise ValueError(f"row index must be non-negative: {value}")
        rows.append(value)
    if len(rows) < 3:
        raise ValueError(f"expected at least 3 row indices, got {len(rows)}")
    return rows


def resolve_row_count(
    dataset: str,
    config: str,
    split: str,
    splits_payload: Dict[str, Any],
    size_payload: Dict[str, Any],
) -> int:
    splits = splits_payload.get("splits", [])
    if not isinstance(splits, list):
        raise RuntimeError("Dataset Viewer /splits payload did not contain a splits list")

    matching_splits = [
        item
        for item in splits
        if isinstance(item, dict)
        and item.get("config") == config
        and item.get("split") == split
    ]
    if not matching_splits:
        raise RuntimeError(f"dataset split {dataset}/{config}/{split} not found")

    size = size_payload.get("size", {})
    size_splits = size.get("splits", []) if isinstance(size, dict) else []
    if not isinstance(size_splits, list):
        raise RuntimeError("Dataset Viewer /size payload did not contain a split size list")

    for item in size_splits:
        if (
            isinstance(item, dict)
            and item.get("config") == config
            and item.get("split") == split
        ):
            return int(item["num_rows"])

    raise RuntimeError(f"could not resolve row count for {dataset}/{config}/{split}")


def case_type_for(ordinal: int, row_index: int, row_count: int, selected_count: int) -> str:
    if ordinal == 0:
        return "early-stream-baseline"
    if ordinal == selected_count - 1:
        return "late-stream-regression-sentinel"
    if row_index >= row_count // 2:
        return "mid-stream-sentinel"
    return "early-stream-neighbor"


def image_payload(row: Dict[str, Any]) -> Dict[str, Any]:
    image = row.get("row", {})
    if isinstance(image, dict):
        image = image.get("image", {})
    if not isinstance(image, dict):
        raise RuntimeError("row does not contain an image payload")
    return image


def prepare_manifest(
    out_dir: pathlib.Path,
    row_indices: List[int],
    dataset: str = DEFAULT_DATASET,
    config: str = DEFAULT_CONFIG,
    split: str = DEFAULT_SPLIT,
) -> pathlib.Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "cache").mkdir(parents=True, exist_ok=True)
    (out_dir / "logs").mkdir(parents=True, exist_ok=True)

    is_valid_payload = fetch_json(api_url("/is-valid", {"dataset": dataset}))
    splits_payload = fetch_json(api_url("/splits", {"dataset": dataset}))
    size_payload = fetch_json(api_url("/size", {"dataset": dataset}))
    row_count = resolve_row_count(dataset, config, split, splits_payload, size_payload)

    for row_index in row_indices:
        if row_index >= row_count:
            raise RuntimeError(f"row index {row_index} outside dataset row count {row_count}")

    metadata = {
        "dataset": dataset,
        "config": config,
        "split": split,
        "row_indices": row_indices,
        "row_count": row_count,
        "is_valid": is_valid_payload,
        "splits": splits_payload,
        "size": size_payload,
        "generated_at_unix": int(time.time()),
    }
    (out_dir / "hf_dataset_metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    manifest_path = out_dir / "hf_sample_manifest.tsv"
    rows_path = out_dir / "hf_rows.jsonl"
    cache_dir = out_dir / "cache"

    with manifest_path.open("w", encoding="utf-8") as manifest, rows_path.open(
        "w", encoding="utf-8"
    ) as rows_out:
        manifest.write("row_idx\tpath\twidth\theight\tcase_type\n")
        for ordinal, row_index in enumerate(row_indices):
            page = fetch_json(
                api_url(
                    "/rows",
                    {
                        "dataset": dataset,
                        "config": config,
                        "split": split,
                        "offset": row_index,
                        "length": 1,
                    },
                )
            )
            rows = page.get("rows", [])
            if not isinstance(rows, list) or len(rows) != 1:
                raise RuntimeError(f"expected 1 row at offset {row_index}, got {len(rows)}")

            row = rows[0]
            if not isinstance(row, dict):
                raise RuntimeError(f"Dataset Viewer returned invalid row at offset {row_index}")

            actual_row_index = int(row["row_idx"])
            if actual_row_index != row_index:
                raise RuntimeError(
                    f"expected row {row_index}, Dataset Viewer returned {actual_row_index}"
                )

            image = image_payload(row)
            src = image.get("src")
            width = int(image.get("width", 0))
            height = int(image.get("height", 0))
            if not src or width <= 0 or height <= 0:
                raise RuntimeError(f"row {row_index} does not contain a downloadable image")

            image_path = cache_dir / f"hf_row_{row_index:05d}.jpg"
            if not image_path.exists() or image_path.stat().st_size == 0:
                image_path.write_bytes(fetch_bytes(str(src)))

            case_type = case_type_for(ordinal, row_index, row_count, len(row_indices))
            rows_out.write(
                json.dumps(
                    {
                        "row_idx": actual_row_index,
                        "src": src,
                        "width": width,
                        "height": height,
                        "case_type": case_type,
                    },
                    sort_keys=True,
                )
                + "\n"
            )
            manifest.write(
                f"{actual_row_index}\t{image_path.resolve()}\t{width}\t{height}\t{case_type}\n"
            )

    print(f"Dataset Viewer verified {dataset}/{config}/{split}: rows={row_indices}")
    print(f"Manifest: {manifest_path}")
    return manifest_path


def default_binary() -> pathlib.Path:
    if platform.system() == "Windows":
        return pathlib.Path("build") / "Debug" / "kin10_hf_dataset_pipeline_test.exe"
    return pathlib.Path("build") / "linux-backend" / "kin10_hf_dataset_pipeline_test"


def run_binary(binary: pathlib.Path, out_dir: pathlib.Path, manifest_path: pathlib.Path) -> int:
    if not binary.exists():
        print(f"KIN-10 HF dataset pipeline test binary does not exist: {binary}", file=sys.stderr)
        return 2

    command = [str(binary), str(out_dir), str(manifest_path)]
    return subprocess.run(command, check=False).returncode


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_dir_pos", nargs="?", help="Output directory")
    parser.add_argument("binary_pos", nargs="?", help="C++ test binary")
    parser.add_argument("rows_pos", nargs="?", help="Comma-separated row indices")
    parser.add_argument("--out-dir", help="Output directory")
    parser.add_argument("--binary", help="C++ test binary")
    parser.add_argument(
        "--rows",
        default=os.environ.get("KIN10_HF_ROW_INDICES"),
        help=f"Comma-separated row indices; default {DEFAULT_ROWS}",
    )
    parser.add_argument("--dataset", default=DEFAULT_DATASET)
    parser.add_argument("--config", default=DEFAULT_CONFIG)
    parser.add_argument("--split", default=DEFAULT_SPLIT)
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    out_dir = pathlib.Path(
        args.out_dir or args.out_dir_pos or "build/kin10_hf_dataset_pipeline"
    ).resolve()
    binary = pathlib.Path(args.binary or args.binary_pos or default_binary()).resolve()
    raw_rows = args.rows or args.rows_pos or DEFAULT_ROWS

    try:
        row_indices = parse_row_indices(raw_rows)
        manifest_path = prepare_manifest(
            out_dir=out_dir,
            row_indices=row_indices,
            dataset=args.dataset,
            config=args.config,
            split=args.split,
        )
    except RemoteServiceUnavailable as exc:
        print(f"skipping HF dataset integration: {exc}", file=sys.stderr)
        return SKIP_EXIT_CODE
    except Exception as exc:  # noqa: BLE001 - command-line runner should report concise context
        print(f"failed to prepare HF dataset manifest: {exc}", file=sys.stderr)
        return 1

    return run_binary(binary, out_dir, manifest_path)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
