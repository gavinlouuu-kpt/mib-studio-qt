#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-review_artifacts/KIN-6}"
BINARY="${2:-build/linux-backend/kin6_batch_pipeline_evidence}"
APP_PROOF_BINARY="${3:-}"

mkdir -p "${OUT_DIR}" "${OUT_DIR}/logs"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 - "${OUT_DIR}" "${SCRIPT_DIR}/../scripts" <<'PY'
import json
import pathlib
import sys
import time
import urllib.parse
import urllib.request

out_dir = pathlib.Path(sys.argv[1])
sys.path.insert(0, sys.argv[2])
from assets_manifest import get_asset  # env/assets.json is the single source of the corpus id

_asset = get_asset("512x96stream-kin10")
dataset = _asset.repo
config = _asset.viewer["config"]
split = _asset.viewer["split"]
base_url = "https://datasets-server.huggingface.co"
page_size = 100


def api_url(path, params):
    return f"{base_url}{path}?{urllib.parse.urlencode(params)}"


def fetch_bytes(url, retries=8):
    last_error = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                return response.read()
        except Exception as exc:  # noqa: BLE001 - script reports final URL on failure
            last_error = exc
            time.sleep(min(20, 2 ** attempt))
    raise RuntimeError(f"failed after {retries} attempts: {url}: {last_error}")


def fetch_json(url):
    return json.loads(fetch_bytes(url).decode("utf-8"))


size_url = api_url("/size", {"dataset": dataset})
splits_url = api_url("/splits", {"dataset": dataset})
size_payload = fetch_json(size_url)
splits_payload = fetch_json(splits_url)

(out_dir / "hf_size.json").write_text(json.dumps(size_payload, indent=2, sort_keys=True) + "\n")
(out_dir / "hf_splits.json").write_text(json.dumps(splits_payload, indent=2, sort_keys=True) + "\n")

matching_splits = [
    item for item in splits_payload.get("splits", [])
    if item.get("config") == config and item.get("split") == split
]
if not matching_splits:
    raise SystemExit(f"dataset split {dataset}/{config}/{split} not found")

expected_rows = None
for item in size_payload.get("size", {}).get("splits", []):
    if item.get("config") == config and item.get("split") == split:
        expected_rows = int(item["num_rows"])
        break

if expected_rows != 5000:
    raise SystemExit(f"expected 5000 rows, Dataset Viewer reported {expected_rows}")

manifest_path = out_dir / "hf_image_downloads.tsv"
rows_path = out_dir / "hf_rows.jsonl"

with manifest_path.open("w", encoding="utf-8") as manifest, rows_path.open("w", encoding="utf-8") as rows_out:
    manifest.write("row_idx\tpath\twidth\theight\n")
    downloaded = 0
    for offset in range(0, expected_rows, page_size):
        rows_url = api_url(
            "/rows",
            {
                "dataset": dataset,
                "config": config,
                "split": split,
                "offset": offset,
                "length": min(page_size, expected_rows - offset),
            },
        )
        page = fetch_json(rows_url)
        rows = page.get("rows", [])
        if not rows:
            raise SystemExit(f"no rows returned at offset {offset}")
        for row in rows:
            row_idx = int(row["row_idx"])
            image = row.get("row", {}).get("image", {})
            src = image.get("src")
            width = int(image.get("width", 0))
            height = int(image.get("height", 0))
            if not src or width <= 0 or height <= 0:
                raise SystemExit(f"row {row_idx} does not contain a downloadable image")

            image_path = out_dir / f"hf_row_{row_idx:05d}.jpg"
            if not image_path.exists() or image_path.stat().st_size == 0:
                image_path.write_bytes(fetch_bytes(src))
            downloaded += 1
            rows_out.write(json.dumps({"row_idx": row_idx, "src": src, "width": width, "height": height}) + "\n")
            manifest.write(f"{row_idx}\t{image_path}\t{width}\t{height}\n")

if downloaded != expected_rows:
    raise SystemExit(f"download manifest row count mismatch: {downloaded} != {expected_rows}")

print(f"Dataset Viewer verified {dataset}/{config}/{split}: {downloaded} rows")
print(f"Manifest: {manifest_path}")
PY

if [[ ! -x "${BINARY}" ]]; then
    echo "Evidence binary is not executable: ${BINARY}" >&2
    exit 1
fi

"${BINARY}" "${OUT_DIR}" "${OUT_DIR}/hf_image_downloads.tsv"

if [[ -n "${APP_PROOF_BINARY}" ]]; then
    if [[ ! -x "${APP_PROOF_BINARY}" ]]; then
        echo "MIB app capture proof binary is not executable: ${APP_PROOF_BINARY}" >&2
        exit 1
    fi

    MOCK_FRAME_DIR="${OUT_DIR}/mib_app_mock_frames"
    rm -rf "${MOCK_FRAME_DIR}"
    mkdir -p "${MOCK_FRAME_DIR}"
    while IFS=$'\t' read -r row_idx image_path width height; do
        if [[ "${row_idx}" == "row_idx" ]]; then
            continue
        fi
        ln -s "$(realpath "${image_path}")" "${MOCK_FRAME_DIR}/hf_row_${row_idx}.jpg"
    done < "${OUT_DIR}/hf_image_downloads.tsv"

    "${APP_PROOF_BINARY}" "${MOCK_FRAME_DIR}" "${OUT_DIR}" 5000 5000
fi

cat > "${OUT_DIR}/README.md" <<EOF
# KIN-6 Review Evidence

Generated from Hugging Face dataset \`gavinlouuu/512x96stream\`, config \`default\`, split \`train\`.

## Regenerate

\`\`\`bash
cmake --preset linux-backend-only
cmake --build --preset linux-backend-only-build --target kin6_batch_pipeline_evidence kin6_mib_app_capture_proof
tools/kin6_generate_hf_evidence.sh ${OUT_DIR} ${BINARY} ${APP_PROOF_BINARY:-build/linux-backend/kin6_mib_app_capture_proof}
\`\`\`

## Files

- \`hf_input_sample.png\` - Hugging Face input frame submitted through the async batch queue.
- \`processed_mask_sample.png\` - processed mask emitted by async batch workers.
- \`contour_overlay_sample.png\` - contour overlay for reviewer inspection.
- \`metrics.json\` - 5,000-frame batch stats, capture-loop probe stats, and per-frame metrics.
- \`mib_app_input_sample.png\` - frame streamed through the mib studio \`AppBackend\`/\`CaptureService\` mock-camera runtime.
- \`mib_app_processed_mask_sample.png\` - mask emitted after the app capture callback enqueued into the async batch pipeline.
- \`mib_app_contour_overlay_sample.png\` - contour overlay from the app-level proof run.
- \`mib_app_capture_proof.json\` - app runtime counters for capture callbacks, enqueue acceptance, drops, worker batch size, and processed frames.
- \`hf_size.json\`, \`hf_splits.json\`, \`hf_rows.jsonl\`, \`hf_image_downloads.tsv\` - Dataset Viewer metadata and image manifest.
EOF
