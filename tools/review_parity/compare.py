#!/usr/bin/env python3
"""Compare the review parity dumps of one file.

Input: ``<dir>/qt`` (tests/frontend/review_parity_qt_dump.cpp), ``<dir>/yofo``
(crates/mib-bridge/examples/review_parity.rs plus
desktop/scripts/review-parity-charts.test.ts) and, when present,
``<dir>/studio`` (crates/mib-bridge/examples/review_parity_studio.rs, #512 S6).
Writes ``<dir>/report.md`` with three tables (Qt ↔ YOFO, MIB Studio ↔ Qt,
MIB Studio ↔ YOFO) and prints it. Exit 1 when a check differs that is not an
accepted difference: the Qt ↔ YOFO decision log of
docs/exec-plans/.../2026-10-01-standalone-review-app.md, and for MIB Studio
decisions D2-D5 of docs/exec-plans/active/2026-10-18-one-review-implementation.md
(accepted until #512 S13).
"""
from __future__ import annotations

import hashlib
import json
import math
import re
import struct
import sys
import zlib
from pathlib import Path

REL = 1e-9

# Chart ranges involving MIB Studio. makeReviewChartData pads the scatter
# extents in long double (80-bit on Linux GCC, 64-bit on MSVC and arm64 macOS)
# and YOFO's chartMath.paddedExtent in double, so the same data can round to
# neighbouring doubles: a padding is three operations, at most a few ulp of
# the range's largest magnitude M apart. |a - b| <= CHART_RANGE_REL * M allows
# about 4500 ulp, far below any change of the data or of the 10 % padding.
CHART_RANGE_REL = 1e-12

# Qt's dump records only the histogram bar labels (one decimal), so the Qt
# ring-ratio range is compared at the labels' resolution.
LABEL_RESOLUTION = 0.05

# Chart TIFFs the facade's review_export_json renders into Export All
# (recording::renderReviewCharts).
STUDIO_CHART_FILES = ("scatter_plot.tiff", "histogram.tiff")

# MIB Studio's accepted differences: decisions D2-D5 of the #512 plan, until S13.
D2 = ("MIB Studio's render_review_overlay draws the ROI yellow 1 px unscaled, Qt and YOFO red 3 px scaled "
      "(decision D2, accepted until #512 S13)")
D3 = ("MIB Studio's facade uses the live px→µm, Qt and YOFO each file's recorded factor "
      "(decision D3, accepted until #512 S13)")
D4 = ("MIB Studio's facade bins over the live ring-ratio range, YOFO over the recorded one "
      "(decision D4, accepted until #512 S13)")
D5 = ("MIB Studio's review_export_json writes backend-rendered chart TIFFs into Export All; the module's "
      "shell snapshots are not exported by the Qt and YOFO halves (decision D5, accepted until #512 S13)")


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None


def close(a: float, b: float) -> bool:
    return math.isclose(a, b, rel_tol=REL, abs_tol=1e-12)


def range_close(a: tuple[float, float], b: tuple[float, float]) -> bool:
    """Two chart ranges agree within CHART_RANGE_REL of their largest magnitude."""
    m = max(abs(v) for v in (*a, *b))
    return all(abs(x - y) <= CHART_RANGE_REL * m for x, y in zip(a, b))


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


# --- images ------------------------------------------------------------------

def read_png(path: Path) -> tuple[int, int, bytes]:
    """8-bit gray / RGB / RGBA non-interlaced PNG → (width, height, RGB bytes)."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path.name}: not a PNG")
    pos, idat, header = 8, bytearray(), None
    while pos < len(data):
        (length,), kind = struct.unpack(">I", data[pos:pos + 4]), data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if header is None:
        raise ValueError(f"{path.name}: no IHDR")
    width, height, depth, color, _, _, interlace = header
    channels = {0: 1, 2: 3, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace:
        raise ValueError(f"{path.name}: unsupported PNG (depth {depth}, colour type {color}, interlace {interlace})")
    raw, row, bpp = zlib.decompress(bytes(idat)), width * channels, channels
    out, prev = bytearray(), bytearray(row)
    for y in range(height):
        kind, line = raw[y * (row + 1)], bytearray(raw[y * (row + 1) + 1:(y + 1) * (row + 1)])
        for i in range(row):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif kind != 0:
                raise ValueError(f"{path.name}: filter {kind}")
        out += line
        prev = line
    return width, height, to_rgb(bytes(out), channels)


def read_netpbm(path: Path) -> tuple[int, int, bytes]:
    """Binary PGM (P5) / PPM (P6), maxval 255 → (width, height, RGB bytes)."""
    data = path.read_bytes()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    magic, width, height = fields[0], int(fields[1]), int(fields[2])
    pixels = data[pos + 1:]
    return width, height, to_rgb(pixels, 1 if magic == b"P5" else 3)


def to_rgb(pixels: bytes, channels: int) -> bytes:
    if channels == 3:
        return pixels
    if channels == 1:
        return bytes(v for g in pixels for v in (g, g, g))
    return bytes(v for i in range(0, len(pixels), 4) for v in pixels[i:i + 3])


def changed_pixels(a: bytes, b: bytes, width: int) -> set[int]:
    """Pixel indices where two RGB buffers of the same size differ."""
    out, row = set(), width * 3
    for start in range(0, len(a), row):
        if a[start:start + row] == b[start:start + row]:
            continue
        for i in range(start, start + row, 3):
            if a[i:i + 3] != b[i:i + 3]:
                out.add(i // 3)
    return out


# The two ROI styles of decision D2 (colour in RGB, band in px): MIB Studio's
# facade (RoiStyle::Facade, cv::rectangle thickness 1) and Qt / ReviewSession
# (drawRoiOverlay, cv::rectangle thickness 3). The band is the width OpenCV
# rasterises (LINE_8): thickness 1 → 1 px, thickness 3 → 5 px with rounded
# corners.
STUDIO_ROI = ((255, 210, 0), 1)
REVIEW_ROI = ((255, 0, 0), 5)


def rectangle_outline(pixels: set[int], rgb: bytes, width: int, style: tuple[tuple[int, int, int], int]) -> bool:
    """`pixels` (those an ROI rectangle changed) all carry the style's colour
    and lie within its band of their bounding box's border."""
    if not pixels:
        return True
    colour, band = bytes(style[0]), style[1]
    xs, ys = [p % width for p in pixels], [p // width for p in pixels]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    return all(rgb[p * 3:p * 3 + 3] == colour and
               min(x - x0, x1 - x, y - y0, y1 - y) < band for p, x, y in zip(pixels, xs, ys))


def overlays(d: Path) -> tuple[dict[str, Path], dict[str, Path]]:
    studio = {p.stem: p for p in sorted((d / "studio/overlays").glob("*.png"))}
    yofo = {p.stem: p for p in sorted((d / "yofo/overlays").glob("*.p[gp]m"))}
    return studio, yofo


def compare_overlays(d: Path) -> tuple[tuple[bool, str], tuple[str, str]]:
    """(no-ROI check, detail), (ROI check result PASS/ACCEPTED/DIFF, detail)."""
    studio, yofo = overlays(d)
    missing = sorted(set(studio) ^ set(yofo))
    images = {}
    for name in sorted(set(studio) & set(yofo)):
        images[name] = (read_png(studio[name]), read_netpbm(yofo[name]))
    plain = [n for n in images if n.endswith("_roi0")]
    plain_diff = [n for n in plain if images[n][0] != images[n][1]]
    plain_ok = bool(plain) and not plain_diff and not missing
    plain_detail = f"{len(plain)} renders" + (f"; differ: {plain_diff[:5]}" if plain_diff else ", pixel-identical")
    if missing:
        plain_detail += f"; only in one half: {missing[:5]}"

    roi = [n for n in images if n.endswith("_roi1")]
    differing, outside, largest = [], [], 0
    for name in roi:
        (sw, sh, son), (yw, yh, yon) = images[name]
        off = images.get(name[:-1] + "0")
        if (sw, sh) != (yw, yh) or off is None:
            outside.append(name)
            continue
        diff = changed_pixels(son, yon, sw)
        if not diff:
            continue
        differing.append(name)
        largest = max(largest, len(diff))
        # Pixels either half's ROI rectangle drew (ROI on vs off, per half),
        # each in its own D2 style; anything else that differs is a finding.
        studio_roi, yofo_roi = changed_pixels(son, off[0][2], sw), changed_pixels(yon, off[1][2], yw)
        if not (diff <= studio_roi | yofo_roi and rectangle_outline(studio_roi, son, sw, STUDIO_ROI)
                and rectangle_outline(yofo_roi, yon, yw, REVIEW_ROI)):
            outside.append(name)
    if not roi or outside or missing:
        result = "DIFF"
        detail = f"{len(roi)} renders" + (f"; differ off the ROI outlines: {outside[:5]}" if outside else "")
    elif differing:
        result = "ACCEPTED"
        detail = (f"{len(differing)} of {len(roi)} renders differ, only on the ROI outlines "
                  f"(at most {largest} px per render)")
    else:
        result, detail = "PASS", f"{len(roi)} renders, pixel-identical (no recorded ROI drawn)"
    return (plain_ok, plain_detail), (result, detail)


# --- the halves ----------------------------------------------------------------

LABEL = re.compile(r"^(-?\d+(?:\.\d+)?)-(-?\d+(?:\.\d+)?)$")


def label_range(labels: list[str]) -> tuple[float, float] | None:
    first, last = (LABEL.match(labels[0]), LABEL.match(labels[-1])) if labels else (None, None)
    return (float(first.group(1)), float(last.group(2))) if first and last else None


def qt_half(d: Path) -> dict:
    s = load(d / "qt/summary.json") or {}
    sc, h = s.get("scatter", {}), s.get("histogram", {})
    return {"name": "qt", "recording": s.get("recording_file"), "factor": s.get("pixel_to_micron"),
            "count": sc.get("count"), "x": sc.get("x"), "y": sc.get("y"),
            "bins": h.get("counts"), "ring": label_range(h.get("labels") or []), "ring_resolution": LABEL_RESOLUTION,
            "y_max": (h.get("y") or {}).get("max"), "all": tree(d / "qt/all"), "csv": d / "qt/metrics.csv"}


def yofo_half(d: Path) -> dict:
    s, c = load(d / "yofo/summary.json") or {}, load(d / "yofo/charts.json") or {}
    sc, h = c.get("scatter", {}), c.get("histogram", {})
    ring = (s["ring_ratio_min"], s["ring_ratio_max"]) if "ring_ratio_min" in s else None
    return {"name": "yofo", "recording": s.get("recording_file"), "factor": s.get("pixel_to_micron"),
            "count": s.get("scatter_count"), "x": sc.get("x"), "y": sc.get("y"),
            "bins": h.get("counts"), "ring": ring, "ring_resolution": 0.0,
            "y_max": (h.get("y") or {}).get("max"), "all": tree(d / "yofo/all"), "csv": d / "yofo/metrics.csv"}


def studio_rows(d: Path, studio: dict, other: dict) -> list[tuple[str, str, str]]:
    """MIB Studio against one other half."""
    rows: list[tuple[str, str, str]] = []
    name = other["name"]

    def check(label: str, ok: bool, detail: str = "", accepted: str = "") -> None:
        rows.append((label, "PASS" if ok else ("ACCEPTED" if accepted else "DIFF"),
                     detail if ok or not accepted else f"{detail} — {accepted}"))

    recording = bool(studio.get("recording_file"))
    app, compared = studio.get("app", {}), studio.get("compared", {})
    charts_app, charts = load(d / "studio/charts_app.json") or {}, load(d / "studio/charts.json") or {}
    check("file kind", other["recording"] == recording, "recording" if recording else "experiment")

    # As opened: MIB Studio's live factor and range (D3 / D4). Accepted only
    # when the facade really used its live value.
    live = app.get("pixel_to_micron")
    used = charts_app.get("pixel_to_micron") if not recording else live
    as_opened = f"studio live {live} / {name} {other['factor']}"
    check("px→µm as opened", live is not None and other["factor"] is not None and close(live, other["factor"]),
          as_opened, accepted=D3 if live is not None and used is not None and close(used, live) else "")
    check("px→µm of the compared outputs",
          compared.get("pixel_to_micron") is not None and other["factor"] is not None
          and close(compared["pixel_to_micron"], other["factor"]),
          f"studio {compared.get('pixel_to_micron')} (from file: {compared.get('pixel_to_micron_from_file')}) / "
          f"{name} {other['factor']}")

    studio_all = tree(d / "studio/all")
    charts_files = {k: v for k, v in studio_all.items() if k in STUDIO_CHART_FILES}
    rest = {k: v for k, v in studio_all.items() if k not in STUDIO_CHART_FILES}
    diff = sorted(k for k in set(rest) | set(other["all"]) if rest.get(k) != other["all"].get(k))
    check("Export All files", bool(rest) and not diff,
          f"{len(rest)} studio / {len(other['all'])} {name} files, chart TIFFs aside"
          + (f"; differ: {diff[:5]}" if diff else ", byte-identical"))
    if recording:
        check("Export All chart files", not charts_files and not any(k in other["all"] for k in STUDIO_CHART_FILES),
              "none (recording file)")
    else:
        expected = set(charts_files) == set(STUDIO_CHART_FILES)
        check("Export All chart files", False,
              f"studio {sorted(charts_files)} / {name} none" + ("" if expected else "; expected both chart TIFFs"),
              accepted=D5 if expected and not any(k in other["all"] for k in STUDIO_CHART_FILES) else "")

    if recording:
        return rows

    a, b = d / "studio/metrics.csv", other["csv"]
    check("metrics.csv", a.is_file() and b.is_file() and a.read_bytes() == b.read_bytes(),
          f"{a.stat().st_size if a.is_file() else '-'} / {b.stat().st_size if b.is_file() else '-'} bytes")

    points = int(charts.get("finite_points", -1))
    check("scatter point count", points == other["count"], f"studio {points} / {name} {other['count']}")
    density = sum(int(cell[2]) for cell in charts.get("density", []))
    check("density cells hold every point", density == other["count"],
          f"studio {density} in {len(charts.get('density', []))} cells / {name} {other['count']} points")
    area, deform = charts.get("area_range"), charts.get("deform_range")
    ok = bool(area and deform and other["x"] and other["y"]) and \
        range_close(tuple(area), (other["x"]["min"], other["x"]["max"])) and \
        range_close(tuple(deform), (other["y"]["min"], other["y"]["max"]))
    check("scatter axis extents", ok,
          f"studio x {area} y {deform} / {name} x {other['x']} y {other['y']} (tolerance {CHART_RANGE_REL:g} of the largest end)")

    bins = [int(v) for v in charts.get("histogram", [])]
    check("histogram bins", bins == other["bins"], f"{len(bins)} bins, max {max(bins or [0])}")

    def same_range(r) -> bool:
        return bool(r) and other["ring"] is not None and all(
            abs(x - y) <= max(other["ring_resolution"], 1e-12 * max(1.0, abs(y))) for x, y in zip(r, other["ring"]))
    ring, ring_app = charts.get("ring_range"), charts_app.get("ring_range")
    live_ring = [app.get("ring_ratio_min"), app.get("ring_ratio_max")]
    check("histogram range as opened", same_range(ring_app), f"studio live {ring_app} / {name} {other['ring']}",
          accepted=D4 if ring_app and ring_app == live_ring else "")
    check("histogram range of the compared outputs", same_range(ring), f"studio {ring} / {name} {other['ring']}")
    # MIB Studio's histogram y axis tops at max(1, largest bin)
    # (desktop/src/components/ReviewCharts.tsx, maxBin); not in the JSON.
    studio_y = max([1, *bins])
    check("histogram y range", other["y_max"] is not None and close(studio_y, other["y_max"]),
          f"studio 0–{studio_y} (ReviewCharts.tsx maxBin) / {name} 0–{other['y_max']}")

    if name == "yofo":
        (plain_ok, plain_detail), (roi_result, roi_detail) = compare_overlays(d)
        check("overlays, ROI off", plain_ok, plain_detail)
        rows.append(("overlays, ROI on", roi_result, f"{roi_detail} — {D2}" if roi_result == "ACCEPTED" else roi_detail))
    return rows


def qt_yofo_rows(d: Path, qt: dict, yo: dict) -> list[tuple[str, str, str]]:
    """The Qt ↔ YOFO sign-off checks of 2026-10-04, unchanged."""
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
    return rows


def table(title: str, rows: list[tuple[str, str, str]]) -> list[str]:
    return [f"## {title}", "", "| Check | Result | Detail |", "|---|---|---|",
            *[f"| {n} | {r} | {t.replace('|', '/')} |" for n, r, t in rows], ""]


def compare(d: Path) -> tuple[str, list[tuple[str, str, str, str]]]:
    """Report text and every row as (pair, check, result, detail)."""
    qt, yo = load(d / "qt/summary.json"), load(d / "yofo/summary.json")
    if qt is None or yo is None:
        raise FileNotFoundError(f"missing qt or yofo dump under {d}")
    sections = [("Qt tab ↔ YOFO Review", qt_yofo_rows(d, qt, yo))]
    studio = load(d / "studio/summary.json")
    if studio is not None:
        sections += [("MIB Studio ↔ Qt tab", studio_rows(d, studio, qt_half(d))),
                     ("MIB Studio ↔ YOFO Review", studio_rows(d, studio, yofo_half(d)))]
    lines = [f"# Review parity: {Path(yo['source']).name}", "", f"Source: `{yo['source']}`", ""]
    if studio is None:
        lines += ["MIB Studio half: not run (no `studio/summary.json`).", ""]
    for title, rows in sections:
        lines += table(title, rows)
    return "\n".join(lines), [(title, *row) for title, rows in sections for row in rows]


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__)
        return 2
    d = Path(argv[0])
    try:
        report, rows = compare(d)
    except FileNotFoundError as e:
        print(e)
        return 2
    (d / "report.md").write_text(report, encoding="utf-8")
    # The report has non-ASCII text (↔, µ, —); a cp1252 console must not fail on it.
    encoding = sys.stdout.encoding or "utf-8"
    print(report.encode(encoding, "replace").decode(encoding))
    return 1 if any(r == "DIFF" for _, _, r, _ in rows) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
