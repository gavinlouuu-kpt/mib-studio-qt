from __future__ import annotations

import base64
import hashlib
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


def plugin(os_name: str, arch: str = "x86_64", version: str = "0.1.0") -> dict:
    ext = "dll" if os_name == "windows" else "so"
    return {"filename": f"mib_processing_core-{LINE}-{version}-{os_name}_{arch}.{ext}", "os": os_name, "arch": arch,
            "version": version, "contract_version": 2, "engine_abi_version": 2,
            "entrypoint": "mib_processing_get_api_v2", "sha256": "a" * 64}


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
            plugin("windows", version="0.2.0"), plugin("linux", version="0.2.0")]), BASE)
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


def write_release(root: Path, version: str = "0.1.0") -> None:
    """Four signed-looking release assets: bytes plus sidecars the publisher accepts."""
    for os_name, ext, scheme in (("windows", "dll", "authenticode"), ("linux", "so", "ed25519")):
        stem = f"mib_processing_core-{LINE}-{version}-{os_name}_x86_64"
        (root / f"{stem}.{ext}").write_bytes(f"{os_name} plugin {version}".encode())
        signing = {"scheme": scheme, "required": True}
        if scheme == "ed25519":
            # Shape-valid detached envelope (the publisher checks sizes and the key hash).
            spki = bytes(range(44))
            signing |= {"public_key_spki_base64": base64.b64encode(spki).decode("ascii"),
                        "public_key_spki_sha256": hashlib.sha256(spki).hexdigest(),
                        "signature_base64": base64.b64encode(bytes(64)).decode("ascii")}
        (root / f"{stem}.json").write_text(json.dumps({
            "schema_version": 1, "algorithm": LINE, "version": version, "filename": f"{stem}.{ext}",
            "os": os_name, "arch": "x86_64", "engine_abi_version": 2, "contract_version": 2,
            "entrypoint": "mib_processing_get_api_v2", "runtime_fingerprint": f"{os_name}-fp",
            "app_min_version": "1.1.2", "app_max_version": "1.1.2", "signing": signing}), encoding="utf-8")


class LinePublishTest(unittest.TestCase):
    def run_publish(self, root: Path, *extra: str, existing=None):
        uploads = []
        existing = existing or {}

        def read(_args, key):
            return existing.get(key), True

        with (mock.patch.object(line_pub.core, "read_existing_object", side_effect=read),
              mock.patch.object(line_pub.core, "upload_object",
                                side_effect=lambda **kw: uploads.append((kw["key"], kw["file_path"].read_bytes())))):
            code = line_pub.main(["--line", LINE, "--channel", "beta", "--from-release", TAG,
                                  "--release-assets-dir", str(root), "--published-at", "2026-10-04T00:00:00Z",
                                  "--endpoint", "https://r2.invalid", "--upload-method", "s3", *extra])
        return code, uploads

    def test_publish_uploads_version_then_index_then_latest(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_release(root)
            code, uploads = self.run_publish(root)
        self.assertEqual(code, 0)
        base = f"beta/processing-core/{LINE}"
        self.assertEqual([key for key, _ in uploads],
                         [f"{base}/versions/0.1.0.json", f"{base}/index.json", f"{base}/latest.json"])
        latest = json.loads(uploads[-1][1])
        self.assertEqual((latest["line"], latest["contract_version"], len(latest["native_plugins"])),
                         (LINE, 2, 2))

    def test_republish_identical_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_release(root)
            _, first = self.run_publish(root)
            version_key, version_bytes = first[0]
            code, second = self.run_publish(root, existing={version_key: version_bytes})
        self.assertEqual(code, 0)
        self.assertNotIn(version_key, [key for key, _ in second])

    def test_republish_different_content_fails_without_upload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_release(root)
            key = f"beta/processing-core/{LINE}/versions/0.1.0.json"
            code, uploads = self.run_publish(root, existing={key: b"{}\n"})
        self.assertEqual(code, 1)
        self.assertEqual(uploads, [])

    def test_stable_channel_is_refused_without_upload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir, \
                mock.patch.object(line_pub.core, "upload_object") as upload:
            root = Path(temp_dir)
            write_release(root)
            code = line_pub.main(["--line", LINE, "--channel", "stable", "--from-release", TAG,
                                  "--release-assets-dir", str(root), "--published-at", "2026-10-04T00:00:00Z",
                                  "--endpoint", "https://r2.invalid", "--upload-method", "s3"])
        self.assertEqual(code, 1)
        upload.assert_not_called()

    def test_mutating_publish_requires_s3_endpoint(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir, \
                mock.patch.object(line_pub.core, "upload_object") as upload:
            root = Path(temp_dir)
            write_release(root)
            code = line_pub.main(["--line", LINE, "--channel", "beta", "--from-release", TAG,
                                  "--release-assets-dir", str(root), "--published-at", "2026-10-04T00:00:00Z"])
        self.assertEqual(code, 1)
        upload.assert_not_called()

    def test_promote_copies_immutable_version_to_latest_and_index(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_release(root)
            _, first = self.run_publish(root)
        existing = dict(first)
        uploads = []
        with (mock.patch.object(line_pub.core, "read_existing_object",
                                side_effect=lambda _a, key: (existing.get(key), True)),
              mock.patch.object(line_pub.core, "upload_object",
                                side_effect=lambda **kw: uploads.append((kw["key"], kw["file_path"].read_bytes())))):
            code = line_pub.main(["--line", LINE, "--channel", "beta", "--promote-version", "0.1.0",
                                  "--published-at", "2026-10-05T00:00:00Z",
                                  "--endpoint", "https://r2.invalid", "--upload-method", "s3"])
        self.assertEqual(code, 0)
        base = f"beta/processing-core/{LINE}"
        self.assertEqual([key for key, _ in uploads], [f"{base}/index.json", f"{base}/latest.json"])
        self.assertEqual(uploads[1][1], existing[f"{base}/versions/0.1.0.json"])  # copied byte for byte

    def test_promote_unknown_version_fails_without_upload(self) -> None:
        with mock.patch.object(line_pub.core, "read_existing_object", return_value=(None, True)), \
                mock.patch.object(line_pub.core, "upload_object") as upload:
            code = line_pub.main(["--line", LINE, "--channel", "beta", "--promote-version", "0.3.2",
                                  "--published-at", "2026-10-05T00:00:00Z",
                                  "--endpoint", "https://r2.invalid", "--upload-method", "s3"])
        self.assertEqual(code, 1)
        upload.assert_not_called()


class LineIntegrityTest(unittest.TestCase):
    def test_manifest_rejects_wrong_abi_entrypoint_or_line_filename(self) -> None:
        cases = {
            "engine ABI": plugin("linux") | {"engine_abi_version": 1},
            "entrypoint": plugin("linux") | {"entrypoint": "mib_processing_get_api"},
            "filename": plugin("linux") | {"filename": "mib_processing_core-subtract-ring-0.1.0-linux_x86_64.so"},
        }
        for label, bad in cases.items():
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, label):
                manifest(plugins=[plugin("windows"), bad])

    def published(self) -> dict:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_release(root)
            _, uploads = LinePublishTest().run_publish(root)
        return dict(uploads)

    def promote(self, existing: dict) -> tuple[int, list]:
        uploads = []
        with (mock.patch.object(line_pub.core, "read_existing_object",
                                side_effect=lambda _a, key: (existing.get(key), True)),
              mock.patch.object(line_pub.core, "upload_object",
                                side_effect=lambda **kw: uploads.append(kw["key"]))):
            code = line_pub.main(["--line", LINE, "--channel", "beta", "--promote-version", "0.1.0",
                                  "--published-at", "2026-10-05T00:00:00Z",
                                  "--endpoint", "https://r2.invalid", "--upload-method", "s3"])
        return code, uploads

    def test_promote_refuses_a_missing_index(self) -> None:
        existing = self.published()
        del existing[f"beta/processing-core/{LINE}/index.json"]
        code, uploads = self.promote(existing)
        self.assertEqual((code, uploads), (1, []))

    def test_promote_refuses_a_document_with_the_wrong_identity(self) -> None:
        existing = self.published()
        key = f"beta/processing-core/{LINE}/versions/0.1.0.json"
        for field, value in (("processing_core_line_manifest_schema_version", 99),
                             ("contract_version", 1),
                             ("release_tag", "mib-processing-subtract-ring-v0.1.0")):
            doc = json.loads(existing[key]) | {field: value}
            with self.subTest(field=field):
                code, uploads = self.promote(existing | {key: (json.dumps(doc, indent=2) + chr(10)).encode()})
                self.assertEqual((code, uploads), (1, []))


if __name__ == "__main__":
    unittest.main()
