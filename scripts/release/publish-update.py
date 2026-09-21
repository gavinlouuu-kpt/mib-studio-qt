#!/usr/bin/env python3
"""Publish a MIB Studio app update package to Cloudflare R2."""
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

from scripts.s3_upload import (
    download_bytes_from_s3,
    upload_file_to_s3,
    upload_file_with_wrangler,
)


DEFAULT_BUCKET = "mib-studio-qt-updates"
DEFAULT_PUBLIC_BASE_URL = "https://updates.yofo.bio"
ARTIFACT_CACHE_CONTROL = "public, max-age=31536000, immutable"
MANIFEST_CACHE_CONTROL = "public, max-age=60, must-revalidate"
INDEX_SCHEMA_VERSION = 1
_PUBLISHED_VERSION = re.compile(
    r"^(?P<numeric>\d+\.\d+\.\d+)"
    r"(?:-beta\.[0-9A-Za-z][0-9A-Za-z.-]*)?$"
)


def _version_sort_key(version: str) -> tuple:
    """Sort key for a version string. A release sorts AFTER its betas; betas
    ascend by number. e.g. 1.0.4-beta.1 < 1.0.4-beta.2 < 1.0.4."""
    core, _, suffix = str(version).partition("-")
    parts = [int(x) if x.isdigit() else 0 for x in core.split(".")]
    parts += [0] * (3 - len(parts))  # pad so 1.0 and 1.0.0 compare equal-ish
    if suffix.startswith("beta."):
        rest = suffix[len("beta."):]
        beta = int(rest) if rest.isdigit() else 0
    else:
        beta = float("inf")  # a release sorts after all of its betas
    return (tuple(parts[:3]), beta)


def _index_sort_key(entry: dict) -> tuple:
    """Order semver first, then equal SHA betas by publication time.

    SHA beta identifiers intentionally have no numeric prerelease ordering.
    Their UTC timestamps make a newly published build lead older builds on the
    same numeric line; the version string is a deterministic final tie-break.
    """
    published = str(entry.get("published_utc") or entry.get("published_at") or "")
    return (*_version_sort_key(entry.get("version", "")), published, str(entry.get("version", "")))


def merge_index(existing: dict, entry: dict, channel: str) -> dict:
    """Insert/replace `entry` (keyed by 'version') into a per-channel index,
    returning a newest-first {schema_version, channel, versions} dict. Pure."""
    versions = [v for v in (existing.get("versions") or []) if v.get("version") != entry.get("version")]
    versions.append(entry)
    versions.sort(key=_index_sort_key, reverse=True)
    return {"schema_version": INDEX_SCHEMA_VERSION, "channel": channel, "versions": versions}


def _index_entry_from_manifest(manifest: dict) -> dict:
    """Map a latest.json-style manifest to an index.json version entry. The
    app's UpdateCatalog reads published_utc (manifests carry published_at)."""
    return {
        "version": manifest.get("version", ""),
        "installer_url": manifest.get("installer_url", ""),
        "installer_sha256": manifest.get("installer_sha256", ""),
        "installer_size_bytes": manifest.get("installer_size_bytes", -1),
        "release_notes_url": manifest.get("release_notes_url", ""),
        "published_utc": manifest.get("published_utc") or manifest.get("published_at", ""),
    }


def fetch_json_url(url: str) -> dict | None:
    """GET a public JSON document; return None on any error (e.g. 404).

    Sends an explicit User-Agent because the default Python-urllib UA is often
    blocked/challenged by CDNs (e.g. Cloudflare), which silently broke index
    reads from CI runners."""
    import urllib.request
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "mib-studio-publish/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:  # noqa: S310 (trusted host)
            return json.loads(resp.read().decode("utf-8"))
    except Exception:
        return None


def resolve_index_update(existing: dict | None, read_ok: bool, entry: dict, channel: str):
    """Decide the index to upload after a publish.

    Returns None to SKIP the upload (the existing index could not be read, so we
    must not clobber it with a single-entry index). Otherwise returns the merged
    index (existing entries preserved + the new entry, newest-first)."""
    if not read_ok:
        return None
    return merge_index(existing or {}, entry, channel)


def read_existing_index(args, index_key: str):
    """Read the channel's current index.json. Returns (index_or_None, read_ok).

    Prefers the S3 API (same endpoint/credentials as uploads — reliable from CI)
    and falls back to the public URL. read_ok=False means the read genuinely
    failed (caller must not overwrite); a missing object is read_ok=True with {}.
    """
    method = resolve_upload_method(args.upload_method, args.endpoint)
    if method == "s3" and args.endpoint:
        try:
            data = download_bytes_from_s3(
                endpoint=args.endpoint, bucket=args.bucket, key=index_key, profile=args.profile)
            if data is None:
                return {}, True  # no index yet (first publish)
            return json.loads(data.decode("utf-8")), True
        except Exception as exc:
            print(f"WARNING: could not read existing {index_key} via S3: {exc}", file=sys.stderr)
            return None, False
    # Wrangler / no endpoint: fall back to the public URL.
    idx = fetch_json_url(join_public_object_url(args.public_base_url, index_key))
    return (idx or {}), True


def join_public_object_url(base_url: str, key: str) -> str:
    return f"{base_url.rstrip('/')}/{key.lstrip('/')}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def detect_version(installer: Path) -> str:
    match = re.fullmatch(r"MIB_Studio_Qt_(?:Setup|Update)_v(\d+\.\d+\.\d+)\.exe", installer.name)
    if not match:
        raise ValueError(
            "Cannot extract version from filename. Expected "
            "MIB_Studio_Qt_Setup_v<version>.exe or MIB_Studio_Qt_Update_v<version>.exe"
        )
    return match.group(1)


def published_artifact_name(installer: Path, version: str) -> str:
    """Return the immutable R2 filename for an explicit release identity.

    Inno Setup emits a numeric filename even for beta builds. R2 objects use
    the full beta identity so two prereleases never overwrite the same
    year-cached key.
    """
    release_match = _PUBLISHED_VERSION.fullmatch(version)
    if release_match is None:
        raise ValueError(
            "Version must be X.Y.Z or X.Y.Z-beta.<identifier> for publishing"
        )
    installer_version = detect_version(installer)
    if release_match.group("numeric") != installer_version:
        raise ValueError(
            "Explicit release numeric version does not match installer filename: "
            f"{version!r} vs {installer_version!r}"
        )
    return installer.name.replace(
        f"_v{installer_version}.exe", f"_v{version}.exe", 1
    )


def write_manifest_file(manifest: dict[str, object], manifest_out: str | None) -> Path:
    if manifest_out:
        path = Path(manifest_out)
        path.parent.mkdir(parents=True, exist_ok=True)
    else:
        handle = tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            prefix="mib_studio_qt_latest_",
            suffix=".json",
            delete=False,
        )
        path = Path(handle.name)
        handle.close()

    path.write_text(json.dumps(manifest, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    return path


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version",
        default=None,
        help="Full release identity (X.Y.Z or X.Y.Z-beta.<id>); required to preserve beta identity",
    )
    parser.add_argument("--installer", required=True, help="Path to MIB_Studio_Qt_(Update|Setup)_vX.Y.Z.exe")
    parser.add_argument("--endpoint", default=os.getenv("MIB_STUDIO_R2_ENDPOINT"))
    parser.add_argument("--bucket", default=DEFAULT_BUCKET)
    parser.add_argument("--public-base-url", default=DEFAULT_PUBLIC_BASE_URL)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--profile", default=os.getenv("MIB_STUDIO_R2_PROFILE"))
    parser.add_argument("--acl", default="")
    parser.add_argument("--release-notes-url", default="")
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
    installer = Path(args.installer)

    print("=== Publishing MIB Studio Qt Update ===")

    if not installer.is_file():
        print(f"ERROR: Installer file does not exist: {installer}", file=sys.stderr)
        return 1

    try:
        version = args.version or detect_version(installer)
        artifact_name = published_artifact_name(installer, version)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    if args.version is None:
        print(f"Extracted version from filename: {version}")

    package_kind = "update package (app files only)" if "MIB_Studio_Qt_Update_v" in installer.name else "full installer"
    print(f"Detected {package_kind}")

    size_bytes = installer.stat().st_size
    if size_bytes <= 0:
        print(f"ERROR: Installer file size is invalid: {size_bytes}", file=sys.stderr)
        return 1

    print("\n1. Computing SHA-256 hash...")
    digest = sha256_file(installer)
    print(f"   Hash: {digest}")
    print(f"   Size: {size_bytes} bytes")

    installer_key = f"{args.channel}/{artifact_name}"
    manifest_key = f"{args.channel}/latest.json"
    installer_url = join_public_object_url(args.public_base_url, installer_key)
    manifest_url = join_public_object_url(args.public_base_url, manifest_key)

    print("\n2. Generating manifest...")
    manifest: dict[str, object] = {
        "version": version,
        "installer_url": installer_url,
        "installer_sha256": digest,
        "installer_size_bytes": size_bytes,
        "published_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    }
    if args.release_notes_url:
        manifest["release_notes_url"] = args.release_notes_url

    manifest_path = write_manifest_file(manifest, args.manifest_out)
    print(f"   Manifest created: {manifest_path}")

    if args.dry_run:
        print("\nDRY RUN: skipped R2 uploads")
        print("\n=== Publish Preview ===")
        print(f"Manifest URL: {manifest_url}")
        print(f"Installer URL: {installer_url}")
        return 0

    index_key = f"{args.channel}/index.json"

    try:
        print("\n3. Uploading installer...")
        upload_object(
            args=args,
            key=installer_key,
            file_path=installer,
            content_type="application/x-msdownload",
            cache_control=ARTIFACT_CACHE_CONTROL,
        )
        print("   Installer uploaded successfully")

        print("\n4. Uploading manifest...")
        upload_object(
            args=args,
            key=manifest_key,
            file_path=manifest_path,
            content_type="application/json",
            cache_control=MANIFEST_CACHE_CONTROL,
        )
        print("   Manifest uploaded successfully")

        print("\n5. Updating version index...")
        existing_index, read_ok = read_existing_index(args, index_key)
        index = resolve_index_update(
            existing_index, read_ok, _index_entry_from_manifest(manifest), args.channel)
        if index is None:
            # Read failed: skip rather than overwrite a good catalog with one entry.
            print("   WARNING: skipping index.json update (could not read existing index)",
                  file=sys.stderr)
        else:
            index_path = write_manifest_file(index, None)
            try:
                upload_object(
                    args=args,
                    key=index_key,
                    file_path=index_path,
                    content_type="application/json",
                    cache_control=MANIFEST_CACHE_CONTROL,
                )
            finally:
                index_path.unlink(missing_ok=True)
            print(f"   Version index updated ({len(index['versions'])} version(s))")
    except Exception as exc:
        print(f"ERROR: upload failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if not args.manifest_out:
            manifest_path.unlink(missing_ok=True)

    print("\n=== Publish Complete ===")
    print(f"Manifest URL: {manifest_url}")
    print(f"Installer URL: {installer_url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
