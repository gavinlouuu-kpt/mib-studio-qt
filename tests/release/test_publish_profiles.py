#!/usr/bin/env python3
"""Contract-compatibility coverage for profile publication metadata."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "release" / "publish-profiles.py"
SPEC = importlib.util.spec_from_file_location("publish_profiles", SCRIPT)
assert SPEC and SPEC.loader
publish_profiles = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = publish_profiles
SPEC.loader.exec_module(publish_profiles)


class ProfileProcessingContractTest(unittest.TestCase):
    def _build(self, contract: object) -> tuple[dict, dict]:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile = root / "test-profile"
            profile.mkdir()
            (profile / "config.json").write_text(
                '{"config_schema_version": 1}\n', encoding="utf-8"
            )
            (profile / "profile.meta.json").write_text(
                json.dumps({"processing_contract_version": contract}), encoding="utf-8"
            )
            entry, metadata, _ = publish_profiles.build_profile_metadata(
                profile_dir=profile,
                channel="stable",
                catalog_url="https://updates.example/profiles/stable/catalog.json",
                public_base_url="https://updates.example",
                profile_prefix="profiles/stable",
                revision="2026.07.14-1",
                add_missing_config_schema=False,
                staging_dir=root / "staging",
            )
            return entry, metadata

    def test_optional_contract_is_cross_linked_into_catalog_and_metadata(self) -> None:
        entry, metadata = self._build(1)
        self.assertEqual(entry["processing_contract_version"], 1)
        self.assertEqual(metadata["processing_contract_version"], 1)

    def test_invalid_contract_is_rejected(self) -> None:
        for value in (0, -1, True, "1"):
            with self.subTest(value=value), self.assertRaisesRegex(
                ValueError, "positive integer or null"
            ):
                self._build(value)


if __name__ == "__main__":
    unittest.main()
