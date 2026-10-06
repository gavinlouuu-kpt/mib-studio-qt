#!/usr/bin/env python3
"""Exit 0 when YOFO Review is configured for auto-update, 1 otherwise.

"Configured" means desktop/src-tauri/tauri.review.conf.json carries a
non-empty minisign public key at plugins.updater.pubkey. The bundle workflow
(.github/workflows/review-bundles.yml) builds signed updater artifacts only
then (and only when the TAURI_SIGNING_PRIVATE_KEY secret exists).
"""
import json
import sys
from pathlib import Path

CONF = Path(__file__).resolve().parents[2] / "desktop" / "src-tauri" / "tauri.review.conf.json"


def pubkey(conf: dict) -> str:
    return str(((conf.get("plugins") or {}).get("updater") or {}).get("pubkey") or "").strip()


if __name__ == "__main__":
    key = pubkey(json.loads(CONF.read_text(encoding="utf-8")))
    print("updater: configured" if key else "updater: no public key in tauri.review.conf.json")
    sys.exit(0 if key else 1)
