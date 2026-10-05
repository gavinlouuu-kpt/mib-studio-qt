#!/usr/bin/env python3
"""Vendor (or verify) the PZ7035 PS-PL ABI bundle used by the PZ record decoder.

usage: vendor_pz7035_abi.py --from PZ7035_REPO [--tag TAG] [--commit SHA]   copy and pin
       vendor_pz7035_abi.py --check                              verify only

The bundle is pz7035-imx426 abi/: the schema pz_mib_abi.json, the generated
C header (every struct offset _Static_assert-ed), the decoder fixtures
(*.bin + fixtures.json) and BUNDLE.sha256. The science profile
abi/profiles/unet_cells_v2.json is copied too. Files land in
third_party/pz7035-abi/ unchanged; PROVENANCE.json pins the source commit and
the sha256 of every vendored file. --check fails if a vendored file no longer
matches BUNDLE.sha256 (bundle files) or PROVENANCE.json (the profile).

ADR 0011: vendor from a tagged bundle on pz7035 main (`--tag abi-vX.Y.Z`).
The tag must resolve to the checkout's commit; PROVENANCE.json records both.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "third_party" / "pz7035-abi"
PROFILES = ["unet_cells_v2.json"]


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def bundle_hashes(bundle: Path) -> dict:
    out = {}
    for line in bundle.read_text().splitlines():
        digest, name = line.split(None, 1)
        out[name.strip()] = digest
    return out


def check() -> int:
    prov = json.loads((DEST / "PROVENANCE.json").read_text())
    bundle = bundle_hashes(DEST / "BUNDLE.sha256")
    bad = []
    for rel, digest in prov["files"].items():
        p = DEST / rel
        if not p.exists() or sha(p) != digest:
            bad.append(rel)
        key = {"pz_mib_abi.json": "../pz_mib_abi.json"}.get(rel, rel)
        if key in bundle and bundle[key] != digest:
            bad.append(f"{rel} (bundle)")
    if bad:
        print("pz7035 ABI vendor check FAILED: " + ", ".join(bad))
        return 1
    source = (prov["tag"] + " ") if prov.get("tag") else ""
    print(f"pz7035 ABI vendor check OK ({len(prov['files'])} files, source {source}{prov['commit'][:12]})")
    return 0


def vendor(src: Path, commit: str, tag: str | None) -> int:
    abi = src / "abi"
    gen = abi / "generated"
    bundle = bundle_hashes(gen / "BUNDLE.sha256")
    if DEST.exists():
        shutil.rmtree(DEST)
    (DEST / "fixtures").mkdir(parents=True)
    (DEST / "profiles").mkdir()
    files = {}
    copies = [(abi / "pz_mib_abi.json", "pz_mib_abi.json"), (gen / "pz_mib_abi.h", "pz_mib_abi.h"),
              (gen / "BUNDLE.sha256", "BUNDLE.sha256"), (gen / "fixtures" / "fixtures.json", "fixtures/fixtures.json")]
    copies += [(p, f"fixtures/{p.name}") for p in sorted((gen / "fixtures").glob("*.bin"))]
    copies += [(abi / "profiles" / n, f"profiles/{n}") for n in PROFILES]
    for s, rel in copies:
        shutil.copy2(s, DEST / rel)
        files[rel] = sha(DEST / rel)
        key = {"pz_mib_abi.json": "../pz_mib_abi.json"}.get(rel, rel)
        if key in bundle and bundle[key] != files[rel]:
            print(f"{rel}: source does not match its BUNDLE.sha256 entry")
            return 1
    prov = dict(repository="gavinlouuu-kpt/pz7035-imx426", commit=commit)
    if tag:
        prov["tag"] = tag
    prov.update(bundle_sha256=sha(DEST / "BUNDLE.sha256"), files=files)
    (DEST / "PROVENANCE.json").write_text(json.dumps(prov, indent=2) + "\n")
    print(f"vendored {len(files)} files from {tag + ' ' if tag else ''}{commit[:12]}")
    return check()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", type=Path)
    ap.add_argument("--commit")
    ap.add_argument("--tag", help="the pz7035 tag the checkout is at (must resolve to its commit)")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        return check()
    if not a.src:
        ap.error("--from or --check")
    commit = a.commit or subprocess.run(["git", "-C", str(a.src), "rev-parse", "HEAD"], check=True,
                                        capture_output=True, text=True).stdout.strip()
    if a.tag:
        tagged = subprocess.run(["git", "-C", str(a.src), "rev-parse", a.tag + "^{commit}"],
                                capture_output=True, text=True)
        if tagged.returncode != 0 or tagged.stdout.strip() != commit:
            print(f"tag {a.tag} does not resolve to {commit[:12]}")
            return 1
    return vendor(a.src, commit, a.tag)


if __name__ == "__main__":
    sys.exit(main())
