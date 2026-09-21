#!/usr/bin/env python3
"""Publish the Young's modulus LUT and manifest to Cloudflare R2."""
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

from scripts.s3_upload import upload_file_to_s3, upload_file_with_wrangler


DEFAULT_BUCKET = "mib-studio-qt-updates"
DEFAULT_PUBLIC_BASE_URL = "https://updates.yofo.bio"
ARTIFACT_CACHE_CONTROL = "public, max-age=31536000, immutable"
MANIFEST_CACHE_CONTROL = "public, max-age=60, must-revalidate"
DEFAULT_LUT_NAME = "scaled_isoelastic_data_LUT_6.16-4.24.txt"


def join_public_object_url(base_url: str, key: str) -> str:
    return f"{base_url.rstrip('/')}/{key.lstrip('/')}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def humanize_lut_name(lut_id: str) -> str:
    return lut_id.replace("_", " ").strip().title()


def write_manifest_file(manifest: dict[str, object], manifest_out: str | None) -> Path:
    if manifest_out:
        path = Path(manifest_out)
        path.parent.mkdir(parents=True, exist_ok=True)
    else:
        handle = tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            prefix="emodulus_lut_latest_",
            suffix=".json",
            delete=False,
        )
        path = Path(handle.name)
        handle.close()

    path.write_text(json.dumps(manifest, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    return path


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lut", default=str(Path("resources") / "isoelastic_curve" / DEFAULT_LUT_NAME))
    parser.add_argument("--lut-id", default=None, help="Stable LUT identifier (defaults to file stem)")
    parser.add_argument("--display-name", default=None, help="Human-friendly LUT name (defaults to a title-cased LUT id)")
    parser.add_argument("--revision", required=True, help="Remote revision label, e.g. 2026.06.11-1")
    parser.add_argument("--endpoint", default=os.getenv("MIB_STUDIO_R2_ENDPOINT"))
    parser.add_argument("--bucket", default=DEFAULT_BUCKET)
    parser.add_argument("--public-base-url", default=DEFAULT_PUBLIC_BASE_URL)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--profile", default=os.getenv("MIB_STUDIO_R2_PROFILE"))
    parser.add_argument("--acl", default="")
    parser.add_argument("--app-min-version", default="")
    parser.add_argument("--app-max-version", default="")
    parser.add_argument("--manifest-out", default=None, help="Write generated manifest to this path")
    parser.add_argument("--dry-run", action="store_true", help="Generate metadata but do not upload")
    parser.add_argument(
        "--upload-method",
        choices=("auto", "s3", "wrangler"),
        default="auto",
        help="Upload through S3 credentials or the authenticated Wrangler CLI",
    )
    parser.add_argument("--wrangler-bin", default=os.getenv("WRANGLER_BIN", "wrangler"))
    parser.add_argument("--debug", action="store_true")
    return parser


def resolve_upload_method(upload_method: str, endpoint: str | None) -> str:
    if upload_method == "auto":
        return "s3" if endpoint else "wrangler"
    return upload_method


def upload_object(
    *,
    args: argparse.Namespace,
    key: str,
    file_path: Path,
    content_type: str,
    cache_control: str,
) -> None:
    method = resolve_upload_method(args.upload_method, args.endpoint)
    if method == "s3":
        if not args.endpoint:
            raise RuntimeError("R2 S3 API endpoint is required for --upload-method s3")
        upload_file_to_s3(
            endpoint=args.endpoint,
            bucket=args.bucket,
            key=key,
            file_path=str(file_path),
            content_type=content_type,
            cache_control=cache_control,
            acl=args.acl or None,
            profile=args.profile,
            debug=args.debug,
        )
        return

    if args.acl:
        print("   Note: --acl is ignored for Wrangler uploads; R2 public access is configured on the bucket/domain.")
    upload_file_with_wrangler(
        bucket=args.bucket,
        key=key,
        file_path=str(file_path),
        content_type=content_type,
        cache_control=cache_control,
        wrangler_bin=args.wrangler_bin,
    )


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    lut_path = Path(args.lut)

    print("=== Publishing Young's modulus LUT ===")

    if not lut_path.is_file():
        print(f"ERROR: LUT file does not exist: {lut_path}", file=sys.stderr)
        return 1

    lut_id = args.lut_id or lut_path.stem
    display_name = args.display_name or humanize_lut_name(lut_id)

    size_bytes = lut_path.stat().st_size
    if size_bytes <= 0:
        print(f"ERROR: LUT file size is invalid: {size_bytes}", file=sys.stderr)
        return 1

    print("\n1. Computing SHA-256 hash...")
    digest = sha256_file(lut_path)
    print(f"   Hash: {digest}")
    print(f"   Size: {size_bytes} bytes")

    lut_key = f"{args.channel}/emodulus-lut/{lut_path.name}"
    manifest_key = f"{args.channel}/emodulus-lut/latest.json"
    lut_url = join_public_object_url(args.public_base_url, lut_key)
    manifest_url = join_public_object_url(args.public_base_url, manifest_key)

    print("\n2. Generating manifest...")
    manifest: dict[str, object] = {
        "manifest_schema_version": 1,
        "lut_id": lut_id,
        "display_name": display_name,
        "revision": args.revision,
        "download_url": lut_url,
        "sha256": digest,
        "size_bytes": size_bytes,
        "published_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    }
    if args.app_min_version:
        manifest["app_min_version"] = args.app_min_version
    if args.app_max_version:
        manifest["app_max_version"] = args.app_max_version

    manifest_path = write_manifest_file(manifest, args.manifest_out)
    print(f"   Manifest created: {manifest_path}")

    if args.dry_run:
        print("\nDRY RUN: skipped R2 uploads")
        print("\n=== Publish Preview ===")
        print(f"Manifest URL: {manifest_url}")
        print(f"LUT URL: {lut_url}")
        return 0

    try:
        print("\n3. Uploading LUT...")
        upload_object(
            args=args,
            key=lut_key,
            file_path=lut_path,
            content_type="text/plain",
            cache_control=ARTIFACT_CACHE_CONTROL,
        )
        print("   LUT uploaded successfully")

        print("\n4. Uploading latest.json...")
        upload_object(
            args=args,
            key=manifest_key,
            file_path=manifest_path,
            content_type="application/json",
            cache_control=MANIFEST_CACHE_CONTROL,
        )
        print("   Manifest uploaded successfully")
    except Exception as exc:
        print(f"ERROR: upload failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if not args.manifest_out:
            manifest_path.unlink(missing_ok=True)

    print("\n=== Publish Complete ===")
    print(f"Manifest URL: {manifest_url}")
    print(f"LUT URL: {lut_url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
