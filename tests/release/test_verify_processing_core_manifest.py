#!/usr/bin/env python3
"""Shape tests for verify-processing-core-manifest.py (file:// only)."""
from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "scripts" / "release" / "verify-processing-core-manifest.py"
_spec = importlib.util.spec_from_file_location("verify_processing_core_manifest", SCRIPT_PATH)
verify = importlib.util.module_from_spec(_spec)
sys.modules["verify_processing_core_manifest"] = verify
_spec.loader.exec_module(verify)


def base_manifest() -> dict:
    return {
        "processing_core_manifest_schema_version": 2,
        "channel": "stable",
        "version": "0.1.0",
        "contract_version": 1,
        "wheel": {
            "package": "mib-processing",
            "version": "0.1.0",
            "release_tag": "mib-processing-v0.1.0",
            "wheels": [{
                "filename": "mib_processing-0.1.0-cp311-cp311-linux_x86_64.whl",
                "platform_tag": "cp311-cp311-linux_x86_64",
                "url": "https://example.invalid/core.whl",
                "sha256": "a" * 64,
                "size_bytes": 123,
            }],
        },
        "native_plugins": [],
        "profile_catalog_url": "https://example.invalid/profiles.json",
        "emodulus_lut_manifest_url": "https://example.invalid/lut.json",
    }


def verify_fixture(root: Path, manifest: dict) -> int:
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return verify.main(["--manifest-url", path.as_uri(), "--skip-referenced"])


class ManifestVerificationTest(unittest.TestCase):
    def test_keeps_accepting_schema_v1_shape(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = base_manifest()
            manifest["processing_core_manifest_schema_version"] = 1
            manifest.pop("version")
            manifest.pop("native_plugins")
            manifest["wheel"]["wheels"][0].pop("filename")
            manifest["wheel"]["wheels"][0].pop("size_bytes")
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 0)

    def test_accepts_schema_v2_with_no_native_plugin(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            self.assertEqual(verify_fixture(Path(temp_dir), base_manifest()), 0)

    def test_rejects_canonical_version_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = base_manifest()
            manifest["wheel"]["version"] = "9.9.9"
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 1)

    def test_validates_native_identity_and_hash_shape(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = base_manifest()
            manifest["native_plugins"] = [{
                "filename": "core.dll",
                "os": "windows",
                "arch": "x86_64",
                "version": "0.1.0",
                "contract_version": 1,
                "engine_abi_version": 1,
                "runtime_fingerprint": "msvc194-md-x64",
                "entrypoint": "mib_processing_get_api",
                "url": "https://example.invalid/core.dll",
                "sha256": "b" * 64,
                "size_bytes": 456,
                "signing": {"scheme": "authenticode", "required": True},
            }]
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 0)
            manifest["native_plugins"][0]["contract_version"] = 2
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 1)

    def test_accepts_linux_shared_library_and_rejects_cross_os_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = base_manifest()
            manifest["native_plugins"] = [{
                "filename": "core.so",
                "os": "linux",
                "arch": "x86_64",
                "version": "0.1.0",
                "contract_version": 1,
                "engine_abi_version": 1,
                "runtime_fingerprint": "linux-x86_64-gcc13-cxx17",
                "entrypoint": "mib_processing_get_api",
                "url": "https://example.invalid/core.so",
                "sha256": "b" * 64,
                "size_bytes": 456,
                "signing": {"scheme": "detached-ed25519", "required": True},
            }]
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 0)
            manifest["native_plugins"][0]["filename"] = "core.dll"
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 1)

    def test_validates_ed25519_detached_signature_material(self) -> None:
        spki = bytes(44)
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = base_manifest()
            manifest["native_plugins"] = [{
                "filename": "core.so",
                "os": "linux",
                "arch": "x86_64",
                "version": "0.1.0",
                "contract_version": 1,
                "engine_abi_version": 1,
                "runtime_fingerprint": "linux-x86_64-gcc13-cxx17",
                "entrypoint": "mib_processing_get_api",
                "url": "https://example.invalid/core.so",
                "sha256": "b" * 64,
                "size_bytes": 456,
                "signing": {
                    "scheme": "ed25519",
                    "required": True,
                    "public_key_spki_base64": base64.b64encode(spki).decode("ascii"),
                    "public_key_spki_sha256": hashlib.sha256(spki).hexdigest(),
                    "signature_base64": base64.b64encode(bytes(64)).decode("ascii"),
                },
            }]
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 0)
            manifest["native_plugins"][0]["signing"]["public_key_spki_sha256"] = "f" * 64
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 1)
            manifest["native_plugins"][0]["signing"].pop("public_key_spki_sha256")
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 1)
            manifest["native_plugins"][0]["filename"] = "core.so"
            manifest["native_plugins"][0]["signing"]["required"] = False
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 1)

    def test_rejects_insecure_artifact_urls(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = base_manifest()
            manifest["wheel"]["wheels"][0]["url"] = "http://example.invalid/core.whl"
            self.assertEqual(verify_fixture(Path(temp_dir), manifest), 1)


if __name__ == "__main__":
    unittest.main()
