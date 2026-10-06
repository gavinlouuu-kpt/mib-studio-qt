#!/usr/bin/env python3
"""Publish a YOFO Review update to Cloudflare R2 (Tauri updater format).

YOFO Review (plan 2026-10-01-standalone-review-app, PR 6) updates through the
Tauri updater plugin. Each channel has one static manifest:

    https://updates.yofo.bio/review-<channel>/latest.json       (stable | beta)

    { "version": "1.2.0", "notes": "...", "pub_date": "2026-10-04T12:00:00Z",
      "platforms": {
        "darwin-aarch64": {"url": ".../YOFO_Review_v1.2.0_aarch64.app.tar.gz",
                           "signature": "<minisign .sig contents>", "sha256": "<hex>"},
        "windows-x86_64": {"url": ".../YOFO_Review_v1.2.0_x64-setup.exe",
                           "signature": "...", "sha256": "..."} } }

`signature` is what the plugin verifies against the public key compiled into
the app; `sha256` is the extra pin desktop/src-tauri/src/review_update.rs
checks before installing (fail closed, like MIB Studio's BE-9 updater).
Artifacts are uploaded first (immutable), the manifest last (short cache), so
a client never sees a manifest pointing at a missing file.

    python3 scripts/release/publish-review-update.py --version 1.2.0 \\
        --macos-bundle "YOFO Review.app.tar.gz" --macos-sig "YOFO Review.app.tar.gz.sig" \\
        --windows-installer "YOFO Review_1.2.0_x64-setup.exe" \\
        --windows-sig "YOFO Review_1.2.0_x64-setup.exe.sig" \\
        --notes-url https://github.com/<org>/<repo>/releases/tag/v1.2.0 [--dry-run]

Credentials: AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY (R2 token) and
MIB_STUDIO_R2_ENDPOINT, as for publish-update.py (docs/howto/auto-update-r2.md).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

DEFAULT_BUCKET = "mib-studio-qt-updates"
DEFAULT_PUBLIC_BASE_URL = "https://updates.yofo.bio"
ARTIFACT_CACHE_CONTROL = "public, max-age=31536000, immutable"
MANIFEST_CACHE_CONTROL = "public, max-age=60, must-revalidate"
VERSION = re.compile(r"^\d+\.\d+\.\d+(?:-beta\.[0-9A-Za-z][0-9A-Za-z.-]*)?$")

# Tauri updater target keys and the published file names.
PLATFORMS = {
    "darwin-aarch64": "YOFO_Review_v{version}_aarch64.app.tar.gz",
    "windows-x86_64": "YOFO_Review_v{version}_x64-setup.exe",
}


def channel_for(version: str) -> str:
    return "beta" if "-beta." in version else "stable"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_latest(
    *,
    version: str,
    channel: str,
    base_url: str,
    notes: str,
    pub_date: str,
    artifacts: dict[str, tuple[Path, str]],
) -> tuple[dict, dict[str, str]]:
    """The latest.json document and {object key: local path} to upload.

    `artifacts` maps a Tauri target key to (artifact path, signature text)."""
    if not VERSION.match(version):
        raise ValueError(f"not a release version: {version!r}")
    if channel not in ("stable", "beta"):
        raise ValueError(f"unknown channel {channel!r}")
    if not artifacts:
        raise ValueError("no platform artifacts given")
    platforms: dict[str, dict] = {}
    uploads: dict[str, str] = {}
    for target, (path, signature) in sorted(artifacts.items()):
        if target not in PLATFORMS:
            raise ValueError(f"unknown platform {target!r}")
        signature = signature.strip()
        if not signature:
            raise ValueError(f"{target}: empty signature")
        key = f"review-{channel}/{PLATFORMS[target].format(version=version)}"
        platforms[target] = {
            "url": f"{base_url.rstrip('/')}/{key}",
            "signature": signature,
            "sha256": sha256_file(path),
        }
        uploads[key] = str(path)
    latest = {"version": version, "notes": notes, "pub_date": pub_date, "platforms": platforms}
    return latest, uploads


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--version", required=True)
    p.add_argument("--channel", choices=("stable", "beta"), help="default: beta for -beta.* versions, else stable")
    p.add_argument("--macos-bundle", type=Path, help="YOFO Review.app.tar.gz (createUpdaterArtifacts)")
    p.add_argument("--macos-sig", type=Path, help="its .sig")
    p.add_argument("--windows-installer", type=Path, help="NSIS *-setup.exe")
    p.add_argument("--windows-sig", type=Path, help="its .sig")
    p.add_argument("--notes-url", default="", help="release notes URL (written as the update notes)")
    p.add_argument("--endpoint", default=os.getenv("MIB_STUDIO_R2_ENDPOINT"))
    p.add_argument("--bucket", default=DEFAULT_BUCKET)
    p.add_argument("--public-base-url", default=DEFAULT_PUBLIC_BASE_URL)
    p.add_argument("--profile", default=os.getenv("MIB_STUDIO_R2_PROFILE"))
    p.add_argument("--manifest-out", type=Path, help="also write latest.json here")
    p.add_argument("--dry-run", action="store_true", help="build and print latest.json, upload nothing")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    artifacts: dict[str, tuple[Path, str]] = {}
    for target, bundle, sig in (
        ("darwin-aarch64", args.macos_bundle, args.macos_sig),
        ("windows-x86_64", args.windows_installer, args.windows_sig),
    ):
        if bundle is None and sig is None:
            continue
        if bundle is None or sig is None or not bundle.is_file() or not sig.is_file():
            print(f"{target}: need both an existing bundle and its .sig", file=sys.stderr)
            return 2
        artifacts[target] = (bundle, sig.read_text(encoding="utf-8"))

    channel = args.channel or channel_for(args.version)
    try:
        latest, uploads = build_latest(
            version=args.version,
            channel=channel,
            base_url=args.public_base_url,
            notes=args.notes_url,
            pub_date=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            artifacts=artifacts,
        )
    except ValueError as e:
        print(f"publish-review-update: {e}", file=sys.stderr)
        return 2

    text = json.dumps(latest, indent=2) + "\n"
    if args.manifest_out:
        args.manifest_out.write_text(text, encoding="utf-8")
    print(text)
    if args.dry_run:
        for key, path in uploads.items():
            print(f"would upload {path} -> {key}")
        print(f"would upload latest.json -> review-{channel}/latest.json")
        return 0
    if not args.endpoint:
        print("MIB_STUDIO_R2_ENDPOINT (or --endpoint) is required to upload", file=sys.stderr)
        return 2

    from scripts.s3_upload import upload_file_to_s3

    for key, path in uploads.items():
        print(f"upload {path} -> {key}")
        upload_file_to_s3(endpoint=args.endpoint, bucket=args.bucket, key=key, file_path=path,
                          content_type="application/octet-stream", cache_control=ARTIFACT_CACHE_CONTROL,
                          acl=None, profile=args.profile, debug=False)
    with tempfile.TemporaryDirectory() as tmp:
        manifest = Path(tmp) / "latest.json"
        manifest.write_text(text, encoding="utf-8")
        key = f"review-{channel}/latest.json"
        print(f"upload latest.json -> {key}")
        upload_file_to_s3(endpoint=args.endpoint, bucket=args.bucket, key=key, file_path=str(manifest),
                          content_type="application/json", cache_control=MANIFEST_CACHE_CONTROL,
                          acl=None, profile=args.profile, debug=False)
    print(f"published YOFO Review {args.version} to {args.public_base_url.rstrip('/')}/review-{channel}/latest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
