#!/usr/bin/env python3
"""Tests for scripts/bump_mib_processing_version.py."""
from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "bump_mib_processing_version.py"
_spec = importlib.util.spec_from_file_location("bump_mib_processing_version", SCRIPT_PATH)
bump = importlib.util.module_from_spec(_spec)
sys.modules["bump_mib_processing_version"] = bump
_spec.loader.exec_module(bump)


def make_repo(root: Path, pyproject_version: str = "0.1.0", package_version: str = "0.1.0") -> None:
    pyproject = root / bump.PYPROJECT
    package = root / bump.PACKAGE_INIT
    pyproject.parent.mkdir(parents=True)
    package.parent.mkdir(parents=True)
    pyproject.write_text(
        f'[project]\nname = "mib-processing"\nversion = "{pyproject_version}"\n', encoding="utf-8"
    )
    package.write_text(
        f'"""fixture"""\n__version__ = "{package_version}"\n', encoding="utf-8"
    )


class VersionBumpTest(unittest.TestCase):
    def test_updates_both_literals(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            make_repo(root)
            updates, current = bump.plan_updates(root, "0.2.0rc1")
            self.assertEqual(current, "0.1.0")
            bump.apply_updates_atomically(updates)
            self.assertIn('version = "0.2.0rc1"', (root / bump.PYPROJECT).read_text())
            self.assertIn('__version__ = "0.2.0rc1"', (root / bump.PACKAGE_INIT).read_text())

    def test_existing_drift_is_rejected_without_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            make_repo(root, "0.1.0", "9.9.9")
            before = {path: (root / path).read_bytes() for path in (bump.PYPROJECT, bump.PACKAGE_INIT)}
            with self.assertRaisesRegex(ValueError, "disagree"):
                bump.plan_updates(root, "0.2.0")
            self.assertEqual(before, {path: (root / path).read_bytes() for path in before})

    def test_cli_dry_run_does_not_write(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            make_repo(root)
            result = bump.main(["0.2.0", "--repo-root", str(root), "--dry-run"])
            self.assertEqual(result, 0)
            self.assertIn('version = "0.1.0"', (root / bump.PYPROJECT).read_text())

    def test_tag_requires_committed_bump_then_tags_that_commit(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            make_repo(root)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "test@example.invalid"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "Test"], cwd=root, check=True)
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "initial"], cwd=root, check=True)

            updates, _ = bump.plan_updates(root, "0.2.0")
            bump.apply_updates_atomically(updates)
            with self.assertRaisesRegex(RuntimeError, "not committed"):
                bump.create_committed_tag(root, "0.2.0")

            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "bump"], cwd=root, check=True)
            tag = bump.create_committed_tag(root, "0.2.0")
            self.assertEqual(tag, "mib-processing-v0.2.0")
            tagged = subprocess.run(
                ["git", "show", f"{tag}:{bump.PYPROJECT.as_posix()}"],
                cwd=root, check=True, capture_output=True, text=True,
            ).stdout
            self.assertIn('version = "0.2.0"', tagged)


if __name__ == "__main__":
    unittest.main()
