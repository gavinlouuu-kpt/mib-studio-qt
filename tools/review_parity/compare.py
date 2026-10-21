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
decisions D2-D5 and D10 of docs/exec-plans/active/2026-10-18-one-review-implementation.md
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

# MIB Studio's accepted differences: decisions D2-D5 and D10 of the #512 plan, until S13.
D2 = ("MIB Studio's render_review_overlay draws the ROI yellow at thickness 1 unscaled, Qt and YOFO red at "
      "thickness 3 scaled, which OpenCV paints as a 5 px band (decision D2, accepted until #512 S13)")
D3 = ("MIB Studio's facade uses the live px→µm, Qt and YOFO each file's recorded factor "
      "(decision D3, accepted until #512 S13)")
D4 = ("MIB Studio's facade bins over the live ring-ratio range, YOFO over the recorded one "
      "(decision D4, accepted until #512 S13)")
D5 = ("MIB Studio's review_export_json writes backend-rendered chart TIFFs into Export All; the module's "
      "shell snapshots are not exported by the Qt and YOFO halves (decision D5, accepted until #512 S13)")
D10 = ("MIB Studio's inline histogram y axis ends at the largest bin, Qt applies nice numbers and YOFO "
       "ceil(1.1 × max): display headroom only, the bins are identical (decision D10, coordinator ruling "
       "2026-10-20, accepted until #512 S13)")


def nice_number(x: float, ceiling: bool) -> float:
    """Qt's AbstractDomain::niceNumber."""
    z = 10 ** math.floor(math.log10(x))
    q = x / z
    if ceiling:
        q = 1 if q <= 1 else 2 if q <= 2 else 5 if q <= 5 else 10
    else:
        q = 1 if q < 1.5 else 2 if q < 3 else 5 if q < 7 else 10
    return q * z


def histogram_y_top(half: str, max_bin: int) -> float:
    """The histogram y-axis top each shell draws for its largest bin:
    MIB Studio max(1, max) (desktop/src/components/ReviewCharts.tsx maxBin);
    YOFO max(1, ceil(1.1 × max)) (Histogram.tsx); Qt the same, then
    QValueAxis::applyNiceNumbers() with the default 5 ticks
    (HdfReviewTab.cpp; Qt's AbstractDomain::looseNiceNumbers on [0, top])."""
    if half == "studio":
        return max(1, max_bin)
    top = max(1, math.ceil(max_bin * 1.1))
    if half == "yofo":
        return top
    step = nice_number(nice_number(top, True) / 4, False)
    return math.ceil(top / step) * step


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


# The two ROI styles of decision D2: colour (RGB) and cv::rectangle thickness.
# MIB Studio's facade (RoiStyle::Facade): thickness 1, unscaled, on Rect(roi) &
# the image. Qt and ReviewSession (drawRoiOverlay): thickness 3 on the ROI
# clamped to the image, which OpenCV (LINE_8) paints as a 5 px band with
# rounded outer corners (roi_band).
STUDIO_ROI = ((255, 210, 0), 1)
REVIEW_ROI = ((255, 0, 0), 3)


def studio_roi_rect(roi, width: int, height: int):
    """RoiStyle::Facade's rectangle, (x0, y0, x1, y1) inclusive: Rect(roi) & the
    image; None when there is none."""
    x, y, w, h = roi
    x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, width) - 1, min(y + h, height) - 1
    return (x0, y0, x1, y1) if w > 0 and h > 0 and x1 >= x0 and y1 >= y0 else None


def review_roi_rect(roi, width: int, height: int):
    """drawRoiOverlay's rectangle on an overlay of the frame's size (scale 1),
    (x0, y0, x1, y1) inclusive; None when there is none."""
    x, y, w, h = roi
    if w <= 0 or h <= 0:
        return None
    x, y = max(0, min(x, width - 1)), max(0, min(y, height - 1))
    w, h = max(1, min(w, width - x)), max(1, min(h, height - y))
    return (x, y, x + w - 1, y + h - 1)


# Half-width of the band cv::rectangle paints around a rectangle's 1 px path,
# per thickness (measured with cv2.rectangle, see roi_band).
ROI_BAND_HALF = {1: 0, 3: 2}


def roi_band(rect, width: int, height: int, thickness: int) -> set[int]:
    """Pixel indices cv::rectangle (LINE_8) paints for `rect` (x0, y0, x1, y1
    inclusive) at `thickness` on a width × height image: the pixels within
    ROI_BAND_HALF (Chebyshev) of the rectangle's 1 px path, less the outer
    corner pixels whose offset (dx, dy) past both edges has dx² + dy² > 2.
    The model matched cv2.rectangle (OpenCV 5.0) on every one of 6237
    thickness-3 rectangles (clamped, small and degenerate ones included) and
    on the thickness-1 ones; tests/tools/data/review_parity_roi_bands.json
    holds cv2 goldens."""
    half = ROI_BAND_HALF[thickness]
    x0, y0, x1, y1 = rect
    out = set()
    for y in range(max(0, y0 - half), min(height, y1 + half + 1)):
        for x in range(max(0, x0 - half), min(width, x1 + half + 1)):
            dx, dy = max(x0 - x, 0, x - x1), max(y0 - y, 0, y - y1)
            if dx and dy and dx * dx + dy * dy > 2:
                continue
            distance = max(dx, dy) if dx or dy else min(x - x0, x1 - x, y - y0, y1 - y)
            if distance <= half:
                out.add(y * width + x)
    return out


def roi_drawn(on: bytes, off: bytes, width: int, height: int, rect, style) -> bool:
    """One half drew exactly its style's rectangle at `rect` (the recorded ROI):
    every pixel of the band cv::rectangle paints in the style's colour, and no
    pixel outside that band changed. With no rectangle (no ROI recorded, or
    one outside the image) nothing may change."""
    changed = changed_pixels(on, off, width)
    if rect is None:
        return not changed
    colour, band = bytes(style[0]), roi_band(rect, width, height, style[1])
    return changed <= band and all(on[p * 3:p * 3 + 3] == colour for p in band)


def overlays(d: Path) -> tuple[dict[str, Path], dict[str, Path]]:
    studio = {p.stem: p for p in sorted((d / "studio/overlays").glob("*.png"))}
    yofo = {p.stem: p for p in sorted((d / "yofo/overlays").glob("*.p[gp]m"))}
    return studio, yofo


def compare_overlays(d: Path, roi) -> tuple[tuple[bool, str], tuple[str, str]]:
    """(no-ROI check, detail), (ROI check result PASS/ACCEPTED/DIFF, detail).
    `roi` is the recorded ROI [x, y, w, h] both halves report, or None."""
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

    renders = [n for n in images if n.endswith("_roi1")]
    differing, wrong, largest = [], [], 0
    for name in renders:
        (sw, sh, son), (yw, yh, yon) = images[name]
        off = images.get(name[:-1] + "0")
        if (sw, sh) != (yw, yh) or off is None or roi is None:
            wrong.append(name)
            continue
        # Each half drew its own style at the recorded ROI, and the halves
        # differ only on pixels those rectangles drew.
        drawn = (roi_drawn(son, off[0][2], sw, sh, studio_roi_rect(roi, sw, sh), STUDIO_ROI)
                 and roi_drawn(yon, off[1][2], yw, yh, review_roi_rect(roi, yw, yh), REVIEW_ROI))
        diff = changed_pixels(son, yon, sw)
        if not drawn or not diff <= changed_pixels(son, off[0][2], sw) | changed_pixels(yon, off[1][2], yw):
            wrong.append(name)
        elif diff:
            differing.append(name)
            largest = max(largest, len(diff))
    where = f"recorded ROI {roi}" if roi and roi[2] > 0 and roi[3] > 0 else "no recorded ROI"
    if not renders or wrong or missing:
        result = "DIFF"
        detail = f"{len(renders)} renders, {where}" + (
            f"; not each half's ROI style at the recorded ROI: {wrong[:5]}" if wrong else "")
    elif differing:
        result = "ACCEPTED"
        detail = (f"{len(differing)} of {len(renders)} renders differ, only where each half drew its ROI style "
                  f"at the {where} (at most {largest} px per render)")
    else:
        result, detail = "PASS", f"{len(renders)} renders, pixel-identical ({where})"
    return (plain_ok, plain_detail), (result, detail)


# --- chart references ------------------------------------------------------------

def ring_histogram(ratios, lo: float, hi: float) -> list[int]:
    """makeReviewChartData's ring-ratio bins: 0.5 wide over [lo, hi], positive
    finite ratios clamped into the edge bins (YOFO's ringRatioHistogram too)."""
    n = max(1, int((hi - lo) / 0.5))
    bins = [0] * n
    for r in ratios:
        if r is None or not math.isfinite(r) or r <= 0:
            continue
        bins[min(n - 1, int((min(max(r, lo), hi) - lo) / 0.5))] += 1
    return bins


# reviewChartsJson bins in long double; a point this close (relative) to an
# interior cell edge may fall on either side of that edge in double.
DENSITY_EDGE = 1e-9


def density_candidates(areas, deforms, area_range, deform_range, resolution: int):
    """reviewChartsJson's density placement of these points on the given grid:
    ({(x, y): count} of the points with one possible cell, [candidate cells of
    each other point]). A point within DENSITY_EDGE of an interior cell edge
    may fall in either cell beside that edge, per axis: 2 or 4 candidates."""
    exact: dict[tuple[int, int], int] = {}
    ambiguous: list[tuple[tuple[int, int], ...]] = []

    def cells(v: float, lo: float, hi: float) -> tuple[int, ...]:
        t = (v - lo) / (hi - lo) * resolution
        k = round(t)
        if 0 < k < resolution and abs(t - k) <= DENSITY_EDGE * max(1.0, abs(t)):
            return (k - 1, k)
        return (min(max(int(t), 0), resolution - 1),)

    for a, f in zip(areas, deforms):
        if a is None or f is None or not (math.isfinite(a) and math.isfinite(f)):
            continue
        options = tuple((x, y) for x in cells(a, *area_range) for y in cells(f, *deform_range))
        if len(options) == 1:
            exact[options[0]] = exact.get(options[0], 0) + 1
        else:
            ambiguous.append(options)
    return exact, ambiguous


def density_placed(cells: dict, exact: dict, ambiguous: list) -> tuple[bool, str]:
    """`cells` ({(x, y): count}) holds every exact point in its cell and each
    ambiguous point in one of its candidate cells, nothing else: the cells'
    surplus over the exact points must be filled exactly by the ambiguous
    points (a bipartite assignment by augmenting paths). Returns (ok, the reason
    when not)."""
    surplus = {k: cells.get(k, 0) - exact.get(k, 0) for k in set(cells) | set(exact)}
    short = sorted(k for k, v in surplus.items() if v < 0)
    if short:
        return False, f"cells short of the points that can only fall there: {short[:5]}"
    # Implied by the assignment below; checked first for its message.
    reachable = {c for options in ambiguous for c in options}
    stray = sorted(k for k, v in surplus.items() if v > 0 and k not in reachable)
    if stray:
        return False, f"points in cells no point can fall in: {stray[:5]}"
    if sum(surplus.values()) != len(ambiguous):
        return False, f"{sum(surplus.values())} points beyond the exact ones for {len(ambiguous)} edge points"
    assigned: dict[tuple[int, int], list[int]] = {k: [] for k, v in surplus.items() if v > 0}

    def place(point: int, seen: set) -> bool:
        for c in ambiguous[point]:
            if c in seen or c not in assigned:
                continue
            seen.add(c)
            if len(assigned[c]) < surplus[c]:
                assigned[c].append(point)
                return True
            for i, other in enumerate(assigned[c]):
                if place(other, seen):
                    assigned[c][i] = point
                    return True
        return False

    if not all(place(i, set()) for i in range(len(ambiguous))):
        return False, "no assignment of the edge points to their neighbouring cells fills the cells"
    return True, ""


def charts_scale_by_factor(scaled: dict, base: dict) -> bool:
    """`scaled` is `base` charted at another px→µm: area extents × (f / f_base)²,
    deformability extents and point count unchanged."""
    f, f_base = scaled.get("pixel_to_micron"), base.get("pixel_to_micron")
    if not (f and f_base and scaled.get("area_range") and base.get("area_range")
            and scaled.get("deform_range") and base.get("deform_range")):
        return False
    k = (f / f_base) ** 2
    return (range_close(tuple(scaled["area_range"]), tuple(v * k for v in base["area_range"]))
            and range_close(tuple(scaled["deform_range"]), tuple(base["deform_range"]))
            and scaled.get("finite_points") == base.get("finite_points"))


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
            "count": s.get("scatter_count"), "x": sc.get("x"), "y": sc.get("y"), "roi": s.get("roi"),
            "bins": h.get("counts"), "ring": ring, "ring_resolution": 0.0,
            "y_max": (h.get("y") or {}).get("max"), "all": tree(d / "yofo/all"), "csv": d / "yofo/metrics.csv"}


def missing_artefacts(d: Path, studio: dict, name: str) -> list[str]:
    """Every artefact the MIB Studio rows against half `name` read that is
    absent: review_parity_studio's summary keys and files, and the other
    half's (with YOFO's scatter dump, the reference points of both pairs)."""
    recording = bool(studio.get("recording_file"))
    keys = ["recording_file", "roi", "app", "compared", "all_export", "failures"]
    if not recording:
        keys += ["d4_probe", "metrics_export", "overlays"]
    missing = [f"studio/summary.json {k}" for k in keys if k not in studio]
    files = ["studio/charts_app.json", "studio/charts.json"]
    if not recording:
        files += ["studio/charts_d4.json", "studio/metrics.csv", f"{name}/metrics.csv", "yofo/scatter.json"]
        files += ["yofo/charts.json"] if name == "yofo" else []
    missing += [f for f in files if not (d / f).is_file()]
    missing += [f"{half}/all/ (empty)" for half in ("studio", name) if not tree(d / half / "all")]
    if not recording:
        other = load(d / f"{name}/summary.json") or {}
        if name == "qt":
            missing += [f"qt/summary.json {k}" for k in ("scatter", "histogram") if k not in other]
        else:
            missing += ["yofo/summary.json roi"] if "roi" not in other else []
            names = sorted(set(studio.get("overlays") or []) | set(other.get("overlays") or []))
            if not names:
                missing.append("overlays (none listed)")
            missing += [f"studio/overlays/{n}.png" for n in names if not (d / f"studio/overlays/{n}.png").is_file()]
            missing += [f"yofo/overlays/{n}.p?m" for n in names
                        if not any((d / f"yofo/overlays/{n}.{x}").is_file() for x in ("pgm", "ppm"))]
    return missing


def studio_rows(d: Path, studio: dict, other: dict) -> list[tuple[str, str, str]]:
    """MIB Studio against one other half. The reference points (ring ratios,
    areas, deformabilities) are YOFO's scatter dump for both pairs; the Qt ↔
    YOFO table holds them equal to Qt's."""
    rows: list[tuple[str, str, str]] = []
    name = other["name"]

    def check(label: str, ok: bool, detail: str = "", accepted: str = "") -> None:
        rows.append((label, "PASS" if ok else ("ACCEPTED" if accepted else "DIFF"),
                     detail if ok or not accepted else f"{detail} — {accepted}"))

    recording = bool(studio.get("recording_file"))
    app, compared = studio.get("app", {}), studio.get("compared", {})
    charts_app, charts = load(d / "studio/charts_app.json") or {}, load(d / "studio/charts.json") or {}
    scatter = load(d / "yofo/scatter.json") or {}
    check("file kind", other["recording"] == recording, "recording" if recording else "experiment")
    missing = missing_artefacts(d, studio, name)
    check("expected artefacts", not missing,
          f"missing: {', '.join(missing[:8])}" + (f" and {len(missing) - 8} more" if len(missing) > 8 else "")
          if missing else "every dump file and summary key the checks read is present")
    failures = studio.get("failures")
    exports = {k: (studio.get(k) or {}).get("state") for k in ("all_export", "metrics_export")
               if k == "all_export" or not recording}
    check("studio run", failures == [] and all(v == "completed" for v in exports.values()),
          f"failures {failures if failures is not None else 'not recorded'}; exports "
          + ", ".join(f"{k} {v}" for k, v in exports.items()))

    # As opened: MIB Studio's live factor (D3). Accepted only when the live
    # factor is the one the charts declare and the one they were computed
    # with: area extents × (live / compared)² against the compared pass.
    live = app.get("pixel_to_micron")
    same_factor = live is not None and other["factor"] is not None and close(live, other["factor"])
    if recording:
        # A recording has no charts and its Export All holds images only, so no
        # as-opened output depends on the factor.
        check("px→µm as opened", same_factor,
              f"studio live {live} / {name} {other['factor']} (no recording output depends on it)",
              accepted=D3 if live is not None else "")
    else:
        declared = charts_app.get("pixel_to_micron") is not None and live is not None \
            and close(charts_app["pixel_to_micron"], live)
        applied = charts_scale_by_factor(charts_app, charts)
        k = (live / compared["pixel_to_micron"]) ** 2 if live and compared.get("pixel_to_micron") else None
        detail = (f"studio live {live} / {name} {other['factor']}; as-opened charts declare "
                  f"{charts_app.get('pixel_to_micron')}, area extents {charts_app.get('area_range')} = compared × "
                  f"{k:.6g}: {'yes' if applied else 'NO'}" if k else f"studio live {live} / {name} {other['factor']}")
        check("px→µm as opened", same_factor and declared and applied, detail,
              accepted=D3 if declared and applied else "")
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
    studio_cells = {(c[0], c[1]): int(c[2]) for c in charts.get("density", [])}
    if name == "yofo":
        # Placement: YOFO's points binned on MIB Studio's grid (its extents
        # and resolution), cell by cell. Only a point on a cell edge may sit
        # in the neighbouring cell across that edge.
        resolution = charts.get("resolution", 128)
        area_r, deform_r = charts.get("area_range"), charts.get("deform_range")
        exact, ambiguous, placed, why = {}, [], False, "no studio grid or YOFO scatter dump"
        if area_r and deform_r and scatter and area_r[1] > area_r[0] and deform_r[1] > deform_r[0]:
            exact, ambiguous = density_candidates(scatter.get("area_um2", []), scatter.get("deformability", []),
                                                  area_r, deform_r, resolution)
            placed, why = density_placed(studio_cells, exact, ambiguous) if exact or ambiguous \
                else (False, "no finite reference points")
        check("density cells", placed,
              f"{len(studio_cells)} studio cells on the {resolution} × {resolution} grid; {sum(exact.values())} "
              f"reference points with one cell, {len(ambiguous)} within {DENSITY_EDGE:g} of a cell edge"
              + (", each in its cell or across that edge" if placed else f"; {why}"))
    else:
        total = sum(studio_cells.values())
        check("density total", total == other["count"],
              f"studio {total} in {len(studio_cells)} cells / {name} {other['count']} points (placement: the YOFO table)")
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

    def binned_over(chart: dict, lo, hi) -> bool:
        """`chart` declares [lo, hi] and its bins are YOFO's ring ratios binned over it."""
        return (lo is not None and hi is not None and chart.get("ring_range") == [lo, hi]
                and [int(v) for v in chart.get("histogram", [])] == ring_histogram(scatter.get("ring_ratio", []), lo, hi))

    # As opened (D4): the live range is declared and binned. ACCEPTED only
    # when it differs from the other half's; equal ranges do not exercise D4.
    ring, ring_app = charts.get("ring_range"), charts_app.get("ring_range")
    live_lo, live_hi = app.get("ring_ratio_min"), app.get("ring_ratio_max")
    live_binned = binned_over(charts_app, live_lo, live_hi)
    if same_range(ring_app):
        check("histogram range as opened", live_binned,
              f"studio live {ring_app} / {name} {other['ring']}; D4 not exercised (the live range is the recorded one)"
              + ("" if live_binned else "; the bins are not the live range's"))
    else:
        check("histogram range as opened", False,
              f"studio live {ring_app} / {name} {other['ring']}; bins over the live range: {'yes' if live_binned else 'NO'}",
              accepted=D4 if live_binned else "")
    probe, charts_d4 = studio.get("d4_probe"), load(d / "studio/charts_d4.json")
    if probe is None or charts_d4 is None:
        check("histogram range, live ≠ recorded (D4 probe)", False,
              "D4 probe missing: " + ", ".join(n for n, v in (("summary d4_probe", probe),
                                                              ("studio/charts_d4.json", charts_d4)) if v is None))
    else:
        lo, hi = probe.get("ring_ratio_min"), probe.get("ring_ratio_max")
        probe_binned = binned_over(charts_d4, lo, hi)
        check("histogram range, live ≠ recorded (D4 probe)", False,
              f"studio live set to {[lo, hi]} / {name} {other['ring']}; bins over the live range: "
              f"{'yes' if probe_binned else 'NO'}",
              accepted=D4 if probe_binned and not same_range([lo, hi]) else "")
    check("histogram range of the compared outputs", same_range(ring), f"studio {ring} / {name} {other['ring']}")
    # MIB Studio's histogram y axis tops at max(1, largest bin)
    # (desktop/src/components/ReviewCharts.tsx, maxBin); not in the JSON.
    # D10 only when the bins are identical and each top is its shell's rule.
    studio_y = histogram_y_top("studio", max(bins or [0]))
    headroom_only = (bins == other["bins"] and other["y_max"] is not None
                     and close(other["y_max"], histogram_y_top(name, max(bins or [0]))))
    check("histogram y range", other["y_max"] is not None and close(studio_y, other["y_max"]),
          f"studio 0–{studio_y} (ReviewCharts.tsx maxBin) / {name} 0–{other['y_max']}",
          accepted=D10 if headroom_only else "")

    if name == "yofo":
        roi, yofo_roi = studio.get("roi"), other.get("roi")
        check("recorded ROI", roi is not None and roi == yofo_roi, f"studio {roi} / yofo {yofo_roi}")
        (plain_ok, plain_detail), (roi_result, roi_detail) = compare_overlays(d, roi if roi == yofo_roi else None)
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
