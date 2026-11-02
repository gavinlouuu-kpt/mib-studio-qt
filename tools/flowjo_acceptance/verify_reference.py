#!/usr/bin/env python3
"""Read-only verification of the bounded, local FlowJo 10.8.1 reference.

No vendor data is distributed. This is a reference-evidence check, not an FCS
importer or an implementation of MIB analysis/statistics.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct


FCS_SHA = "6687290a19cfcd2a86e8fe5af7c2077c4db10302af45b71eb6b95f6272706ada"
HISTOGRAMS = (
    ("FSC-A", 0, "fsc-a-histogram.tsv",
     "8497ed8a7a1804eaafafe92f6fe17c932f12f74f94304f9151f9e3418b0c2038"),
    ("SSC-A", 2, "ssc-a-histogram.tsv",
     "dd219c97391968f8565a698cec3a1a1cbcaddace0cf3a6701f56dedf0378ac3d"),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pinned_bytes(path, sha, maximum):
    # Bound even an accidentally substituted input before loading it.
    with path.open("rb") as handle:
        data = handle.read(maximum + 1)
    require(len(data) <= maximum, f"oversized reference: {path.name}")
    require(hashlib.sha256(data).hexdigest() == sha,
            f"reference hash mismatch: {path.name}")
    return data


def verify(root):
    data = pinned_bytes(root / "private/fixtures/fj-vendor-control.fcs",
                        FCS_SHA, 13_000_000)
    require(data[:6] == b"FCS3.0", "unexpected reference version")
    start, end = (int(data[i:i + 8]) for i in (26, 34))
    require(0 <= start <= end < len(data), "invalid DATA bounds")
    require(end - start + 1 == 229585 * 14 * 4, "unexpected DATA length")
    counts = [Counter(), Counter()]
    selected = 0
    boundaries = 0
    for event in struct.iter_unpack(">14f", memoryview(data)[start:end + 1]):
        x, y = event[0], event[2]
        boundaries += x in (50000, 150000) or y in (10000, 50000)
        if 50000 <= x <= 150000 and 10000 <= y <= 50000:
            selected += 1
            for index, value in enumerate((x, y)):
                counts[index][math.floor(value / 64 + 0.5) * 64] += 1
    require(selected == 190705, "rectangle count mismatch")
    require(boundaries == 0, "reference boundary inventory changed")
    channels = {}
    for index, (name, _, filename, sha) in enumerate(HISTOGRAMS):
        text = pinned_bytes(root / "evidence" / filename, sha, 1_000_000)
        rows = [line.split("\t") for line in text.decode("utf-8-sig").splitlines()
                if line.strip()]
        require(len(rows) == 4096 and all(len(row) == 2 for row in rows),
                f"invalid histogram shape: {name}")
        actual = {}
        for i, (coordinate, count) in enumerate(rows):
            x, n = float(coordinate), int(count)
            require(x == i * 64 and n >= 0, f"invalid histogram bin: {name}")
            actual[int(x)] = n
        require(sum(actual.values()) == selected, f"histogram count mismatch: {name}")
        coordinates = actual.keys() | counts[index].keys()
        mismatch = sum(actual.get(x, 0) != counts[index].get(x, 0) for x in coordinates)
        require(mismatch == 0, f"histogram vector mismatch: {name}")
        channels[name] = {"bins": len(rows), "count": selected, "mismatches": mismatch}
    return {"schema": "mib-flowjo-reference-check/v1", "status": "passed",
            "source_sha256": FCS_SHA, "events": 229585, "selected": selected,
            "boundary_events": boundaries, "channels": channels,
            "evidence_kind": "recomputed historical v10.8.1 reference",
            "limits": ["not a fresh FlowJo run", "not FlowJo 11 parity",
                       "not MIB module acceptance", "does not prove gate-edge semantics",
                       "does not establish saved statistic estimators or workspace reopen"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_root", type=Path,
                        help="local flowjo-windows-10.8.1 directory")
    args = parser.parse_args()
    try:
        result = verify(args.reference_root)
    except (OSError, ValueError, UnicodeError, struct.error) as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
