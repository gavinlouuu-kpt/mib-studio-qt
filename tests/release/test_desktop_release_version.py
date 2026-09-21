"""Regression tests for desktop release version and artifact identity."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from resolve_desktop_release_version import (  # noqa: E402
    Version,
    bump_version,
    resolve_current_version,
)


class DesktopReleaseVersionResolverTest(unittest.TestCase):
    def test_higher_reachable_beta_tag_outranks_stale_default(self) -> None:
        current = resolve_current_version(
            Version.parse("1.0.3"),
            ["v1.0.4", "v1.0.6-beta.1", "v1.0.6-beta.3"],
        )
        self.assertEqual(current, Version.parse("1.0.6"))
        self.assertEqual(bump_version(current, "patch"), Version.parse("1.0.7"))

    def test_unrelated_and_malformed_tags_do_not_change_version(self) -> None:
        current = resolve_current_version(
            Version.parse("2.3.4"),
            ["mib-processing-v9.0.0", "v2.3", "v2.3.5-rc.1", "notes"],
        )
        self.assertEqual(current, Version.parse("2.3.4"))

    def test_cli_uses_highest_release_tag_reachable_from_head(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "cmake").mkdir()
            (root / "cmake" / "MIBVersion.cmake").write_text(
                'set(DEFAULT_VERSION "1.0.3")\n', encoding="utf-8"
            )
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(
                ["git", "config", "user.email", "release-test@example.invalid"],
                cwd=root,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Release Test"], cwd=root, check=True
            )
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            subprocess.run(["git", "tag", "v1.0.6-beta.3"], cwd=root, check=True)
            (root / "after-tag.txt").write_text("head\n", encoding="utf-8")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "head"], cwd=root, check=True)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPTS_DIR / "resolve_desktop_release_version.py"),
                    "--repo-root",
                    str(root),
                    "--bump",
                    "patch",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            payload = json.loads(result.stdout)
            self.assertEqual(payload["default_version"], "1.0.3")
            self.assertEqual(payload["current_version"], "1.0.6")
            self.assertEqual(payload["next_version"], "1.0.7")


class DesktopReleaseCMakeOverrideTest(unittest.TestCase):
    def run_module(self, version: str, full_version: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "identity.txt"
            wrapper = Path(directory) / "verify.cmake"
            wrapper.write_text(
                textwrap.dedent(
                    f"""
                    set(CMAKE_SOURCE_DIR "{REPO_ROOT.as_posix()}")
                    set(CMAKE_BINARY_DIR "{Path(directory).as_posix()}")
                    include("{(REPO_ROOT / 'cmake' / 'MIBVersion.cmake').as_posix()}")
                    file(WRITE "{output.as_posix()}" "${{PROJECT_VERSION}}|${{PROJECT_VERSION_FULL}}")
                    """
                ),
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    "cmake",
                    f"-DMIB_RELEASE_VERSION_OVERRIDE={version}",
                    f"-DMIB_RELEASE_VERSION_FULL_OVERRIDE={full_version}",
                    "-P",
                    str(wrapper),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )
            if output.exists():
                result.identity = output.read_text(encoding="utf-8")  # type: ignore[attr-defined]
            return result

    def test_override_sets_exact_numeric_and_beta_identity(self) -> None:
        result = self.run_module("1.0.6", "1.0.6-beta.abc123")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.identity, "1.0.6|1.0.6-beta.abc123")  # type: ignore[attr-defined]

    def test_override_rejects_numeric_full_version_divergence(self) -> None:
        result = self.run_module("1.0.6", "1.0.5-beta.abc123")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same numeric version", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
