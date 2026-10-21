"""tools/review_parity/compare.py (#512 S6): the MIB Studio pairs, the chart-range
tolerance and the accepted-difference handling (decisions D2-D5 and D10).
Every acceptance is tested both ways: the difference it covers, and a
regression it must refuse (a declared but unapplied live input, a missing,
misplaced or incomplete ROI, moved density cells, bins that differ, absent
dump files). The expected ROI bands are cv2.rectangle goldens
(tests/tools/data/review_parity_roi_bands.json), not compare.py's model."""
import importlib.util
import json
import math
import shutil
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("review_parity_compare", ROOT / "tools/review_parity/compare.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)

W, H = 24, 16          # overlay renders
ROI = [4, 3, 16, 10]   # recorded ROI: the path is x 4..19, y 3..12
YELLOW, RED = compare.STUDIO_ROI[0], compare.REVIEW_ROI[0]
GOLDENS = json.loads((ROOT / "tests/tools/data/review_parity_roi_bands.json").read_text(encoding="utf-8"))["cases"]


def png(path: Path, rgb: bytes, width: int = W, height: int = H) -> None:
    """RGB PNG; the rows cycle through filter types 0-4 so the decoder sees each."""
    row, lines, prev = width * 3, bytearray(), bytes(width * 3)
    for y in range(height):
        line, kind = rgb[y * row:(y + 1) * row], y % 5
        out = bytearray()
        for i in range(row):
            a = line[i - 3] if i >= 3 else 0
            b, c = prev[i], prev[i - 3] if i >= 3 else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = [0, a, b, (a + b) // 2, a if pa <= pb and pa <= pc else b if pb <= pc else c][kind]
            out.append((line[i] - pred) & 255)
        lines += bytes([kind]) + out
        prev = line

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(bytes(lines))) + chunk(b"IEND", b""))


def ppm(path: Path, rgb: bytes) -> None:
    path.write_bytes(f"P6\n{W} {H}\n255\n".encode() + rgb)


def pgm(path: Path, gray: bytes) -> None:
    path.write_bytes(f"P5\n{W} {H}\n255\n".encode() + gray)


def paint(rgb: bytes, pixels, colour) -> bytes:
    out = bytearray(rgb)
    for p in pixels:
        out[p * 3:p * 3 + 3] = bytes(colour)
    return bytes(out)


def golden(roi, thickness: int, width: int = W, height: int = H) -> set[int]:
    """The pixels cv2.rectangle painted for this case (pixel indices)."""
    case, = [c for c in GOLDENS if c["roi"] == list(roi) and c["thickness"] == thickness
             and (c["width"], c["height"]) == (width, height)]
    return {y * width + x for y, row in enumerate(case["rows"]) for x, v in enumerate(row) if v == "#"}


def shift(pixels, dx: int, dy: int) -> set[int]:
    return {(p // W + dy) * W + p % W + dx for p in pixels}


def box(x0: int, y0: int, x1: int, y1: int, half: int) -> set[int]:
    """Every pixel within `half` of the path of (x0, y0, x1, y1), square
    corners: [x0 - half, x1 + half] × [y0 - half, y1 + half] less the inside."""
    return {y * W + x for y in range(y0 - half, y1 + half + 1) for x in range(x0 - half, x1 + half + 1)
            if not (x0 + half < x < x1 - half and y0 + half < y < y1 - half)}


GRAY = bytes(40 + (i * 7) % 150 for i in range(W * H))
PLAIN = bytes(v for g in GRAY for v in (g, g, g))
STUDIO_ON = paint(PLAIN, golden(ROI, 1), YELLOW)   # RoiStyle::Facade: yellow, thickness 1
YOFO_ON = paint(PLAIN, golden(ROI, 3), RED)        # drawRoiOverlay: red, thickness 3 (a 5 px band)

# Valid rows: 8 finite points (2 non-finite), recorded at 0.25 µm/px.
AREAS = [6.25] * 5 + [7.375] * 3 + [None, None]
DEFORMS = [0.1] * 5 + [0.28] * 3 + [0.2, None]
RATIOS = [15.2] * 3 + [16.2] * 5 + [None, None]
LIVE, RECORDED = 0.4886, 0.25
OVERLAYS = ["valid_000000_m0_roi0", "valid_000000_m0_roi1"]


def write(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False) if not isinstance(value, str) else value, encoding="utf-8")


def cells_of(areas, deforms, area_range, deform_range, resolution: int = 128) -> dict:
    """reviewChartsJson's density bin, written out here rather than taken from compare.py."""
    out: dict = {}
    for a, f in zip(areas, deforms):
        if a is None or f is None:
            continue
        x = min(max(int((a - area_range[0]) / (area_range[1] - area_range[0]) * resolution), 0), resolution - 1)
        y = min(max(int((f - deform_range[0]) / (deform_range[1] - deform_range[0]) * resolution), 0), resolution - 1)
        out[(x, y)] = out.get((x, y), 0) + 1
    return out


def charts_for(factor: float, lo: float, hi: float) -> dict:
    """What reviewChartsJson returns for the fixture's points."""
    k = (factor / RECORDED) ** 2
    area = [v * k if v is not None else None for v in AREAS]
    x, y = (6.25 * k, 7.375 * k), (0.1, 0.28)
    area_range = [x[0] - 0.1 * (x[1] - x[0]), x[1] + 0.1 * (x[1] - x[0])]
    deform_range = [y[0] - 0.1 * (y[1] - y[0]), y[1] + 0.1 * (y[1] - y[0])]
    cells = cells_of(area, DEFORMS, area_range, deform_range)
    return {"valid": True, "pixel_to_micron": factor, "rows": "10", "finite_points": "8",
            "area_range": area_range, "deform_range": deform_range, "resolution": 128,
            "density": [[cx, cy, str(n)] for (cx, cy), n in sorted(cells.items())],
            "histogram": [str(v) for v in compare.ring_histogram(RATIOS, lo, hi)], "ring_range": [lo, hi]}


def make_dumps(d: Path) -> None:
    """Three consistent halves of one experiment file recorded at 0.25 µm/px."""
    compared = charts_for(RECORDED, 15.0, 25.0)
    bins = [int(v) for v in compared["histogram"]]
    x = {"min": compared["area_range"][0], "max": compared["area_range"][1]}
    y = {"min": compared["deform_range"][0], "max": compared["deform_range"][1]}
    labels = [f"{15 + 0.5 * k:.1f}-{15 + 0.5 * (k + 1):.1f}" for k in range(20)]
    top = max(bins)
    common = {"source": "/data/run.h5", "recording_file": False}
    write(d / "qt/summary.json", {**common, "pixel_to_micron": RECORDED,
                                  "status_text": "Valid: 8, Invalid: 2 · run complete — ok",
                                  "scatter": {"count": 8, "x": x, "y": y},
                                  "histogram": {"counts": bins, "labels": labels,
                                                "y": {"min": 0, "max": compare.histogram_y_top("qt", top)}},
                                  "core_status_text": "saved. Core 90%: 8 of 8 cells"})
    write(d / "yofo/summary.json", {**common, "pixel_to_micron": RECORDED, "pixel_to_micron_from_file": True,
                                    "accounting_summary": " · run complete — ok", "scatter_count": 8,
                                    "ring_ratio_min": 15.0, "ring_ratio_max": 25.0, "roi": ROI,
                                    "core_message": "Core 90%: 8 of 8 cells", "overlays": OVERLAYS})
    write(d / "yofo/scatter.json", {"valid": True, "pixel_to_micron": RECORDED, "area_um2": AREAS,
                                    "deformability": DEFORMS, "ring_ratio": RATIOS})
    write(d / "yofo/charts.json", {"scatter": {"count": 8, "x": dict(x), "y": dict(y)},
                                   "histogram": {"counts": bins, "labels": labels,
                                                 "y": {"min": 0, "max": compare.histogram_y_top("yofo", top)}}})
    core = {"cell_count": 8, "population_count": 8, "contours": [[1, 2]], "computed_at_ns": 1}
    write(d / "qt/core.json", core)
    write(d / "yofo/core.json", {**core, "computed_at_ns": 2})
    write(d / "studio/charts.json", compared)
    write(d / "studio/charts_app.json", charts_for(LIVE, 15.0, 25.0))
    write(d / "studio/charts_d4.json", charts_for(LIVE, 10.0, 30.0))
    write(d / "studio/summary.json", {**common, "roi": ROI,
                                      "app": {"pixel_to_micron": LIVE, "ring_ratio_min": 15.0, "ring_ratio_max": 25.0},
                                      "d4_probe": {"ring_ratio_min": 10.0, "ring_ratio_max": 30.0},
                                      "compared": {"pixel_to_micron": RECORDED, "pixel_to_micron_from_file": True,
                                                   "ring_ratio_min": 15.0, "ring_ratio_max": 25.0},
                                      "all_export": {"state": "completed"}, "metrics_export": {"state": "completed"},
                                      "overlays": OVERLAYS, "failures": []})
    for half in ("qt", "yofo", "studio"):
        write(d / half / "metrics.csv", "frame,area\n1,2.5\n")
        write(d / half / "all/run/metrics.csv", "frame,area\n1,2.5\n")
        write(d / half / "all/run/valid_frame_000000.tiff", "image")
    for name in compare.STUDIO_CHART_FILES:
        write(d / "studio/all/run" / name, "chart")
    (d / "studio/overlays").mkdir(parents=True)
    (d / "yofo/overlays").mkdir(parents=True)
    # Mode 0, ROI off: YOFO keeps Mono8, MIB Studio returns RGB.
    png(d / "studio/overlays/valid_000000_m0_roi0.png", PLAIN)
    pgm(d / "yofo/overlays/valid_000000_m0_roi0.pgm", GRAY)
    png(d / "studio/overlays/valid_000000_m0_roi1.png", STUDIO_ON)
    ppm(d / "yofo/overlays/valid_000000_m0_roi1.ppm", YOFO_ON)


def result(rows, pair: str, check: str) -> str:
    found = [r for p, c, r, _ in rows if p.startswith(pair) and c == check]
    assert len(found) == 1, (pair, check, found)
    return found[0]


class ReviewParityCompareTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        make_dumps(self.dir)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def rows(self):
        return compare.compare(self.dir)[1]

    def edit(self, rel: str, change):
        path = self.dir / rel
        value = json.loads(path.read_text(encoding="utf-8"))
        change(value)
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")

    def studio(self, check: str, pair: str = "MIB Studio ↔ YOFO") -> str:
        return result(self.rows(), pair, check)

    def test_consistent_halves_pass_with_the_accepted_differences(self):
        rows = self.rows()
        self.assertEqual(compare.main([str(self.dir)]), 0)
        self.assertIn("MIB Studio ↔ YOFO Review", (self.dir / "report.md").read_text(encoding="utf-8"))
        for pair in ("MIB Studio ↔ Qt", "MIB Studio ↔ YOFO"):
            self.assertEqual(result(rows, pair, "px→µm as opened"), "ACCEPTED")
            self.assertEqual(result(rows, pair, "Export All chart files"), "ACCEPTED")
            self.assertEqual(result(rows, pair, "histogram range as opened"), "PASS")
            self.assertEqual(result(rows, pair, "histogram range, live ≠ recorded (D4 probe)"), "ACCEPTED")
            self.assertEqual(result(rows, pair, "histogram y range"), "ACCEPTED")
        self.assertEqual(result(rows, "MIB Studio ↔ YOFO", "density cells"), "PASS")
        self.assertEqual(result(rows, "MIB Studio ↔ YOFO", "overlays, ROI off"), "PASS")
        self.assertEqual(result(rows, "MIB Studio ↔ YOFO", "overlays, ROI on"), "ACCEPTED")
        accepted = {c for _, c, r, _ in rows if r == "ACCEPTED"}
        self.assertEqual(accepted, {"px→µm as opened", "Export All chart files", "overlays, ROI on",
                                    "histogram y range", "histogram range, live ≠ recorded (D4 probe)"})
        self.assertEqual(result(rows, "Qt tab ↔ YOFO", "histogram y range"), "PASS")
        self.assertIn("D4 not exercised", [t for p, c, _, t in rows if c == "histogram range as opened"][0])
        self.assertFalse([r for r in rows if r[2] == "DIFF"])

    def test_chart_range_tolerance(self):
        m = compare.CHART_RANGE_REL
        self.assertEqual(m, 1e-12, "the documented tolerance (tools/review_parity/README.md)")
        lo, hi = json.loads((self.dir / "studio/charts.json").read_text(encoding="utf-8"))["area_range"]
        self.assertTrue(compare.range_close((lo, hi), (lo, hi)))
        nudged = hi
        for _ in range(8):
            nudged = math.nextafter(nudged, math.inf)
        self.assertTrue(compare.range_close((lo, hi), (lo, nudged)), "a few ulp pass")
        self.assertFalse(compare.range_close((lo, hi), (lo, hi * (1 + 10 * m))))
        # Relative to the range's largest magnitude: an end near zero is not
        # held to its own ulp.
        self.assertTrue(compare.range_close((-1e-17, 3260.0), (0.0, 3260.0)))
        self.edit("yofo/charts.json", lambda v: v["scatter"]["x"].update(max=nudged))
        self.assertEqual(self.studio("scatter axis extents"), "PASS")
        self.edit("yofo/charts.json", lambda v: v["scatter"]["x"].update(max=hi * (1 + 1e-10)))
        self.assertEqual(self.studio("scatter axis extents"), "DIFF")
        self.assertEqual(self.studio("scatter axis extents", "MIB Studio ↔ Qt"), "PASS")

    def test_d3_needs_the_live_factor_applied(self):
        # Declared but not applied: the as-opened charts name the live factor
        # yet their areas are the recorded factor's.
        unapplied = {**charts_for(RECORDED, 15.0, 25.0), "pixel_to_micron": LIVE}
        write(self.dir / "studio/charts_app.json", unapplied)
        self.assertEqual(self.studio("px→µm as opened", "MIB Studio ↔ Qt"), "DIFF")
        # Applied but declared as another factor.
        write(self.dir / "studio/charts_app.json", {**charts_for(LIVE, 15.0, 25.0), "pixel_to_micron": 0.3})
        self.assertEqual(self.studio("px→µm as opened"), "DIFF")
        write(self.dir / "studio/charts_app.json", {"valid": False, "error": "No HDF file open"})
        self.assertEqual(self.studio("px→µm as opened"), "DIFF")
        write(self.dir / "studio/charts_app.json", charts_for(LIVE, 15.0, 25.0))
        self.edit("studio/summary.json", lambda v: v["compared"].update(pixel_to_micron=LIVE))
        self.assertEqual(self.studio("px→µm of the compared outputs"), "DIFF")

    def test_d4_needs_the_live_range_binned(self):
        # As opened with a live range other than the recorded one: accepted
        # when declared and binned over it.
        write(self.dir / "studio/charts_app.json", charts_for(LIVE, 10.0, 20.0))
        self.edit("studio/summary.json", lambda v: v["app"].update(ring_ratio_min=10.0, ring_ratio_max=20.0))
        self.assertEqual(self.studio("histogram range as opened"), "ACCEPTED")
        self.assertEqual(self.studio("histogram range as opened", "MIB Studio ↔ Qt"), "ACCEPTED")
        # Declared but not applied: the live range named, the recorded range's bins.
        write(self.dir / "studio/charts_app.json", {**charts_for(LIVE, 15.0, 25.0), "ring_range": [10.0, 20.0]})
        self.assertEqual(self.studio("histogram range as opened"), "DIFF")
        # The probe the same way.
        write(self.dir / "studio/charts_d4.json", {**charts_for(LIVE, 15.0, 25.0), "ring_range": [10.0, 30.0]})
        self.assertEqual(self.studio("histogram range, live ≠ recorded (D4 probe)"), "DIFF")
        # Equal ranges with bins that are not the range's: not exercised, and wrong.
        write(self.dir / "studio/charts_app.json", {**charts_for(LIVE, 15.0, 25.0), "histogram": ["8"] + ["0"] * 19})
        self.edit("studio/summary.json", lambda v: v["app"].update(ring_ratio_min=15.0, ring_ratio_max=25.0))
        self.assertEqual(self.studio("histogram range as opened"), "DIFF")
        write(self.dir / "studio/charts.json", {**charts_for(RECORDED, 10.0, 20.0)})
        self.assertEqual(self.studio("histogram range of the compared outputs", "MIB Studio ↔ Qt"), "DIFF")

    def test_density_placement(self):
        self.edit("studio/charts.json", lambda v: v.update(density=[[0, 0, "8"]]))   # every point in cell (0, 0)
        self.assertEqual(self.studio("density cells"), "DIFF")
        self.assertEqual(self.studio("density total", "MIB Studio ↔ Qt"), "PASS", "the total alone cannot see it")
        self.edit("studio/charts.json", lambda v: v.update(density=[[c[0], c[1] + 1, c[2]] for c in v["density"]]))
        self.assertEqual(self.studio("density cells"), "DIFF")

    def test_density_edge_points_move_only_across_their_edge(self):
        # Codex's case: points 0, 1 and 0.5 on both axes over [-0.1, 1.1] at
        # 128 cells. 0.5 lands on the edge between cells 63 and 64 (63.99… in
        # double); 0 and 1 sit inside cells 10 and 117.
        grid = {"area_range": [-0.1, 1.1], "deform_range": [-0.1, 1.1], "resolution": 128}
        self.edit("yofo/scatter.json", lambda v: v.update(area_um2=[0.0, 1.0, 0.5], deformability=[0.0, 1.0, 0.5]))
        self.assertEqual(cells_of([0.0, 1.0, 0.5], [0.0, 1.0, 0.5], grid["area_range"], grid["deform_range"]),
                         {(10, 10): 1, (117, 117): 1, (63, 63): 1})

        def studio(*cells):
            self.edit("studio/charts.json", lambda v: v.update(grid, density=[[x, y, "1"] for x, y in cells]))
            return self.studio("density cells")
        self.assertEqual(studio((10, 10), (117, 117), (63, 63)), "PASS")
        # The edge point across its edge (long double puts it at 64): accepted, on either axis.
        self.assertEqual(studio((10, 10), (117, 117), (64, 64)), "PASS")
        self.assertEqual(studio((10, 10), (117, 117), (63, 64)), "PASS")
        # The non-edge point moved (10, 10) → (90, 90): refused, though one point is on an edge.
        self.assertEqual(studio((90, 90), (117, 117), (64, 64)), "DIFF")
        self.assertEqual(studio((11, 10), (117, 117), (63, 63)), "DIFF")
        # The edge point two cells away, or counted twice.
        self.assertEqual(studio((10, 10), (117, 117), (65, 64)), "DIFF")
        self.edit("studio/charts.json", lambda v: v.update(density=[[10, 10, "1"], [117, 117, "1"], [63, 63, "1"],
                                                                    [64, 64, "1"]]))
        self.assertEqual(self.studio("density cells"), "DIFF")
        # The non-edge point moved into the edge point's cell: the count fits, the placement does not.
        self.edit("studio/charts.json", lambda v: v.update(density=[[63, 63, "2"], [117, 117, "1"]]))
        self.assertEqual(self.studio("density cells"), "DIFF")
        # Two edge points on different edges, both counted beside one of them.
        self.edit("yofo/scatter.json", lambda v: v.update(area_um2=[0.5, 0.5], deformability=[0.0, 1.0]))
        self.assertEqual(studio((64, 10), (63, 117)), "PASS")
        self.edit("studio/charts.json", lambda v: v.update(density=[[63, 10, "2"]]))
        self.assertEqual(self.studio("density cells"), "DIFF")
        # Two edge points with overlapping candidates: one each side, or both on one side.
        self.edit("yofo/scatter.json", lambda v: v.update(area_um2=[0.5, 0.5], deformability=[0.0, 0.0]))
        self.assertEqual(studio((63, 10), (64, 10)), "PASS")
        self.edit("studio/charts.json", lambda v: v.update(density=[[64, 10, "2"]]))
        self.assertEqual(self.studio("density cells"), "PASS")
        self.edit("studio/charts.json", lambda v: v.update(density=[[64, 10, "1"], [64, 11, "1"]]))
        self.assertEqual(self.studio("density cells"), "DIFF")

    def test_d5_covers_only_the_chart_tiffs(self):
        write(self.dir / "studio/all/run/extra.tiff", "x")
        rows = self.rows()
        self.assertEqual(result(rows, "MIB Studio ↔ Qt", "Export All files"), "DIFF")
        self.assertEqual(result(rows, "MIB Studio ↔ Qt", "Export All chart files"), "ACCEPTED")
        (self.dir / "studio/all/run/extra.tiff").unlink()
        (self.dir / "studio/all/run/histogram.tiff").unlink()
        self.assertEqual(self.studio("Export All chart files"), "DIFF")
        write(self.dir / "studio/all/run/metrics.csv", "frame,area\n1,2.6\n")
        self.assertEqual(self.studio("Export All files"), "DIFF")

    def test_d2_needs_each_style_at_the_recorded_roi(self):
        studio_on = self.dir / "studio/overlays/valid_000000_m0_roi1.png"
        yofo_on = self.dir / "yofo/overlays/valid_000000_m0_roi1.ppm"
        self.assertEqual(self.studio("overlays, ROI on"), "ACCEPTED")
        # MIB Studio stops drawing the ROI while YOFO still does.
        png(studio_on, PLAIN)
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        # A yellow rectangle in the wrong place.
        png(studio_on, paint(PLAIN, shift(golden(ROI, 1), 1, 1), YELLOW))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        # A stray yellow pixel inside the rectangle.
        png(studio_on, paint(STUDIO_ON, [7 * W + 10], YELLOW))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        # MIB Studio's rectangle with one pixel missing.
        png(studio_on, paint(STUDIO_ON, [3 * W + 10], PLAIN[(3 * W + 10) * 3:(3 * W + 10) * 3 + 3]))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        png(studio_on, STUDIO_ON)
        # YOFO's band offset, or in another colour.
        ppm(yofo_on, paint(PLAIN, shift(golden(ROI, 3), 0, 1), RED))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        ppm(yofo_on, paint(PLAIN, golden(ROI, 3), (0, 255, 0)))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        # Incomplete YOFO bands: a thin 1 px red rectangle at the ROI, a 3 px
        # band, the band with one pixel missing, or square corners.
        ppm(yofo_on, paint(PLAIN, golden(ROI, 1), RED))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        ppm(yofo_on, paint(PLAIN, box(4, 3, 19, 12, 1), RED))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        missing = sorted(golden(ROI, 3))[17]
        ppm(yofo_on, paint(YOFO_ON, [missing], PLAIN[missing * 3:missing * 3 + 3]))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        self.assertTrue(golden(ROI, 3) < box(4, 3, 19, 12, 2))
        ppm(yofo_on, paint(PLAIN, box(4, 3, 19, 12, 2), RED))
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        ppm(yofo_on, YOFO_ON)
        self.assertEqual(self.studio("overlays, ROI on"), "ACCEPTED")
        # The halves disagree on the recorded ROI.
        self.edit("yofo/summary.json", lambda v: v.update(roi=[4, 3, 16, 11]))
        self.assertEqual(self.studio("recorded ROI"), "DIFF")
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        # No recorded ROI: ROI on must equal ROI off on both sides, a PASS.
        for half in ("studio", "yofo"):
            self.edit(f"{half}/summary.json", lambda v: v.update(roi=[0, 0, 0, 0]))
        png(studio_on, PLAIN)
        ppm(yofo_on, PLAIN)
        self.assertEqual(self.studio("overlays, ROI on"), "PASS")
        ppm(yofo_on, YOFO_ON)
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")
        # ROI off must be pixel-identical.
        png(self.dir / "studio/overlays/valid_000000_m0_roi0.png", paint(PLAIN, [5], (0, 0, 0)))
        self.assertEqual(self.studio("overlays, ROI off"), "DIFF")
        (self.dir / "yofo/overlays/valid_000000_m0_roi1.ppm").unlink()
        self.assertEqual(self.studio("overlays, ROI on"), "DIFF")

    def test_roi_geometry(self):
        self.assertEqual(compare.studio_roi_rect(ROI, W, H), (4, 3, 19, 12))
        self.assertEqual(compare.review_roi_rect(ROI, W, H), (4, 3, 19, 12))
        # Clipped at the image: the facade intersects, drawRoiOverlay clamps.
        self.assertEqual(compare.studio_roi_rect([20, 12, 10, 10], W, H), (20, 12, 23, 15))
        self.assertEqual(compare.review_roi_rect([20, 12, 10, 10], W, H), (20, 12, 23, 15))
        self.assertIsNone(compare.studio_roi_rect([0, 0, 0, 0], W, H))
        self.assertIsNone(compare.review_roi_rect([0, 0, 0, 0], W, H))

    def test_roi_band_matches_opencv(self):
        # compare.roi_band against every cv2.rectangle golden: the facade's
        # thickness 1 on Rect & image, drawRoiOverlay's thickness 3 on the
        # clamped ROI.
        self.assertEqual(len(GOLDENS), 7)
        for case in GOLDENS:
            width, height, roi, thickness = case["width"], case["height"], case["roi"], case["thickness"]
            rect = (compare.review_roi_rect if thickness == 3 else compare.studio_roi_rect)(roi, width, height)
            expected = {y * width + x for y, row in enumerate(case["rows"]) for x, v in enumerate(row) if v == "#"}
            self.assertEqual(compare.roi_band(rect, width, height, thickness), expected, case["note"])

    def test_d10_needs_identical_bins_and_each_shells_rule(self):
        for pair in ("MIB Studio ↔ Qt", "MIB Studio ↔ YOFO"):
            self.assertEqual(self.studio("histogram y range", pair), "ACCEPTED")
        # The rules reproduce the sign-off run's tops (largest bin → Qt / YOFO / MIB Studio).
        for top, qt, yofo in ((64, 80, 71), (248, 300, 273), (8, 10, 9), (0, 1, 1)):
            self.assertEqual((compare.histogram_y_top("qt", top), compare.histogram_y_top("yofo", top),
                              compare.histogram_y_top("studio", top)), (qt, yofo, max(1, top)))
        # Bins that differ with the same largest bin: not headroom only.
        self.edit("studio/charts.json", lambda v: v.update(histogram=["2", "1"] + v["histogram"][2:]))
        self.assertEqual(self.studio("histogram bins"), "DIFF")
        self.assertEqual(self.studio("histogram y range"), "DIFF")
        self.assertEqual(self.studio("histogram y range", "MIB Studio ↔ Qt"), "DIFF")
        write(self.dir / "studio/charts.json", charts_for(RECORDED, 15.0, 25.0))
        # A YOFO top that is not ceil(1.1 × max), or a Qt top other than its nice numbers.
        self.edit("yofo/charts.json", lambda v: v["histogram"]["y"].update(max=7))
        self.assertEqual(self.studio("histogram y range"), "DIFF")
        self.assertEqual(self.studio("histogram y range", "MIB Studio ↔ Qt"), "ACCEPTED")
        self.edit("qt/summary.json", lambda v: v["histogram"]["y"].update(max=8))
        self.assertEqual(self.studio("histogram y range", "MIB Studio ↔ Qt"), "DIFF")
        # Equal tops pass without the acceptance.
        self.edit("qt/summary.json", lambda v: v["histogram"]["y"].update(max=5))
        self.assertEqual(self.studio("histogram y range", "MIB Studio ↔ Qt"), "PASS")

    def test_absent_dump_files_are_differences(self):
        self.assertEqual(self.studio("expected artefacts"), "PASS")
        self.assertEqual(self.studio("studio run"), "PASS")
        # No D4 probe evidence: not a silent skip.
        (self.dir / "studio/charts_d4.json").unlink()
        for pair in ("MIB Studio ↔ Qt", "MIB Studio ↔ YOFO"):
            self.assertEqual(self.studio("histogram range, live ≠ recorded (D4 probe)", pair), "DIFF")
            self.assertEqual(self.studio("expected artefacts", pair), "DIFF")
        detail = [t for p, c, _, t in self.rows() if c.startswith("histogram range, live")][0]
        self.assertIn("D4 probe missing", detail)
        write(self.dir / "studio/charts_d4.json", charts_for(LIVE, 10.0, 30.0))
        self.edit("studio/summary.json", lambda v: v.pop("d4_probe"))
        self.assertEqual(self.studio("histogram range, live ≠ recorded (D4 probe)"), "DIFF")
        self.assertIn("studio/summary.json d4_probe",
                      [t for p, c, _, t in self.rows() if c == "expected artefacts"][0])
        self.assertEqual(compare.main([str(self.dir)]), 1)
        # Each other absent artefact is named.
        make = lambda: (shutil.rmtree(self.dir), self.dir.mkdir(), make_dumps(self.dir))
        for rel in ("studio/metrics.csv", "studio/charts_app.json", "yofo/scatter.json",
                    "studio/overlays/valid_000000_m0_roi1.png", "yofo/overlays/valid_000000_m0_roi0.pgm"):
            make()
            (self.dir / rel).unlink()
            detail = [t for p, c, _, t in self.rows() if p.startswith("MIB Studio ↔ YOFO") and c == "expected artefacts"][0]
            self.assertEqual(self.studio("expected artefacts"), "DIFF", rel)
            self.assertIn(rel.replace(".pgm", ".p?m"), detail)
        make()
        shutil.rmtree(self.dir / "studio/all")
        self.assertIn("studio/all/ (empty)", [t for p, c, _, t in self.rows() if c == "expected artefacts"][0])
        # A failure the studio run recorded, or an export that did not complete.
        make()
        self.edit("studio/summary.json", lambda v: v.update(failures=["render_review_overlay x: empty"]))
        self.assertEqual(self.studio("studio run"), "DIFF")
        make()
        self.edit("studio/summary.json", lambda v: v["metrics_export"].update(state="failed"))
        self.assertEqual(self.studio("studio run"), "DIFF")
        make()
        self.edit("studio/summary.json", lambda v: v.pop("failures"))
        self.assertEqual(self.studio("studio run"), "DIFF")
        self.assertEqual(self.studio("expected artefacts"), "DIFF")

    def test_unaccepted_studio_difference_fails(self):
        self.edit("studio/charts.json", lambda v: v.update(histogram=["3", "0", "4"] + ["0"] * 17))
        rows = self.rows()
        for pair in ("MIB Studio ↔ Qt", "MIB Studio ↔ YOFO"):
            self.assertEqual(result(rows, pair, "histogram bins"), "DIFF")
            self.assertEqual(result(rows, pair, "histogram y range"), "DIFF")
        self.assertEqual(compare.main([str(self.dir)]), 1)

    def test_without_the_studio_half(self):
        shutil.rmtree(self.dir / "studio")
        report, rows = compare.compare(self.dir)
        self.assertEqual({p for p, *_ in rows}, {"Qt tab ↔ YOFO Review"})
        self.assertIn("MIB Studio half: not run", report)
        self.assertEqual(compare.main([str(self.dir)]), 0)

    def test_png_decoder(self):
        rgb = bytes((i * 37 + 11) & 255 for i in range(W * H * 3))
        png(self.dir / "p.png", rgb)
        self.assertEqual(compare.read_png(self.dir / "p.png"), (W, H, rgb))
        pgm(self.dir / "g.pgm", GRAY)
        self.assertEqual(compare.read_netpbm(self.dir / "g.pgm"), (W, H, PLAIN))


if __name__ == "__main__":
    unittest.main()
