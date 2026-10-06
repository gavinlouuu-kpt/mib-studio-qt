#!/usr/bin/env python3
"""Read-only access to ``env/assets.json``, the manifest of external datasets and
model weights the build, tests and scripts depend on.

Stdlib only, so it can be imported by test harnesses and CI steps without a
virtual environment. Nothing here touches the network; downloading is
``scripts/provision-assets.py``.

Typical use from another script under ``scripts/`` or ``tools/``::

    import sys, pathlib
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
    from assets_manifest import get_asset, asset_dir

    ds = get_asset("512x96stream-kin10")
    ds.repo, ds.revision, ds.viewer["rows"]
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterator, List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = REPO_ROOT / "env" / "assets.json"
HUB_BASE_URL = "https://huggingface.co"


class AssetError(KeyError):
    """Unknown asset id or malformed manifest entry."""


@dataclass(frozen=True)
class Asset:
    id: str
    kind: str
    repo: str
    repo_type: str
    revision: str
    visibility: str
    required: bool
    token_required: bool
    files: List[Dict[str, Any]] = field(default_factory=list)
    viewer: Optional[Dict[str, Any]] = None
    hf_datasets: Optional[Dict[str, Any]] = None
    consumers: List[str] = field(default_factory=list)
    notes: str = ""

    @property
    def hub_url(self) -> str:
        prefix = "datasets/" if self.repo_type == "dataset" else ""
        return f"{HUB_BASE_URL}/{prefix}{self.repo}"

    def resolve_url(self, path: str) -> str:
        """Raw-file URL for ``path`` at the pinned revision."""
        prefix = "datasets/" if self.repo_type == "dataset" else ""
        return f"{HUB_BASE_URL}/{prefix}{self.repo}/resolve/{self.revision}/{path}"

    @property
    def materialised(self) -> bool:
        """Whether provision-assets.py downloads files for this asset."""
        return bool(self.files)

    def file_paths(self, count: Optional[int] = None) -> Iterator[str]:
        """Expand the ``files`` entries into concrete Hub paths.

        Indexed patterns (``image.{index:04d}.tiff``) expand to ``count``
        entries (default ``default_count``, capped at ``index_count``).
        """
        for entry in self.files:
            if "path" in entry:
                yield entry["path"]
                continue
            pattern = entry["pattern"]
            start = int(entry.get("index_start", 0))
            total = int(entry["index_count"])
            wanted = int(entry.get("default_count", total)) if count is None else int(count)
            wanted = max(0, min(wanted, total))
            for index in range(start, start + wanted):
                yield pattern.format(index=index)

    def expected_sha256(self, path: str) -> Optional[str]:
        for entry in self.files:
            if entry.get("path") == path:
                return entry.get("sha256")
        return None


def load_manifest(path: Path = MANIFEST_PATH) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def assets_root(manifest: Optional[Dict[str, Any]] = None) -> Path:
    """Directory assets are provisioned into.

    ``MIB_ASSETS_DIR`` overrides the manifest ``root`` (relative paths resolve
    against the repository root), mirroring the CMake cache variable.
    """
    override = os.environ.get("MIB_ASSETS_DIR")
    if override:
        return Path(override).expanduser().resolve()
    manifest = manifest or load_manifest()
    root = Path(manifest.get("root", "build/vendor/assets"))
    return root if root.is_absolute() else (REPO_ROOT / root)


def iter_assets(manifest: Optional[Dict[str, Any]] = None) -> Iterator[Asset]:
    manifest = manifest or load_manifest()
    for raw in manifest.get("assets", []):
        try:
            yield Asset(
                id=raw["id"],
                kind=raw["kind"],
                repo=raw["repo"],
                repo_type=raw.get("repo_type", "dataset" if raw["kind"] == "dataset" else "model"),
                revision=raw["revision"],
                visibility=raw.get("visibility", "public"),
                required=bool(raw.get("required", False)),
                token_required=bool(raw.get("token_required", raw.get("visibility") == "private")),
                files=list(raw.get("files", [])),
                viewer=raw.get("viewer"),
                hf_datasets=raw.get("hf_datasets"),
                consumers=list(raw.get("consumers", [])),
                notes=raw.get("notes", ""),
            )
        except KeyError as exc:
            raise AssetError(f"env/assets.json entry {raw.get('id', '?')!r} is missing {exc}") from exc


def get_asset(asset_id: str, manifest: Optional[Dict[str, Any]] = None) -> Asset:
    for asset in iter_assets(manifest):
        if asset.id == asset_id:
            return asset
    known = ", ".join(a.id for a in iter_assets(manifest))
    raise AssetError(f"unknown asset id {asset_id!r}; known: {known}")


def asset_dir(asset: Asset, root: Optional[Path] = None) -> Path:
    """Where ``asset`` is (or will be) provisioned: ``<root>/<kind>s/<id>/``."""
    root = root or assets_root()
    return root / f"{asset.kind}s" / asset.id


def asset_file(asset: Asset, path: str, root: Optional[Path] = None) -> Path:
    return asset_dir(asset, root) / path


def hub_ids(manifest: Optional[Dict[str, Any]] = None) -> set:
    """All ``owner/name`` Hub ids declared in the manifest (for check_docs)."""
    return {asset.repo for asset in iter_assets(manifest)}


if __name__ == "__main__":  # pragma: no cover - tiny CLI for shell use
    import sys

    if len(sys.argv) == 3 and sys.argv[1] == "--dir":
        print(asset_dir(get_asset(sys.argv[2])))
    elif len(sys.argv) == 3 and sys.argv[1] == "--json":
        asset = get_asset(sys.argv[2])
        print(json.dumps(asset.__dict__, indent=2, sort_keys=True))
    else:
        for asset in iter_assets():
            print(f"{asset.id}\t{asset.kind}\t{asset.repo}@{asset.revision[:12]}\t{asset.visibility}")
