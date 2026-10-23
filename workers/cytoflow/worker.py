#!/usr/bin/env python3
"""One bounded immutable polygon batch through the pinned real Cytoflow engine.

Internal worker protocol only, not a public Studio/Review ABI. No HDF5, images,
mechanics interpolation, arbitrary expressions, scripts, pickle or filesystem paths.
Native MIB composes masks and evaluates authoritative scalar values.
"""
import importlib.metadata
import json
import math
import os
import sys

os.environ["MPLBACKEND"] = "Agg"
os.environ["ETS_TOOLKIT"] = "null"
os.environ["NUMBA_NUM_THREADS"] = "1"
os.environ["OMP_NUM_THREADS"] = "1"
os.environ["OPENBLAS_NUM_THREADS"] = "1"

MAX_BYTES = 4 * 1024 * 1024
MAX_EVENTS = 10000
REVISION_FIELDS = {"snapshot", "sourceFingerprint", "plan", "application",
                   "overrideRevision", "parameters", "qc", "mechanics", "engine"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def exact_fields(value, names):
    require(isinstance(value, dict) and value.keys() == set(names), "unexpected object fields")


def text(value, limit=256):
    require(isinstance(value, str) and 0 < len(value) <= limit and
            not any(ord(c) < 32 for c in value), "invalid exact string")
    return value


def number(value):
    require(type(value) in (int, float), "measurement must be a number or null")
    require(math.isfinite(value) and abs(value) <= 1e100, "measurement outside bounded numeric domain")
    return float(value)


def polygon(vertices):
    require(isinstance(vertices, list) and 3 <= len(vertices) <= 129, "polygon vertex budget")
    points = []
    for pair in vertices:
        require(isinstance(pair, list) and len(pair) == 2, "invalid polygon vertex")
        points.append((number(pair[0]), number(pair[1])))
    if points[-1] == points[0]:
        points.pop()
    require(3 <= len(points) <= 128 and len(set(points)) == len(points), "duplicate polygon vertex")

    def cross(a, b, c):
        return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])

    def on(a, b, c):
        return cross(a, b, c) == 0 and min(a[0], b[0]) <= c[0] <= max(a[0], b[0]) and \
            min(a[1], b[1]) <= c[1] <= max(a[1], b[1])

    def intersects(a, b, c, d):
        x, y, z, w = cross(a, b, c), cross(a, b, d), cross(c, d, a), cross(c, d, b)
        return ((x > 0 > y or y > 0 > x) and (z > 0 > w or w > 0 > z)) or \
            on(a, b, c) or on(a, b, d) or on(c, d, a) or on(c, d, b)

    n = len(points)
    for i in range(n):
        # Refuse a backtracking adjacent edge, including collinear overlap.
        a, b, c = points[i-1], points[i], points[(i+1) % n]
        require(not (cross(a, b, c) == 0 and
                     ((a[0]-b[0])*(c[0]-b[0]) + (a[1]-b[1])*(c[1]-b[1]) > 0)),
                "overlapping polygon edges")
        for j in range(i+1, n):
            if j == i+1 or (i == 0 and j == n-1):
                continue
            require(not intersects(points[i], points[(i+1) % n], points[j], points[(j+1) % n]),
                    "self-intersecting polygon")
    area = math.fsum(points[i][0]*points[(i+1) % n][1] -
                     points[(i+1) % n][0]*points[i][1] for i in range(n))
    require(area != 0, "zero-area polygon")
    return points


def evaluate(request):
    exact_fields(request, ["schema", "job", "revision", "vertices", "events"])
    require(request["schema"] == "mib-cytoflow-polygon/1", "unsupported internal schema")
    job = text(request["job"])
    revision = request["revision"]
    exact_fields(revision, REVISION_FIELDS)
    for value in revision.values():
        text(value)
    require(revision["engine"] == "cytoflow-1.3.2", "engine revision mismatch")
    vertices = polygon(request["vertices"])
    events = request["events"]
    require(isinstance(events, list) and len(events) <= MAX_EVENTS, "event budget exceeded")
    seen, valid, unknown = set(), [], set()
    for event in events:
        exact_fields(event, ["key", "x", "y"])
        key = text(event["key"], 512)
        require(key not in seen, "duplicate event key")
        seen.add(key)
        x = None if event["x"] is None else number(event["x"])
        y = None if event["y"] is None else number(event["y"])
        if x is None or y is None:
            unknown.add(key)
        else:
            valid.append((key, x, y))

    require(importlib.metadata.version("cytoflow") == "1.3.2", "unqualified engine version")
    import cytoflow as flow
    import pandas as pd
    output = {key: "unknown" for key in unknown}
    if valid:
        ex = flow.Experiment()
        ex.add_channel("mib_x")
        ex.add_channel("mib_y")
        ex.add_events(pd.DataFrame([(x, y) for _, x, y in valid], columns=["mib_x", "mib_y"]), {})
        ex.add_condition("mib_key", "object", pd.Series([key for key, _, _ in valid]))
        result = flow.PolygonOp(name="mib_gate", xchannel="mib_x", ychannel="mib_y",
                                xscale="linear", yscale="linear", vertices=vertices).apply(ex)
        require(len(result.data) == len(valid), "engine lost rows")
        expected = {key for key, _, _ in valid}
        for key, selected in zip(result.data.mib_key, result.data.mib_gate):
            require(isinstance(key, str) and key in expected and key not in output,
                    "engine changed or duplicated keys")
            require(type(selected).__name__ in ("bool", "bool_"), "invalid engine membership")
            output[key] = "true" if selected else "false"
    require(output.keys() == seen, "incomplete worker coverage")
    return {"schema": request["schema"], "job": job, "revision": revision,
            "rows": [{"key": event["key"], "value": output[event["key"]]} for event in events]}


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON field")
        result[key] = value
    return result


def main():
    try:
        raw = sys.stdin.buffer.read(MAX_BYTES + 1)
        require(len(raw) <= MAX_BYTES, "request byte budget exceeded")
        def reject_constant(_):
            raise ValueError("nonfinite JSON literal")
        request = json.loads(raw, object_pairs_hook=no_duplicates, parse_constant=reject_constant)
        result = evaluate(request)
        sys.stdout.write(json.dumps(result, separators=(",", ":"), allow_nan=False) + "\n")
        return 0
    except Exception as exc:
        # No input values, source paths or stack traces in the protocol error.
        sys.stderr.write(f"analysis worker refused: {type(exc).__name__}: {str(exc)[:512]}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
