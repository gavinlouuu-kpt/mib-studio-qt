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
    "absdiff-laplacian": {"contract_version": 2, "engine_abi_version": 2,
                          "entrypoint": "mib_processing_get_api_v2"},
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
        # Defence in depth for manual publishes (CI already checks the sidecars):
        # an immutable registry entry must never name the wrong ABI or line.
        if int(plugin.get("engine_abi_version", -1)) != identity["engine_abi_version"]:
            raise ValueError(f"{plugin.get('filename')}: engine ABI {plugin.get('engine_abi_version')!r}, "
                             f"expected {identity['engine_abi_version']}")
        if plugin.get("entrypoint") != identity["entrypoint"]:
            raise ValueError(f"{plugin.get('filename')}: entrypoint {plugin.get('entrypoint')!r}, "
                             f"expected {identity['entrypoint']}")
        if not str(plugin.get("filename", "")).startswith(f"mib_processing_core-{line}-{version}-"):
            raise ValueError(f"{plugin.get('filename')}: filename is not a {line} {version} artifact")
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
    if manifest.get("processing_core_line_manifest_schema_version") != LINE_MANIFEST_SCHEMA_VERSION:
        raise RuntimeError(f"{version_key} has an unsupported line manifest schema")
    if manifest.get("contract_version") != LINES[args.line]["contract_version"]:
        raise RuntimeError(f"{version_key} is not a Contract-{LINES[args.line]['contract_version']} document")
    try:
        tagged = version_from_line_tag(args.line, str(manifest.get("release_tag", "")))
    except ValueError as exc:
        raise RuntimeError(f"{version_key} has an invalid release tag") from exc
    if tagged != version:
        raise RuntimeError(f"{version_key} release tag does not name {version}")
    index_bytes, index_ok = core.read_existing_object(args, f"{base}/index.json")
    if not index_ok or index_bytes is None:
        # A missing catalog would be rebuilt with only this version, dropping history.
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
