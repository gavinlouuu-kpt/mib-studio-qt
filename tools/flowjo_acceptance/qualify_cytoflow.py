#!/usr/bin/env python3
"""Bounded real-engine probe; not a production worker or general acceptance.

Use a pinned isolated environment. No vendor code/data or GUI is required.
Any unmet MIB requirement is emitted as a failed check, not silently adapted.
"""
import importlib.metadata
import json
import os
import platform

os.environ.setdefault("MPLBACKEND", "Agg")
os.environ.setdefault("ETS_TOOLKIT", "null")
os.environ.setdefault("NUMBA_NUM_THREADS", "1")


def qualify():
    import cytoflow as flow
    import pandas as pd

    version = importlib.metadata.version("cytoflow")
    if version != "1.3.2":
        raise ValueError(f"unqualified engine version: {version}")
    # Keys deliberately exceed JS precision and collide by row across sources.
    keys = [f"source-{i % 2}/valid/{9007199254740993 + i // 2}" for i in range(9)]
    points = [(0, 0), (2, 0), (2, 2), (0, 2), (1, 0), (1, 1),
              (3, 1), (float("nan"), 1), (1, float("nan"))]
    ex = flow.Experiment()
    ex.add_channel("Area")
    ex.add_channel("Deformability")
    ex.add_events(pd.DataFrame(points, columns=["Area", "Deformability"]), {})
    ex.add_condition("event_key", "object", pd.Series(keys))
    vertices = [(0., 0.), (2., 0.), (2., 2.), (0., 2.)]
    checks = []

    def check(name, actual, expected):
        checks.append({"name": name, "passed": actual == expected,
                       "actual": actual, "expected": expected})

    results = []
    for winding, polygon in (("ccw", vertices), ("cw", list(reversed(vertices)))):
        out = flow.PolygonOp(name="gate", xchannel="Area", ychannel="Deformability",
                             vertices=polygon, xscale="linear", yscale="linear").apply(ex)
        check(f"exact_keys_{winding}", out.data.event_key.tolist(), keys)
        check(f"edge_vertex_missing_{winding}", out.data.gate.tolist(),
              [True, True, True, True, True, True, False, False, False])
        selected = out.query("gate == True")
        check(f"query_keys_{winding}", selected.data.event_key.tolist(), keys[:6])
        results.append(out)
    check("input_unchanged", "gate" in ex.data.columns, False)
    reordered = ex.clone(deep=True)
    reordered.data = reordered.data.iloc[::-1].reset_index(drop=True)
    out = flow.PolygonOp(name="gate", xchannel="Area", ychannel="Deformability",
                         vertices=vertices).apply(reordered)
    check("reordered_key_membership", sorted(out.data.loc[out.data.gate, "event_key"]),
          sorted(keys[:6]))
    # Equality endpoints are explicitly required by #523, but RangeOp may refuse.
    try:
        equal = flow.RangeOp(name="equal", channel="Area", low=1., high=1.).apply(ex)
        equal_actual = equal.data.equal.tolist()
    except Exception as exc:
        equal_actual = {"refused": type(exc).__name__, "reason": str(exc)}
    check("equal_range_required_by_MIB", equal_actual,
          [False, False, False, False, True, True, False, False, True])
    # Do not expose NOT of boolean placeholders as qualified three-valued logic.
    check("missing_values_not_admitted_by_naive_negation",
          (~results[0].data.gate).tolist()[7:], [False, False])
    return {"schema": "mib-cytoflow-qualification/v1", "engine": version,
            "python": platform.python_version(),
            "packages": {d.metadata["Name"]: d.version
                         for d in importlib.metadata.distributions()},
            "checks": checks, "passed": all(c["passed"] for c in checks),
            "limits": "Synthetic real-engine probe only; no MIB adapter, live integration, "
                      "performance, advanced operations or distribution qualification."}


def main():
    try:
        result = qualify()
    except Exception as exc:
        result = {"schema": "mib-cytoflow-qualification/v1", "passed": False,
                  "runtime_error": f"{type(exc).__name__}: {exc}"}
    print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
