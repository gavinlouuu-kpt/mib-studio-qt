#!/usr/bin/env python3
"""Unit tests for publish-processing-core.py (no network or R2 access)."""

from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT_PATH = Path(__file__).resolve().parent / "publish-processing-core.py"
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


class ReadWheelVersionTest(unittest.TestCase):
    def test_reads_version_from_real_pyproject(self) -> None:
        pyproject = Path(__file__).resolve().parent / "bindings" / "python" / "pyproject.toml"
        wrapper = (
            Path(__file__).resolve().parent
            / "bindings" / "python" / "python" / "mib_processing" / "__init__.py"
        )
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
