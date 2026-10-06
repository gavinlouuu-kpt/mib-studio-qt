#!/usr/bin/env python3
"""Select a small Contract 3 gold subset from the PZ7035 U-Net cell vectors.

usage: build_unet_cells_vector_subset.py FULL.json PROFILE.json OUT.json

FULL.json is the PL conformance file written by pz7035-imx426
scripts/gen_cells_vectors.py (plan T3.10, schema: cases[] with the profile page,
E-modulus table and frames[] of gray + mask + expected RESULT words). Its
expected values come from the bit-exact reference model that the PL cell stage
equals in simulation and on hardware.

PROFILE.json is pz7035-imx426 abi/profiles/unet_cells_v2.json; its page field
table and conformance tolerances are copied in, so the C++ test decodes the page
without the ABI sources. The subset otherwise keeps the schema.

Per case it greedily picks frames until every feature seen in that case is
covered: each reason, cut-off, target group,
E-modulus out of coverage, blemishes, the empty frame, and truncation. The
result is the gold reference of processing_contract3_cells_conformance_test;
the full file can be checked with MIB_UNET_CELLS_VECTORS=FULL.json.
"""
import argparse
import hashlib
import json
from pathlib import Path


def features(frame):
    f = set()
    if frame["empty"]:
        f.add("empty")
    if frame["truncated"]:
        f.add("truncated")
    if frame["blemishes"]:
        f.add("blemishes")
    for r in frame["results"]:
        f.add("reason:" + r["reason"])
        if r["cut_off"]:
            f.add("cut_off")
        if r["target"]:
            f.add("target")
        if r["payload"][0] >> 23 & 1:
            f.add("emod_out_of_coverage")
        if r["validity"] >> 10 & 1:
            f.add("emod")
    return f


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("full", type=Path)
    ap.add_argument("profile", type=Path)
    ap.add_argument("out", type=Path)
    a = ap.parse_args()
    raw = a.full.read_bytes()
    doc = json.loads(raw)
    for case in doc["cases"]:
        want = set().union(*(features(f) for f in case["frames"]))
        chosen, have = [], set()
        while have != want:
            best = max(case["frames"], key=lambda f: (len(features(f) - have), -len(f["results"])))
            chosen.append(best)
            have |= features(best)
        case["frames"] = [f for f in case["frames"] if any(f is c for c in chosen)]
        case["covered"] = sorted(want)
    prof = json.loads(a.profile.read_text())
    assert (prof["science_profile"], prof["profile_version"]) == (doc["science_profile"], doc["profile_version"])
    doc["page_fields"] = [dict(name=f["name"], word=f["word"], lo=f["bits"][0], hi=f["bits"][1], format=f["format"])
                          for f in prof["page"]]
    doc["conformance_tolerances"] = prof["conformance_tolerances"]
    doc["source"] = dict(file=a.full.name, sha256=hashlib.sha256(raw).hexdigest(),
                         generator="pz7035-imx426 scripts/gen_cells_vectors.py",
                         subset="scripts/build_unet_cells_vector_subset.py")
    a.out.write_text(json.dumps(doc, separators=(",", ":")) + "\n")
    print(f"{sum(len(c['frames']) for c in doc['cases'])} frames, {a.out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
