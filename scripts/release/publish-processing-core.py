#!/usr/bin/env python3
"""Publish the versioned processing-core registry to Cloudflare R2.

The registry has three views over the same release:

* ``latest.json`` is the short-cache active pointer and remains a complete
  manifest for schema-v1 consumers.
* ``versions/<version>.json`` is an immutable, addressable release manifest.
* ``index.json`` is a short-cache catalog used by version selectors.  The
  catalog also feeds a PEP 503 package page for reproducible pip pins.

See docs/portable-processing-sync.md for the public contract.
"""
from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import html
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from scripts.s3_upload import (
    download_bytes_from_s3,
    upload_file_to_s3,
    upload_file_with_wrangler,
)

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python <3.11 fallback
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ModuleNotFoundError:
        tomllib = None  # type: ignore[assignment]


DEFAULT_BUCKET = "mib-studio-qt-updates"
DEFAULT_PUBLIC_BASE_URL = "https://updates.yofo.bio"
DEFAULT_REPO = "KPT1020/mib-studio-qt"
MANIFEST_SCHEMA_VERSION = 2
INDEX_SCHEMA_VERSION = 1
DEFAULT_CONTRACT_VERSION = 1
MUTABLE_CACHE_CONTROL = "public, max-age=60, must-revalidate"
IMMUTABLE_CACHE_CONTROL = "public, max-age=31536000, immutable"
PEP503_PACKAGE = "mib-processing"
_VERSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+!-]*$")
_CHANNEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
_TAG_PREFIX = "mib-processing-v"
_NATIVE_LIBRARY_SUFFIXES = {
    "windows": (".dll",),
    "linux": (".so",),
    "macos": (".dylib",),
}
_NATIVE_ARCH_ALIASES = {
    "amd64": "x86_64",
    "x64": "x86_64",
    "arm64": "aarch64",
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def validate_published_at(value: str) -> str:
    """Return an RFC 3339-style, timezone-aware publication timestamp."""
    if not isinstance(value, str) or not value or value != value.strip():
        raise ValueError(f"Invalid publication timestamp: {value!r}")
    try:
        parsed = datetime.fromisoformat(
            value[:-1] + "+00:00" if value.endswith("Z") else value
        )
    except ValueError as exc:
        raise ValueError(f"Invalid publication timestamp: {value!r}") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise ValueError(f"Publication timestamp must include a timezone: {value!r}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def join_public_object_url(base_url: str, key: str) -> str:
    return f"{base_url.rstrip('/')}/{key.lstrip('/')}"


def validate_version(version: str) -> str:
    """Return a path-safe release version or raise ValueError."""
    if not _VERSION_RE.fullmatch(version) or version in {".", ".."}:
        raise ValueError(
            f"Invalid processing-core version {version!r}; use letters, digits, '.', '_', '+', '!', or '-'"
        )
    return version


def version_object_component(version: str) -> str:
    return urllib.parse.quote(validate_version(version), safe="._+-!")


def validate_channel(channel: str) -> str:
    if not _CHANNEL_RE.fullmatch(channel) or channel in {".", ".."}:
        raise ValueError(f"Invalid registry channel: {channel!r}")
    return channel


def version_from_release_tag(release_tag: str) -> str:
    if not release_tag.startswith(_TAG_PREFIX):
        raise ValueError(f"Release tag must have the form {_TAG_PREFIX}<version>: {release_tag!r}")
    version = validate_version(release_tag[len(_TAG_PREFIX):])
    if not version:
        raise ValueError(f"Release tag must include a version: {release_tag!r}")
    return version


def read_wheel_version(pyproject_path: Path) -> str:
    if tomllib is None:
        raise RuntimeError(
            "Reading the wheel version from pyproject.toml requires Python 3.11+ "
            "or the 'tomli' backport; install tomli or pass --wheel-version explicitly."
        )
    with pyproject_path.open("rb") as fh:
        data = tomllib.load(fh)
    try:
        return validate_version(str(data["project"]["version"]))
    except KeyError as exc:
        raise ValueError(f"{pyproject_path} has no [project].version") from exc


def _wheel_filename_parts(path: Path) -> tuple[str, str, str]:
    if path.suffix != ".whl":
        raise ValueError(f"Not a wheel file: {path}")
    parts = path.stem.split("-")
    if len(parts) not in (5, 6):
        raise ValueError(f"Invalid wheel filename: {path.name}")
    distribution, version = parts[0], parts[1]
    if distribution.replace("_", "-").lower() != PEP503_PACKAGE:
        raise ValueError(f"Expected a {PEP503_PACKAGE} wheel, got {path.name}")
    platform_tag = "-".join(parts[-3:])
    return version, platform_tag, path.name


def build_wheel_entries(
    wheel_paths: list[Path],
    repo: str,
    release_tag: str,
    *,
    expected_version: str | None = None,
) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    seen_names: set[str] = set()
    for path in sorted(wheel_paths, key=lambda candidate: candidate.name):
        if not path.is_file():
            raise ValueError(f"Wheel file does not exist: {path}")
        if path.stat().st_size <= 0:
            raise ValueError(f"Wheel file is empty: {path}")
        wheel_version, platform_tag, filename = _wheel_filename_parts(path)
        if expected_version is not None and wheel_version != expected_version:
            raise ValueError(
                f"Wheel {filename} declares version {wheel_version}, expected {expected_version}"
            )
        if filename in seen_names:
            raise ValueError(f"Duplicate wheel release asset: {filename}")
        seen_names.add(filename)
        entries.append({
            "filename": filename,
            "platform_tag": platform_tag,
            "url": f"https://github.com/{repo}/releases/download/{release_tag}/{filename}",
            "sha256": sha256_file(path),
            "size_bytes": path.stat().st_size,
        })
    return entries


def _load_native_descriptor(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"Cannot read native plugin descriptor {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"Native plugin descriptor must be a JSON object: {path}")
    return value


def discover_native_descriptors(asset_dir: Path) -> list[Path]:
    """Find release sidecars without mistaking unrelated JSON assets for one."""
    descriptors: list[Path] = []
    for path in sorted(asset_dir.glob("*.json")):
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(value, dict) and value.get("filename") and value.get("engine_abi_version") is not None:
            descriptors.append(path)
    return descriptors


def build_native_plugin_entries(
    descriptor_paths: list[Path],
    *,
    asset_dir: Path,
    repo: str,
    release_tag: str,
    expected_version: str,
    expected_contract_version: int,
) -> list[dict[str, Any]]:
    """Build trusted metadata from native-plugin sidecars and local release bytes.

    URLs, digests, and sizes always come from the publisher.  A sidecar cannot
    redirect a client or claim a digest for bytes that were not inspected.
    """
    entries: list[dict[str, Any]] = []
    seen_names: set[str] = set()
    seen_platforms: set[tuple[str, str]] = set()
    required = ("filename", "os", "arch", "engine_abi_version", "runtime_fingerprint", "entrypoint")
    for descriptor_path in sorted(descriptor_paths, key=lambda candidate: candidate.name):
        descriptor = _load_native_descriptor(descriptor_path)
        missing = [key for key in required if descriptor.get(key) in (None, "")]
        if missing:
            raise ValueError(f"Native plugin descriptor {descriptor_path} is missing: {', '.join(missing)}")

        filename = str(descriptor["filename"])
        native_os = str(descriptor["os"]).strip().lower()
        raw_arch = str(descriptor["arch"]).strip().lower()
        native_arch = _NATIVE_ARCH_ALIASES.get(raw_arch, raw_arch)
        allowed_suffixes = _NATIVE_LIBRARY_SUFFIXES.get(native_os)
        if Path(filename).name != filename:
            raise ValueError(f"Native plugin filename must be a basename: {filename!r}")
        if allowed_suffixes is None:
            raise ValueError(f"Native plugin declares unsupported OS {native_os!r}: {filename!r}")
        if not filename.lower().endswith(allowed_suffixes):
            expected = " or ".join(allowed_suffixes)
            raise ValueError(
                f"Native plugin for {native_os} must end in {expected}: {filename!r}"
            )
        artifact = asset_dir / filename
        if not artifact.is_file():
            raise ValueError(f"Native plugin asset named by {descriptor_path.name} does not exist: {artifact}")
        if artifact.stat().st_size <= 0:
            raise ValueError(f"Native plugin asset is empty: {artifact}")
        if filename in seen_names:
            raise ValueError(f"Duplicate native plugin release asset: {filename}")
        seen_names.add(filename)
        platform = (native_os, native_arch)
        if platform in seen_platforms:
            raise ValueError(f"Duplicate native plugin platform: {native_os}/{native_arch}")
        seen_platforms.add(platform)

        descriptor_version = str(descriptor.get("version", expected_version))
        if descriptor_version != expected_version:
            raise ValueError(
                f"Native plugin {filename} declares version {descriptor_version}, expected {expected_version}"
            )
        descriptor_contract = int(descriptor.get("contract_version", expected_contract_version))
        if descriptor_contract != expected_contract_version:
            raise ValueError(
                f"Native plugin {filename} declares contract {descriptor_contract}, "
                f"expected {expected_contract_version}"
            )

        signing = descriptor.get("signing", {})
        if signing is not None and not isinstance(signing, dict):
            raise ValueError(f"Native plugin signing metadata must be an object: {descriptor_path}")
        signing = dict(signing or {})
        signing_scheme = str(signing.get("scheme") or signing.get("format") or "").strip().lower()
        if not signing_scheme:
            raise ValueError(f"Native plugin signing scheme is required: {descriptor_path}")
        if signing.get("required", True) is not True:
            raise ValueError(f"Native plugin signatures must be required: {descriptor_path}")
        signing.pop("format", None)
        signing["scheme"] = signing_scheme
        signing["required"] = True
        if signing_scheme == "ed25519":
            # A detached-signature scheme must ship its transport material and
            # the key hash must be derived from the actual key bytes, mirroring
            # how artifact URL/digest/size come from the real release assets.
            spki_b64 = str(signing.get("public_key_spki_base64") or "").strip()
            signature_b64 = str(signing.get("signature_base64") or "").strip()
            try:
                spki = base64.b64decode(spki_b64, validate=True)
                signature_bytes = base64.b64decode(signature_b64, validate=True)
            except (ValueError, binascii.Error) as exc:
                raise ValueError(
                    f"Native plugin ed25519 signing material is not valid base64: {descriptor_path}"
                ) from exc
            if len(spki) != 44:
                raise ValueError(
                    f"Native plugin ed25519 public key must be a 44-byte DER SPKI: {descriptor_path}"
                )
            if len(signature_bytes) != 64:
                raise ValueError(
                    f"Native plugin ed25519 signature must be 64 bytes: {descriptor_path}"
                )
            derived_spki_sha256 = hashlib.sha256(spki).hexdigest()
            declared_spki_sha256 = str(signing.get("public_key_spki_sha256") or "").strip().lower()
            if declared_spki_sha256 and declared_spki_sha256 != derived_spki_sha256:
                raise ValueError(
                    f"Native plugin ed25519 public_key_spki_sha256 does not match the key bytes: "
                    f"{descriptor_path}"
                )
            signing["public_key_spki_base64"] = spki_b64
            signing["public_key_spki_sha256"] = derived_spki_sha256
            signing["signature_base64"] = signature_b64
        entries.append({
            "filename": filename,
            "os": native_os,
            "arch": native_arch,
            "artifact_kind": str(descriptor.get("artifact_kind", "shared_library")),
            "version": expected_version,
            "contract_version": expected_contract_version,
            "engine_abi_version": int(descriptor["engine_abi_version"]),
            "runtime_fingerprint": str(descriptor["runtime_fingerprint"]),
            "app_min_version": descriptor.get("app_min_version"),
            "app_max_version": descriptor.get("app_max_version"),
            "entrypoint": str(descriptor["entrypoint"]),
            "url": f"https://github.com/{repo}/releases/download/{release_tag}/{filename}",
            "sha256": sha256_file(artifact),
            "size_bytes": artifact.stat().st_size,
            "descriptor_url": (
                f"https://github.com/{repo}/releases/download/{release_tag}/{descriptor_path.name}"
            ),
            "signing": signing,
        })
    return entries


def inspect_github_release(repo: str, release_tag: str, gh_bin: str = "gh") -> dict[str, Any]:
    command = [
        gh_bin, "release", "view", release_tag, "--repo", repo,
        "--json", "tagName,publishedAt,url,assets",
    ]
    try:
        completed = subprocess.run(command, check=True, capture_output=True, text=True)
        release = json.loads(completed.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise RuntimeError(f"Could not inspect GitHub Release {repo}@{release_tag}: {detail.strip()}") from exc
    if release.get("tagName") != release_tag:
        raise RuntimeError(
            f"GitHub returned release tag {release.get('tagName')!r}, expected {release_tag!r}"
        )
    return release


def download_github_release(repo: str, release_tag: str, destination: Path, gh_bin: str = "gh") -> None:
    destination.mkdir(parents=True, exist_ok=True)
    command = [gh_bin, "release", "download", release_tag, "--repo", repo, "--dir", str(destination)]
    try:
        subprocess.run(command, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(f"Could not download GitHub Release {repo}@{release_tag}: {exc}") from exc


def build_manifest(
    *,
    channel: str,
    contract_version: int,
    wheel_version: str,
    release_tag: str,
    repo: str,
    wheel_paths: list[Path],
    public_base_url: str,
    native_plugins: list[dict[str, Any]] | None = None,
    published_at: str | None = None,
) -> dict[str, Any]:
    validate_channel(channel)
    validate_version(wheel_version)
    if not isinstance(contract_version, int) or isinstance(contract_version, bool) or contract_version < 1:
        raise ValueError(f"Contract version must be a positive integer: {contract_version!r}")
    if not wheel_paths:
        raise ValueError("At least one mib-processing wheel is required")
    tagged_version = version_from_release_tag(release_tag)
    if tagged_version != wheel_version:
        raise ValueError(
            f"Release tag {release_tag!r} names version {tagged_version}, but wheel version is {wheel_version}"
        )
    return {
        "processing_core_manifest_schema_version": MANIFEST_SCHEMA_VERSION,
        "channel": channel,
        "version": wheel_version,
        "published_at": validate_published_at(published_at or utc_now()),
        "contract_version": contract_version,
        "wheel": {
            "package": PEP503_PACKAGE,
            "version": wheel_version,
            "release_tag": release_tag,
            "release_url": f"https://github.com/{repo}/releases/tag/{release_tag}",
            "wheels": build_wheel_entries(
                wheel_paths, repo, release_tag, expected_version=wheel_version,
            ),
        },
        "native_plugins": list(native_plugins or []),
        "profile_catalog_url": join_public_object_url(public_base_url, f"profiles/{channel}/catalog.json"),
        "emodulus_lut_manifest_url": join_public_object_url(
            public_base_url, f"{channel}/emodulus-lut/latest.json"
        ),
    }


def _version_sort_key(version: str) -> tuple[Any, ...]:
    """Comparable newest-first key for the SemVer/PEP 440 forms we publish."""
    match = re.fullmatch(r"(\d+)(?:\.(\d+))?(?:\.(\d+))?(.*)", version)
    if not match:
        return (0, 0, 0), -1, 0, version.lower()
    core = tuple(int(value or 0) for value in match.groups()[:3])
    suffix = match.group(4).lower()
    prerelease = re.fullmatch(r"[-.]?(a|alpha|b|beta|rc)[.-]?(\d*)", suffix)
    if prerelease:
        phase = {"a": 0, "alpha": 0, "b": 1, "beta": 1, "rc": 2}[prerelease.group(1)]
        number = int(prerelease.group(2) or 0)
        return core, phase, number, ""
    if not suffix or suffix.startswith("+"):
        return core, 3, 0, suffix
    return core, -1, 0, suffix


def index_entry_from_manifest(manifest: dict[str, Any], public_base_url: str) -> dict[str, Any]:
    version = str(manifest["version"])
    wheel = manifest["wheel"]
    return {
        "version": version,
        "contract_version": manifest["contract_version"],
        "published_at": manifest["published_at"],
        "release_tag": wheel["release_tag"],
        "release_url": wheel["release_url"],
        "manifest_url": join_public_object_url(
            public_base_url,
            f"{manifest['channel']}/processing-core/versions/{version_object_component(version)}.json",
        ),
        "wheels": list(wheel.get("wheels", [])),
        "native_plugins": list(manifest.get("native_plugins", [])),
    }


def merge_index(
    existing: dict[str, Any],
    manifest: dict[str, Any],
    public_base_url: str,
) -> dict[str, Any]:
    """Insert/replace one version and make it the active channel pointer."""
    channel = str(manifest["channel"])
    if existing:
        existing_channel = existing.get("channel")
        if existing_channel not in (None, channel):
            raise ValueError(f"Existing index channel is {existing_channel!r}, expected {channel!r}")
        schema = existing.get("processing_core_index_schema_version", INDEX_SCHEMA_VERSION)
        if schema != INDEX_SCHEMA_VERSION:
            raise ValueError(f"Unsupported processing-core index schema: {schema!r}")
    existing_versions = existing.get("versions") or []
    if not isinstance(existing_versions, list):
        raise ValueError("Existing processing-core index.versions is not a list")
    seen_existing: set[str] = set()
    for value in existing_versions:
        if not isinstance(value, dict) or not isinstance(value.get("version"), str):
            raise ValueError("Existing processing-core index contains an invalid version entry")
        validate_version(value["version"])
        if value["version"] in seen_existing:
            raise ValueError(f"Existing processing-core index contains duplicate version {value['version']!r}")
        if not isinstance(value.get("wheels", []), list) or not isinstance(value.get("native_plugins", []), list):
            raise ValueError(f"Existing processing-core index has invalid artifacts for {value['version']!r}")
        seen_existing.add(value["version"])
    entry = index_entry_from_manifest(manifest, public_base_url)
    versions = [
        value for value in existing_versions if value.get("version") != entry["version"]
    ]
    versions.append(entry)
    versions.sort(key=lambda value: _version_sort_key(str(value.get("version", ""))), reverse=True)
    return {
        "processing_core_index_schema_version": INDEX_SCHEMA_VERSION,
        "channel": channel,
        "active_version": manifest["version"],
        "updated_at": manifest["published_at"],
        "versions": versions,
    }


def render_pep503_index(index: dict[str, Any]) -> str:
    links: list[tuple[str, str, str]] = []
    for version in index.get("versions", []):
        for wheel in version.get("wheels", []):
            filename = str(
                wheel.get("filename")
                or urllib.parse.urlparse(str(wheel.get("url", ""))).path.rsplit("/", 1)[-1]
            )
            url = str(wheel.get("url", ""))
            digest = str(wheel.get("sha256", ""))
            if filename and url and re.fullmatch(r"[0-9a-fA-F]{64}", digest):
                links.append((filename, url, digest.lower()))
    links.sort(key=lambda item: item[0])
    body = ["<!DOCTYPE html>", "<html>", "  <head><title>Links for mib-processing</title></head>", "  <body>"]
    for filename, url, digest in links:
        href = f"{url}#sha256={digest}"
        body.append(f'    <a href="{html.escape(href, quote=True)}">{html.escape(filename)}</a><br>')
    body.extend(["  </body>", "</html>", ""])
    return "\n".join(body)


def fetch_public_object(url: str) -> tuple[bytes | None, bool]:
    request = urllib.request.Request(url, headers={"User-Agent": "mib-processing-publisher/2.0"})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:  # noqa: S310 (configured public registry)
            return response.read(), True
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            return None, True
        print(f"WARNING: GET {url} failed: HTTP {exc.code}", file=sys.stderr)
        return None, False
    except (OSError, urllib.error.URLError) as exc:
        print(f"WARNING: GET {url} failed: {exc}", file=sys.stderr)
        return None, False


def read_existing_object(args: argparse.Namespace, key: str) -> tuple[bytes | None, bool]:
    method = resolve_upload_method(args.upload_method, args.endpoint)
    if method == "s3" and args.endpoint:
        try:
            return (
                download_bytes_from_s3(
                    endpoint=args.endpoint,
                    bucket=args.bucket,
                    key=key,
                    profile=args.profile,
                ),
                True,
            )
        except Exception as exc:
            print(f"WARNING: could not read existing {key} through S3: {exc}", file=sys.stderr)
            return None, False
    return fetch_public_object(join_public_object_url(args.public_base_url, key))


def parse_existing_json(data: bytes | None, key: str) -> dict[str, Any]:
    if data is None:
        return {}
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Existing {key} is not valid UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"Existing {key} is not a JSON object")
    return value


def resolve_immutable_update(
    existing: bytes | None,
    read_ok: bool,
    manifest: dict[str, Any],
    key: str,
) -> bool:
    """Return True when an immutable object must be uploaded, False if identical."""
    if not read_ok:
        raise RuntimeError(f"Refusing to publish because existing immutable object could not be read: {key}")
    if existing is None:
        return True
    expected = serialize_json(manifest)
    if existing != expected:
        raise RuntimeError(f"Immutable processing-core version already exists with different content: {key}")
    return False


def serialize_json(value: dict[str, Any]) -> bytes:
    """Serialize registry JSON identically on Unix and Windows."""
    return (json.dumps(value, indent=2, sort_keys=False) + "\n").encode("utf-8")


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Immutable equality is byte-level. Avoid newline translation during a
    # manual Windows publish so it remains identical to Linux CI output.
    path.write_bytes(serialize_json(value))


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--contract-version", type=int, default=DEFAULT_CONTRACT_VERSION)
    parser.add_argument(
        "--wheel-version",
        default=None,
        help="Wheel version. Defaults to bindings/python/pyproject.toml; must match --from-release.",
    )
    parser.add_argument(
        "--pyproject",
        default=str(Path(__file__).resolve().parents[2] / "bindings" / "python" / "pyproject.toml"),
        help="Path to the authoritative wheel pyproject.toml.",
    )
    parser.add_argument("--release-tag", default=None, help="Defaults to mib-processing-v<wheel-version>")
    parser.add_argument(
        "--from-release",
        metavar="TAG",
        default=None,
        help="Discover/download wheel and native assets from this GitHub Release tag.",
    )
    parser.add_argument(
        "--promote-version",
        metavar="VERSION",
        default=None,
        help=(
            "Promote an already-published immutable versions/<version>.json byte-for-byte. "
            "This is the rollback path and does not rebuild release metadata."
        ),
    )
    parser.add_argument(
        "--release-assets-dir",
        default=None,
        help="Use already-downloaded release assets (fixture/offline mode; requires --from-release).",
    )
    parser.add_argument("--published-at", default=None, help="Stable ISO-8601 publication timestamp override")
    parser.add_argument("--gh-bin", default=os.getenv("GH_BIN", "gh"))
    parser.add_argument("--repo", default=DEFAULT_REPO, help="owner/repo for GitHub Release URLs")
    parser.add_argument(
        "--wheel", action="append", default=[], dest="wheels",
        help="Local wheel release asset; repeat for multiple tags.",
    )
    parser.add_argument(
        "--native-plugin-descriptor", action="append", default=[], dest="native_descriptors",
        help="JSON sidecar naming a sibling signed DLL; repeat per platform.",
    )
    parser.add_argument("--endpoint", default=os.getenv("MIB_STUDIO_R2_ENDPOINT"))
    parser.add_argument("--bucket", default=DEFAULT_BUCKET)
    parser.add_argument("--public-base-url", default=DEFAULT_PUBLIC_BASE_URL)
    parser.add_argument("--profile", default=os.getenv("MIB_STUDIO_R2_PROFILE"))
    parser.add_argument("--acl", default="")
    parser.add_argument("--manifest-out", default=None, help="Write latest.json preview to this path")
    parser.add_argument("--version-manifest-out", default=None)
    parser.add_argument("--index-out", default=None)
    parser.add_argument("--pep503-out", default=None)
    parser.add_argument("--dry-run", action="store_true", help="Generate all registry documents but do not upload")
    parser.add_argument(
        "--upload-method", choices=("auto", "s3", "wrangler"), default="auto",
        help=(
            "Upload transport. Mutating registry operations require the S3 API for "
            "strongly consistent preflight reads; Wrangler remains available to dry runs."
        ),
    )
    parser.add_argument("--wrangler-bin", default=os.getenv("WRANGLER_BIN", "wrangler"))
    parser.add_argument("--debug", action="store_true")
    return parser


def resolve_upload_method(upload_method: str, endpoint: str | None) -> str:
    if upload_method == "auto":
        return "s3" if endpoint else "wrangler"
    return upload_method


def require_consistent_mutating_transport(args: argparse.Namespace) -> None:
    """Reject publication paths that would preflight through the public CDN."""
    if args.dry_run:
        return
    method = resolve_upload_method(args.upload_method, args.endpoint)
    if method != "s3" or not args.endpoint:
        raise ValueError(
            "Mutating processing-core publication requires --upload-method s3 and an R2 "
            "endpoint so immutable/catalog preflight reads bypass the public CDN cache"
        )


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
        print("   Note: --acl is ignored for Wrangler uploads; configure public access on R2.")
    upload_file_with_wrangler(
        bucket=args.bucket,
        key=key,
        file_path=str(file_path),
        content_type=content_type,
        cache_control=cache_control,
        wrangler_bin=args.wrangler_bin,
    )


def _copy_preview(path_value: str | None, source: Path) -> None:
    if not path_value:
        return
    destination = Path(path_value)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(source.read_bytes())


def _validate_promotion_manifest(
    manifest: dict[str, Any], *, version: str, channel: str, key: str,
) -> None:
    """Validate only the stable identity needed to copy an immutable manifest.

    Promotion intentionally does not re-render the manifest.  Older immutable
    documents must remain promotable after this publisher's formatting or
    optional fields evolve.
    """
    schema = manifest.get("processing_core_manifest_schema_version")
    if schema not in (1, MANIFEST_SCHEMA_VERSION):
        raise RuntimeError(
            f"Existing immutable manifest has unsupported schema {schema!r}: {key}"
        )
    for field in ("contract_version", "profile_catalog_url", "emodulus_lut_manifest_url"):
        if manifest.get(field) in (None, ""):
            raise RuntimeError(f"Existing immutable manifest is missing {field}: {key}")

    wheel = manifest.get("wheel")
    if not isinstance(wheel, dict):
        raise RuntimeError(f"Existing immutable manifest has no wheel object: {key}")
    for field in ("package", "version", "release_tag", "wheels"):
        if wheel.get(field) in (None, ""):
            raise RuntimeError(f"Existing immutable manifest wheel is missing {field}: {key}")
    if wheel.get("package") != PEP503_PACKAGE:
        raise RuntimeError(f"Existing immutable manifest names an unexpected wheel package: {key}")
    if not isinstance(wheel.get("wheels"), list) or not wheel["wheels"]:
        raise RuntimeError(f"Existing immutable manifest has no wheel artifacts: {key}")
    if manifest.get("channel") != channel:
        raise RuntimeError(
            f"Existing immutable manifest channel is {manifest.get('channel')!r}, expected {channel!r}: {key}"
        )
    manifest_version = manifest.get("version", wheel.get("version"))
    if manifest_version != version or wheel.get("version") != version:
        raise RuntimeError(
            f"Existing immutable manifest does not identify version {version!r}: {key}"
        )
    try:
        tagged_version = version_from_release_tag(str(wheel["release_tag"]))
    except ValueError as exc:
        raise RuntimeError(f"Existing immutable manifest has an invalid release tag: {key}") from exc
    if tagged_version != version:
        raise RuntimeError(
            f"Existing immutable manifest release tag does not match {version!r}: {key}"
        )
    if schema >= 2 and not isinstance(manifest.get("native_plugins"), list):
        raise RuntimeError(f"Existing schema-v2 manifest has invalid native_plugins: {key}")


def promote_existing_version(args: argparse.Namespace) -> int:
    """Promote an existing immutable document without reconstructing its bytes."""
    try:
        channel = validate_channel(args.channel)
        version = validate_version(args.promote_version)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if args.from_release or args.wheels or args.native_descriptors or args.release_assets_dir:
        print(
            "ERROR: --promote-version cannot be combined with release or local artifact inputs",
            file=sys.stderr,
        )
        return 1
    if not args.dry_run and not args.published_at:
        print(
            "ERROR: --published-at is required for a reproducible non-dry-run promotion",
            file=sys.stderr,
        )
        return 1

    base_key = f"{channel}/processing-core"
    version_key = f"{base_key}/versions/{version_object_component(version)}.json"
    index_key = f"{base_key}/index.json"
    latest_key = f"{base_key}/latest.json"
    pep503_key = f"{base_key}/simple/{PEP503_PACKAGE}/index.html"
    pep503_route_key = f"{base_key}/simple/{PEP503_PACKAGE}/"

    immutable_bytes, immutable_read_ok = read_existing_object(args, version_key)
    index_bytes, index_read_ok = read_existing_object(args, index_key)
    if not immutable_read_ok or immutable_bytes is None:
        print(f"ERROR: immutable version is unavailable; refusing promotion: {version_key}", file=sys.stderr)
        return 1
    if not index_read_ok or index_bytes is None:
        print(f"ERROR: catalog is unavailable; refusing promotion: {index_key}", file=sys.stderr)
        return 1

    try:
        manifest = parse_existing_json(immutable_bytes, version_key)
        _validate_promotion_manifest(
            manifest, version=version, channel=channel, key=version_key,
        )
        index = parse_existing_json(index_bytes, index_key)
        if index.get("channel") != channel:
            raise RuntimeError(
                f"Existing catalog channel is {index.get('channel')!r}, expected {channel!r}"
            )
        if index.get("processing_core_index_schema_version") != INDEX_SCHEMA_VERSION:
            raise RuntimeError(
                "Existing catalog has an unsupported processing-core index schema"
            )
        versions = index.get("versions")
        if not isinstance(versions, list) or not any(
            isinstance(entry, dict) and entry.get("version") == version for entry in versions
        ):
            raise RuntimeError(
                f"Existing catalog does not contain immutable version {version!r}"
            )
        promoted_index = dict(index)
        promoted_index["active_version"] = version
        promoted_index["updated_at"] = validate_published_at(args.published_at or utc_now())
    except (RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="processing_core_promotion_") as output_temp:
        output_dir = Path(output_temp)
        latest_path = output_dir / "latest.json"
        index_path = output_dir / "index.json"
        pep503_path = output_dir / "index.html"
        latest_path.write_bytes(immutable_bytes)
        write_json(index_path, promoted_index)
        pep503_path.write_text(render_pep503_index(promoted_index), encoding="utf-8")
        _copy_preview(args.manifest_out, latest_path)
        _copy_preview(args.version_manifest_out, latest_path)
        _copy_preview(args.index_out, index_path)
        _copy_preview(args.pep503_out, pep503_path)

        print(f"Promoting existing processing core {version!r} on channel {channel!r}")
        print(f"Immutable source: {join_public_object_url(args.public_base_url, version_key)}")
        print(f"Active pointer: {join_public_object_url(args.public_base_url, latest_key)}")
        if args.dry_run:
            print("DRY RUN: validated immutable bytes and catalog; skipped mutable uploads")
            return 0

        try:
            # latest.json is the canonical active pointer and is always written
            # last. Selectors must not treat index.active_version as canonical.
            upload_object(
                args=args, key=index_key, file_path=index_path,
                content_type="application/json", cache_control=MUTABLE_CACHE_CONTROL,
            )
            upload_object(
                args=args, key=pep503_key, file_path=pep503_path,
                content_type="text/html; charset=utf-8", cache_control=MUTABLE_CACHE_CONTROL,
            )
            upload_object(
                args=args, key=pep503_route_key, file_path=pep503_path,
                content_type="text/html; charset=utf-8", cache_control=MUTABLE_CACHE_CONTROL,
            )
            upload_object(
                args=args, key=latest_key, file_path=latest_path,
                content_type="application/json", cache_control=MUTABLE_CACHE_CONTROL,
            )
        except Exception as exc:
            print(f"ERROR: promotion failed: {exc}", file=sys.stderr)
            return 1
    print("=== Promotion Complete ===")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    print("=== Publishing Processing Core Registry ===")

    try:
        require_consistent_mutating_transport(args)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if args.promote_version is not None:
        return promote_existing_version(args)

    if args.release_assets_dir and not args.from_release:
        print("ERROR: --release-assets-dir requires --from-release", file=sys.stderr)
        return 1
    if args.from_release and (args.wheels or args.native_descriptors):
        print("ERROR: use either --from-release or explicit --wheel/--native-plugin-descriptor", file=sys.stderr)
        return 1
    if not args.dry_run and not args.from_release and not args.published_at:
        print(
            "ERROR: --published-at is required for reproducible explicit publication",
            file=sys.stderr,
        )
        return 1
    if (
        not args.dry_run
        and args.from_release
        and args.release_assets_dir
        and not args.published_at
    ):
        print(
            "ERROR: fixture-backed publication requires --published-at",
            file=sys.stderr,
        )
        return 1

    release_temp: tempfile.TemporaryDirectory[str] | None = None
    try:
        pyproject_version = read_wheel_version(Path(args.pyproject))
        if args.from_release:
            release_tag = args.from_release
            tagged_version = version_from_release_tag(release_tag)
            wheel_version = args.wheel_version or tagged_version
            if wheel_version != tagged_version:
                raise ValueError(
                    f"--wheel-version {wheel_version} does not match --from-release {release_tag}"
                )
            if pyproject_version != wheel_version:
                raise ValueError(
                    f"Authoritative pyproject version {pyproject_version} does not match release tag {release_tag}"
                )
            release_metadata: dict[str, Any] = {}
            if args.release_assets_dir:
                asset_dir = Path(args.release_assets_dir)
                if not asset_dir.is_dir():
                    raise ValueError(f"Release assets directory does not exist: {asset_dir}")
            else:
                release_metadata = inspect_github_release(args.repo, release_tag, args.gh_bin)
                release_temp = tempfile.TemporaryDirectory(prefix="mib_processing_release_")
                asset_dir = Path(release_temp.name)
                download_github_release(args.repo, release_tag, asset_dir, args.gh_bin)
            wheel_paths = sorted(asset_dir.glob("*.whl"))
            descriptor_paths = discover_native_descriptors(asset_dir)
            release_published_at = release_metadata.get("publishedAt")
            if release_metadata and not args.published_at and not release_published_at:
                raise ValueError(
                    f"GitHub Release {release_tag} did not provide a stable publishedAt timestamp"
                )
            published_at = args.published_at or release_published_at or utc_now()
            if not wheel_paths:
                raise ValueError(f"GitHub Release {release_tag} contains no .whl assets")
        else:
            wheel_version = args.wheel_version or pyproject_version
            if wheel_version != pyproject_version:
                raise ValueError(
                    f"--wheel-version {wheel_version} does not match authoritative "
                    f"pyproject version {pyproject_version}"
                )
            release_tag = args.release_tag or f"{_TAG_PREFIX}{wheel_version}"
            version_from_release_tag(release_tag)
            wheel_paths = [Path(value) for value in args.wheels]
            descriptor_paths = [Path(value) for value in args.native_descriptors]
            asset_dir = descriptor_paths[0].parent if descriptor_paths else Path.cwd()
            if descriptor_paths and any(path.parent.resolve() != asset_dir.resolve() for path in descriptor_paths):
                raise ValueError("All native plugin descriptors and DLLs must share one asset directory")
            published_at = args.published_at or utc_now()

        native_plugins = build_native_plugin_entries(
            descriptor_paths,
            asset_dir=asset_dir,
            repo=args.repo,
            release_tag=release_tag,
            expected_version=wheel_version,
            expected_contract_version=args.contract_version,
        )
        manifest = build_manifest(
            channel=args.channel,
            contract_version=args.contract_version,
            wheel_version=wheel_version,
            release_tag=release_tag,
            repo=args.repo,
            wheel_paths=wheel_paths,
            public_base_url=args.public_base_url,
            native_plugins=native_plugins,
            published_at=published_at,
        )
    except (RuntimeError, ValueError, FileNotFoundError) as exc:
        if release_temp is not None:
            release_temp.cleanup()
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    version_component = version_object_component(wheel_version)
    base_key = f"{args.channel}/processing-core"
    latest_key = f"{base_key}/latest.json"
    version_key = f"{base_key}/versions/{version_component}.json"
    index_key = f"{base_key}/index.json"
    pep503_key = f"{base_key}/simple/{PEP503_PACKAGE}/index.html"
    # R2 custom domains serve object keys literally and do not synthesize
    # directory indexes. Pip requests the normalized project URL with a
    # trailing slash, so publish the same HTML at that exact key too.
    pep503_route_key = f"{base_key}/simple/{PEP503_PACKAGE}/"

    with tempfile.TemporaryDirectory(prefix="processing_core_registry_") as output_temp:
        output_dir = Path(output_temp)
        latest_path = output_dir / "latest.json"
        version_path = output_dir / "version.json"
        index_path = output_dir / "index.json"
        pep503_path = output_dir / "index.html"
        write_json(latest_path, manifest)
        write_json(version_path, manifest)

        if args.dry_run:
            index = merge_index({}, manifest, args.public_base_url)
        else:
            existing_version, version_read_ok = read_existing_object(args, version_key)
            existing_index_bytes, index_read_ok = read_existing_object(args, index_key)
            try:
                upload_version = resolve_immutable_update(
                    existing_version, version_read_ok, manifest, version_key,
                )
                if not index_read_ok:
                    raise RuntimeError(
                        f"Refusing to publish because existing catalog could not be read: {index_key}"
                    )
                existing_index = parse_existing_json(existing_index_bytes, index_key)
                index = merge_index(existing_index, manifest, args.public_base_url)
            except (RuntimeError, ValueError) as exc:
                if release_temp is not None:
                    release_temp.cleanup()
                print(f"ERROR: {exc}", file=sys.stderr)
                return 1

        write_json(index_path, index)
        pep503_path.write_text(render_pep503_index(index), encoding="utf-8")
        _copy_preview(args.manifest_out, latest_path)
        _copy_preview(args.version_manifest_out, version_path)
        _copy_preview(args.index_out, index_path)
        _copy_preview(args.pep503_out, pep503_path)

        print(f"Channel: {args.channel}")
        print(f"Contract version: {args.contract_version}")
        print(f"Core version: {wheel_version}")
        print(f"Release assets: {len(wheel_paths)} wheel(s), {len(native_plugins)} native plugin(s)")
        print(f"Latest: {join_public_object_url(args.public_base_url, latest_key)}")
        print(f"Immutable: {join_public_object_url(args.public_base_url, version_key)}")
        print(f"Catalog: {join_public_object_url(args.public_base_url, index_key)}")
        print(f"PEP 503: {join_public_object_url(args.public_base_url, pep503_key)}")

        if args.dry_run:
            print("\nDRY RUN: skipped R2 uploads")
            print(f"Would upload immutable manifest first: s3://{args.bucket}/{version_key}")
            print(f"Would upload catalog and package index: s3://{args.bucket}/{index_key}")
            print(f"Would upload pip project route: s3://{args.bucket}/{pep503_route_key}")
            print(f"Would promote active pointer last: s3://{args.bucket}/{latest_key}")
            if release_temp is not None:
                release_temp.cleanup()
            return 0

        try:
            if upload_version:
                print(f"\n1. Uploading immutable {version_key}...")
                upload_object(
                    args=args, key=version_key, file_path=version_path,
                    content_type="application/json", cache_control=IMMUTABLE_CACHE_CONTROL,
                )
            else:
                print(f"\n1. Immutable {version_key} is identical; skipping upload")

            print(f"2. Updating {index_key}...")
            upload_object(
                args=args, key=index_key, file_path=index_path,
                content_type="application/json", cache_control=MUTABLE_CACHE_CONTROL,
            )
            print(f"3. Updating {pep503_key}...")
            upload_object(
                args=args, key=pep503_key, file_path=pep503_path,
                content_type="text/html; charset=utf-8", cache_control=MUTABLE_CACHE_CONTROL,
            )
            print(f"4. Updating pip route {pep503_route_key}...")
            upload_object(
                args=args, key=pep503_route_key, file_path=pep503_path,
                content_type="text/html; charset=utf-8", cache_control=MUTABLE_CACHE_CONTROL,
            )
            print(f"5. Promoting {latest_key} last...")
            upload_object(
                args=args, key=latest_key, file_path=latest_path,
                content_type="application/json", cache_control=MUTABLE_CACHE_CONTROL,
            )
        except Exception as exc:
            print(f"ERROR: publish failed: {exc}", file=sys.stderr)
            return 1
        finally:
            if release_temp is not None:
                release_temp.cleanup()

    print("\n=== Publish Complete ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
