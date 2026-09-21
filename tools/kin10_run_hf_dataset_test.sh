#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-build/linux-backend/kin10_hf_dataset_pipeline}"
BINARY="${2:-build/linux-backend/kin10_hf_dataset_pipeline_test}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_ROWS="$(python3 -c "import sys; sys.path.insert(0, '${SCRIPT_DIR}/../scripts'); from assets_manifest import get_asset; print(','.join(map(str, get_asset('512x96stream-kin10').viewer['rows'])))")"
ROW_INDICES="${3:-${KIN10_HF_ROW_INDICES:-${DEFAULT_ROWS}}}"

mkdir -p "${OUT_DIR}" "${OUT_DIR}/cache" "${OUT_DIR}/logs"

python3 - "${OUT_DIR}" "${ROW_INDICES}" "${SCRIPT_DIR}/../scripts" <<'PY'
import json
import pathlib
import sys
import time
import urllib.parse
import urllib.request

out_dir = pathlib.Path(sys.argv[1])
row_indices_arg = sys.argv[2]
sys.path.insert(0, sys.argv[3])
from assets_manifest import get_asset  # env/assets.json is the single source of the corpus id

_asset = get_asset("512x96stream-kin10")
dataset = _asset.repo
config = _asset.viewer["config"]
split = _asset.viewer["split"]
base_url = "https://datasets-server.huggingface.co"

row_indices = []
for raw in row_indices_arg.split(","):
    raw = raw.strip()
    if not raw:
        continue
    value = int(raw)
    if value < 0:
        raise SystemExit(f"row index must be non-negative: {value}")
    row_indices.append(value)

if len(row_indices) < 3:
    raise SystemExit(f"expected at least 3 row indices, got {len(row_indices)}")


def api_url(path, params):
    return f"{base_url}{path}?{urllib.parse.urlencode(params)}"


def fetch_bytes(url, retries=5):
    last_error = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                return response.read()
        except Exception as exc:  # noqa: BLE001 - final error includes URL context
            last_error = exc
            time.sleep(min(10, 2**attempt))
    raise RuntimeError(f"failed after {retries} attempts: {url}: {last_error}")


def fetch_json(url):
    return json.loads(fetch_bytes(url).decode("utf-8"))


is_valid_url = api_url("/is-valid", {"dataset": dataset})
splits_url = api_url("/splits", {"dataset": dataset})
size_url = api_url("/size", {"dataset": dataset})

is_valid_payload = fetch_json(is_valid_url)
splits_payload = fetch_json(splits_url)
size_payload = fetch_json(size_url)

matching_splits = [
    item
    for item in splits_payload.get("splits", [])
    if item.get("config") == config and item.get("split") == split
]
if not matching_splits:
    raise SystemExit(f"dataset split {dataset}/{config}/{split} not found")

expected_rows = None
for item in size_payload.get("size", {}).get("splits", []):
    if item.get("config") == config and item.get("split") == split:
        expected_rows = int(item["num_rows"])
        break

if expected_rows is None:
    raise SystemExit(f"could not resolve row count for {dataset}/{config}/{split}")

for row_index in row_indices:
    if row_index >= expected_rows:
        raise SystemExit(f"row index {row_index} outside dataset row count {expected_rows}")

metadata = {
    "dataset": dataset,
    "config": config,
    "split": split,
    "row_indices": row_indices,
    "row_count": expected_rows,
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
cache_dir.mkdir(parents=True, exist_ok=True)

with manifest_path.open("w", encoding="utf-8") as manifest, rows_path.open("w", encoding="utf-8") as rows_out:
    manifest.write("row_idx\tpath\twidth\theight\tcase_type\n")
    for ordinal, row_index in enumerate(row_indices):
        rows_url = api_url(
            "/rows",
            {
                "dataset": dataset,
                "config": config,
                "split": split,
                "offset": row_index,
                "length": 1,
            },
        )
        page = fetch_json(rows_url)
        rows = page.get("rows", [])
        if len(rows) != 1:
            raise SystemExit(f"expected 1 row at offset {row_index}, got {len(rows)}")

        row = rows[0]
        actual_row_index = int(row["row_idx"])
        if actual_row_index != row_index:
            raise SystemExit(f"expected row {row_index}, Dataset Viewer returned {actual_row_index}")

        image = row.get("row", {}).get("image", {})
        src = image.get("src")
        width = int(image.get("width", 0))
        height = int(image.get("height", 0))
        if not src or width <= 0 or height <= 0:
            raise SystemExit(f"row {row_index} does not contain a downloadable image")

        image_path = cache_dir / f"hf_row_{row_index:05d}.jpg"
        if not image_path.exists() or image_path.stat().st_size == 0:
            image_path.write_bytes(fetch_bytes(src))

        if ordinal == 0:
            case_type = "early-stream-baseline"
        elif ordinal == len(row_indices) - 1:
            case_type = "late-stream-regression-sentinel"
        elif row_index >= expected_rows // 2:
            case_type = "mid-stream-sentinel"
        else:
            case_type = "early-stream-neighbor"

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
        manifest.write(f"{actual_row_index}\t{image_path}\t{width}\t{height}\t{case_type}\n")

print(f"Dataset Viewer verified {dataset}/{config}/{split}: rows={row_indices}")
print(f"Manifest: {manifest_path}")
PY

if [[ ! -x "${BINARY}" ]]; then
    echo "KIN-10 HF dataset pipeline test binary is not executable: ${BINARY}" >&2
    exit 1
fi

"${BINARY}" "${OUT_DIR}" "${OUT_DIR}/hf_sample_manifest.tsv"
