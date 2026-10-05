#!/usr/bin/env python3
"""Build or publish a platform-specific Tauri application update manifest.

The Tauri shell deliberately does not consume the legacy Qt feed.  Its update
contract is scoped by channel, OS and architecture so an installer for another
desktop family can never be offered to the shell.
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
from scripts.s3_upload import upload_file_to_s3, upload_file_with_wrangler


DEFAULT_BUCKET = "mib-studio-qt-updates"
DEFAULT_PUBLIC_BASE_URL = "https://updates.yofo.bio"
ARTIFACT_CACHE_CONTROL = "public, max-age=31536000, immutable"
MANIFEST_CACHE_CONTROL = "public, max-age=60, must-revalidate"
VERSION = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?(?:\+[0-9A-Za-z][0-9A-Za-z.-]*)?$")
IDENTITY = re.compile(r"^[a-z0-9][a-z0-9._-]*$")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_identity(version: str, os_name: str, arch: str, installer: Path) -> None:
    if not VERSION.fullmatch(version):
        raise ValueError("version must be semantic X.Y.Z with optional prerelease/build metadata")
    if os_name not in {"linux", "windows"} or not IDENTITY.fullmatch(os_name):
        raise ValueError("os must be linux or windows")
    if not IDENTITY.fullmatch(arch):
        raise ValueError("arch must contain only lowercase letters, digits, '.', '_' or '-'")
    suffix = installer.suffix.lower()
    allowed = {"linux": {".deb", ".rpm"}, "windows": {".exe", ".msi"}}[os_name]
    if suffix not in allowed:
        raise ValueError(f"{os_name} Tauri updates require one of: {', '.join(sorted(allowed))}")


def public_url(base_url: str, key: str) -> str:
    return f"{base_url.rstrip('/')}/{key.lstrip('/')}"


def build_manifest(
    installer: Path,
    *,
    version: str,
    channel: str,
    os_name: str,
    arch: str,
    public_base_url: str,
    release_notes_url: str = "",
    published_at: str | None = None,
) -> tuple[str, dict[str, object]]:
    if channel not in {"stable", "beta"}:
        raise ValueError("channel must be stable or beta")
    if not installer.is_file():
        raise ValueError(f"installer does not exist: {installer}")
    if installer.stat().st_size <= 0:
        raise ValueError("installer must be non-empty")
    validate_identity(version, os_name, arch, installer)
    prefix = f"{channel}/tauri/{os_name}-{arch}"
    artifact_key = f"{prefix}/{installer.name}"
    manifest = {
        "version": version,
        "installer_url": public_url(public_base_url, artifact_key),
        "installer_sha256": sha256_file(installer),
        "installer_size_bytes": installer.stat().st_size,
        "channel": channel,
        "artifact_family": "tauri",
        "os": os_name,
        "arch": arch,
        "published_at": published_at or datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    }
    if release_notes_url:
        manifest["release_notes_url"] = release_notes_url
    return prefix, manifest


def write_json(value: dict[str, object], destination: str | None) -> Path:
    if destination:
        path = Path(destination)
        path.parent.mkdir(parents=True, exist_ok=True)
    else:
        handle = tempfile.NamedTemporaryFile("w", encoding="utf-8", prefix="mib_tauri_latest_", suffix=".json", delete=False)
        path = Path(handle.name)
        handle.close()
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    return path


def upload(args: argparse.Namespace, key: str, path: Path, content_type: str, cache_control: str) -> None:
    method = args.upload_method
    if method == "auto":
        method = "s3" if args.endpoint else "wrangler"
    if method == "s3":
        if not args.endpoint:
            raise RuntimeError("--endpoint is required for --upload-method s3")
        upload_file_to_s3(
            endpoint=args.endpoint,
            bucket=args.bucket,
            key=key,
            file_path=str(path),
            content_type=content_type,
            cache_control=cache_control,
            profile=args.profile,
            debug=args.debug,
        )
    else:
        upload_file_with_wrangler(
            bucket=args.bucket,
            key=key,
            file_path=str(path),
            content_type=content_type,
            cache_control=cache_control,
            wrangler_bin=args.wrangler_bin,
        )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installer", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--channel", default="stable", choices=("stable", "beta"))
    parser.add_argument("--os", dest="os_name", required=True, choices=("linux", "windows"))
    parser.add_argument("--arch", required=True)
    parser.add_argument("--bucket", default=DEFAULT_BUCKET)
    parser.add_argument("--public-base-url", default=DEFAULT_PUBLIC_BASE_URL)
    parser.add_argument("--endpoint", default=os.getenv("MIB_STUDIO_R2_ENDPOINT"))
    parser.add_argument("--profile", default=os.getenv("MIB_STUDIO_R2_PROFILE"))
    parser.add_argument("--upload-method", choices=("auto", "s3", "wrangler"), default="auto")
    parser.add_argument("--wrangler-bin", default=os.getenv("WRANGLER_BIN", "wrangler"))
    parser.add_argument("--release-notes-url", default="")
    parser.add_argument("--manifest-out")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--debug", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    installer = Path(args.installer)
    try:
        prefix, manifest = build_manifest(
            installer,
            version=args.version,
            channel=args.channel,
            os_name=args.os_name,
            arch=args.arch,
            public_base_url=args.public_base_url,
            release_notes_url=args.release_notes_url,
        )
        manifest_path = write_json(manifest, args.manifest_out)
        manifest_key = f"{prefix}/latest.json"
        artifact_key = f"{prefix}/{installer.name}"
        print(json.dumps({"manifest": public_url(args.public_base_url, manifest_key), "artifact": public_url(args.public_base_url, artifact_key), "manifest_path": str(manifest_path)}, indent=2))
        if args.dry_run:
            return 0
        try:
            upload(args, artifact_key, installer, "application/octet-stream", ARTIFACT_CACHE_CONTROL)
            upload(args, manifest_key, manifest_path, "application/json", MANIFEST_CACHE_CONTROL)
        finally:
            if not args.manifest_out:
                manifest_path.unlink(missing_ok=True)
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
