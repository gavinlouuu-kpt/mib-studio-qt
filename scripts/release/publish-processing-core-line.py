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
