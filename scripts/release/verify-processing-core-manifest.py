#!/usr/bin/env python3
"""Verify that a processing-core manifest and everything it points at are publicly reachable.

See docs/portable-processing-sync.md. Deliberately dependency-free (stdlib
urllib/json/re only) to demonstrate a non-Qt consumer needs nothing but an
HTTP client to resolve config + LUT + engine as one pinned set.
"""
from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen


DEFAULT_MANIFEST_URL = "https://updates.yofo.bio/stable/processing-core/latest.json"
DEFAULT_HEADERS = {"User-Agent": "MIB-Studio-Processing-Core-Verifier/1.0"}
SHA256_RE = re.compile(r"[0-9a-fA-F]{64}")
NATIVE_LIBRARY_SUFFIXES = {
    "windows": (".dll",),
    "linux": (".so",),
    "macos": (".dylib",),
}


def fetch(url: str, *, method: str = "GET", headers: dict[str, str] | None = None, timeout: int = 30):
    request_headers = dict(DEFAULT_HEADERS)
    if headers:
        request_headers.update(headers)
    request = Request(url, method=method, headers=request_headers)
    return urlopen(request, timeout=timeout)


def require_absolute_url(value: str, field_name: str) -> None:
    parsed = urlparse(value)
    if parsed.scheme != "https" or not parsed.netloc:
        raise ValueError(f"{field_name} is not an absolute HTTPS URL: {value}")


def check_reachable(url: str, *, timeout: int) -> tuple[bool, str]:
    try:
        with fetch(url, method="HEAD", timeout=timeout) as response:
            status = response.status
    except (HTTPError, URLError, TimeoutError):
        try:
            with fetch(url, method="GET", headers={"Range": "bytes=0-0"}, timeout=timeout) as response:
                status = response.status
                response.read(1)
        except (HTTPError, URLError, TimeoutError) as exc:
            return False, str(exc)
    if status < 200 or status >= 300:
        return False, f"HTTP {status}"
    return True, f"HTTP {status}"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest-url", default=DEFAULT_MANIFEST_URL)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument(
        "--skip-referenced",
        action="store_true",
        help="Only validate the manifest's own shape; skip reachability checks on the URLs it references.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    print("=== Verifying processing-core manifest ===")
    print(f"Manifest URL: {args.manifest_url}")

    try:
        with fetch(args.manifest_url, timeout=args.timeout) as response:
            # file:// responses have no HTTP status (status is None); a
            # successful open/read is itself the success signal there. This
            # lets --manifest-url file://... exercise the parsing/shape
            # checks below without network access (e.g. in CI or local dev).
            status = getattr(response, "status", None)
            body = response.read()
    except (HTTPError, URLError, TimeoutError) as exc:
        print(f"ERROR: manifest request failed: {exc}", file=sys.stderr)
        return 1

    if status is not None and (status < 200 or status >= 300):
        print(f"ERROR: manifest returned HTTP {status}", file=sys.stderr)
        return 1

    try:
        manifest = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        print(f"ERROR: manifest is not valid JSON: {exc}", file=sys.stderr)
        return 1

    for field in (
        "processing_core_manifest_schema_version",
        "channel",
        "contract_version",
        "wheel",
        "profile_catalog_url",
        "emodulus_lut_manifest_url",
    ):
        if field not in manifest:
            print(f"ERROR: manifest is missing required field: {field}", file=sys.stderr)
            return 1

    wheel = manifest["wheel"]
    for field in ("package", "version", "release_tag", "wheels"):
        if field not in wheel:
            print(f"ERROR: manifest.wheel is missing required field: {field}", file=sys.stderr)
            return 1

    if not isinstance(wheel["wheels"], list) or not wheel["wheels"]:
        print("ERROR: manifest.wheel.wheels must be a non-empty list", file=sys.stderr)
        return 1

    schema_version = manifest["processing_core_manifest_schema_version"]
    if schema_version not in (1, 2):
        print(f"ERROR: unsupported processing-core manifest schema: {schema_version!r}", file=sys.stderr)
        return 1
    if schema_version >= 2:
        if not isinstance(manifest.get("version"), str) or not manifest["version"]:
            print("ERROR: schema-v2 manifest requires a non-empty canonical version", file=sys.stderr)
            return 1
        if manifest["version"] != wheel["version"]:
            print("ERROR: manifest.version must equal manifest.wheel.version", file=sys.stderr)
            return 1
        if not isinstance(manifest.get("native_plugins"), list):
            print("ERROR: schema-v2 manifest.native_plugins must be a list", file=sys.stderr)
            return 1

    referenced_urls: list[tuple[str, str]] = []
    for entry in wheel["wheels"]:
        for field in ("platform_tag", "url", "sha256"):
            if field not in entry:
                print(f"ERROR: wheel entry is missing required field: {field}", file=sys.stderr)
                return 1
        if not SHA256_RE.fullmatch(str(entry["sha256"])):
            print(f"ERROR: wheel entry sha256 is not a 64-character hex string: {entry['platform_tag']}", file=sys.stderr)
            return 1
        if schema_version >= 2:
            if not entry.get("filename"):
                print("ERROR: schema-v2 wheel entry is missing filename", file=sys.stderr)
                return 1
            if not isinstance(entry.get("size_bytes"), int) or entry["size_bytes"] <= 0:
                print(f"ERROR: wheel entry has invalid size_bytes: {entry.get('filename')}", file=sys.stderr)
                return 1
        try:
            require_absolute_url(str(entry["url"]), f"wheel[{entry['platform_tag']}].url")
        except ValueError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        referenced_urls.append((f"wheel {entry['platform_tag']}", str(entry["url"])))

    for entry in manifest.get("native_plugins", []):
        required = (
            "filename", "os", "arch", "version", "contract_version",
            "engine_abi_version", "runtime_fingerprint", "entrypoint", "url", "sha256", "size_bytes",
        )
        missing = [field for field in required if entry.get(field) in (None, "")]
        if missing:
            print(f"ERROR: native plugin entry is missing: {', '.join(missing)}", file=sys.stderr)
            return 1
        if entry["version"] != manifest.get("version"):
            print("ERROR: native plugin version does not match manifest.version", file=sys.stderr)
            return 1
        if entry["contract_version"] != manifest["contract_version"]:
            print("ERROR: native plugin contract_version does not match manifest", file=sys.stderr)
            return 1
        if not SHA256_RE.fullmatch(str(entry["sha256"])):
            print(f"ERROR: native plugin sha256 is invalid: {entry['filename']}", file=sys.stderr)
            return 1
        if not isinstance(entry["size_bytes"], int) or entry["size_bytes"] <= 0:
            print(f"ERROR: native plugin size_bytes is invalid: {entry['filename']}", file=sys.stderr)
            return 1
        native_os = str(entry["os"]).strip().lower()
        suffixes = NATIVE_LIBRARY_SUFFIXES.get(native_os)
        if suffixes is None or not str(entry["filename"]).lower().endswith(suffixes):
            print(
                f"ERROR: native plugin filename does not match OS {native_os}: {entry['filename']}",
                file=sys.stderr,
            )
            return 1
        signing = entry.get("signing")
        if not isinstance(signing, dict) or signing.get("required") is not True or not signing.get("scheme"):
            print(f"ERROR: native plugin signing policy is invalid: {entry['filename']}", file=sys.stderr)
            return 1
        if str(signing.get("scheme")).strip().lower() == "ed25519":
            try:
                spki = base64.b64decode(str(signing.get("public_key_spki_base64") or ""), validate=True)
                signature_bytes = base64.b64decode(str(signing.get("signature_base64") or ""), validate=True)
            except (ValueError, binascii.Error):
                spki = b""
                signature_bytes = b""
            declared_spki_sha256 = str(signing.get("public_key_spki_sha256") or "").strip().lower()
            if (
                len(spki) != 44
                or len(signature_bytes) != 64
                or hashlib.sha256(spki).hexdigest() != declared_spki_sha256
            ):
                print(
                    f"ERROR: native plugin ed25519 signing material is invalid: {entry['filename']}",
                    file=sys.stderr,
                )
                return 1
        try:
            require_absolute_url(str(entry["url"]), f"native_plugins[{entry['filename']}].url")
        except ValueError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        referenced_urls.append((f"native plugin {entry['filename']}", str(entry["url"])))

    for field_name, url_field in (
        ("profile_catalog_url", "profile_catalog_url"),
        ("emodulus_lut_manifest_url", "emodulus_lut_manifest_url"),
    ):
        try:
            require_absolute_url(str(manifest[url_field]), field_name)
        except ValueError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        referenced_urls.append((field_name, str(manifest[url_field])))

    print(f"Channel: {manifest['channel']}")
    print(f"Contract version: {manifest['contract_version']}")
    print(f"Wheel: {wheel['package']} {wheel['version']} ({wheel['release_tag']}, {len(wheel['wheels'])} platform(s))")
    if schema_version >= 2:
        print(f"Native plugins: {len(manifest['native_plugins'])}")
    print(f"Profile catalog: {manifest['profile_catalog_url']}")
    print(f"Emodulus LUT manifest: {manifest['emodulus_lut_manifest_url']}")

    if args.skip_referenced:
        print("\n--skip-referenced set: not checking reachability of referenced URLs.")
        print("Manifest shape is valid.")
        return 0

    print("\nChecking reachability of referenced URLs...")
    all_ok = True
    for label, url in referenced_urls:
        ok, detail = check_reachable(url, timeout=args.timeout)
        status_text = "OK" if ok else "FAILED"
        print(f"  [{status_text}] {label}: {url} ({detail})")
        all_ok = all_ok and ok

    if not all_ok:
        print("\nERROR: one or more referenced URLs are not reachable.", file=sys.stderr)
        return 1

    print("\nManifest and all referenced resources are publicly reachable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
