#!/usr/bin/env python3
"""Download the external datasets and model weights declared in env/assets.json.

Stdlib only. Files are fetched from the Hugging Face Hub at the pinned
revision, verified against the manifest SHA-256 when one is declared, and
written under ``<root>/<kind>s/<id>/`` (root: ``build/vendor/assets`` or
``MIB_ASSETS_DIR``). A ``provisioned.json`` beside the files records what was
downloaded, so ``--check`` can answer "is this tree complete?" without network.

    python3 scripts/provision-assets.py --required-only     # what the build needs
    python3 scripts/provision-assets.py --public-only        # everything that needs no token
    python3 scripts/provision-assets.py --asset yolo11n-seg --asset 512x96stream-mock-frames --count 200
    python3 scripts/provision-assets.py --check --required-only   # no network; exit 1 if incomplete
    python3 scripts/provision-assets.py --list

Private assets need ``HF_TOKEN`` (or a token saved by ``hf auth login``).
Exit codes: 0 ok, 1 missing or mismatched, 2 authentication needed, 3 network
failure, 4 bad arguments.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, Iterable, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from assets_manifest import (  # noqa: E402
    Asset,
    asset_dir,
    assets_root,
    get_asset,
    iter_assets,
    load_manifest,
)

EXIT_OK, EXIT_MISSING, EXIT_AUTH, EXIT_NETWORK, EXIT_USAGE = 0, 1, 2, 3, 4
USER_AGENT = "mib-studio-qt-provision-assets/1.0"


def find_token() -> Optional[str]:
    token = os.environ.get("HF_TOKEN") or os.environ.get("HUGGING_FACE_HUB_TOKEN")
    if token:
        return token.strip()
    for candidate in (
        Path(os.environ.get("HF_HOME", "~/.cache/huggingface")).expanduser() / "token",
        Path("~/.cache/huggingface/token").expanduser(),
    ):
        try:
            if candidate.is_file():
                return candidate.read_text(encoding="utf-8").strip() or None
        except OSError:
            continue
    return None


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class AuthRequired(RuntimeError):
    pass


class NetworkFailure(RuntimeError):
    pass


def download(url: str, dest: Path, token: Optional[str], retries: int = 5) -> str:
    """Stream ``url`` to ``dest`` (atomic rename); return the SHA-256."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    headers = {"User-Agent": USER_AGENT}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    last_error: Optional[Exception] = None
    for attempt in range(retries):
        try:
            request = urllib.request.Request(url, headers=headers)
            digest = hashlib.sha256()
            with urllib.request.urlopen(request, timeout=120) as response, tmp.open("wb") as out:
                for chunk in iter(lambda: response.read(1 << 20), b""):
                    out.write(chunk)
                    digest.update(chunk)
            tmp.replace(dest)
            return digest.hexdigest()
        except urllib.error.HTTPError as exc:
            if exc.code in (401, 403):
                raise AuthRequired(url) from exc
            if exc.code == 404:
                raise NetworkFailure(f"404 not found: {url}") from exc
            last_error = exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_error = exc
        time.sleep(min(10, 2 ** attempt))
    raise NetworkFailure(f"failed after {retries} attempts: {url}: {last_error}")


def select_assets(args: argparse.Namespace) -> List[Asset]:
    manifest = load_manifest()
    if args.asset:
        return [get_asset(asset_id, manifest) for asset_id in args.asset]
    chosen = [a for a in iter_assets(manifest) if a.materialised]
    if args.required_only:
        chosen = [a for a in chosen if a.required]
    elif args.public_only:
        chosen = [a for a in chosen if not a.token_required]
    elif not args.all:
        chosen = [a for a in chosen if a.required]  # default == --required-only
    return chosen


def provision_one(asset: Asset, root: Path, token: Optional[str], count: Optional[int], force: bool,
                  jobs: int = 8) -> Dict:
    target = asset_dir(asset, root)
    target.mkdir(parents=True, exist_ok=True)
    record = {"id": asset.id, "repo": asset.repo, "repo_type": asset.repo_type,
              "revision": asset.revision, "files": []}
    paths = list(asset.file_paths(count))
    print(f"[{asset.id}] {asset.repo}@{asset.revision[:12]} -> {target} ({len(paths)} file(s))")
    pending: List[str] = []
    for path in paths:
        dest = target / path
        expected = asset.expected_sha256(path)
        if dest.is_file() and dest.stat().st_size > 0 and not force:
            if expected is None or sha256_of(dest) == expected:
                record["files"].append({"path": path, "sha256": expected or sha256_of(dest),
                                        "bytes": dest.stat().st_size, "status": "cached"})
                continue
        pending.append(path)

    def fetch(path: str) -> Dict:
        dest = target / path
        expected = asset.expected_sha256(path)
        actual = download(asset.resolve_url(path), dest, token if asset.token_required or token else None)
        if expected and actual != expected:
            dest.unlink(missing_ok=True)
            raise SystemExit(
                f"[{asset.id}] SHA-256 mismatch for {path}\n  expected {expected}\n  actual   {actual}\n"
                f"  The Hub file at revision {asset.revision} does not match env/assets.json; "
                f"do not silently update the pin—investigate."
            )
        return {"path": path, "sha256": actual, "bytes": dest.stat().st_size, "status": "downloaded"}

    # Indexed datasets are thousands of small files: fetch them concurrently.
    workers = max(1, min(jobs, len(pending))) if pending else 1
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for result in pool.map(fetch, pending):
            record["files"].append(result)
            done += 1
            if len(pending) > 20 and done % 100 == 0:
                print(f"  {done}/{len(pending)} downloaded")
    record["files"].sort(key=lambda f: paths.index(f["path"]))
    (target / "provisioned.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"[{asset.id}] ok: {len(pending)} downloaded, {len(paths) - len(pending)} cached")
    return record


def check_one(asset: Asset, root: Path, count: Optional[int]) -> List[str]:
    problems: List[str] = []
    target = asset_dir(asset, root)
    for path in asset.file_paths(count):
        dest = target / path
        if not dest.is_file() or dest.stat().st_size == 0:
            problems.append(f"missing {dest}")
            continue
        expected = asset.expected_sha256(path)
        if expected and sha256_of(dest) != expected:
            problems.append(f"sha256 mismatch {dest}")
    return problems


def fix_command(asset: Asset) -> str:
    cmd = f"python3 scripts/provision-assets.py --asset {asset.id}"
    if asset.token_required:
        cmd = f"HF_TOKEN=<token with read access to {asset.hub_url}> " + cmd
    return cmd


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--all", action="store_true", help="every materialisable asset, private ones included")
    group.add_argument("--required-only", action="store_true", help="assets marked required (default)")
    group.add_argument("--public-only", action="store_true", help="every asset that needs no token")
    parser.add_argument("--asset", action="append", metavar="ID", help="explicit asset id (repeatable)")
    parser.add_argument("--count", type=int, help="for indexed assets: how many files to materialise")
    parser.add_argument("--root", type=Path, help="override the assets root (default: MIB_ASSETS_DIR or manifest root)")
    parser.add_argument("--check", action="store_true", help="verify only; no network")
    parser.add_argument("--list", action="store_true", help="print the manifest and exit")
    parser.add_argument("--force", action="store_true", help="re-download even if cached")
    parser.add_argument("--jobs", type=int, default=8, help="parallel downloads for multi-file assets (default 8)")
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.list:
        for asset in iter_assets():
            flag = "required" if asset.required else "optional"
            tok = ", token" if asset.token_required else ""
            mat = "" if asset.materialised else ", not materialised"
            print(f"{asset.id:28} {asset.kind:8} {asset.repo}@{asset.revision[:12]}  ({asset.visibility}, {flag}{tok}{mat})")
        return EXIT_OK

    try:
        assets = select_assets(args)
    except KeyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    if not assets:
        print("nothing selected")
        return EXIT_OK

    root = (args.root.resolve() if args.root else assets_root())

    if args.check:
        problems: List[str] = []
        for asset in assets:
            if not asset.materialised:
                continue
            found = check_one(asset, root, args.count)
            if found:
                problems.extend(found[:5] + ([f"... {len(found) - 5} more"] if len(found) > 5 else []))
                problems.append(f"  fix: {fix_command(asset)}")
            else:
                print(f"[{asset.id}] OK")
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return EXIT_MISSING
        return EXIT_OK

    token = find_token()
    for asset in assets:
        if not asset.materialised:
            print(f"[{asset.id}] nothing to materialise (fetched at use time); skipping")
            continue
        if asset.token_required and not token:
            print(f"[{asset.id}] needs a token: set HF_TOKEN with read access to {asset.hub_url}", file=sys.stderr)
            return EXIT_AUTH
        try:
            provision_one(asset, root, token, args.count, args.force, args.jobs)
        except AuthRequired as exc:
            print(f"[{asset.id}] authentication rejected for {exc}; set HF_TOKEN with read access to {asset.hub_url}",
                  file=sys.stderr)
            return EXIT_AUTH
        except NetworkFailure as exc:
            print(f"[{asset.id}] network failure: {exc}", file=sys.stderr)
            return EXIT_NETWORK
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
