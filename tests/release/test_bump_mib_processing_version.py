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
            self.assertEqual(tag, "mib-processing-subtract-ring-v0.2.0")
            tagged = subprocess.run(
                ["git", "show", f"{tag}:{bump.PYPROJECT.as_posix()}"],
                cwd=root, check=True, capture_output=True, text=True,
            ).stdout
            self.assertIn('version = "0.2.0"', tagged)

    def test_absdiff_line_bumps_only_its_version_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            make_repo(root)
            version_file = root / "processing-cores" / "absdiff-laplacian.version"
            version_file.parent.mkdir(parents=True)
            version_file.write_text("0.1.0\n", encoding="utf-8")
            updates, current = bump.plan_updates(root, "0.2.0", line="absdiff-laplacian")
            self.assertEqual(current, "0.1.0")
            self.assertEqual(set(updates), {version_file})
            self.assertEqual(updates[version_file], "0.2.0\n")

    def test_absdiff_version_file_must_hold_one_version_line(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            version_file = root / "processing-cores" / "absdiff-laplacian.version"
            version_file.parent.mkdir(parents=True)
            version_file.write_text("0.1.0\n0.2.0\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "exactly one version line"):
                bump.plan_updates(root, "0.2.0", line="absdiff-laplacian")

    def test_tag_prefix_per_line(self) -> None:
        self.assertEqual(bump.tag_for("subtract-ring", "0.3.3"), "mib-processing-subtract-ring-v0.3.3")
        self.assertEqual(
            bump.tag_for("absdiff-laplacian", "0.1.0"), "mib-processing-absdiff-laplacian-v0.1.0"
        )
        with self.assertRaisesRegex(ValueError, "Unknown processing-core line"):
            bump.tag_for("ring", "0.1.0")

    def test_absdiff_tag_after_committed_bump(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            make_repo(root)
            version_file = root / "processing-cores" / "absdiff-laplacian.version"
            version_file.parent.mkdir(parents=True)
            version_file.write_text("0.1.0\n", encoding="utf-8")
            for args in (["init", "-q"], ["config", "user.email", "test@example.invalid"],
                         ["config", "user.name", "Test"], ["add", "."], ["commit", "-qm", "initial"]):
                subprocess.run(["git", *args], cwd=root, check=True)
            updates, _ = bump.plan_updates(root, "0.2.0", line="absdiff-laplacian")
            bump.apply_updates_atomically(updates)
            with self.assertRaisesRegex(RuntimeError, "not committed"):
                bump.create_committed_tag(root, "0.2.0", line="absdiff-laplacian")
            subprocess.run(["git", "commit", "-qam", "bump"], cwd=root, check=True)
            # processing-core-line.yml releases absdiff-laplacian tags (T1.1b PR 2).
            tag = bump.create_committed_tag(root, "0.2.0", line="absdiff-laplacian")
            self.assertEqual(tag, "mib-processing-absdiff-laplacian-v0.2.0")
            head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                                  capture_output=True, text=True).stdout.strip()
            tagged = subprocess.run(["git", "rev-parse", f"{tag}^{{commit}}"], cwd=root, check=True,
                                    capture_output=True, text=True).stdout.strip()
            self.assertEqual(tagged, head)

    def test_publisher_uses_subtract_ring_tag_prefix(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "publish_processing_core", SCRIPT_PATH.parent / "release" / "publish-processing-core.py"
        )
        publisher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(publisher)
        self.assertEqual(publisher._TAG_PREFIX, "mib-processing-subtract-ring-v")


if __name__ == "__main__":
    unittest.main()
