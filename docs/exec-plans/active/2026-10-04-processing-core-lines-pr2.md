# Processing-core lines, PR 2 (T1.1b, sign and publish absdiff-laplacian) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Status: active (2026-10-04). Branch `feat/t1.1b-core-lines-pr2`, stacked on `feat/t1.1b-core-lines` (#476), which is stacked on #473.

**Goal:** A pushed `mib-processing-absdiff-laplacian-v<version>` tag builds, audits, gold-checks, signs and publishes the Contract-2 core to its own registry subtree on the `beta` channel, and it can be promoted or rolled back there. The subtract-ring release path does not change.

**Architecture:** A new native-only workflow `processing-core-line.yml` handles the absdiff tag. It reuses the build and sign recipes of `python-wheel.yml`, restricted to the absdiff target and artifacts. A new native-only line publisher `scripts/release/publish-processing-core-line.py` writes `{channel}/processing-core/absdiff-laplacian/{versions/<v>.json, index.json, latest.json}`. It imports the hashing, immutability and upload helpers of `publish-processing-core.py` instead of copying them. `processing-core-promote.yml` gains a `line` input.

**Tech Stack:** GitHub Actions, Python 3.12+ (stdlib, boto3 for S3 as today), PowerShell/signtool (Authenticode), OpenSSL Ed25519.

**Spec:** `docs/exec-plans/active/2026-09-25-contract2-safe-rollout.md` (T1.1, Phase 1 exit gate: "a signed Contract 2 core passes the Contract 2 references on Linux and Windows"), `docs/decisions/0007-one-contract-per-shipped-core.md` (Consequences: "Every release signs one artifact per maintained contract, and the registry exposes the contract of each artifact"), and PR 1's plan `2026-10-02-processing-core-lines-pr1.md`.

## Decisions (Gavin, 2026-10-04)

- A new native-only workflow for absdiff tags. `python-wheel.yml` stays untouched.
- A native-only line manifest in its own subtree, `{channel}/processing-core/absdiff-laplacian/`. Existing keys and current apps are unaffected.
- The same production signer as subtract-ring: the Authenticode certificate and Ed25519 key in the `Production` environment, with the SPKI pins unchanged.
- Beta only until Phase 3 opt-in. Publishing or promoting absdiff to `stable` is refused in code. Lifting that is a separate, reviewed change that removes the line from `BETA_ONLY_LINES`.

## Global Constraints

- Line name `absdiff-laplacian`, contract 2, engine ABI 2, entrypoint `mib_processing_get_api_v2`. Tag `mib-processing-absdiff-laplacian-v<version>`; the version must equal `processing-cores/absdiff-laplacian.version`.
- Release assets are exactly the 4 native files: `mib_processing_core-absdiff-laplacian-<v>-{windows_x86_64.dll,windows_x86_64.json,linux_x86_64.so,linux_x86_64.json}`. There are no wheels.
- Registry objects: `<channel>/processing-core/absdiff-laplacian/versions/<v>.json` (immutable, byte-identical on re-run), `.../index.json`, `.../latest.json` (written last). Do not touch `<channel>/processing-core/{latest,index}.json` or `versions/`.
- Mutating publication goes through `--upload-method s3` with the R2 endpoint, the same rule as `require_consistent_mutating_transport`.
- The Windows release DLL must keep importing no `opencv*`/`Qt*`/`hdf5*`/`python*` DLLs and export exactly `mib_processing_get_api_v2`.
- Vault maintenance and `scripts/check_docs.py` apply as in PR 1.

## Review Focus

1. **A tag whose version disagrees with `processing-cores/absdiff-laplacian.version`.** The workflow must fail before signing (Task 3 validate job, asserted in `test_processing_core_line_workflow.py`).
2. **A re-run of the same tag after a partial publish.** The immutable version document must compare byte-identical and skip, not fail or overwrite (Task 2 `test_republish_identical_is_idempotent`).
3. **A publish or promote to `stable`.** It must be refused for a beta-only line, with no upload (Task 1 and Task 2 tests).
4. **A manifest missing one platform** (only Windows signed). It must be refused, so rigs never see a half release (Task 1 `test_manifest_requires_both_platforms`).
5. **Promoting a version absent from the line subtree** (for example a subtract-ring version typed into the absdiff promote). It must fail with no pointer change (Task 2 `test_promote_unknown_version_fails_without_upload`).

---

### Task 1: Line manifest and index (pure functions)

**Files:**
- Create: `scripts/release/publish-processing-core-line.py` (module part)
- Test: `tests/release/test_publish_processing_core_line.py`

**Interfaces:**
- Produces (module `publish_processing_core_line`):
  - `LINES: dict[str, dict]` = `{"absdiff-laplacian": {"contract_version": 2, "engine_abi_version": 2}}`
  - `BETA_ONLY_LINES = {"absdiff-laplacian"}`
  - `LINE_MANIFEST_SCHEMA_VERSION = 1`, `LINE_INDEX_SCHEMA_VERSION = 1`
  - `REQUIRED_PLATFORMS = {("windows", "x86_64"), ("linux", "x86_64")}`
  - `tag_prefix(line) -> str`, `version_from_line_tag(line, tag) -> str`
  - `check_channel(line, channel) -> str` (raises `ValueError` for stable on a beta-only line)
  - `line_base_key(channel, line) -> str` returning `"<channel>/processing-core/<line>"`
  - `build_line_manifest(*, line, channel, version, release_tag, repo, native_plugins, published_at, public_base_url) -> dict`
  - `merge_line_index(existing, manifest, public_base_url) -> dict`
- Consumes from `publish-processing-core.py` (loaded with `importlib`): `validate_version`, `validate_channel`, `validate_published_at`, `version_object_component`, `join_public_object_url`, `_version_sort_key`.

- [ ] **Step 1: Write the failing tests**

```python
# tests/release/test_publish_processing_core_line.py
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
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `python tests/release/test_publish_processing_core_line.py -v`. Expected: ERROR `FileNotFoundError` (the script does not exist).

- [ ] **Step 3: Implement the module part**

```python
#!/usr/bin/env python3
"""Publish one native-only processing-core line (ADR 0007, T1.1b PR 2).

Registry layout per line, beside (never inside) the subtract-ring keys:
  <channel>/processing-core/<line>/versions/<version>.json   immutable
  <channel>/processing-core/<line>/index.json                all versions + active pointer
  <channel>/processing-core/<line>/latest.json               active version, written last
subtract-ring keeps publish-processing-core.py (wheel + native, legacy keys).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import tempfile
from pathlib import Path
from typing import Any

_CORE_PATH = Path(__file__).resolve().parent / "publish-processing-core.py"
_spec = importlib.util.spec_from_file_location("publish_processing_core", _CORE_PATH)
core = importlib.util.module_from_spec(_spec)
sys.modules.setdefault("publish_processing_core", core)
_spec.loader.exec_module(core)

LINES: dict[str, dict[str, int]] = {
    "absdiff-laplacian": {"contract_version": 2, "engine_abi_version": 2},
}
# Lines that may only publish/promote on beta until the rollout plan's Phase 3
# (opt-in on rigs) is approved; removing a line here is that approval.
BETA_ONLY_LINES = {"absdiff-laplacian"}
LINE_MANIFEST_SCHEMA_VERSION = 1
LINE_INDEX_SCHEMA_VERSION = 1
REQUIRED_PLATFORMS = {("windows", "x86_64"), ("linux", "x86_64")}


def _line(line: str) -> dict[str, int]:
    if line not in LINES:
        raise ValueError(f"Unknown native-only processing-core line: {line!r}")
    return LINES[line]


def tag_prefix(line: str) -> str:
    _line(line)
    return f"mib-processing-{line}-v"


def version_from_line_tag(line: str, tag: str) -> str:
    prefix = tag_prefix(line)
    if not tag.startswith(prefix) or len(tag) == len(prefix):
        raise ValueError(f"Release tag must have the form {prefix}<version>: {tag!r}")
    return core.validate_version(tag[len(prefix):])


def check_channel(line: str, channel: str) -> str:
    _line(line)
    channel = core.validate_channel(channel)
    if line in BETA_ONLY_LINES and channel != "beta":
        raise ValueError(f"{line} is beta only until Phase 3 opt-in; refusing channel {channel!r}")
    return channel


def line_base_key(channel: str, line: str) -> str:
    return f"{channel}/processing-core/{line}"


def build_line_manifest(*, line: str, channel: str, version: str, release_tag: str, repo: str,
                        native_plugins: list[dict[str, Any]], published_at: str,
                        public_base_url: str) -> dict[str, Any]:
    identity = _line(line)
    channel = check_channel(line, channel)
    version = core.validate_version(version)
    tagged = version_from_line_tag(line, release_tag)
    if tagged != version:
        raise ValueError(f"Release tag {release_tag!r} names version {tagged}, but the line version is {version}")
    platforms = set()
    for plugin in native_plugins:
        if int(plugin.get("contract_version", -1)) != identity["contract_version"]:
            raise ValueError(f"{plugin.get('filename')}: contract {plugin.get('contract_version')!r}, "
                             f"expected {identity['contract_version']}")
        if str(plugin.get("version")) != version:
            raise ValueError(f"{plugin.get('filename')}: version {plugin.get('version')!r}, expected {version}")
        platforms.add((str(plugin.get("os")), str(plugin.get("arch"))))
    missing = sorted(REQUIRED_PLATFORMS - platforms)
    if missing:
        raise ValueError("Release lacks signed plugins for: " + ", ".join(f"{o}/{a}" for o, a in missing))
    return {
        "processing_core_line_manifest_schema_version": LINE_MANIFEST_SCHEMA_VERSION,
        "line": line,
        "channel": channel,
        "version": version,
        "published_at": core.validate_published_at(published_at),
        "contract_version": identity["contract_version"],
        "engine_abi_version": identity["engine_abi_version"],
        "release_tag": release_tag,
        "release_url": f"https://github.com/{repo}/releases/tag/{release_tag}",
        "manifest_url": core.join_public_object_url(
            public_base_url,
            f"{line_base_key(channel, line)}/versions/{core.version_object_component(version)}.json"),
        "native_plugins": list(native_plugins),
    }


def merge_line_index(existing: dict[str, Any], manifest: dict[str, Any],
                     public_base_url: str) -> dict[str, Any]:
    line, channel = manifest["line"], manifest["channel"]
    if existing:
        if existing.get("line") != line:
            raise ValueError(f"Existing index line is {existing.get('line')!r}, expected {line!r}")
        if existing.get("channel") != channel:
            raise ValueError(f"Existing index channel is {existing.get('channel')!r}, expected {channel!r}")
        if existing.get("processing_core_line_index_schema_version") != LINE_INDEX_SCHEMA_VERSION:
            raise ValueError("Unsupported processing-core line index schema")
    versions = [v for v in existing.get("versions", []) if v.get("version") != manifest["version"]]
    versions.append({key: manifest[key] for key in (
        "version", "contract_version", "engine_abi_version", "published_at", "release_tag",
        "release_url", "manifest_url", "native_plugins")})
    versions.sort(key=lambda value: core._version_sort_key(str(value["version"])), reverse=True)
    return {
        "processing_core_line_index_schema_version": LINE_INDEX_SCHEMA_VERSION,
        "line": line,
        "channel": channel,
        "active_version": manifest["version"],
        "updated_at": manifest["published_at"],
        "versions": versions,
    }
```

- [ ] **Step 4: Run the tests and confirm they pass.** Run `python tests/release/test_publish_processing_core_line.py -v`. Expected: 7 passed.

- [ ] **Step 5: Commit** `release(processing): native-only line manifest and index for absdiff-laplacian`.

### Task 2: Line publisher CLI (publish, idempotent re-run, promote)

**Files:**
- Modify: `scripts/release/publish-processing-core-line.py` (add `main`)
- Test: `tests/release/test_publish_processing_core_line.py` (add `LinePublishTest`)
- Modify: `tests/CMakeLists.txt` (ctest `scripts.publish_processing_core_line`)

**Interfaces:**
- CLI: `publish-processing-core-line.py --line <line> --channel <ch> (--from-release <tag> [--release-assets-dir DIR] | --promote-version <v>) [--published-at TS] [--dry-run] [--out-dir DIR] [--repo R] [--public-base-url U] [--bucket B] [--endpoint E] [--upload-method s3|wrangler|auto] [--profile P] [--acl A] [--wrangler-bin W] [--gh-bin G] [--debug]`.
- Publishing reuses `core.inspect_github_release`, `core.download_github_release`, `core.discover_native_descriptors`, `core.build_native_plugin_entries` (with `release_tag` and `expected_contract_version` per line), `core.read_existing_object`, `core.parse_existing_json`, `core.resolve_immutable_update`, `core.write_json`, `core.upload_object`, `core.require_consistent_mutating_transport`, `core.utc_now`, and `core.IMMUTABLE_CACHE_CONTROL` / `MUTABLE_CACHE_CONTROL`.
- The upload order is the immutable version document, then `index.json`, then `latest.json`.

- [ ] **Step 1: Write the failing tests** (append to the test file, before `if __name__`)

```python
def write_release(root: Path, version: str = "0.1.0") -> None:
    """Four signed-looking release assets: bytes plus sidecars the publisher accepts."""
    for os_name, ext, scheme in (("windows", "dll", "authenticode"), ("linux", "so", "ed25519")):
        stem = f"mib_processing_core-{LINE}-{version}-{os_name}_x86_64"
        (root / f"{stem}.{ext}").write_bytes(f"{os_name} plugin {version}".encode())
        signing = {"scheme": scheme, "required": True}
        if scheme == "ed25519":
            signing |= {"public_key_spki_base64": "AA==", "public_key_spki_sha256": "b" * 64,
                        "signature_base64": "AA=="}
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
```

- [ ] **Step 2: Run them and confirm they fail.** Expected: ERROR `AttributeError: module ... has no attribute 'main'`.

- [ ] **Step 3: Implement `main`** (append to the script)

```python
def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--line", required=True, choices=sorted(LINES))
    parser.add_argument("--channel", required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--from-release", help="mib-processing-<line>-v<version> GitHub Release to publish")
    action.add_argument("--promote-version", help="Existing immutable line version to activate")
    parser.add_argument("--release-assets-dir", help="Use these downloaded assets instead of gh (tests, previews)")
    parser.add_argument("--published-at")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out-dir", help="Also write the rendered documents here")
    parser.add_argument("--repo", default=core.DEFAULT_REPO)
    parser.add_argument("--public-base-url", default=core.DEFAULT_PUBLIC_BASE_URL)
    parser.add_argument("--bucket", default=core.DEFAULT_BUCKET)
    parser.add_argument("--endpoint")
    parser.add_argument("--upload-method", default="auto", choices=("auto", "s3", "wrangler"))
    parser.add_argument("--profile")
    parser.add_argument("--acl", default="")
    parser.add_argument("--wrangler-bin", default="wrangler")
    parser.add_argument("--gh-bin", default="gh")
    parser.add_argument("--debug", action="store_true")
    return parser


def _upload(args, key: str, path: Path, cache_control: str) -> None:
    if args.dry_run:
        print(f"DRY RUN: would upload {key}")
        return
    core.upload_object(args=args, key=key, file_path=path, content_type="application/json",
                       cache_control=cache_control)


def _write(out: Path, name: str, value: dict[str, Any], args) -> Path:
    path = out / name
    core.write_json(path, value)
    if args.out_dir:
        core.write_json(Path(args.out_dir) / name, value)
    return path


def _publish(args, base: str, out: Path) -> int:
    release_tag = args.from_release
    version = version_from_line_tag(args.line, release_tag)
    if args.release_assets_dir:
        asset_dir = Path(args.release_assets_dir)
        published_at = args.published_at
        if not published_at:
            raise ValueError("--release-assets-dir requires --published-at")
    else:
        metadata = core.inspect_github_release(args.repo, release_tag, args.gh_bin)
        asset_dir = out / "assets"
        core.download_github_release(args.repo, release_tag, asset_dir, args.gh_bin)
        published_at = args.published_at or metadata.get("publishedAt")
        if not published_at:
            raise ValueError(f"GitHub Release {release_tag} did not provide publishedAt")
    if any(asset_dir.glob("*.whl")):
        raise ValueError(f"{release_tag}: a native-only line release must not carry wheels")
    plugins = core.build_native_plugin_entries(
        core.discover_native_descriptors(asset_dir), asset_dir=asset_dir, repo=args.repo,
        release_tag=release_tag, expected_version=version,
        expected_contract_version=LINES[args.line]["contract_version"])
    manifest = build_line_manifest(line=args.line, channel=args.channel, version=version,
                                   release_tag=release_tag, repo=args.repo, native_plugins=plugins,
                                   published_at=published_at, public_base_url=args.public_base_url)
    version_key = f"{base}/versions/{core.version_object_component(version)}.json"
    existing_version, version_ok = core.read_existing_object(args, version_key)
    upload_version = core.resolve_immutable_update(existing_version, version_ok, manifest, version_key)
    index_bytes, index_ok = core.read_existing_object(args, f"{base}/index.json")
    if not index_ok:
        raise RuntimeError(f"Refusing to publish because {base}/index.json could not be read")
    index = merge_line_index(core.parse_existing_json(index_bytes, f"{base}/index.json"), manifest,
                             args.public_base_url)
    version_path = _write(out, "version.json", manifest, args)
    index_path = _write(out, "index.json", index, args)
    latest_path = _write(out, "latest.json", manifest, args)
    if upload_version:
        _upload(args, version_key, version_path, core.IMMUTABLE_CACHE_CONTROL)
    _upload(args, f"{base}/index.json", index_path, core.MUTABLE_CACHE_CONTROL)
    _upload(args, f"{base}/latest.json", latest_path, core.MUTABLE_CACHE_CONTROL)
    print(f"Published {args.line} {version} to {base}")
    return 0


def _promote(args, base: str, out: Path) -> int:
    version = core.validate_version(args.promote_version)
    if not args.published_at:
        raise ValueError("--published-at is required for promotion")
    version_key = f"{base}/versions/{core.version_object_component(version)}.json"
    version_bytes, ok = core.read_existing_object(args, version_key)
    if not ok or version_bytes is None:
        raise RuntimeError(f"No immutable {args.line} version {version} at {version_key}")
    manifest = core.parse_existing_json(version_bytes, version_key)
    if (manifest.get("line"), manifest.get("channel"), manifest.get("version")) != (args.line, args.channel, version):
        raise RuntimeError(f"{version_key} does not identify {args.line} {version} on {args.channel}")
    index_bytes, index_ok = core.read_existing_object(args, f"{base}/index.json")
    if not index_ok:
        raise RuntimeError(f"Refusing to promote because {base}/index.json could not be read")
    index = merge_line_index(core.parse_existing_json(index_bytes, f"{base}/index.json"), manifest,
                             args.public_base_url)
    index["updated_at"] = core.validate_published_at(args.published_at)
    index_path = _write(out, "index.json", index, args)
    latest_path = out / "latest.json"
    latest_path.write_bytes(version_bytes)  # promotion never re-renders the immutable document
    _upload(args, f"{base}/index.json", index_path, core.MUTABLE_CACHE_CONTROL)
    _upload(args, f"{base}/latest.json", latest_path, core.MUTABLE_CACHE_CONTROL)
    print(f"Activated {args.line} {version} on {args.channel}")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    try:
        check_channel(args.line, args.channel)
        core.require_consistent_mutating_transport(args)
        base = line_base_key(args.channel, args.line)
        with tempfile.TemporaryDirectory(prefix="processing_core_line_") as temp:
            out = Path(temp)
            return _promote(args, base, out) if args.promote_version else _publish(args, base, out)
    except (RuntimeError, ValueError, FileNotFoundError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
```

Register the ctest beside `scripts.publish_processing_core`:

```cmake
    add_test(
        NAME scripts.publish_processing_core_line
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/release/test_publish_processing_core_line.py
    )
    set_tests_properties(scripts.publish_processing_core_line PROPERTIES LABELS "scripts;release" TIMEOUT 30)
```

- [ ] **Step 4: Run the tests and confirm they pass.** Expected: 14 passed. `core.build_native_plugin_entries` may require signing fields beyond the fixture (for example Authenticode metadata). If so, extend `write_release` to the minimum the existing publisher tests use. Do not relax the publisher.

- [ ] **Step 5: Commit** `release(processing): native-only line publisher with idempotent publish and promote`.

### Task 3: `processing-core-line.yml`, the absdiff release workflow

**Files:**
- Create: `.github/workflows/processing-core-line.yml`
- Test: `tests/release/test_processing_core_line_workflow.py` (registered as ctest `scripts.processing_core_line_workflow`)

**Interfaces:**
- Consumes: Task 2 CLI and PR 1 artifacts (`mib_processing_core-absdiff-laplacian-*`, `scripts/run_native_core_conformance.py`, `scripts/check_core_line_sidecars.py` not needed).
- Jobs:
  - `validate`: tag version == version file; `channel=beta`.
  - `build-windows` and `build-linux`: absdiff target plus v2 tests, sidecar check, gold, export audit; unsigned artifact per OS.
  - `sign-windows` and `sign-linux`: `Production`, tag-only.
  - `release`: `Production`, tag-only. Validates exactly 4 assets, creates or verifies an immutable prerelease, publishes with the Task 2 CLI, and verifies the public `latest.json`.

- [ ] **Step 1: Write the failing structural test**

```python
# tests/release/test_processing_core_line_workflow.py
"""Structure of the absdiff-laplacian release workflow (what CI cannot run on a PR)."""
import unittest
from pathlib import Path

import yaml

WF = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "processing-core-line.yml"
TAG_REF = "refs/tags/mib-processing-absdiff-laplacian-v"


class LineWorkflow(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.wf = yaml.safe_load(WF.read_text(encoding="utf-8"))
        cls.jobs = cls.wf["jobs"]
        cls.text = WF.read_text(encoding="utf-8")

    def test_triggers_only_on_the_absdiff_tag_and_core_paths(self) -> None:
        on = self.wf[True] if True in self.wf else self.wf["on"]  # PyYAML parses `on:` as True
        self.assertEqual(on["push"]["tags"], ["mib-processing-absdiff-laplacian-v*"])
        self.assertIn("pull_request", on)

    def test_signing_and_release_are_tag_only_in_production(self) -> None:
        for job in ("sign-windows", "sign-linux", "release"):
            self.assertEqual(self.jobs[job].get("environment"), "Production", job)
            self.assertIn(TAG_REF, self.jobs[job]["if"], job)

    def test_version_gate_runs_before_signing(self) -> None:
        self.assertIn("processing-cores/absdiff-laplacian.version", self.text)
        for job in ("build-windows", "build-linux"):
            self.assertIn("validate", self.jobs[job]["needs"])

    def test_publishes_beta_through_the_line_publisher(self) -> None:
        self.assertIn("publish-processing-core-line.py", self.text)
        self.assertIn("--channel beta", self.text)
        self.assertNotIn("--channel stable", self.text)
        self.assertNotIn("publish-processing-core.py ", self.text)

    def test_only_absdiff_artifacts_are_signed(self) -> None:
        self.assertIn("mib_processing_core-absdiff-laplacian-*-windows_x86_64.dll", self.text)
        self.assertIn("mib_processing_core-absdiff-laplacian-*-linux_x86_64.so", self.text)
        self.assertNotIn("subtract-ring", self.text)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it and confirm it fails** (`FileNotFoundError`).

- [ ] **Step 3: Write the workflow.** Build it from `python-wheel.yml` with these exact transformations:
  - **`validate`** (ubuntu): read `processing-cores/absdiff-laplacian.version`. On a tag, require `github.ref_name == "mib-processing-absdiff-laplacian-v" + version`. Output `version`.
  - **`build-windows`:** copy the `build-native-plugin` steps up to and including "Build and verify independently configured ABI fixtures". In "Configure and build native plugin", build only `mib_processing_core_absdiff_laplacian processing_core_abi_v2_c_test processing_core_v2_plugin_test`. The compat flag uses `startsWith(github.ref, 'refs/tags/mib-processing-absdiff-laplacian-v')`. The verify ctest regex is `processing\.core_(abi_v2_c|v2_plugin)`. Then copy "Locate and validate absdiff-laplacian release assets", "Contract-2 gold against the built absdiff-laplacian DLL", the Windows export audit with only the absdiff entry in `$plugins`, and "Upload unsigned absdiff-laplacian plugin artifact" (artifact name `mib_processing-native-windows-x86_64-absdiff-laplacian-unsigned`).
  - **`build-linux`:** the same transformation of `build-native-plugin-linux`. The verify regex is `processing\.core_(abi_v2_c|v2_plugin)`. The audit `for pair in` list holds only the absdiff entry. Keep the Ed25519 rehearsal and publisher validation for the absdiff descriptor only, using `build_native_plugin_entries` directly as in PR 1.
  - **`sign-windows`:** copy `sign-native-plugin` with `needs: build-windows`, `if: startsWith(github.ref, 'refs/tags/mib-processing-absdiff-laplacian-v')`, the download name `…-absdiff-laplacian-unsigned`, the glob `mib_processing_core-absdiff-laplacian-*-windows_x86_64.dll`, and upload name `mib_processing-native-windows-x86_64-absdiff-laplacian`.
  - **`sign-linux`:** the same for `sign-native-plugin-linux`, with glob `native-dist/mib_processing_core-absdiff-laplacian-*-linux_x86_64.so`.
  - **`release`** (ubuntu, `environment: Production`, tag-only, `needs: [validate, sign-windows, sign-linux]`, `concurrency: processing-core-registry-beta-absdiff-laplacian`):
    1. Download both signed artifacts into `release-assets/`.
    2. Validate that the flat set is exactly the 4 names for `needs.validate.outputs.version`.
    3. `gh release create "$GITHUB_REF_NAME" release-assets/* --prerelease --latest=false --verify-tag`, or, if it already exists, `gh release view` and require the identical asset name set.
    4. `python scripts/release/publish-processing-core-line.py --line absdiff-laplacian --channel beta --from-release "$GITHUB_REF_NAME" --upload-method s3`, with the same R2 env and secrets as the existing release job.
    5. Fetch `https://updates.yofo.bio/beta/processing-core/absdiff-laplacian/latest.json?run=$GITHUB_RUN_ID` and require `version == needs.validate.outputs.version`, `contract_version == 2`, and 2 `native_plugins`.
  - **`pull_request` / `push` paths:** `processing-cores/**`, `scripts/release/publish-processing-core-line.py`, `scripts/run_native_core_conformance.py`, `src/backend/processing/**`, `include/backend/processing/**`, `.github/workflows/processing-core-line.yml`.

- [ ] **Step 4: Verify.**
  - Run the structural test. Expected: 5 passed.
  - Parse the YAML, then `bash -n` every bash step (feeding LF bytes) and parse every pwsh step with the PowerShell parser, as in PR 1.
  - After pushing: the PR runs `validate` (with no tag), `build-windows` and `build-linux`, which must pass. The sign and release jobs are skipped.

- [ ] **Step 5: Commit** `ci(processing): absdiff-laplacian release workflow (native-only, beta)`.

### Task 4: Promote and roll back per line

**Files:**
- Modify: `.github/workflows/processing-core-promote.yml`
- Test: `tests/release/test_processing_core_line_workflow.py` (add `PromoteWorkflow`)

- [ ] **Step 1: Write the failing test**

```python
PROMOTE = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "processing-core-promote.yml"


class PromoteWorkflow(unittest.TestCase):
    def test_line_input_routes_absdiff_to_the_line_publisher(self) -> None:
        wf = yaml.safe_load(PROMOTE.read_text(encoding="utf-8"))
        on = wf[True] if True in wf else wf["on"]
        line = on["workflow_dispatch"]["inputs"]["line"]
        self.assertEqual(line["default"], "subtract-ring")
        self.assertEqual(line["options"], ["subtract-ring", "absdiff-laplacian"])
        text = PROMOTE.read_text(encoding="utf-8")
        self.assertIn("publish-processing-core-line.py", text)
        self.assertIn("processing-core/absdiff-laplacian/latest.json", text)
        self.assertIn("inputs.line", wf["concurrency"]["group"])
```

- [ ] **Step 2: Run it and confirm it fails** (`KeyError: 'line'`).

- [ ] **Step 3: Implement.**
  - Add input `line` (choice: `subtract-ring` (default) or `absdiff-laplacian`).
  - Concurrency group: `processing-core-registry-${{ inputs.channel }}-${{ inputs.line }}`.
  - The "Promote immutable version" step branches on `LINE`: `subtract-ring` runs the existing command unchanged; `absdiff-laplacian` runs `python scripts/release/publish-processing-core-line.py --line absdiff-laplacian --channel "$CHANNEL" --promote-version "$VERSION" --published-at "$published_at" --upload-method s3`. Stable is refused by the script.
  - The verify step picks its URL by line: `…/processing-core/latest.json` for subtract-ring (unchanged), `…/processing-core/absdiff-laplacian/latest.json` for absdiff, which checks `version` only and skips `verify-processing-core-manifest.py`, since that script is wheel-schema specific.

- [ ] **Step 4: Run the tests and confirm they pass**, plus YAML and `bash -n` validation.

- [ ] **Step 5: Commit** `ci(processing): promote or roll back per core line`.

### Task 5: Allow absdiff tags in the bump script

**Files:** `scripts/bump_mib_processing_version.py` (`RELEASED_LINES`), `tests/release/test_bump_mib_processing_version.py`.

- [ ] **Step 1:** Change `test_absdiff_tag_is_refused_until_its_release_workflow_exists` into `test_absdiff_tag_after_committed_bump`. It expects `RuntimeError("not committed")` before the commit, then tag `mib-processing-absdiff-laplacian-v0.2.0` pointing at HEAD after it.
- [ ] **Step 2:** Run it. Expected: FAIL (`no release workflow`).
- [ ] **Step 3:** `RELEASED_LINES = {"subtract-ring", "absdiff-laplacian"}`, and update the comment to name both workflows.
- [ ] **Step 4:** Run it. Expected: all pass.
- [ ] **Step 5: Commit** `release(processing): absdiff-laplacian tags are releasable`.

### Task 6: Docs, runbooks and vault

**Files:**
- `docs/portable-processing-sync.md`: the Core lines table gains a "Registry" column; a new subsection "absdiff-laplacian registry line" covers the layout, the manifest fields and that it is beta only. Remove "PR 2" from future tense.
- `docs/howto/release-workflow.md` and `docs/howto/branching-and-releases.md`: add `processing-core-line.yml` to the CI trigger table and the release procedure (bump `--line absdiff-laplacian`, commit, `--create-tag`, push the tag; promote with `line=absdiff-laplacian`).
- `docs/exec-plans/active/2026-09-25-contract2-safe-rollout.md`: T1.1b status done when merged; next T1.1c.
- `knowledge_map/current-state/Recent-Work.md`, `knowledge_map/services/ProcessingService.md` and `knowledge_map/Vault-Maintenance.md` (add the line publisher and workflow to the core-lines row).

- [ ] **Step 1:** Edit. **Step 2:** `python scripts/check_docs.py` (expected OK) and `python tests/release/test_core_line_versions.py` (the runbook check, expected OK). **Step 3: Commit** `docs(processing): absdiff-laplacian release and registry line (T1.1b PR 2)`.

### Task 7: Verification and PR

- [ ] **Step 1:** Rig PC (PowerShell): full build; `ctest --preset windows-ninja-test`; `ctest --test-dir build-ninja -L "processing|release|scripts"`. Expected: green, except the known rig flakes (`scripts.exporter_soak`, `performance.monitoring_density_contention`), each named in the PR.
- [ ] **Step 2:** Dry run against the real built artifacts: stage the local absdiff DLL plus its sidecar, and a Linux placeholder pair, in a temp directory, then run the publisher with `--dry-run --out-dir`. Confirm the rendered documents (the locally built DLL is unsigned, so Authenticode fields come from the sidecar only).
- [ ] **Step 3:** Push and open the PR against `develop`, stacked on #476. The body states the first real run needs a tag push, by Gavin or on his say-so, after #473 and #476 merge.
