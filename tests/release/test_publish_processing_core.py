#!/usr/bin/env python3
"""Unit tests for publish-processing-core.py (no network or R2 access)."""

from __future__ import annotations

import base64
import contextlib
import hashlib
import importlib.util
import io
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "scripts" / "release" / "publish-processing-core.py"
_spec = importlib.util.spec_from_file_location("publish_processing_core", SCRIPT_PATH)
publish_processing_core = importlib.util.module_from_spec(_spec)
sys.modules["publish_processing_core"] = publish_processing_core
_spec.loader.exec_module(publish_processing_core)


def make_wheel(root: Path, version: str = "0.1.0", python: str = "cp311") -> Path:
    wheel = root / f"mib_processing-{version}-{python}-{python}-linux_x86_64.whl"
    wheel.write_bytes(f"wheel {version} {python}".encode())
    return wheel


def make_pyproject(root: Path, version: str = "0.1.0") -> Path:
    # CLI tests must not depend on the real repository version: the publisher
    # cross-checks the authoritative pyproject, so fixtures carry their own.
    path = root / "pyproject.toml"
    path.write_text(
        f'[project]\nname = "mib-processing"\nversion = "{version}"\n',
        encoding="utf-8",
    )
    return path


def make_manifest(root: Path, version: str, published_at: str = "2026-07-13T00:00:00Z") -> dict:
    wheel = make_wheel(root, version)
    return publish_processing_core.build_manifest(
        channel="stable",
        contract_version=1,
        wheel_version=version,
        release_tag=f"mib-processing-v{version}",
        repo="KPT1020/mib-studio-qt",
        wheel_paths=[wheel],
        public_base_url="https://updates.example",
        published_at=published_at,
    )


SUBTRACT_RING_GOLDEN_DIR = REPO_ROOT / "tests" / "release" / "fixtures" / "processing_core_subtract_ring"
GOLDEN_PUBLISHED_AT = "2026-07-13T00:00:00Z"


def write_subtract_ring_release_assets(assets: Path, version: str = "0.1.0") -> None:
    """Deterministic Contract-1 release assets: two wheels plus Windows/Linux cores."""
    assets.mkdir(parents=True, exist_ok=True)
    make_wheel(assets, version, "cp311")
    make_wheel(assets, version, "cp313")
    dll = assets / f"mib_processing_core-{version}-windows_x86_64.dll"
    dll.write_bytes(b"signed windows fixture")
    (assets / f"mib_processing_core-{version}-windows_x86_64.json").write_text(json.dumps({
        "schema_version": 1,
        "version": version,
        "filename": dll.name,
        "os": "windows",
        "arch": "x86_64",
        "engine_abi_version": 1,
        "contract_version": 1,
        "entrypoint": "mib_processing_get_api",
        "runtime_fingerprint": "windows-x86_64-msvc1942-md-cxx17",
        "app_min_version": "0.8.0",
        "app_max_version": None,
        "signing": {"required": True, "scheme": "authenticode"},
    }), encoding="utf-8")
    shared_library = assets / f"mib_processing_core-{version}-linux_x86_64.so"
    shared_library.write_bytes(b"signed linux fixture")
    (assets / f"mib_processing_core-{version}-linux_x86_64.json").write_text(json.dumps({
        "schema_version": 1,
        "version": version,
        "filename": shared_library.name,
        "os": "linux",
        "arch": "x86_64",
        "engine_abi_version": 1,
        "contract_version": 1,
        "entrypoint": "mib_processing_get_api",
        "runtime_fingerprint": "linux-x86_64-gcc13-cxx17",
        "app_min_version": "0.8.0",
        "app_max_version": None,
        "signing": {
            "required": True,
            "scheme": "ed25519",
            "public_key_spki_base64": base64.b64encode(bytes(44)).decode("ascii"),
            "signature_base64": base64.b64encode(bytes(64)).decode("ascii"),
        },
    }), encoding="utf-8")


def run_subtract_ring_golden_scenario(module, root: Path, assets: Path) -> dict[str, bytes]:
    """Produce every subtract-ring registry document the publisher emits.

    The same scenario generated tests/release/fixtures/processing_core_subtract_ring
    from the publisher before core lines existed, so comparing against those
    bytes proves the default line is unchanged.
    """
    pyproject = make_pyproject(root)
    out = root / "dry-run"
    result = module.main([
        "--from-release", "mib-processing-v0.1.0",
        "--pyproject", str(pyproject),
        "--release-assets-dir", str(assets),
        "--published-at", GOLDEN_PUBLISHED_AT,
        "--dry-run",
        "--manifest-out", str(out / "latest.json"),
        "--version-manifest-out", str(out / "version.json"),
        "--index-out", str(out / "index.json"),
        "--pep503-out", str(out / "index.html"),
    ])
    if result != 0:
        raise AssertionError(f"golden dry run failed with {result}")
    documents = {
        f"dry_run/{name}": (out / name).read_bytes()
        for name in ("latest.json", "version.json", "index.json", "index.html")
    }

    # A real (mocked) publication that merges into an existing catalog.
    previous_dir = root / "previous"
    previous_dir.mkdir()
    previous = module.merge_index(
        {}, make_manifest(previous_dir, "0.0.9", "2026-07-01T00:00:00Z"), "https://updates.yofo.bio",
    )
    existing_index = module.serialize_json(previous)
    uploads: list[str] = []

    def capture(**kwargs) -> None:
        index = len(uploads) + 1
        documents[f"publish/{index}"] = kwargs["file_path"].read_bytes()
        uploads.append(f"{kwargs['key']}\t{kwargs['content_type']}\t{kwargs['cache_control']}")

    argv = ["--pyproject", str(pyproject), "--published-at", GOLDEN_PUBLISHED_AT,
            "--endpoint", "https://r2.invalid", "--upload-method", "s3"]
    for wheel in sorted(assets.glob("mib_processing-*.whl")):
        argv += ["--wheel", str(wheel)]
    for descriptor in sorted(assets.glob("mib_processing_core-0.1.0-*.json")):
        argv += ["--native-plugin-descriptor", str(descriptor)]
    with (
        mock.patch.object(
            module, "read_existing_object",
            side_effect=[(None, True), (existing_index, True)],
        ),
        mock.patch.object(module, "upload_object", side_effect=capture),
    ):
        result = module.main(argv)
    if result != 0:
        raise AssertionError(f"golden publication failed with {result}")
    documents["publish/uploads.tsv"] = ("\n".join(uploads) + "\n").encode("utf-8")
    return documents


class VersionAndWheelTest(unittest.TestCase):
    def test_rejects_unsafe_version(self) -> None:
        for value in ("../1.0", "1/2", "", ".", "..", "v 1"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                publish_processing_core.validate_version(value)

    def test_rejects_unsafe_channel(self) -> None:
        for value in ("../stable", "stable/next", "", ".", "..", "stable next"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                publish_processing_core.validate_channel(value)

    def test_release_tag_and_wheel_must_name_canonical_version(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            wrong = make_wheel(root, "0.2.0")
            with self.assertRaisesRegex(ValueError, "declares version"):
                publish_processing_core.build_wheel_entries(
                    [wrong], "OWNER/REPO", "mib-processing-v0.1.0", expected_version="0.1.0"
                )
            with self.assertRaisesRegex(ValueError, "names version"):
                publish_processing_core.build_manifest(
                    channel="stable",
                    contract_version=1,
                    wheel_version="0.2.0",
                    release_tag="mib-processing-v0.1.0",
                    repo="OWNER/REPO",
                    wheel_paths=[wrong],
                    public_base_url="https://example.invalid",
                )

    def test_wheel_entry_has_resolver_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            wheel = make_wheel(Path(temp_dir))
            entry = publish_processing_core.build_wheel_entries(
                [wheel], "OWNER/REPO", "mib-processing-v0.1.0", expected_version="0.1.0"
            )[0]
            self.assertEqual(entry["filename"], wheel.name)
            self.assertEqual(entry["platform_tag"], "cp311-cp311-linux_x86_64")
            self.assertEqual(entry["size_bytes"], wheel.stat().st_size)
            self.assertEqual(entry["sha256"], publish_processing_core.sha256_file(wheel))
            self.assertEqual(
                entry["url"],
                f"https://github.com/OWNER/REPO/releases/download/mib-processing-v0.1.0/{wheel.name}",
            )


class NativePluginTest(unittest.TestCase):
    def test_descriptor_is_cross_checked_and_publisher_hashes_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            dll = root / "mib_processing_core-0.1.0-windows_x86_64.dll"
            dll.write_bytes(b"signed dll fixture")
            descriptor = root / "mib_processing_core-0.1.0-windows_x86_64.json"
            descriptor.write_text(json.dumps({
                "filename": dll.name,
                "version": "0.1.0",
                "contract_version": 1,
                "os": "windows",
                "arch": "amd64",
                "engine_abi_version": 1,
                "runtime_fingerprint": "msvc194-md-x64",
                "app_min_version": "0.8.0",
                "app_max_version": None,
                "entrypoint": "mib_processing_get_api",
                "url": "https://attacker.invalid/not-used",
                "sha256": "0" * 64,
                "signing": {"scheme": "authenticode", "subject": "Test Publisher"},
            }), encoding="utf-8")

            entries = publish_processing_core.build_native_plugin_entries(
                [descriptor],
                asset_dir=root,
                repo="OWNER/REPO",
                release_tag="mib-processing-v0.1.0",
                expected_version="0.1.0",
                expected_contract_version=1,
            )

            self.assertEqual(len(entries), 1)
            entry = entries[0]
            self.assertEqual(entry["sha256"], publish_processing_core.sha256_file(dll))
            self.assertEqual(entry["size_bytes"], len(b"signed dll fixture"))
            self.assertEqual(entry["version"], "0.1.0")
            self.assertEqual(entry["contract_version"], 1)
            self.assertEqual(entry["arch"], "x86_64")
            self.assertEqual(entry["signing"]["scheme"], "authenticode")
            self.assertTrue(entry["signing"]["required"])
            self.assertTrue(entry["url"].startswith("https://github.com/OWNER/REPO/releases/download/"))
            self.assertEqual(publish_processing_core.discover_native_descriptors(root), [descriptor])

    def test_linux_shared_library_descriptor_is_portable(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            shared_library = root / "mib_processing_core-0.1.0-linux_x86_64.so"
            shared_library.write_bytes(b"signed linux shared-library fixture")
            descriptor = root / "mib_processing_core-0.1.0-linux_x86_64.json"
            descriptor.write_text(json.dumps({
                "filename": shared_library.name,
                "version": "0.1.0",
                "contract_version": 1,
                "os": "linux",
                "arch": "x86_64",
                "engine_abi_version": 1,
                "runtime_fingerprint": "linux-x86_64-gcc13-cxx17",
                "entrypoint": "mib_processing_get_api",
                "signing": {"scheme": "detached-ed25519", "required": True},
            }), encoding="utf-8")

            entries = publish_processing_core.build_native_plugin_entries(
                [descriptor],
                asset_dir=root,
                repo="OWNER/REPO",
                release_tag="mib-processing-v0.1.0",
                expected_version="0.1.0",
                expected_contract_version=1,
            )

            self.assertEqual(entries[0]["os"], "linux")
            self.assertEqual(entries[0]["arch"], "x86_64")
            self.assertEqual(entries[0]["signing"]["scheme"], "detached-ed25519")

            mismatched = json.loads(descriptor.read_text(encoding="utf-8"))
            mismatched["filename"] = "core.dll"
            descriptor.write_text(json.dumps(mismatched), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "linux must end in .so"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor],
                    asset_dir=root,
                    repo="OWNER/REPO",
                    release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0",
                    expected_contract_version=1,
                )

            mismatched["filename"] = shared_library.name
            mismatched["signing"]["required"] = False
            descriptor.write_text(json.dumps(mismatched), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "signatures must be required"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor],
                    asset_dir=root,
                    repo="OWNER/REPO",
                    release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0",
                    expected_contract_version=1,
                )

    def test_ed25519_signing_material_is_validated_and_key_hash_derived(self) -> None:
        spki = bytes(44)
        signature = bytes(64)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            shared_library = root / "mib_processing_core-0.1.0-linux_x86_64.so"
            shared_library.write_bytes(b"ed25519-signed linux shared-library fixture")
            descriptor = root / "mib_processing_core-0.1.0-linux_x86_64.json"
            payload = {
                "filename": shared_library.name,
                "version": "0.1.0",
                "contract_version": 1,
                "os": "linux",
                "arch": "x86_64",
                "engine_abi_version": 1,
                "runtime_fingerprint": "linux-x86_64-gcc13-cxx17",
                "entrypoint": "mib_processing_get_api",
                "signing": {
                    "scheme": "ed25519",
                    "required": True,
                    "public_key_spki_base64": base64.b64encode(spki).decode("ascii"),
                    "signature_base64": base64.b64encode(signature).decode("ascii"),
                },
            }
            descriptor.write_text(json.dumps(payload), encoding="utf-8")

            entries = publish_processing_core.build_native_plugin_entries(
                [descriptor],
                asset_dir=root,
                repo="OWNER/REPO",
                release_tag="mib-processing-v0.1.0",
                expected_version="0.1.0",
                expected_contract_version=1,
            )
            signing = entries[0]["signing"]
            self.assertEqual(signing["scheme"], "ed25519")
            self.assertEqual(
                signing["public_key_spki_sha256"], hashlib.sha256(spki).hexdigest()
            )
            self.assertEqual(
                base64.b64decode(signing["signature_base64"], validate=True), signature
            )

            payload["signing"]["public_key_spki_sha256"] = "f" * 64
            descriptor.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "does not match the key bytes"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="OWNER/REPO",
                    release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0", expected_contract_version=1,
                )

            del payload["signing"]["public_key_spki_sha256"]
            payload["signing"]["signature_base64"] = base64.b64encode(bytes(63)).decode("ascii")
            descriptor.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "signature must be 64 bytes"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="OWNER/REPO",
                    release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0", expected_contract_version=1,
                )

            payload["signing"]["signature_base64"] = "not*base64"
            descriptor.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not valid base64"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="OWNER/REPO",
                    release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0", expected_contract_version=1,
                )

            del payload["signing"]["public_key_spki_base64"]
            payload["signing"]["signature_base64"] = base64.b64encode(signature).decode("ascii")
            descriptor.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "44-byte DER SPKI"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="OWNER/REPO",
                    release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0", expected_contract_version=1,
                )

    def test_descriptor_rejects_contract_or_path_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            descriptor = root / "native.json"
            descriptor.write_text(json.dumps({
                "filename": "../core.dll",
                "version": "0.1.0",
                "contract_version": 2,
                "os": "windows",
                "arch": "x86_64",
                "engine_abi_version": 1,
                "runtime_fingerprint": "test",
                "entrypoint": "mib_processing_get_api",
            }), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "basename"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="O/R", release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0", expected_contract_version=1,
                )


class ManifestAndIndexTest(unittest.TestCase):
    def test_registry_json_serialization_is_stable_lf_bytes(self) -> None:
        encoded = publish_processing_core.serialize_json({"first": 1, "second": "value"})
        self.assertEqual(
            encoded,
            b'{\n  "first": 1,\n  "second": "value"\n}\n',
        )
        self.assertNotIn(b"\r\n", encoded)

    def test_manifest_requires_valid_timestamp_and_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            wheel = make_wheel(root)
            common = {
                "channel": "stable",
                "wheel_version": "0.1.0",
                "release_tag": "mib-processing-v0.1.0",
                "repo": "OWNER/REPO",
                "wheel_paths": [wheel],
                "public_base_url": "https://updates.example",
            }
            with self.assertRaisesRegex(ValueError, "timezone"):
                publish_processing_core.build_manifest(
                    contract_version=1,
                    published_at="2026-07-13T00:00:00",
                    **common,
                )
            with self.assertRaisesRegex(ValueError, "positive integer"):
                publish_processing_core.build_manifest(
                    contract_version=0,
                    published_at="2026-07-13T00:00:00Z",
                    **common,
                )

    def test_schema_v2_is_additive_and_canonical(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = make_manifest(Path(temp_dir), "0.1.0")
            self.assertEqual(manifest["processing_core_manifest_schema_version"], 2)
            self.assertEqual(manifest["version"], "0.1.0")
            self.assertEqual(manifest["wheel"]["version"], manifest["version"])
            self.assertEqual(manifest["contract_version"], 1)
            self.assertEqual(manifest["native_plugins"], [])
            self.assertIn("profile_catalog_url", manifest)
            self.assertIn("emodulus_lut_manifest_url", manifest)

    def test_merge_preserves_history_and_changes_only_active_pointer_for_rollback(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            v100 = make_manifest(root, "1.0.0", "2026-07-10T00:00:00Z")
            v110b = make_manifest(root, "1.1.0b1", "2026-07-11T00:00:00Z")
            v110 = make_manifest(root, "1.1.0", "2026-07-12T00:00:00Z")
            index = publish_processing_core.merge_index({}, v100, "https://updates.example")
            index = publish_processing_core.merge_index(index, v110b, "https://updates.example")
            index = publish_processing_core.merge_index(index, v110, "https://updates.example")
            self.assertEqual(
                [entry["version"] for entry in index["versions"]],
                ["1.1.0", "1.1.0b1", "1.0.0"],
            )
            self.assertEqual(index["active_version"], "1.1.0")

            rolled_back = publish_processing_core.merge_index(index, v100, "https://updates.example")
            self.assertEqual(rolled_back["active_version"], "1.0.0")
            self.assertEqual(len(rolled_back["versions"]), 3)
            self.assertEqual(rolled_back["versions"][0]["version"], "1.1.0")

    def test_index_rejects_channel_or_schema_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = make_manifest(Path(temp_dir), "1.0.0")
            with self.assertRaisesRegex(ValueError, "channel"):
                publish_processing_core.merge_index(
                    {"channel": "beta", "versions": []}, manifest, "https://updates.example"
                )
            with self.assertRaisesRegex(ValueError, "schema"):
                publish_processing_core.merge_index(
                    {"channel": "stable", "processing_core_index_schema_version": 99},
                    manifest,
                    "https://updates.example",
                )
            with self.assertRaisesRegex(ValueError, "invalid version entry"):
                publish_processing_core.merge_index(
                    {"channel": "stable", "versions": ["corrupt"]},
                    manifest,
                    "https://updates.example",
                )

    def test_pep503_page_contains_hash_pinned_links_for_all_history(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            index = publish_processing_core.merge_index(
                {}, make_manifest(root, "1.0.0"), "https://updates.example"
            )
            index = publish_processing_core.merge_index(
                index, make_manifest(root, "1.1.0"), "https://updates.example"
            )
            page = publish_processing_core.render_pep503_index(index)
            self.assertIn("mib_processing-1.0.0-cp311-cp311-linux_x86_64.whl", page)
            self.assertIn("mib_processing-1.1.0-cp311-cp311-linux_x86_64.whl", page)
            self.assertEqual(page.count("#sha256="), 2)


class ImmutablePublicationTest(unittest.TestCase):
    def test_missing_uploads_identical_is_idempotent_and_conflict_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = make_manifest(Path(temp_dir), "1.0.0")
            encoded = publish_processing_core.serialize_json(manifest)
            self.assertTrue(
                publish_processing_core.resolve_immutable_update(None, True, manifest, "version.json")
            )
            self.assertFalse(
                publish_processing_core.resolve_immutable_update(encoded, True, manifest, "version.json")
            )
            different = dict(manifest)
            different["contract_version"] = 2
            with self.assertRaisesRegex(RuntimeError, "different content"):
                publish_processing_core.resolve_immutable_update(
                    json.dumps(different).encode(), True, manifest, "version.json"
                )
            with self.assertRaisesRegex(RuntimeError, "could not be read"):
                publish_processing_core.resolve_immutable_update(None, False, manifest, "version.json")

    @mock.patch.object(publish_processing_core.subprocess, "run")
    def test_release_inspection_uses_gh_json(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess(
            [], 0, stdout=json.dumps({
                "tagName": "mib-processing-v0.1.0",
                "publishedAt": "2026-07-13T00:00:00Z",
                "url": "https://example.invalid/release",
                "assets": [],
            }), stderr="",
        )
        release = publish_processing_core.inspect_github_release(
            "OWNER/REPO", "mib-processing-v0.1.0", "fake-gh"
        )
        self.assertEqual(release["publishedAt"], "2026-07-13T00:00:00Z")
        run.assert_called_once_with(
            [
                "fake-gh", "release", "view", "mib-processing-v0.1.0", "--repo", "OWNER/REPO",
                "--json", "tagName,publishedAt,url,assets",
            ],
            check=True,
            capture_output=True,
            text=True,
        )


class CommandLineTest(unittest.TestCase):
    def test_mutating_publish_refuses_public_cdn_preflight(self) -> None:
        with mock.patch.object(publish_processing_core, "read_existing_object") as read:
            result = publish_processing_core.main([
                "--promote-version", "0.1.0",
                "--published-at", "2026-07-13T00:00:00Z",
                "--upload-method", "wrangler",
            ])
        self.assertEqual(result, 1)
        read.assert_not_called()

    def test_live_release_requires_published_at_metadata(self) -> None:
        def download(_repo: str, _tag: str, destination: Path, _gh_bin: str) -> None:
            destination.mkdir(parents=True, exist_ok=True)
            make_wheel(destination)

        with (
            tempfile.TemporaryDirectory() as temp_dir,
            mock.patch.object(
                publish_processing_core,
                "inspect_github_release",
                return_value={"tagName": "mib-processing-v0.1.0"},
            ),
            mock.patch.object(publish_processing_core, "download_github_release", side_effect=download),
            mock.patch.object(publish_processing_core, "upload_object") as upload,
        ):
            result = publish_processing_core.main([
                "--from-release", "mib-processing-v0.1.0",
                "--wheel-version", "0.1.0",
                "--pyproject", str(make_pyproject(Path(temp_dir))),
                "--endpoint", "https://r2.invalid",
                "--upload-method", "s3",
            ])
        self.assertEqual(result, 1)
        upload.assert_not_called()

    def test_release_published_at_makes_dry_run_bytes_repeatable(self) -> None:
        def download(_repo: str, _tag: str, destination: Path, _gh_bin: str) -> None:
            destination.mkdir(parents=True, exist_ok=True)
            make_wheel(destination)

        metadata = {
            "tagName": "mib-processing-v0.1.0",
            "publishedAt": "2026-07-13T00:00:00Z",
        }
        with (
            tempfile.TemporaryDirectory() as temp_dir,
            mock.patch.object(
                publish_processing_core,
                "inspect_github_release",
                return_value=metadata,
            ),
            mock.patch.object(publish_processing_core, "download_github_release", side_effect=download),
        ):
            root = Path(temp_dir)
            outputs = [root / "first.json", root / "second.json"]
            for output in outputs:
                result = publish_processing_core.main([
                    "--from-release", "mib-processing-v0.1.0",
                    "--wheel-version", "0.1.0",
                    "--pyproject", str(make_pyproject(root)),
                    "--dry-run",
                    "--manifest-out", str(output),
                ])
                self.assertEqual(result, 0)
            self.assertEqual(outputs[0].read_bytes(), outputs[1].read_bytes())

    def test_fixture_backed_from_release_dry_run_emits_all_documents(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            assets = root / "assets"
            assets.mkdir()
            make_wheel(assets, "0.1.0", "cp310")
            make_wheel(assets, "0.1.0", "cp313")
            dll = assets / "mib_processing_core-0.1.0-windows_x86_64.dll"
            dll.write_bytes(b"signed fixture")
            (assets / "mib_processing_core-0.1.0-windows_x86_64.json").write_text(
                json.dumps({
                    "filename": dll.name,
                    "version": "0.1.0",
                    "contract_version": 1,
                    "os": "windows",
                    "arch": "x86_64",
                    "engine_abi_version": 1,
                    "runtime_fingerprint": "msvc194-md-x64",
                    "app_min_version": "0.8.0",
                    "app_max_version": None,
                    "entrypoint": "mib_processing_get_api",
                    "signing": {"scheme": "authenticode"},
                }),
                encoding="utf-8",
            )
            latest = root / "latest.json"
            versioned = root / "versions" / "0.1.0.json"
            catalog = root / "index.json"
            pep = root / "simple" / "index.html"

            result = publish_processing_core.main([
                "--from-release", "mib-processing-v0.1.0",
                "--wheel-version", "0.1.0",
                "--pyproject", str(make_pyproject(root)),
                "--release-assets-dir", str(assets),
                "--published-at", "2026-07-13T00:00:00Z",
                "--dry-run",
                "--manifest-out", str(latest),
                "--version-manifest-out", str(versioned),
                "--index-out", str(catalog),
                "--pep503-out", str(pep),
            ])

            self.assertEqual(result, 0)
            self.assertEqual(json.loads(latest.read_text()), json.loads(versioned.read_text()))
            manifest = json.loads(latest.read_text())
            self.assertEqual(len(manifest["wheel"]["wheels"]), 2)
            self.assertEqual(len(manifest["native_plugins"]), 1)
            self.assertEqual(json.loads(catalog.read_text())["active_version"], "0.1.0")
            self.assertEqual(pep.read_text().count("#sha256="), 2)

    def test_publish_order_promotes_latest_last(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            wheel = make_wheel(root)
            uploaded: list[str] = []

            with (
                mock.patch.object(
                    publish_processing_core,
                    "read_existing_object",
                    side_effect=[(None, True), (None, True)],
                ),
                mock.patch.object(
                    publish_processing_core,
                    "upload_object",
                    side_effect=lambda **kwargs: uploaded.append(kwargs["key"]),
                ),
            ):
                result = publish_processing_core.main([
                    "--wheel", str(wheel),
                    "--wheel-version", "0.1.0",
                    "--pyproject", str(make_pyproject(wheel.parent)),
                    "--published-at", "2026-07-13T00:00:00Z",
                    "--endpoint", "https://r2.invalid",
                    "--upload-method", "s3",
                ])

            self.assertEqual(result, 0)
            self.assertEqual(uploaded, [
                "stable/processing-core/versions/0.1.0.json",
                "stable/processing-core/index.json",
                "stable/processing-core/simple/mib-processing/index.html",
                "stable/processing-core/simple/mib-processing/",
                "stable/processing-core/latest.json",
            ])

    def test_unreadable_catalog_refuses_every_upload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            wheel = make_wheel(Path(temp_dir))
            with (
                mock.patch.object(
                    publish_processing_core,
                    "read_existing_object",
                    side_effect=[(None, True), (None, False)],
                ),
                mock.patch.object(publish_processing_core, "upload_object") as upload,
            ):
                result = publish_processing_core.main([
                    "--wheel", str(wheel),
                    "--wheel-version", "0.1.0",
                    "--pyproject", str(make_pyproject(wheel.parent)),
                    "--published-at", "2026-07-13T00:00:00Z",
                    "--endpoint", "https://r2.invalid",
                    "--upload-method", "s3",
                ])
            self.assertEqual(result, 1)
            upload.assert_not_called()

    def test_explicit_publish_requires_stable_timestamp(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            wheel = make_wheel(Path(temp_dir))
            with mock.patch.object(publish_processing_core, "upload_object") as upload:
                result = publish_processing_core.main([
                    "--wheel", str(wheel),
                    "--wheel-version", "0.1.0",
                    "--pyproject", str(make_pyproject(wheel.parent)),
                    "--endpoint", "https://r2.invalid",
                    "--upload-method", "s3",
                ])
            self.assertEqual(result, 1)
            upload.assert_not_called()

    def test_promote_copies_immutable_bytes_exactly_and_writes_latest_last(self) -> None:
        immutable_manifest = {
            "processing_core_manifest_schema_version": 2,
            "channel": "stable",
            "version": "1.0.0",
            "contract_version": 1,
            "wheel": {
                "package": "mib-processing",
                "version": "1.0.0",
                "release_tag": "mib-processing-v1.0.0",
                "wheels": [{"filename": "fixture.whl"}],
            },
            "native_plugins": [],
            "profile_catalog_url": "https://updates.example/profiles/stable/catalog.json",
            "emodulus_lut_manifest_url": "https://updates.example/stable/emodulus-lut/latest.json",
        }
        # Deliberately differs from the publisher's pretty-printer. Promotion
        # must preserve immutable bytes rather than reconstructing this JSON.
        immutable = json.dumps(immutable_manifest, separators=(",", ":")).encode() + b"\n"
        index = {
            "processing_core_index_schema_version": 1,
            "channel": "stable",
            "active_version": "1.1.0",
            "updated_at": "2026-07-12T00:00:00Z",
            "versions": [
                {"version": "1.1.0", "wheels": []},
                {"version": "1.0.0", "wheels": []},
            ],
        }
        uploaded: list[tuple[str, bytes]] = []
        with (
            mock.patch.object(
                publish_processing_core,
                "read_existing_object",
                side_effect=[(immutable, True), (json.dumps(index).encode(), True)],
            ),
            mock.patch.object(
                publish_processing_core,
                "upload_object",
                side_effect=lambda **kwargs: uploaded.append(
                    (kwargs["key"], kwargs["file_path"].read_bytes())
                ),
            ),
        ):
            result = publish_processing_core.main([
                "--promote-version", "1.0.0",
                "--published-at", "2026-07-13T01:02:03Z",
                "--endpoint", "https://r2.invalid",
                "--upload-method", "s3",
            ])

        self.assertEqual(result, 0)
        self.assertEqual(uploaded[-1][0], "stable/processing-core/latest.json")
        self.assertEqual(uploaded[-1][1], immutable)
        promoted_index = json.loads(uploaded[0][1])
        self.assertEqual(promoted_index["active_version"], "1.0.0")
        self.assertEqual(promoted_index["updated_at"], "2026-07-13T01:02:03Z")

    def test_promote_fails_closed_when_version_is_not_catalogued(self) -> None:
        immutable = publish_processing_core.serialize_json({
            "processing_core_manifest_schema_version": 2,
            "channel": "stable",
            "version": "1.0.0",
            "contract_version": 1,
            "wheel": {
                "package": "mib-processing",
                "version": "1.0.0",
                "release_tag": "mib-processing-v1.0.0",
                "wheels": [{"filename": "fixture.whl"}],
            },
            "native_plugins": [],
            "profile_catalog_url": "https://updates.example/profiles/stable/catalog.json",
            "emodulus_lut_manifest_url": "https://updates.example/stable/emodulus-lut/latest.json",
        })
        index = json.dumps({
            "processing_core_index_schema_version": 1,
            "channel": "stable",
            "versions": [],
        }).encode()
        with (
            mock.patch.object(
                publish_processing_core,
                "read_existing_object",
                side_effect=[(immutable, True), (index, True)],
            ),
            mock.patch.object(publish_processing_core, "upload_object") as upload,
        ):
            result = publish_processing_core.main([
                "--promote-version", "1.0.0", "--dry-run",
            ])
        self.assertEqual(result, 1)
        upload.assert_not_called()

    def test_empty_promote_version_is_rejected_as_a_promotion(self) -> None:
        with mock.patch.object(publish_processing_core, "read_existing_object") as read:
            result = publish_processing_core.main(["--promote-version", "", "--dry-run"])
        self.assertEqual(result, 1)
        read.assert_not_called()


def quiet_main(argv: list[str]) -> int:
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return publish_processing_core.main(argv)


class SubtractRingRegressionTest(unittest.TestCase):
    """The default line must emit exactly the pre-core-line registry bytes."""

    def assert_matches_golden(self, documents: dict[str, bytes]) -> None:
        expected_names = sorted(
            path.relative_to(SUBTRACT_RING_GOLDEN_DIR).as_posix()
            for path in SUBTRACT_RING_GOLDEN_DIR.rglob("*") if path.is_file()
        )
        self.assertEqual(sorted(documents), expected_names)
        for name, data in documents.items():
            with self.subTest(document=name):
                self.assertEqual(data, (SUBTRACT_RING_GOLDEN_DIR / name).read_bytes())

    def run_scenario(self, root: Path, assets: Path) -> dict[str, bytes]:
        with contextlib.redirect_stdout(io.StringIO()):
            return run_subtract_ring_golden_scenario(publish_processing_core, root, assets)

    def test_default_line_documents_are_byte_identical_to_pre_line_publisher(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_subtract_ring_release_assets(root / "assets")
            self.assert_matches_golden(self.run_scenario(root, root / "assets"))

    def test_default_line_ignores_absdiff_laplacian_assets_in_the_same_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_subtract_ring_release_assets(root / "assets")
            write_absdiff_laplacian_release_assets(root / "assets")
            self.assert_matches_golden(self.run_scenario(root, root / "assets"))

    def test_explicit_subtract_ring_line_is_the_default(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_subtract_ring_release_assets(root / "assets")
            outputs = []
            for extra in ([], ["--line", "subtract-ring"]):
                out = root / f"out{len(outputs)}.json"
                self.assertEqual(quiet_main([
                    "--from-release", "mib-processing-v0.1.0",
                    "--pyproject", str(make_pyproject(root)),
                    "--release-assets-dir", str(root / "assets"),
                    "--published-at", GOLDEN_PUBLISHED_AT,
                    "--dry-run", "--manifest-out", str(out),
                    *extra,
                ]), 0)
                outputs.append(out.read_bytes())
            self.assertEqual(outputs[0], outputs[1])
            self.assertEqual(outputs[0], (SUBTRACT_RING_GOLDEN_DIR / "dry_run" / "latest.json").read_bytes())

    def test_subtract_ring_rejects_other_line_contract_abi_or_entrypoint(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_subtract_ring_release_assets(root)
            descriptor = root / "mib_processing_core-0.1.0-linux_x86_64.json"
            original = json.loads(descriptor.read_text(encoding="utf-8"))
            cases = {
                "engine ABI": {"engine_abi_version": 2},
                "entrypoint": {"entrypoint": "mib_processing_get_api_v2"},
                "algorithm": {"algorithm": "absdiff-laplacian"},
                "declares contract": {"contract_version": 2},
            }
            for message, change in cases.items():
                with self.subTest(field=message):
                    descriptor.write_text(json.dumps({**original, **change}), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, message):
                        publish_processing_core.build_native_plugin_entries(
                            [descriptor], asset_dir=root, repo="O/R",
                            release_tag="mib-processing-v0.1.0",
                            expected_version="0.1.0", expected_contract_version=1,
                        )
            descriptor.write_text(json.dumps({**original, "algorithm": "subtract-ring"}), encoding="utf-8")
            entry = publish_processing_core.build_native_plugin_entries(
                [descriptor], asset_dir=root, repo="O/R", release_tag="mib-processing-v0.1.0",
                expected_version="0.1.0", expected_contract_version=1,
            )[0]
            self.assertNotIn("algorithm", entry)
            with self.assertRaisesRegex(ValueError, "implements contract 1"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="O/R", release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0", expected_contract_version=2,
                )

    def test_subtract_ring_rejects_contract_override(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_subtract_ring_release_assets(root / "assets")
            self.assertEqual(quiet_main([
                "--from-release", "mib-processing-v0.1.0",
                "--pyproject", str(make_pyproject(root)),
                "--release-assets-dir", str(root / "assets"),
                "--contract-version", "2", "--dry-run",
            ]), 1)


C2 = "absdiff-laplacian"
C2_TAG = "mib-processing-absdiff-laplacian-v0.1.0"
C2_PREFIX = "stable/processing-core-absdiff-laplacian"


def c2_descriptor(artifact: str, native_os: str, version: str = "0.1.0", **overrides) -> dict:
    signing = (
        {"required": True, "scheme": "authenticode"}
        if native_os == "windows"
        else {
            "required": True,
            "scheme": "ed25519",
            "public_key_spki_base64": base64.b64encode(bytes(44)).decode("ascii"),
            "signature_base64": base64.b64encode(bytes(64)).decode("ascii"),
        }
    )
    payload = {
        "schema_version": 1,
        "version": version,
        "filename": artifact,
        "os": native_os,
        "arch": "x86_64",
        "algorithm": C2,
        "engine_abi_version": 2,
        "contract_version": 2,
        "entrypoint": "mib_processing_get_api_v2",
        "runtime_fingerprint": f"{native_os}-x86_64-fixture-cxx17",
        "app_min_version": "1.2.0",
        "app_max_version": None,
        "signing": signing,
    }
    payload.update(overrides)
    return payload


def write_absdiff_laplacian_release_assets(assets: Path, version: str = "0.1.0") -> list[Path]:
    """Contract-2 native cores for Windows and Linux. Returns the descriptors."""
    assets.mkdir(parents=True, exist_ok=True)
    descriptors = []
    for native_os, suffix in (("windows", ".dll"), ("linux", ".so")):
        stem = f"mib_processing_core-absdiff-laplacian-{version}-{native_os}_x86_64"
        (assets / f"{stem}{suffix}").write_bytes(f"signed {native_os} contract 2 fixture".encode())
        descriptor = assets / f"{stem}.json"
        descriptor.write_text(json.dumps(c2_descriptor(f"{stem}{suffix}", native_os, version)), encoding="utf-8")
        descriptors.append(descriptor)
    return descriptors


def make_c2_manifest(root: Path, version: str, published_at: str = "2026-07-13T00:00:00Z") -> dict:
    assets = root / f"c2-{version}"
    descriptors = write_absdiff_laplacian_release_assets(assets, version)
    tag = f"mib-processing-absdiff-laplacian-v{version}"
    plugins = publish_processing_core.build_native_plugin_entries(
        descriptors, asset_dir=assets, repo="KPT1020/mib-studio-qt", release_tag=tag,
        expected_version=version, line=C2,
    )
    return publish_processing_core.build_manifest(
        channel="stable",
        version=version,
        release_tag=tag,
        repo="KPT1020/mib-studio-qt",
        public_base_url="https://updates.example",
        native_plugins=plugins,
        published_at=published_at,
        line=C2,
    )


class AbsdiffLaplacianLineTest(unittest.TestCase):
    def dry_run(self, root: Path, assets: Path, *extra: str) -> tuple[int, dict, dict]:
        out = root / "out"
        result = quiet_main([
            "--line", C2,
            "--from-release", C2_TAG,
            "--pyproject", str(make_pyproject(root)),
            "--release-assets-dir", str(assets),
            "--published-at", GOLDEN_PUBLISHED_AT,
            "--dry-run",
            "--manifest-out", str(out / "latest.json"),
            "--version-manifest-out", str(out / "version.json"),
            "--index-out", str(out / "index.json"),
            *extra,
        ])
        if result != 0:
            return result, {}, {}
        self.assertEqual((out / "latest.json").read_bytes(), (out / "version.json").read_bytes())
        return (
            result,
            json.loads((out / "latest.json").read_text(encoding="utf-8")),
            json.loads((out / "index.json").read_text(encoding="utf-8")),
        )

    def test_registry_key_paths_and_upload_order(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            descriptors = write_absdiff_laplacian_release_assets(root / "assets")
            uploaded: list[str] = []
            argv = [
                "--line", C2,
                "--pyproject", str(make_pyproject(root)),
                "--published-at", GOLDEN_PUBLISHED_AT,
                "--endpoint", "https://r2.invalid", "--upload-method", "s3",
            ]
            for descriptor in descriptors:
                argv += ["--native-plugin-descriptor", str(descriptor)]
            with (
                mock.patch.object(
                    publish_processing_core, "read_existing_object",
                    side_effect=[(None, True), (None, True)],
                ) as read,
                mock.patch.object(
                    publish_processing_core, "upload_object",
                    side_effect=lambda **kwargs: uploaded.append(kwargs["key"]),
                ),
            ):
                self.assertEqual(quiet_main(argv), 0)
            self.assertEqual(
                [call.args[1] for call in read.call_args_list],
                [f"{C2_PREFIX}/versions/0.1.0.json", f"{C2_PREFIX}/index.json"],
            )
            self.assertEqual(uploaded, [
                f"{C2_PREFIX}/versions/0.1.0.json",
                f"{C2_PREFIX}/index.json",
                f"{C2_PREFIX}/latest.json",
            ])
            self.assertEqual(
                publish_processing_core.registry_base_key("beta", C2),
                "beta/processing-core-absdiff-laplacian",
            )
            self.assertEqual(publish_processing_core.registry_base_key("beta"), "beta/processing-core")

    def test_manifest_and_index_carry_no_wheel_and_no_pep503_page(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            assets = root / "assets"
            write_absdiff_laplacian_release_assets(assets)
            make_wheel(assets, "0.1.0", "cp311")  # a stray research wheel is ignored
            result, manifest, index = self.dry_run(root, assets)
            self.assertEqual(result, 0)
            self.assertNotIn("wheel", manifest)
            self.assertEqual(manifest["line"], C2)
            self.assertEqual(manifest["contract_version"], 2)
            self.assertEqual(manifest["processing_core_manifest_schema_version"], 2)
            self.assertEqual(manifest["release_tag"], C2_TAG)
            self.assertEqual(
                manifest["release_url"],
                f"https://github.com/KPT1020/mib-studio-qt/releases/tag/{C2_TAG}",
            )
            self.assertEqual(len(manifest["native_plugins"]), 2)
            for plugin in manifest["native_plugins"]:
                self.assertEqual(plugin["algorithm"], C2)
                self.assertEqual(plugin["contract_version"], 2)
                self.assertEqual(plugin["engine_abi_version"], 2)
                self.assertEqual(plugin["entrypoint"], "mib_processing_get_api_v2")
                self.assertIn(f"/releases/download/{C2_TAG}/mib_processing_core-absdiff-laplacian-0.1.0-", plugin["url"])
            self.assertEqual(index["line"], C2)
            self.assertEqual(index["active_version"], "0.1.0")
            [entry] = index["versions"]
            self.assertNotIn("wheels", entry)
            self.assertEqual(entry["line"], C2)
            self.assertEqual(entry["release_tag"], C2_TAG)
            self.assertEqual(
                entry["manifest_url"],
                f"https://updates.yofo.bio/{C2_PREFIX}/versions/0.1.0.json",
            )
            self.assertEqual(entry["native_plugins"], manifest["native_plugins"])

            with self.assertRaisesRegex(ValueError, "no PEP 503 page"):
                publish_processing_core.render_pep503_index(index)
            self.assertEqual(self.dry_run(root, assets, "--pep503-out", str(root / "p.html"))[0], 1)
            self.assertFalse((root / "p.html").exists())
            with self.assertRaisesRegex(ValueError, "publishes no wheel"):
                publish_processing_core.build_manifest(
                    channel="stable", version="0.1.0", release_tag=C2_TAG, repo="O/R",
                    public_base_url="https://updates.example",
                    wheel_paths=[make_wheel(root)], native_plugins=manifest["native_plugins"],
                    published_at=GOLDEN_PUBLISHED_AT, line=C2,
                )
            with self.assertRaisesRegex(ValueError, "native processing core is required"):
                publish_processing_core.build_manifest(
                    channel="stable", version="0.1.0", release_tag=C2_TAG, repo="O/R",
                    public_base_url="https://updates.example", published_at=GOLDEN_PUBLISHED_AT,
                    line=C2,
                )

    def test_explicit_wheel_input_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            descriptors = write_absdiff_laplacian_release_assets(root)
            with mock.patch.object(publish_processing_core, "upload_object") as upload:
                result = quiet_main([
                    "--line", C2, "--pyproject", str(make_pyproject(root)), "--dry-run",
                    "--wheel", str(make_wheel(root)),
                    "--native-plugin-descriptor", str(descriptors[0]),
                ])
            self.assertEqual(result, 1)
            upload.assert_not_called()

    def test_release_without_line_assets_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_subtract_ring_release_assets(root / "assets")
            self.assertEqual(self.dry_run(root, root / "assets")[0], 1)

    def test_mixed_line_asset_directory_discovery(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            assets = root / "assets"
            write_subtract_ring_release_assets(assets)
            c2_descriptors = write_absdiff_laplacian_release_assets(assets)
            (assets / "release-notes.json").write_text(json.dumps({"filename": "x", "engine_abi_version": 1}))
            self.assertEqual(
                publish_processing_core.discover_native_descriptors(assets),
                [
                    assets / "mib_processing_core-0.1.0-linux_x86_64.json",
                    assets / "mib_processing_core-0.1.0-windows_x86_64.json",
                ],
            )
            self.assertEqual(
                publish_processing_core.discover_native_descriptors(assets, "subtract-ring"),
                publish_processing_core.discover_native_descriptors(assets),
            )
            self.assertEqual(
                publish_processing_core.discover_native_descriptors(assets, C2),
                sorted(c2_descriptors),
            )
            result, manifest, _index = self.dry_run(root, assets)
            self.assertEqual(result, 0)
            self.assertEqual(
                sorted(plugin["filename"] for plugin in manifest["native_plugins"]),
                [
                    "mib_processing_core-absdiff-laplacian-0.1.0-linux_x86_64.so",
                    "mib_processing_core-absdiff-laplacian-0.1.0-windows_x86_64.dll",
                ],
            )

    def test_descriptor_must_match_line_contract_abi_algorithm_entrypoint_and_name(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            stem = "mib_processing_core-absdiff-laplacian-0.1.0-linux_x86_64"
            (root / f"{stem}.so").write_bytes(b"contract 2 fixture")
            (root / "mib_processing_core-0.1.0-linux_x86_64.so").write_bytes(b"contract 1 fixture")
            descriptor = root / f"{stem}.json"
            cases = {
                "declares contract 1": {"contract_version": 1},
                "must declare contract_version": {"contract_version": None},
                "engine ABI 1": {"engine_abi_version": 1},
                "algorithm 'subtract-ring'": {"algorithm": "subtract-ring"},
                "must declare algorithm": {"algorithm": None},
                "entrypoint 'mib_processing_get_api'": {"entrypoint": "mib_processing_get_api"},
                "not named for the absdiff-laplacian line": {
                    "filename": "mib_processing_core-0.1.0-linux_x86_64.so"
                },
            }
            for message, change in cases.items():
                with self.subTest(case=message):
                    payload = c2_descriptor(f"{stem}.so", "linux", **change)
                    payload = {key: value for key, value in payload.items() if value is not None or key == "app_max_version"}
                    descriptor.write_text(json.dumps(payload), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, message):
                        publish_processing_core.build_native_plugin_entries(
                            [descriptor], asset_dir=root, repo="O/R", release_tag=C2_TAG,
                            expected_version="0.1.0", line=C2,
                        )
            with self.assertRaisesRegex(ValueError, "implements contract 2"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="O/R", release_tag=C2_TAG,
                    expected_version="0.1.0", expected_contract_version=1, line=C2,
                )
            # A Contract-2 descriptor is refused by the default subtract-ring line too.
            descriptor.write_text(json.dumps(c2_descriptor(f"{stem}.so", "linux")), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not named for the subtract-ring line"):
                publish_processing_core.build_native_plugin_entries(
                    [descriptor], asset_dir=root, repo="O/R", release_tag="mib-processing-v0.1.0",
                    expected_version="0.1.0", expected_contract_version=1,
                )

    def test_release_tag_must_match_line(self) -> None:
        tag_version = publish_processing_core.version_from_release_tag
        self.assertEqual(tag_version(C2_TAG, C2), "0.1.0")
        self.assertEqual(tag_version("mib-processing-v0.1.0"), "0.1.0")
        with self.assertRaisesRegex(ValueError, "belongs to the subtract-ring line"):
            tag_version("mib-processing-v0.1.0", C2)
        with self.assertRaisesRegex(ValueError, "belongs to the absdiff-laplacian line"):
            tag_version(C2_TAG)
        with self.assertRaisesRegex(ValueError, "form mib-processing-absdiff-laplacian-v"):
            tag_version("other-v0.1.0", C2)
        with self.assertRaises(ValueError):
            tag_version("mib-processing-absdiff-laplacian-v", C2)
        with self.assertRaisesRegex(ValueError, "Unknown processing-core line"):
            tag_version(C2_TAG, "absdiff")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_absdiff_laplacian_release_assets(root / "assets")
            write_subtract_ring_release_assets(root / "assets")
            pyproject = str(make_pyproject(root))
            common = ["--pyproject", pyproject, "--release-assets-dir", str(root / "assets"), "--dry-run"]
            self.assertEqual(quiet_main(["--line", C2, "--from-release", "mib-processing-v0.1.0", *common]), 1)
            self.assertEqual(quiet_main(["--from-release", C2_TAG, *common]), 1)
            self.assertEqual(quiet_main(["--line", C2, "--from-release", C2_TAG, *common]), 0)
            self.assertEqual(quiet_main([
                "--line", C2, "--from-release", C2_TAG, "--contract-version", "1", *common,
            ]), 1)
            self.assertEqual(quiet_main([
                "--line", C2, "--from-release", C2_TAG, "--version", "0.1.0", *common,
            ]), 0)
            self.assertEqual(quiet_main([
                "--line", C2, "--from-release", C2_TAG, "--version", "0.2.0", *common,
            ]), 1)
            descriptor = root / "assets" / "mib_processing_core-absdiff-laplacian-0.1.0-linux_x86_64.json"
            self.assertEqual(quiet_main([
                "--line", C2, "--pyproject", pyproject, "--dry-run",
                "--release-tag", "mib-processing-v0.1.0",
                "--native-plugin-descriptor", str(descriptor),
            ]), 1)

    def test_from_release_downloads_the_line_tag(self) -> None:
        downloads: list[str] = []

        def download(_repo: str, tag: str, destination: Path, _gh_bin: str) -> None:
            downloads.append(tag)
            write_absdiff_laplacian_release_assets(destination)

        with (
            tempfile.TemporaryDirectory() as temp_dir,
            mock.patch.object(
                publish_processing_core, "inspect_github_release",
                return_value={"tagName": C2_TAG, "publishedAt": GOLDEN_PUBLISHED_AT},
            ) as inspect,
            mock.patch.object(publish_processing_core, "download_github_release", side_effect=download),
        ):
            root = Path(temp_dir)
            out = root / "latest.json"
            result = quiet_main([
                "--line", C2, "--from-release", C2_TAG,
                "--pyproject", str(make_pyproject(root)),
                "--dry-run", "--manifest-out", str(out),
            ])
            self.assertEqual(result, 0)
            inspect.assert_called_once_with("KPT1020/mib-studio-qt", C2_TAG, "gh")
            self.assertEqual(downloads, [C2_TAG])
            manifest = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(manifest["published_at"], GOLDEN_PUBLISHED_AT)
            self.assertEqual(len(manifest["native_plugins"]), 2)

    def test_immutable_version_republish_is_idempotent_and_conflict_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            descriptors = write_absdiff_laplacian_release_assets(root / "assets")
            pyproject = str(make_pyproject(root))
            argv = ["--line", C2, "--pyproject", pyproject, "--published-at", GOLDEN_PUBLISHED_AT]
            for descriptor in descriptors:
                argv += ["--native-plugin-descriptor", str(descriptor)]
            preview = root / "version.json"
            index_preview = root / "index.json"
            self.assertEqual(quiet_main([
                *argv, "--dry-run", "--version-manifest-out", str(preview),
                "--index-out", str(index_preview),
            ]), 0)
            immutable = preview.read_bytes()
            live = argv + ["--endpoint", "https://r2.invalid", "--upload-method", "s3"]

            uploaded: list[str] = []
            with (
                mock.patch.object(
                    publish_processing_core, "read_existing_object",
                    side_effect=[(immutable, True), (index_preview.read_bytes(), True)],
                ),
                mock.patch.object(
                    publish_processing_core, "upload_object",
                    side_effect=lambda **kwargs: uploaded.append(kwargs["key"]),
                ),
            ):
                self.assertEqual(quiet_main(live), 0)
            self.assertEqual(uploaded, [f"{C2_PREFIX}/index.json", f"{C2_PREFIX}/latest.json"])

            conflicting = json.loads(immutable)
            conflicting["native_plugins"] = conflicting["native_plugins"][:1]
            with (
                mock.patch.object(
                    publish_processing_core, "read_existing_object",
                    side_effect=[(publish_processing_core.serialize_json(conflicting), True), (None, True)],
                ),
                mock.patch.object(publish_processing_core, "upload_object") as upload,
            ):
                self.assertEqual(quiet_main(live), 1)
            upload.assert_not_called()

            with (
                mock.patch.object(
                    publish_processing_core, "read_existing_object",
                    side_effect=[(None, False), (None, True)],
                ),
                mock.patch.object(publish_processing_core, "upload_object") as upload,
            ):
                self.assertEqual(quiet_main(live), 1)
            upload.assert_not_called()

    def test_merge_index_keeps_history_and_refuses_other_lines(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            base = "https://updates.example"
            v010 = make_c2_manifest(root, "0.1.0", "2026-07-10T00:00:00Z")
            v020 = make_c2_manifest(root, "0.2.0", "2026-07-11T00:00:00Z")
            index = publish_processing_core.merge_index({}, v010, base)
            index = publish_processing_core.merge_index(index, v020, base)
            self.assertEqual(list(index)[:3], ["processing_core_index_schema_version", "channel", "line"])
            self.assertEqual(index["line"], C2)
            self.assertEqual([entry["version"] for entry in index["versions"]], ["0.2.0", "0.1.0"])
            self.assertEqual(index["active_version"], "0.2.0")
            self.assertEqual(
                index["versions"][1]["manifest_url"],
                f"{base}/{C2_PREFIX}/versions/0.1.0.json",
            )
            for entry in index["versions"]:
                self.assertNotIn("wheels", entry)
                self.assertEqual(entry["contract_version"], 2)

            rolled_back = publish_processing_core.merge_index(index, v010, base)
            self.assertEqual(rolled_back["active_version"], "0.1.0")
            self.assertEqual(len(rolled_back["versions"]), 2)

            c1_manifest = make_manifest(root, "0.1.0")
            c1_index = publish_processing_core.merge_index({}, c1_manifest, base)
            self.assertNotIn("line", c1_index)
            with self.assertRaisesRegex(ValueError, "Existing index line is None"):
                publish_processing_core.merge_index(c1_index, v010, base)
            with self.assertRaisesRegex(ValueError, "Existing index line is 'absdiff-laplacian'"):
                publish_processing_core.merge_index(index, c1_manifest, base)

            with_wheels = json.loads(json.dumps(index))
            with_wheels["versions"][0]["wheels"] = []
            with self.assertRaisesRegex(ValueError, "carries wheel data"):
                publish_processing_core.merge_index(with_wheels, v010, base)
            wrong_contract = json.loads(json.dumps(index))
            wrong_contract["versions"][0]["contract_version"] = 1
            with self.assertRaisesRegex(ValueError, "declares contract 1"):
                publish_processing_core.merge_index(wrong_contract, v010, base)
            wrong_line = json.loads(json.dumps(index))
            del wrong_line["versions"][0]["line"]
            with self.assertRaisesRegex(ValueError, "belongs to line None"):
                publish_processing_core.merge_index(wrong_line, v010, base)

            with_wheel = dict(v010, wheel={"wheels": []})
            with self.assertRaisesRegex(ValueError, "must not carry wheel data"):
                publish_processing_core.index_entry_from_manifest(with_wheel, base)

    def test_promote_copies_bytes_without_pep503_and_refuses_other_line(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            v010 = make_c2_manifest(root, "0.1.0", "2026-07-10T00:00:00Z")
            v020 = make_c2_manifest(root, "0.2.0", "2026-07-11T00:00:00Z")
            index = publish_processing_core.merge_index(
                publish_processing_core.merge_index({}, v010, "https://updates.example"),
                v020, "https://updates.example",
            )
            immutable = json.dumps(v010, separators=(",", ":")).encode() + b"\n"
            uploaded: list[tuple[str, bytes]] = []
            with (
                mock.patch.object(
                    publish_processing_core, "read_existing_object",
                    side_effect=[(immutable, True), (json.dumps(index).encode(), True)],
                ) as read,
                mock.patch.object(
                    publish_processing_core, "upload_object",
                    side_effect=lambda **kwargs: uploaded.append(
                        (kwargs["key"], kwargs["file_path"].read_bytes())
                    ),
                ),
            ):
                result = quiet_main([
                    "--line", C2, "--promote-version", "0.1.0",
                    "--published-at", "2026-07-13T01:02:03Z",
                    "--endpoint", "https://r2.invalid", "--upload-method", "s3",
                ])
            self.assertEqual(result, 0)
            self.assertEqual(read.call_args_list[0].args[1], f"{C2_PREFIX}/versions/0.1.0.json")
            self.assertEqual([key for key, _ in uploaded], [f"{C2_PREFIX}/index.json", f"{C2_PREFIX}/latest.json"])
            self.assertEqual(uploaded[-1][1], immutable)
            self.assertEqual(json.loads(uploaded[0][1])["active_version"], "0.1.0")

            c1_immutable = publish_processing_core.serialize_json(make_manifest(root, "0.1.0"))
            for line_args, manifest_bytes, catalog in (
                (["--line", C2], c1_immutable, index),
                ([], publish_processing_core.serialize_json(v010), index),
            ):
                with self.subTest(line=line_args), (
                    mock.patch.object(
                        publish_processing_core, "read_existing_object",
                        side_effect=[(manifest_bytes, True), (json.dumps(catalog).encode(), True)],
                    )
                ), mock.patch.object(publish_processing_core, "upload_object") as upload:
                    result = quiet_main([*line_args, "--promote-version", "0.1.0", "--dry-run"])
                    self.assertEqual(result, 1)
                    upload.assert_not_called()


class ReadWheelVersionTest(unittest.TestCase):
    def test_reads_version_from_real_pyproject(self) -> None:
        pyproject = REPO_ROOT / "bindings" / "python" / "pyproject.toml"
        wrapper = REPO_ROOT / "bindings" / "python" / "python" / "mib_processing" / "__init__.py"
        match = re.search(
            r'^__version__\s*=\s*["\']([^"\']+)["\']',
            wrapper.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
        self.assertIsNotNone(match)
        self.assertEqual(publish_processing_core.read_wheel_version(pyproject), match.group(1))

    def test_missing_project_version_raises(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            bad_pyproject = Path(temp_dir) / "pyproject.toml"
            bad_pyproject.write_text("[build-system]\nrequires = []\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                publish_processing_core.read_wheel_version(bad_pyproject)


if __name__ == "__main__":
    unittest.main()
