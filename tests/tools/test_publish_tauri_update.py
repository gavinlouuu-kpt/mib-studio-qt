"""Regression tests for the platform-scoped Tauri update publisher."""
from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("publish_tauri_update", ROOT / "scripts/release/publish-tauri-update.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def test_manifest_matches_tauri_feed_contract() -> None:
    with tempfile.TemporaryDirectory() as root:
        installer = pathlib.Path(root) / "mib-studio-desktop_1.2.3_x64-setup.exe"
        installer.write_bytes(b"candidate")
        prefix, manifest = MODULE.build_manifest(
            installer,
            version="1.2.3-beta.7",
            channel="beta",
            os_name="windows",
            arch="x86_64",
            public_base_url="https://updates.example",
            published_at="2026-09-23T00:00:00Z",
        )
        assert prefix == "beta/tauri/windows-x86_64"
        assert manifest["artifact_family"] == "tauri"
        assert manifest["os"] == "windows"
        assert manifest["arch"] == "x86_64"
        assert manifest["channel"] == "beta"
        assert manifest["installer_url"] == "https://updates.example/beta/tauri/windows-x86_64/mib-studio-desktop_1.2.3_x64-setup.exe"
        assert manifest["installer_size_bytes"] == 9
        assert manifest["installer_sha256"] == MODULE.hashlib.sha256(b"candidate").hexdigest()


def test_linux_and_windows_extensions_are_scoped() -> None:
    with tempfile.TemporaryDirectory() as root:
        deb = pathlib.Path(root) / "mib.deb"
        deb.write_bytes(b"deb")
        MODULE.build_manifest(deb, version="1.0.0", channel="stable", os_name="linux", arch="x86_64", public_base_url="https://x")
        try:
            MODULE.build_manifest(deb, version="1.0.0", channel="stable", os_name="windows", arch="x86_64", public_base_url="https://x")
        except ValueError as error:
            assert "windows" in str(error)
        else:
            raise AssertionError("Linux artifact was accepted for Windows")


def test_dry_run_writes_a_reusable_manifest_without_uploading() -> None:
    with tempfile.TemporaryDirectory() as root:
        installer = pathlib.Path(root) / "mib.rpm"
        manifest = pathlib.Path(root) / "latest.json"
        installer.write_bytes(b"rpm")
        assert MODULE.main([
            "--installer", str(installer), "--version", "1.0.0", "--os", "linux", "--arch", "x86_64",
            "--manifest-out", str(manifest), "--dry-run",
        ]) == 0
        data = json.loads(manifest.read_text())
        assert data["artifact_family"] == "tauri"
        assert data["installer_url"].endswith("/stable/tauri/linux-x86_64/mib.rpm")


if __name__ == "__main__":
    test_manifest_matches_tauri_feed_contract()
    test_linux_and_windows_extensions_are_scoped()
    test_dry_run_writes_a_reusable_manifest_without_uploading()
    print("ok")
