#!/usr/bin/env python3
"""Publish a public profile catalog to Cloudflare R2."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from scripts.s3_upload import upload_file_to_s3, upload_file_with_wrangler


DEFAULT_BUCKET = "mib-studio-qt-updates"
DEFAULT_PUBLIC_BASE_URL = "https://updates.yofo.bio"
CATALOG_SCHEMA_VERSION = 1
PROFILE_META_SCHEMA_VERSION = 1
CONFIG_SCHEMA_VERSION = 1
CATALOG_CACHE_CONTROL = "public, max-age=60, must-revalidate"
PROFILE_CACHE_CONTROL = "public, max-age=300, must-revalidate"
CHANGELOG_CACHE_CONTROL = "public, max-age=300, must-revalidate"
PROFILE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*[a-z0-9]$")


def join_public_object_url(base_url: str, key: str) -> str:
    return f"{base_url.rstrip('/')}/{key.lstrip('/')}"


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as fh:
            value = json.load(fh)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path} is not valid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=False) + "\n", encoding="utf-8")


def prepare_config_for_upload(config_path: Path, *, add_missing_schema: bool, staging_dir: Path) -> Path:
    config = load_json(config_path)
    if "config_schema_version" in config:
        if config["config_schema_version"] != CONFIG_SCHEMA_VERSION:
            raise ValueError(
                f"{config_path} has unsupported config_schema_version={config['config_schema_version']!r}"
            )
        return config_path

    if not add_missing_schema:
        raise ValueError(
            f"{config_path} is missing config_schema_version. "
            "Add it explicitly or rerun with --add-missing-config-schema."
        )

    config = {"config_schema_version": CONFIG_SCHEMA_VERSION, **config}
    normalized_path = staging_dir / "config.json"
    write_json(normalized_path, config)
    return normalized_path


def discover_profile_dirs(profiles_root: Path, selected_ids: list[str]) -> list[Path]:
    if selected_ids:
        profile_dirs = [profiles_root / profile_id for profile_id in selected_ids]
    else:
        profile_dirs = [path for path in sorted(profiles_root.iterdir()) if path.is_dir()]

    valid_dirs: list[Path] = []
    for profile_dir in profile_dirs:
        if not profile_dir.is_dir():
            raise ValueError(f"Profile directory does not exist: {profile_dir}")
        if not (profile_dir / "config.json").is_file():
            continue
        valid_dirs.append(profile_dir)
    if not valid_dirs:
        raise ValueError(f"No profile directories with config.json found under {profiles_root}")
    return valid_dirs


def build_profile_metadata(
    *,
    profile_dir: Path,
    channel: str,
    catalog_url: str,
    public_base_url: str,
    profile_prefix: str,
    revision: str,
    add_missing_config_schema: bool,
    staging_dir: Path,
) -> tuple[dict[str, Any], dict[str, Any], list[tuple[str, Path, str, str]]]:
    profile_id = profile_dir.name
    if not PROFILE_ID_RE.fullmatch(profile_id):
        raise ValueError(
            f"Invalid profile id {profile_id!r}; use lowercase letters, numbers, and hyphens"
        )

    config_path = profile_dir / "config.json"
    meta_path = profile_dir / "profile.meta.json"
    script_path = profile_dir / "egrabberConfig.js"
    changelog_path = profile_dir / "CHANGELOG.md"

    upload_config_path = prepare_config_for_upload(
        config_path,
        add_missing_schema=add_missing_config_schema,
        staging_dir=staging_dir,
    )
    config_digest = sha256_file(upload_config_path)
    script_digest = sha256_file(script_path) if script_path.is_file() else None

    base_meta: dict[str, Any] = {}
    if meta_path.is_file():
        base_meta = load_json(meta_path)

    display_name = str(base_meta.get("display_name") or profile_id.replace("-", " ").title())
    description = str(base_meta.get("description") or "")
    profile_revision = str(base_meta.get("revision") or revision)
    app_min_version = base_meta.get("app_min_version", "0.8.0")
    app_max_version = base_meta.get("app_max_version")
    processing_contract_version = base_meta.get("processing_contract_version")
    if processing_contract_version is not None and (
        isinstance(processing_contract_version, bool)
        or not isinstance(processing_contract_version, int)
        or processing_contract_version <= 0
    ):
        raise ValueError(
            f"{meta_path} processing_contract_version must be a positive integer or null"
        )

    profile_object_prefix = f"{profile_prefix}/{profile_id}"
    config_key = f"{profile_object_prefix}/config.json"
    script_key = f"{profile_object_prefix}/egrabberConfig.js"
    meta_key = f"{profile_object_prefix}/profile.meta.json"
    changelog_key = f"{profile_object_prefix}/CHANGELOG.md"

    meta = {
        "profile_meta_schema_version": PROFILE_META_SCHEMA_VERSION,
        "profile_id": profile_id,
        "display_name": display_name,
        "description": description,
        "source": {
            "type": "r2-public-catalog",
            "channel": channel,
            "catalog_url": catalog_url,
        },
        "revision": profile_revision,
        "config_schema_version": CONFIG_SCHEMA_VERSION,
        "config_sha256": config_digest,
        "camera_script_sha256": script_digest,
        "app_min_version": app_min_version,
        "app_max_version": app_max_version,
        "processing_contract_version": processing_contract_version,
        "last_checked_utc": None,
        "last_updated_utc": profile_revision,
    }

    catalog_entry: dict[str, Any] = {
        "profile_id": profile_id,
        "display_name": display_name,
        "description": description,
        "revision": profile_revision,
        "profile_meta_url": join_public_object_url(public_base_url, meta_key),
        "config_url": join_public_object_url(public_base_url, config_key),
        "camera_script_url": join_public_object_url(public_base_url, script_key) if script_digest else None,
        "config_sha256": config_digest,
        "camera_script_sha256": script_digest,
        "app_min_version": app_min_version,
        "app_max_version": app_max_version,
        "processing_contract_version": processing_contract_version,
    }

    uploads = [
        (config_key, upload_config_path, "application/json", PROFILE_CACHE_CONTROL),
    ]
    if script_path.is_file():
        uploads.append((script_key, script_path, "application/javascript", PROFILE_CACHE_CONTROL))
    if changelog_path.is_file():
        uploads.append((changelog_key, changelog_path, "text/markdown; charset=utf-8", CHANGELOG_CACHE_CONTROL))

    return catalog_entry, meta, uploads


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profiles-root", required=True, help="Directory containing one subdirectory per profile")
    parser.add_argument("--profile-id", action="append", default=[], help="Profile id to publish; repeat to limit selection")
    parser.add_argument("--endpoint", default=os.getenv("MIB_STUDIO_R2_ENDPOINT"))
    parser.add_argument("--bucket", default=DEFAULT_BUCKET)
    parser.add_argument("--public-base-url", default=DEFAULT_PUBLIC_BASE_URL)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--profile", default=os.getenv("MIB_STUDIO_R2_PROFILE"))
    parser.add_argument("--acl", default="")
    parser.add_argument("--catalog-out", default=None, help="Write generated catalog JSON to this path")
    parser.add_argument("--dry-run", action="store_true", help="Generate metadata but do not upload")
    parser.add_argument("--add-missing-config-schema", action="store_true")
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
    profiles_root = Path(args.profiles_root)

    print("=== Publishing MIB Studio Profile Catalog ===")
    if not profiles_root.is_dir():
        print(f"ERROR: profiles root does not exist: {profiles_root}", file=sys.stderr)
        return 1

    profile_prefix = f"profiles/{args.channel}"
    catalog_key = f"{profile_prefix}/catalog.json"
    catalog_url = join_public_object_url(args.public_base_url, catalog_key)
    published_at = utc_now()

    try:
        profile_dirs = discover_profile_dirs(profiles_root, args.profile_id)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    temp_dir = Path(tempfile.mkdtemp(prefix="mib_profile_catalog_"))
    catalog_path = Path(args.catalog_out) if args.catalog_out else temp_dir / "catalog.json"
    generated_uploads: list[tuple[str, Path, str, str]] = []
    catalog_entries: list[dict[str, Any]] = []

    try:
        for profile_dir in profile_dirs:
            try:
                entry, meta, uploads = build_profile_metadata(
                    profile_dir=profile_dir,
                    channel=args.channel,
                    catalog_url=catalog_url,
                    public_base_url=args.public_base_url,
                    profile_prefix=profile_prefix,
                    revision=published_at,
                    add_missing_config_schema=args.add_missing_config_schema,
                    staging_dir=temp_dir / profile_dir.name,
                )
            except ValueError as exc:
                print(f"ERROR: {exc}", file=sys.stderr)
                return 1

            meta_path = temp_dir / entry["profile_id"] / "profile.meta.json"
            write_json(meta_path, meta)
            generated_uploads.extend(uploads)
            generated_uploads.append(
                (
                    f"{profile_prefix}/{entry['profile_id']}/profile.meta.json",
                    meta_path,
                    "application/json",
                    PROFILE_CACHE_CONTROL,
                )
            )
            catalog_entries.append(entry)

        catalog = {
            "catalog_schema_version": CATALOG_SCHEMA_VERSION,
            "channel": args.channel,
            "published_at": published_at,
            "profiles": catalog_entries,
        }
        write_json(catalog_path, catalog)

        print(f"Profiles: {len(catalog_entries)}")
        print(f"Catalog: {catalog_path}")
        print(f"Catalog URL: {catalog_url}")

        if args.dry_run:
            print("\nDRY RUN: skipped R2 uploads")
            for key, path, _, _ in generated_uploads:
                print(f"Would upload: {path} -> s3://{args.bucket}/{key}")
            print(f"Would upload: {catalog_path} -> s3://{args.bucket}/{catalog_key}")
            return 0

        for key, path, content_type, cache_control in generated_uploads:
            print(f"\nUploading {key}...")
            upload_object(args=args, key=key, file_path=path, content_type=content_type, cache_control=cache_control)

        print(f"\nUploading {catalog_key}...")
        upload_object(
            args=args,
            key=catalog_key,
            file_path=catalog_path,
            content_type="application/json",
            cache_control=CATALOG_CACHE_CONTROL,
        )
    except Exception as exc:
        print(f"ERROR: publish failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if not args.catalog_out:
            shutil.rmtree(temp_dir, ignore_errors=True)

    print("\n=== Publish Complete ===")
    print(f"Catalog URL: {catalog_url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
