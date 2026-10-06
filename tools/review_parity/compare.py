#!/usr/bin/env python3
"""Compare the Qt and YOFO Review parity dumps of one file.

Input: ``<dir>/qt`` (tests/frontend/review_parity_qt_dump.cpp) and
``<dir>/yofo`` (crates/mib-bridge/examples/review_parity.rs plus
desktop/scripts/review-parity-charts.test.ts). Writes ``<dir>/report.md``
and prints it. Exit 1 when a check differs that is not an accepted difference
of the plan's decision log (docs/exec-plans/.../2026-10-01-standalone-review-app.md).
"""
from __future__ import annotations

import hashlib
import json
import math
import sys
from pathlib import Path

REL = 1e-9


def load(path: Path):
    return json.loads(path.read_text()) if path.is_file() else None


def close(a: float, b: float) -> bool:
    return math.isclose(a, b, rel_tol=REL, abs_tol=1e-12)


def tree(root: Path) -> dict[str, str]:
    """Relative path → SHA-256 of every file under the single export folder."""
    if not root.is_dir():
        return {}
    subdirs = [p for p in root.iterdir() if p.is_dir()]
    base = subdirs[0] if len(subdirs) == 1 else root
    return {str(p.relative_to(base)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(base.rglob("*")) if p.is_file()}


def same_json(a, b, path="") -> list[str]:
    """Differences between two JSON values (floats to REL)."""
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                out.append(f"{path}/{k}: only in {'qt' if k in a else 'yofo'}")
            else:
                out += same_json(a[k], b[k], f"{path}/{k}")
        return out
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return [f"{path}: length {len(a)} vs {len(b)}"]
        out = []
        for i, (x, y) in enumerate(zip(a, b)):
            out += same_json(x, y, f"{path}[{i}]")
            if len(out) > 5:
                return out[:5] + ["…"]
        return out
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool):
        return [] if close(float(a), float(b)) else [f"{path}: {a} vs {b}"]
    return [] if a == b else [f"{path}: {a!r} vs {b!r}"]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    d = Path(sys.argv[1])
    qt, yo = load(d / "qt/summary.json"), load(d / "yofo/summary.json")
    if qt is None or yo is None:
        print(f"missing dump under {d}")
        return 2
    charts = load(d / "yofo/charts.json") or {}
    rows: list[tuple[str, str, str]] = []  # (check, result, detail)

    def check(name: str, ok: bool, detail: str = "", accepted: str = "") -> None:
        rows.append((name, "PASS" if ok else ("ACCEPTED" if accepted else "DIFF"), detail if ok or not accepted else f"{detail} — {accepted}"))

    recording = bool(yo["recording_file"])
    check("file kind", qt["recording_file"] == recording, "recording" if recording else "experiment")
    check("px→µm", close(qt["pixel_to_micron"], yo["pixel_to_micron"]),
          f"qt {qt['pixel_to_micron']} / yofo {yo['pixel_to_micron']} (from file: {yo['pixel_to_micron_from_file']})")

    acc = yo.get("accounting_summary", "")
    status = qt.get("status_text", "")
    if recording or not acc:
        check("accounting text", acc in status, f"yofo {acc!r} in qt status {status!r}")
    else:
        check("accounting text", acc in status, f"yofo {acc!r}; qt status {status!r}",
              accepted="Qt replaces the accounting suffix with 'Valid/Invalid' for experiment files (decision log)")

    qt_all, yo_all = tree(d / "qt/all"), tree(d / "yofo/all")
    diff = sorted(k for k in set(qt_all) | set(yo_all) if qt_all.get(k) != yo_all.get(k))
    check("Export All files", bool(qt_all) and not diff,
          f"{len(qt_all)} qt / {len(yo_all)} yofo files" + (f"; differ: {diff[:5]}" if diff else ", byte-identical"))

    if not recording:
        a, b = d / "qt/metrics.csv", d / "yofo/metrics.csv"
        check("metrics.csv", a.is_file() and b.is_file() and a.read_bytes() == b.read_bytes(),
              f"{a.stat().st_size if a.is_file() else '-'} / {b.stat().st_size if b.is_file() else '-'} bytes")

        qs, ys = qt.get("scatter", {}), charts.get("scatter", {})
        check("scatter point count", qs.get("count") == ys.get("count") == yo.get("scatter_count"),
              f"qt {qs.get('count')} / yofo {ys.get('count')}")
        ext = same_json({"x": qs.get("x"), "y": qs.get("y")}, {"x": ys.get("x"), "y": ys.get("y")})
        check("scatter axis extents", not ext, "; ".join(ext) or f"x {qs.get('x')} y {qs.get('y')}")

        qh, yh = qt.get("histogram", {}), charts.get("histogram", {})
        check("histogram bins", qh.get("counts") == yh.get("counts"), f"{len(qh.get('counts') or [])} bins, max {max(qh.get('counts') or [0])}")
        check("histogram labels", qh.get("labels") == yh.get("labels"), f"{(qh.get('labels') or ['-'])[0]} … {(qh.get('labels') or ['-'])[-1]}")
        qy, yy = (qh.get("y") or {}).get("max"), (yh.get("y") or {}).get("max")
        check("histogram y range", qy is not None and yy is not None and close(qy, yy), f"qt 0–{qy} / yofo 0–{yy}",
              accepted="Qt rounds the y axis with applyNiceNumbers(); YOFO uses ceil(1.1 × max) (decision log)")

        qc, yc = load(d / "qt/core.json"), load(d / "yofo/core.json")
        if qc is None or yc is None:
            check("core record", False, f"qt {'present' if qc else 'missing'} / yofo {'present' if yc else 'missing'}")
        else:
            for c in (qc, yc):
                c.pop("computed_at_ns", None)
            diffs = same_json(qc, yc)
            check("core record", not diffs, "; ".join(diffs) or
                  f"{qc.get('cell_count')} of {qc.get('population_count')} cells, {len(qc.get('contours', []))} loop(s); computed_at_ns ignored")
            msg = yo.get("core_message", "")
            check("core status text", bool(msg) and msg in qt.get("core_status_text", ""),
                  f"yofo {msg!r} / qt {qt.get('core_status_text', '')!r}")

    lines = [f"# Review parity: {Path(yo['source']).name}", "",
             f"Source: `{yo['source']}`", "", "| Check | Result | Detail |", "|---|---|---|"]
    lines += [f"| {n} | {r} | {t.replace('|', '/')} |" for n, r, t in rows]
    report = "\n".join(lines) + "\n"
    (d / "report.md").write_text(report)
    print(report)
    return 1 if any(r == "DIFF" for _, r, _ in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
