#!/usr/bin/env python3
"""Atomically bump mib-processing's authoritative and public Python versions.

``bindings/python/pyproject.toml`` is the source of truth.  The package-level
``__version__`` remains a literal for import-time compatibility and is updated
in the same transaction.  ``--create-tag`` is intentionally a second-step
operation: it succeeds only when the version files are already committed at
HEAD, preventing a tag that points to the pre-bump commit.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


PYPROJECT = Path("bindings/python/pyproject.toml")
PACKAGE_INIT = Path("bindings/python/python/mib_processing/__init__.py")
TAG_PREFIX = "mib-processing-v"
# ADR 0007: one release tag per core line (see .github/workflows/python-wheel.yml).
LINE_TAG_PREFIXES = {
    "subtract-ring": TAG_PREFIX,
    "absdiff-laplacian": "mib-processing-absdiff-laplacian-v",
}
_SAFE_VERSION = re.compile(r"^[0-9][A-Za-z0-9._+!-]*$")
_PYPROJECT_VERSION = re.compile(r'(?m)^(version\s*=\s*)"([^"]+)"\s*$')
_PACKAGE_VERSION = re.compile(r'(?m)^(__version__\s*=\s*)"([^"]+)"\s*$')


def validate_version(version: str) -> str:
    if not _SAFE_VERSION.fullmatch(version) or "/" in version or "\\" in version:
        raise ValueError(f"Invalid mib-processing version: {version!r}")
    return version


def _replace_one(text: str, pattern: re.Pattern[str], version: str, source: Path) -> tuple[str, str]:
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one version literal in {source}, found {len(matches)}")
    current = matches[0].group(2)
    updated = pattern.sub(lambda match: f'{match.group(1)}"{version}"', text, count=1)
    return updated, current


def plan_updates(repo_root: Path, version: str) -> tuple[dict[Path, str], str]:
    version = validate_version(version)
    pyproject_path = repo_root / PYPROJECT
    package_path = repo_root / PACKAGE_INIT
    pyproject_text = pyproject_path.read_text(encoding="utf-8")
    package_text = package_path.read_text(encoding="utf-8")
    updated_pyproject, pyproject_current = _replace_one(
        pyproject_text, _PYPROJECT_VERSION, version, PYPROJECT,
    )
    updated_package, package_current = _replace_one(
        package_text, _PACKAGE_VERSION, version, PACKAGE_INIT,
    )
    if pyproject_current != package_current:
        raise ValueError(
            f"Existing wheel version literals disagree: {PYPROJECT}={pyproject_current}, "
            f"{PACKAGE_INIT}={package_current}"
        )
    return {
        pyproject_path: updated_pyproject,
        package_path: updated_package,
    }, pyproject_current


def _stage_replacement(path: Path, content: str) -> Path:
    handle = tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="", prefix=f".{path.name}.", suffix=".tmp",
        dir=path.parent, delete=False,
    )
    staged = Path(handle.name)
    try:
        with handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(staged, path.stat().st_mode)
        return staged
    except Exception:
        staged.unlink(missing_ok=True)
        raise


def apply_updates_atomically(updates: dict[Path, str]) -> None:
    """Stage both files, replace them, and roll back both on any failure."""
    originals = {path: path.read_bytes() for path in updates}
    staged: dict[Path, Path] = {}
    replaced: list[Path] = []
    try:
        staged = {path: _stage_replacement(path, content) for path, content in updates.items()}
        for path, temporary in staged.items():
            os.replace(temporary, path)
            replaced.append(path)
    except Exception:
        for path in replaced:
            rollback = _stage_replacement(path, originals[path].decode("utf-8"))
            os.replace(rollback, path)
        raise
    finally:
        for temporary in staged.values():
            temporary.unlink(missing_ok=True)


def _git(repo_root: Path, *args: str, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args], cwd=repo_root, check=True, text=True,
        capture_output=capture,
    )


def create_committed_tag(repo_root: Path, version: str, message: str | None = None,
                         line: str = "subtract-ring") -> str:
    """Create the release tag only when HEAD already contains the bumped literals."""
    if line not in LINE_TAG_PREFIXES:
        raise ValueError(f"Unknown core line {line!r}; expected one of {sorted(LINE_TAG_PREFIXES)}")
    tag = f"{LINE_TAG_PREFIXES[line]}{validate_version(version)}"
    dirty = _git(
        repo_root, "status", "--porcelain", "--", str(PYPROJECT), str(PACKAGE_INIT), capture=True,
    ).stdout.strip()
    if dirty:
        raise RuntimeError(
            "Version files are not committed. Commit the bump, then rerun with --create-tag "
            "so the tag points at the versioned commit."
        )

    for relative_path, pattern in ((PYPROJECT, _PYPROJECT_VERSION), (PACKAGE_INIT, _PACKAGE_VERSION)):
        head_text = _git(repo_root, "show", f"HEAD:{relative_path.as_posix()}", capture=True).stdout
        match = pattern.search(head_text)
        if match is None or match.group(2) != version:
            raise RuntimeError(f"HEAD does not contain version {version} in {relative_path}")

    existing = subprocess.run(
        ["git", "rev-parse", "-q", "--verify", f"refs/tags/{tag}"],
        cwd=repo_root, text=True, capture_output=True,
    )
    if existing.returncode == 0:
        raise RuntimeError(f"Tag already exists: {tag}")
    default_message = (f"mib-processing {version}" if line == "subtract-ring"
                       else f"mib-processing {line} {version}")
    _git(repo_root, "tag", "-a", tag, "-m", message or default_message)
    return tag


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="New wheel version, for example 0.2.0 or 0.2.0rc1")
    parser.add_argument(
        "--repo-root", default=str(Path(__file__).resolve().parents[1]),
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--dry-run", action="store_true", help="Validate and show changes without writing")
    parser.add_argument(
        "--create-tag", action="store_true",
        help="Create the line's annotated release tag (mib-processing-v<version> for "
             "subtract-ring); requires the version files committed at HEAD",
    )
    parser.add_argument(
        "--line", choices=sorted(LINE_TAG_PREFIXES), default="subtract-ring",
        help="Core line to tag (ADR 0007). absdiff-laplacian creates "
             "mib-processing-absdiff-laplacian-v<version>.",
    )
    parser.add_argument("--tag-message", default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    try:
        updates, current = plan_updates(repo_root, args.version)
        changed = any(path.read_text(encoding="utf-8") != content for path, content in updates.items())
        print(f"mib-processing version: {current} -> {args.version}")
        for path in updates:
            print(f"  {path.relative_to(repo_root)}")

        if args.dry_run:
            print("DRY RUN: no files or tags changed")
            return 0

        if args.create_tag and changed:
            raise RuntimeError(
                "--create-tag is a post-commit step. Run the bump without --create-tag, "
                "commit both version files, then rerun with --create-tag."
            )

        if changed:
            apply_updates_atomically(updates)
            print("Updated both version literals atomically.")
        else:
            print("Version literals already match; no files changed.")

        if args.create_tag:
            tag = create_committed_tag(repo_root, args.version, args.tag_message, args.line)
            print(f"Created annotated tag: {tag}")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
