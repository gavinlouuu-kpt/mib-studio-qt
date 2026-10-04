from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "release" / "publish-processing-core-line.py"
_spec = importlib.util.spec_from_file_location("publish_processing_core_line", SCRIPT_PATH)
line_pub = importlib.util.module_from_spec(_spec)
sys.modules["publish_processing_core_line"] = line_pub
_spec.loader.exec_module(line_pub)

LINE = "absdiff-laplacian"
TAG = "mib-processing-absdiff-laplacian-v0.1.0"
BASE = "https://updates.example"


def plugin(os_name: str, arch: str = "x86_64") -> dict:
    ext = "dll" if os_name == "windows" else "so"
    return {"filename": f"mib_processing_core-{LINE}-0.1.0-{os_name}_{arch}.{ext}", "os": os_name, "arch": arch,
            "version": "0.1.0", "contract_version": 2, "engine_abi_version": 2, "sha256": "a" * 64}


def manifest(channel: str = "beta", version: str = "0.1.0", plugins=None) -> dict:
    return line_pub.build_line_manifest(
        line=LINE, channel=channel, version=version, release_tag=f"mib-processing-{LINE}-v{version}",
        repo="OWNER/REPO", native_plugins=plugins if plugins is not None else [plugin("windows"), plugin("linux")],
        published_at="2026-10-04T00:00:00Z", public_base_url=BASE)


class LineManifestTest(unittest.TestCase):
    def test_tag_parsing_is_per_line(self) -> None:
        self.assertEqual(line_pub.version_from_line_tag(LINE, TAG), "0.1.0")
        for bad in ("mib-processing-subtract-ring-v0.1.0", "mib-processing-v0.1.0", "absdiff-laplacian-v0.1.0"):
            with self.subTest(tag=bad), self.assertRaises(ValueError):
                line_pub.version_from_line_tag(LINE, bad)
        with self.assertRaises(ValueError):
            line_pub.tag_prefix("subtract-ring")  # published by publish-processing-core.py

    def test_beta_only_line_refuses_stable(self) -> None:
        self.assertEqual(line_pub.check_channel(LINE, "beta"), "beta")
        with self.assertRaisesRegex(ValueError, "beta only"):
            line_pub.check_channel(LINE, "stable")

    def test_manifest_identity(self) -> None:
        doc = manifest()
        self.assertEqual(doc["processing_core_line_manifest_schema_version"], 1)
        self.assertEqual((doc["line"], doc["channel"], doc["version"]), (LINE, "beta", "0.1.0"))
        self.assertEqual((doc["contract_version"], doc["engine_abi_version"]), (2, 2))
        self.assertEqual(doc["release_tag"], TAG)
        self.assertEqual(doc["release_url"], f"https://github.com/OWNER/REPO/releases/tag/{TAG}")
        self.assertEqual(len(doc["native_plugins"]), 2)

    def test_manifest_requires_both_platforms(self) -> None:
        with self.assertRaisesRegex(ValueError, "linux/x86_64"):
            manifest(plugins=[plugin("windows")])

    def test_manifest_rejects_wrong_contract_or_version(self) -> None:
        wrong_contract = plugin("linux") | {"contract_version": 1}
        with self.assertRaisesRegex(ValueError, "contract"):
            manifest(plugins=[plugin("windows"), wrong_contract])
        with self.assertRaisesRegex(ValueError, "names version"):
            line_pub.build_line_manifest(
                line=LINE, channel="beta", version="0.2.0", release_tag=TAG, repo="OWNER/REPO",
                native_plugins=[plugin("windows"), plugin("linux")], published_at="2026-10-04T00:00:00Z",
                public_base_url=BASE)

    def test_index_merge_sorts_and_activates(self) -> None:
        first = line_pub.merge_line_index({}, manifest(version="0.1.0"), BASE)
        second = line_pub.merge_line_index(first, manifest(version="0.2.0", plugins=[
            plugin("windows") | {"version": "0.2.0"}, plugin("linux") | {"version": "0.2.0"}]), BASE)
        self.assertEqual(second["active_version"], "0.2.0")
        self.assertEqual([v["version"] for v in second["versions"]], ["0.2.0", "0.1.0"])
        self.assertEqual(second["line"], LINE)
        self.assertEqual(second["versions"][0]["manifest_url"],
                         f"{BASE}/beta/processing-core/{LINE}/versions/0.2.0.json")

    def test_index_rejects_other_line_or_channel(self) -> None:
        index = line_pub.merge_line_index({}, manifest(), BASE)
        with self.assertRaisesRegex(ValueError, "line"):
            line_pub.merge_line_index(index | {"line": "subtract-ring"}, manifest(), BASE)
        with self.assertRaisesRegex(ValueError, "channel"):
            line_pub.merge_line_index(index | {"channel": "stable"}, manifest(), BASE)


if __name__ == "__main__":
    unittest.main()
