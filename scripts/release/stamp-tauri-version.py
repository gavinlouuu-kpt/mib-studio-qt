#!/usr/bin/env python3
"""Stamp the repository version into the Tauri shell configs.

MIB Studio (Qt) takes its version from ``cmake/MIBVersion.cmake`` (the
``DEFAULT_VERSION`` fallback or the newest ``v*`` tag reachable from HEAD).
The React + Tauri products — MIB Studio and YOFO Review (plan
2026-10-01-standalone-review-app) — must carry the **same** version so one
``vX.Y.Z`` tag releases every product and a file recorded by X.Y opens in
X.Y. This script writes that version into ``desktop/src-tauri/tauri.conf.json``
(the base config both products share) and ``desktop/package.json``; the
``tauri.review.conf.json`` overlay inherits it through the config merge.

Usage::

    python3 scripts/release/stamp-tauri-version.py            # stamp
    python3 scripts/release/stamp-tauri-version.py --check    # exit 1 on drift
    python3 scripts/release/stamp-tauri-version.py --version 1.2.3-beta.1

Committed files carry the numeric ``X.Y.Z`` only, and ``--check`` compares
that core: develop merges cut ``vX.Y.Z-beta.<sha>`` tags (build-windows.yml),
and no commit can contain the SHA of a tag pointing at itself. Builds that
ship a pre-release stamp the full version (this script without ``--check``)
in their workspace, never in a commit.

Without ``--version`` the version is resolved like CMake does: the newest
``v*`` tag reachable from HEAD when it is at least ``DEFAULT_VERSION``, else
``DEFAULT_VERSION``. The pre-release suffix is kept (Tauri accepts semver).
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
VERSION_FILE = REPO_ROOT / "cmake" / "MIBVersion.cmake"
TAURI_CONF = REPO_ROOT / "desktop" / "src-tauri" / "tauri.conf.json"
PACKAGE_JSON = REPO_ROOT / "desktop" / "package.json"

_DEFAULT_VERSION = re.compile(r'(?m)^set\(DEFAULT_VERSION\s+"(?P<version>[0-9]+\.[0-9]+\.[0-9]+)"\)\s*$')
_TAG = re.compile(r"^v?(?P<version>[0-9]+\.[0-9]+\.[0-9]+(?:-beta\.[0-9A-Za-z][0-9A-Za-z.-]*)?)$")


def numeric(version: str) -> tuple[int, int, int]:
    core = version.split("-", 1)[0]
    major, minor, patch = (int(part) for part in core.split("."))
    return major, minor, patch


def default_version() -> str:
    match = _DEFAULT_VERSION.search(VERSION_FILE.read_text(encoding="utf-8"))
    if match is None:
        raise SystemExit(f"{VERSION_FILE}: DEFAULT_VERSION not found")
    return match.group("version")


def git_tag_version() -> str | None:
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--abbrev=0", "--match", "v*"],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    match = _TAG.match(out)
    return match.group("version") if match else None


def resolve_version() -> str:
    fallback = default_version()
    tagged = git_tag_version()
    if tagged is not None and numeric(tagged) >= numeric(fallback):
        return tagged
    return fallback


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", help="explicit version (default: resolved from tags / DEFAULT_VERSION)")
    parser.add_argument("--check", action="store_true", help="report drift instead of writing")
    args = parser.parse_args(argv)

    version = args.version or resolve_version()
    if not _TAG.match(version):
        raise SystemExit(f"not a release version: {version!r}")

    # --check: the committed numeric core (see the module docstring).
    expected = version.split("-", 1)[0] if args.check and not args.version else version
    drift = []
    for path in (TAURI_CONF, PACKAGE_JSON):
        data = read_json(path)
        if data.get("version") != expected:
            drift.append((path, data.get("version")))
            if not args.check:
                data["version"] = version
                write_json(path, data)

    if args.check:
        for path, current in drift:
            print(f"{path.relative_to(REPO_ROOT)}: version {current!r}, expected {expected!r}")
        if drift:
            return 1
        print(f"tauri version {expected}: in sync (resolved {version})")
        return 0

    for path, current in drift:
        print(f"{path.relative_to(REPO_ROOT)}: {current!r} -> {version!r}")
    if not drift:
        print(f"tauri version {version}: already stamped")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
