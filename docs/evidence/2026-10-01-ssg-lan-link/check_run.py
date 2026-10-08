"""Hand-off 6a.4 check of one recorded sorting run (read-only).

Usage: python check_run.py <experiment.h5>

Prints the rf.generator readiness gate stored at Start, the rf_generator_*
provenance attributes, the /trigger_events ordering invariants with the
fireUs - grabUs distribution, and the series_meta summary when multi-image
was on. Exit code 1 when a check fails.
"""
import json
import sys

import h5py
import numpy as np

failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)


def text(value):
    return value.decode("utf-8", "replace") if isinstance(value, bytes) else value


def main(path):
    with h5py.File(path, "r") as f:
        print(f"file: {path}")

        print("\n[readiness gate at Start]")
        gate = None
        if "run_provenance" in f and "readiness_json" in f["run_provenance"].attrs:
            readiness = json.loads(text(f["run_provenance"].attrs["readiness_json"]))
            for g in readiness.get("gates", []):
                if g.get("id") == "rf.generator":
                    gate = g
        print("  " + (json.dumps(gate, ensure_ascii=False) if gate else "(no rf.generator gate stored)"))
        check(gate is not None and str(gate.get("status", "")).lower() == "pass", "rf.generator gate is pass")

        print("\n[rf_generator_* attributes]")
        found = {}

        def visit(name, obj):
            for key, value in obj.attrs.items():
                if key.startswith("rf_generator_"):
                    found[key] = (name or "/", text(value))

        visit("", f)
        f.visititems(visit)
        for key in sorted(found):
            where, value = found[key]
            print(f"  {where}@{key} = {value!r}")
        check("rf_generator_schema_version" in found, "rf_generator_schema_version present")
        check(bool(found.get("rf_generator_identity", ("", ""))[1]), "rf_generator_identity not empty")

        print("\n[/trigger_events]")
        if "trigger_events" not in f:
            check(False, "/trigger_events dataset present")
        else:
            ev = f["trigger_events"][...]
            print(f"  rows: {len(ev)}  fields: {', '.join(ev.dtype.names)}")
            check(len(ev) > 0, "at least one sort request recorded")
            if len(ev):
                outcomes = dict(zip(*np.unique(ev["outcome"], return_counts=True)))
                print(f"  outcome counts: { {int(k): int(v) for k, v in outcomes.items()} }")
                fired = ev[ev["outcome"] == 0]
                check(len(fired) == len(ev), "every row has outcome 0 (Fired)")
                ordered = (
                    (fired["requestUs"] <= fired["wakeUs"])
                    & (fired["wakeUs"] <= fired["fireUs"])
                    & (fired["fireUs"] <= fired["pulseDoneUs"])
                )
                check(bool(ordered.all()), "requestUs <= wakeUs <= fireUs <= pulseDoneUs on every fired row")
                check(bool((np.diff(ev["sequence"].astype(np.int64)) == 1).all()), "sequence is consecutive")
                check(
                    bool((ev["lineEdgeTimestamp"] == 0).all() and (ev["lineEdgeHostUs"] == 0).all()),
                    "lineEdge* columns are 0 (no loopback yet)",
                )
                if len(fired):
                    lag = fired["fireUs"].astype(np.int64) - fired["grabUs"].astype(np.int64)
                    width = fired["pulseDoneUs"].astype(np.int64) - fired["fireUs"].astype(np.int64)
                    print(
                        f"  fireUs - grabUs [us]: median {np.median(lag):.0f}  max {lag.max()}  "
                        f"min {lag.min()}  p95 {np.percentile(lag, 95):.0f}"
                    )
                    print(f"  pulseDoneUs - fireUs [us]: median {np.median(width):.0f}  max {width.max()}")

        print("\n[/valid_frames/series_meta]")
        vf = f.get("valid_frames")
        if vf is None or "series_meta" not in vf:
            print("  not present (multi-image off)")
        else:
            meta = vf["series_meta"]
            contiguous = vf["series_contiguous"][...]
            print(f"  series_meta shape {meta.shape}  fields: {', '.join(meta.dtype.names)}")
            print(f"  series_contiguous: {int(contiguous.sum())} of {len(contiguous)} are 1")
            check(meta.shape[0] > 0, "series_meta has rows")
            check(bool((contiguous == 1).all()), "every series is contiguous")

    print("\nRESULT: " + ("PASS" if not failures else f"FAIL ({len(failures)}): " + "; ".join(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
