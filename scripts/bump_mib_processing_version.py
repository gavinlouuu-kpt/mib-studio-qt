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
# Processing-core lines (ADR 0007), each versioned independently. subtract-ring
# shares the wheel version (pyproject + __init__); other lines own one file.
LINES: dict[str, Path | None] = {
    "subtract-ring": None,
    "absdiff-laplacian": Path("processing-cores/absdiff-laplacian.version"),
}
# Lines whose mib-processing-<line>-v* tag triggers a release workflow
# (.github/workflows/python-wheel.yml).
RELEASED_LINES = {"subtract-ring"}
_SAFE_VERSION = re.compile(r"^[0-9][A-Za-z0-9._+!-]*$")
_LINE_VERSION_FILE = re.compile(r"^([0-9][A-Za-z0-9._+!-]*)\r?\n$")
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


def _check_line(line: str) -> str:
    if line not in LINES:
        raise ValueError(f"Unknown processing-core line: {line!r}")
    return line


def tag_for(line: str, version: str) -> str:
    """Release tag of one core line: mib-processing-<line>-v<version>."""
    return f"mib-processing-{_check_line(line)}-v{validate_version(version)}"


def _line_version(text: str, source: Path) -> str:
    match = _LINE_VERSION_FILE.fullmatch(text)
    if match is None:
        raise ValueError(f"{source} must hold exactly one version line")
    return match.group(1)


def plan_updates(
    repo_root: Path, version: str, line: str = "subtract-ring"
) -> tuple[dict[Path, str], str]:
    version = validate_version(version)
    version_file = LINES[_check_line(line)]
    if version_file is not None:
        path = repo_root / version_file
        current = _line_version(path.read_text(encoding="utf-8"), version_file)
        return {path: f"{version}\n"}, current
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


def create_committed_tag(
    repo_root: Path, version: str, message: str | None = None, line: str = "subtract-ring"
) -> str:
    """Create the release tag only when HEAD already contains the bumped literals."""
    tag = tag_for(line, version)
    if line not in RELEASED_LINES:
        raise RuntimeError(
            f"{line} has no release workflow yet (T1.1b PR 2): a pushed {tag} would spend the "
            "version without building, signing or publishing anything. Bump the version only."
        )
    version_file = LINES[line]
    tracked = (PYPROJECT, PACKAGE_INIT) if version_file is None else (version_file,)
    dirty = _git(
        repo_root, "status", "--porcelain", "--", *(str(path) for path in tracked), capture=True,
    ).stdout.strip()
    if dirty:
        raise RuntimeError(
            "Version files are not committed. Commit the bump, then rerun with --create-tag "
            "so the tag points at the versioned commit."
        )

    for relative_path in tracked:
        head_text = _git(repo_root, "show", f"HEAD:{relative_path.as_posix()}", capture=True).stdout
        if version_file is None:
            pattern = _PYPROJECT_VERSION if relative_path == PYPROJECT else _PACKAGE_VERSION
            match = pattern.search(head_text)
            head_version = match.group(2) if match else None
        else:
            try:
                head_version = _line_version(head_text, relative_path)
            except ValueError:
                head_version = None
        if head_version != version:
            raise RuntimeError(f"HEAD does not contain version {version} in {relative_path}")

    existing = subprocess.run(
        ["git", "rev-parse", "-q", "--verify", f"refs/tags/{tag}"],
        cwd=repo_root, text=True, capture_output=True,
    )
    if existing.returncode == 0:
        raise RuntimeError(f"Tag already exists: {tag}")
    _git(repo_root, "tag", "-a", tag, "-m", message or f"mib-processing {line} {version}")
    return tag


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="New line version, for example 0.2.0 or 0.2.0rc1")
    parser.add_argument(
        "--line", choices=sorted(LINES), default="subtract-ring",
        help="Processing-core line to bump (subtract-ring also bumps the wheel)",
    )
    parser.add_argument(
        "--repo-root", default=str(Path(__file__).resolve().parents[1]),
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--dry-run", action="store_true", help="Validate and show changes without writing")
    parser.add_argument(
        "--create-tag", action="store_true",
        help="Create annotated mib-processing-<line>-v<version>; requires the version files committed at HEAD",
    )
    parser.add_argument("--tag-message", default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    try:
        updates, current = plan_updates(repo_root, args.version, args.line)
        changed = any(path.read_text(encoding="utf-8") != content for path, content in updates.items())
        print(f"mib-processing {args.line} version: {current} -> {args.version}")
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
