#!/usr/bin/env python3
"""Verify that an update manifest and referenced installer are publicly reachable."""
from __future__ import annotations

import argparse
import json
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen


DEFAULT_MANIFEST_URL = "https://updates.yofo.bio/stable/latest.json"
DEFAULT_HEADERS = {"User-Agent": "MIB-Studio-Update-Verifier/1.0"}


def fetch(url: str, *, method: str = "GET", headers: dict[str, str] | None = None, timeout: int = 30):
    request_headers = dict(DEFAULT_HEADERS)
    if headers:
        request_headers.update(headers)
    request = Request(url, method=method, headers=request_headers)
    return urlopen(request, timeout=timeout)


def require_absolute_url(value: str, field_name: str) -> None:
    parsed = urlparse(value)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError(f"Manifest {field_name} is not an absolute HTTP(S) URL: {value}")


def get_header(headers, name: str) -> str | None:
    return headers.get(name)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest-url", default=DEFAULT_MANIFEST_URL)
    parser.add_argument("--timeout", type=int, default=30)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    print("=== Verifying MIB Studio update manifest ===")
    print(f"Manifest URL: {args.manifest_url}")

    try:
        with fetch(args.manifest_url, timeout=args.timeout) as response:
            status = response.status
            body = response.read()
            manifest_headers = response.headers
    except (HTTPError, URLError, TimeoutError) as exc:
        print(f"ERROR: manifest request failed: {exc}", file=sys.stderr)
        return 1

    if status < 200 or status >= 300:
        print(f"ERROR: manifest returned HTTP {status}", file=sys.stderr)
        return 1

    try:
        manifest = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        print(f"ERROR: manifest is not valid JSON: {exc}", file=sys.stderr)
        return 1

    for field in ("version", "installer_url", "installer_sha256", "installer_size_bytes"):
        if field not in manifest:
            print(f"ERROR: manifest is missing required field: {field}", file=sys.stderr)
            return 1

    try:
        require_absolute_url(str(manifest["installer_url"]), "installer_url")
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    sha256 = str(manifest["installer_sha256"])
    if not re.fullmatch(r"[0-9a-fA-F]{64}", sha256):
        print("ERROR: manifest installer_sha256 is not a 64-character SHA-256 hex string", file=sys.stderr)
        return 1

    try:
        expected_size = int(manifest["installer_size_bytes"])
    except (TypeError, ValueError):
        print("ERROR: manifest installer_size_bytes is not an integer", file=sys.stderr)
        return 1
    if expected_size <= 0:
        print("ERROR: manifest installer_size_bytes must be greater than zero", file=sys.stderr)
        return 1

    print(f"Manifest Content-Type: {get_header(manifest_headers, 'Content-Type')}")
    print(f"Version: {manifest['version']}")
    print(f"Installer URL: {manifest['installer_url']}")

    used_range_probe = False
    try:
        with fetch(str(manifest["installer_url"]), method="HEAD", timeout=args.timeout) as response:
            installer_status = response.status
            installer_headers = response.headers
    except (HTTPError, URLError, TimeoutError):
        used_range_probe = True
        try:
            with fetch(
                str(manifest["installer_url"]),
                method="GET",
                headers={"Range": "bytes=0-0"},
                timeout=args.timeout,
            ) as response:
                installer_status = response.status
                installer_headers = response.headers
                response.read(1)
        except (HTTPError, URLError, TimeoutError) as exc:
            print(f"ERROR: installer request failed: {exc}", file=sys.stderr)
            return 1

    if installer_status < 200 or installer_status >= 300:
        print(f"ERROR: installer returned HTTP {installer_status}", file=sys.stderr)
        return 1

    content_length = get_header(installer_headers, "Content-Length")
    if used_range_probe:
        content_range = get_header(installer_headers, "Content-Range")
        match = re.search(r"/(\d+)$", content_range or "")
        if match:
            actual_size = int(match.group(1))
            if actual_size != expected_size:
                print(
                    f"ERROR: installer Content-Range size {actual_size} does not match manifest size {expected_size}",
                    file=sys.stderr,
                )
                return 1
        else:
            print("Installer Content-Range header not present; size check skipped")
    elif content_length:
        actual_size = int(content_length)
        if actual_size != expected_size:
            print(
                f"ERROR: installer Content-Length {actual_size} does not match manifest size {expected_size}",
                file=sys.stderr,
            )
            return 1

    print(f"Installer Content-Type: {get_header(installer_headers, 'Content-Type')}")
    if content_length:
        print(f"Installer Content-Length: {content_length}")
    else:
        print("Installer Content-Length header not present; size check skipped")

    print("Manifest and installer are publicly reachable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
