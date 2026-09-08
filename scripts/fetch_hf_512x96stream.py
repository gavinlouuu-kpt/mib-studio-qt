#!/usr/bin/env python3
"""Fetch frames from the public Hugging Face dataset gavinlouuu/512x96stream.

Downloads image.NNNN.tiff files (512x96 grayscale stream frames) into a local
folder for MockCamera playback (MIB_MOCK_CAMERA_DIR / ConnectTab "Configure
Mock...", or tests/tools/mock_pipeline_timing_run). Stdlib only; honors
HTTPS_PROXY. Existing files are skipped, so re-runs are incremental.

Usage:
    python3 scripts/fetch_hf_512x96stream.py --out data/mock_frames_512x96 \
        [--count 1000] [--start 0] [--jobs 8]
"""

import argparse
import concurrent.futures
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

BASE_URL = "https://huggingface.co/datasets/gavinlouuu/512x96stream/resolve/main"


def fetch_one(out_dir: Path, index: int, retries: int = 5) -> str:
    name = f"image.{index:04d}.tiff"
    dest = out_dir / name
    if dest.exists() and dest.stat().st_size > 0:
        return "skipped"
    url = f"{BASE_URL}/{name}"
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=60) as resp:
                data = resp.read()
            tmp = dest.with_suffix(".tmp")
            tmp.write_bytes(data)
            tmp.rename(dest)
            return "ok"
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                return "missing"
            if attempt + 1 == retries:
                return f"error: HTTP {exc.code}"
        except Exception as exc:  # noqa: BLE001 - report and retry
            if attempt + 1 == retries:
                return f"error: {exc}"
        time.sleep(1.5 * (attempt + 1))
    return "error: retries exhausted"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--count", type=int, default=1000, help="frames to fetch")
    parser.add_argument("--start", type=int, default=0, help="first frame index")
    parser.add_argument("--jobs", type=int, default=8, help="parallel downloads")
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    indices = range(args.start, args.start + args.count)
    counts = {"ok": 0, "skipped": 0, "missing": 0, "error": 0}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for i, result in enumerate(pool.map(lambda n: fetch_one(out_dir, n), indices), 1):
            key = result.split(":")[0] if result.startswith("error") else result
            counts[key] = counts.get(key, 0) + 1
            if result.startswith("error"):
                print(f"  {result}", file=sys.stderr)
            if i % 100 == 0:
                print(f"  {i}/{args.count} processed "
                      f"(ok={counts['ok']} skipped={counts['skipped']})")

    print(f"done: ok={counts['ok']} skipped={counts['skipped']} "
          f"missing={counts['missing']} errors={counts['error']} -> {out_dir}")
    return 1 if counts["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
