"""tools/review_parity/compare.py (#512 S6): the MIB Studio pairs, the chart-range
tolerance and the accepted-difference handling (decisions D2-D5)."""
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

W, H = 6, 4


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


def put(rgb: bytes, pixel: int, colour: tuple[int, int, int]) -> bytes:
    out = bytearray(rgb)
    out[pixel * 3:pixel * 3 + 3] = bytes(colour)
    return bytes(out)


GRAY = bytes(range(10, 10 + W * H))
PLAIN = bytes(v for g in GRAY for v in (g, g, g))
TOP_ROW = range(W)  # pixels an ROI outline covers in these fixtures


def write(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value) if not isinstance(value, str) else value)


def make_dumps(d: Path) -> None:
    """Three consistent halves of one experiment file recorded at 0.25 µm/px."""
    bins = [3, 0, 5] + [0] * 17
    x, y = {"min": 6.1375, "max": 7.4875}, {"min": 0.082, "max": 0.29800000000000004}
    labels = [f"{15 + 0.5 * k:.1f}-{15 + 0.5 * (k + 1):.1f}" for k in range(20)]
    common = {"source": "/data/run.h5", "recording_file": False}
    write(d / "qt/summary.json", {**common, "pixel_to_micron": 0.25, "status_text": "Valid: 8, Invalid: 2 · ok",
                                  "scatter": {"count": 8, "x": x, "y": y},
                                  "histogram": {"counts": bins, "labels": labels, "y": {"min": 0, "max": 5}},
                                  "core_status_text": "saved. Core 90%: 8 of 8 cells"})
    write(d / "yofo/summary.json", {**common, "pixel_to_micron": 0.25, "pixel_to_micron_from_file": True,
                                    "accounting_summary": " · ok", "scatter_count": 8, "ring_ratio_min": 15.0,
                                    "ring_ratio_max": 25.0, "core_message": "Core 90%: 8 of 8 cells"})
    write(d / "yofo/charts.json", {"scatter": {"count": 8, "x": dict(x), "y": dict(y)},
                                   "histogram": {"counts": bins, "labels": labels, "y": {"min": 0, "max": 5}}})
    core = {"cell_count": 8, "population_count": 8, "contours": [[1, 2]], "computed_at_ns": 1}
    write(d / "qt/core.json", core)
    write(d / "yofo/core.json", {**core, "computed_at_ns": 2})
    charts = {"valid": True, "pixel_to_micron": 0.25, "rows": "10", "finite_points": "8",
              "area_range": [x["min"], x["max"]], "deform_range": [y["min"], y["max"]],
              "density": [[0, 0, "5"], [3, 9, "3"]], "histogram": [str(v) for v in bins], "ring_range": [15.0, 25.0]}
    write(d / "studio/charts.json", charts)
    write(d / "studio/charts_app.json", {**charts, "pixel_to_micron": 0.4886,
                                         "area_range": [v * (0.4886 / 0.25) ** 2 for v in charts["area_range"]]})
    write(d / "studio/summary.json", {**common, "app": {"pixel_to_micron": 0.4886, "ring_ratio_min": 15.0,
                                                        "ring_ratio_max": 25.0},
                                      "compared": {"pixel_to_micron": 0.25, "pixel_to_micron_from_file": True,
                                                   "ring_ratio_min": 15.0, "ring_ratio_max": 25.0}})
    for half in ("qt", "yofo", "studio"):
        write(d / half / "metrics.csv", "frame,area\n1,2.5\n")
        write(d / half / "all/run/metrics.csv", "frame,area\n1,2.5\n")
        write(d / half / "all/run/valid_frame_000000.tiff", "image")
    for name in compare.STUDIO_CHART_FILES:
        write(d / "studio/all/run" / name, "chart")
    (d / "studio/overlays").mkdir(parents=True)
    (d / "yofo/overlays").mkdir(parents=True)
    # Mode 0, ROI off: YOFO keeps Mono8, MIB Studio returns RGB; the ROI
    # outlines: MIB Studio yellow on pixel 0, YOFO red on the top row.
    png(d / "studio/overlays/valid_000000_m0_roi0.png", PLAIN)
    pgm(d / "yofo/overlays/valid_000000_m0_roi0.pgm", GRAY)
    png(d / "studio/overlays/valid_000000_m0_roi1.png", put(PLAIN, 0, (255, 210, 0)))
    red = PLAIN
    for p in TOP_ROW:
        red = put(red, p, (255, 0, 0))
    ppm(d / "yofo/overlays/valid_000000_m0_roi1.ppm", red)


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
        value = json.loads(path.read_text())
        change(value)
        path.write_text(json.dumps(value))

    def test_consistent_halves_pass_with_d2_d3_d5_accepted(self):
        rows = self.rows()
        self.assertEqual(compare.main([str(self.dir)]), 0)
        self.assertTrue((self.dir / "report.md").is_file())
        for pair in ("MIB Studio ↔ Qt", "MIB Studio ↔ YOFO"):
            self.assertEqual(result(rows, pair, "px→µm as opened"), "ACCEPTED")
            self.assertEqual(result(rows, pair, "Export All chart files"), "ACCEPTED")
            self.assertEqual(result(rows, pair, "histogram range as opened"), "PASS")
        self.assertEqual(result(rows, "MIB Studio ↔ YOFO", "overlays, ROI off"), "PASS")
        self.assertEqual(result(rows, "MIB Studio ↔ YOFO", "overlays, ROI on"), "ACCEPTED")
        accepted = {c for _, c, r, _ in rows if r == "ACCEPTED"}
        self.assertEqual(accepted, {"px→µm as opened", "Export All chart files", "overlays, ROI on"})
        self.assertFalse([r for r in rows if r[2] == "DIFF"])

    def test_chart_range_tolerance(self):
        m = compare.CHART_RANGE_REL
        self.assertEqual(m, 1e-12, "the documented tolerance (tools/review_parity/README.md)")
        self.assertTrue(compare.range_close((6.1375, 7.4875), (6.1375, 7.4875)))
        nudged = 7.4875
        for _ in range(8):
            nudged = math.nextafter(nudged, math.inf)
        self.assertTrue(compare.range_close((6.1375, 7.4875), (6.1375, nudged)), "a few ulp pass")
        self.assertFalse(compare.range_close((6.1375, 7.4875), (6.1375, 7.4875 * (1 + 10 * m))))
        # Relative to the range's largest magnitude: an end near zero is not
        # held to its own ulp.
        self.assertTrue(compare.range_close((-1e-17, 3260.0), (0.0, 3260.0)))
        self.edit("yofo/charts.json", lambda v: v["scatter"]["x"].update(max=nudged))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "scatter axis extents"), "PASS")
        self.edit("yofo/charts.json", lambda v: v["scatter"]["x"].update(max=7.4875 * (1 + 1e-10)))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "scatter axis extents"), "DIFF")
        self.assertEqual(result(self.rows(), "MIB Studio ↔ Qt", "scatter axis extents"), "PASS")

    def test_d3_needs_the_live_factor(self):
        self.edit("studio/charts_app.json", lambda v: v.update(pixel_to_micron=0.3))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ Qt", "px→µm as opened"), "DIFF")
        self.edit("studio/summary.json", lambda v: v["compared"].update(pixel_to_micron=0.4886))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "px→µm of the compared outputs"), "DIFF")

    def test_d4_needs_the_live_range(self):
        self.edit("studio/charts_app.json", lambda v: v.update(ring_range=[10.0, 20.0]))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "histogram range as opened"), "DIFF")
        self.edit("studio/summary.json", lambda v: v["app"].update(ring_ratio_min=10.0, ring_ratio_max=20.0))
        rows = self.rows()
        self.assertEqual(result(rows, "MIB Studio ↔ YOFO", "histogram range as opened"), "ACCEPTED")
        self.assertEqual(result(rows, "MIB Studio ↔ Qt", "histogram range as opened"), "ACCEPTED")
        self.edit("studio/charts.json", lambda v: v.update(ring_range=[10.0, 20.0]))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ Qt", "histogram range of the compared outputs"), "DIFF")

    def test_d5_covers_only_the_chart_tiffs(self):
        write(self.dir / "studio/all/run/extra.tiff", "x")
        rows = self.rows()
        self.assertEqual(result(rows, "MIB Studio ↔ Qt", "Export All files"), "DIFF")
        self.assertEqual(result(rows, "MIB Studio ↔ Qt", "Export All chart files"), "ACCEPTED")
        (self.dir / "studio/all/run/extra.tiff").unlink()
        (self.dir / "studio/all/run/histogram.tiff").unlink()
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "Export All chart files"), "DIFF")
        write(self.dir / "studio/all/run/metrics.csv", "frame,area\n1,2.6\n")
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "Export All files"), "DIFF")

    def test_d2_covers_only_the_roi_outlines(self):
        studio_on = self.dir / "studio/overlays/valid_000000_m0_roi1.png"
        png(studio_on, put(put(PLAIN, 0, (255, 210, 0)), W * 2 + 3, (0, 0, 255)))  # a pixel off both outlines
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "overlays, ROI on"), "DIFF")
        png(studio_on, PLAIN)
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "overlays, ROI on"), "ACCEPTED")
        # MIB Studio's style: a 1 px yellow rectangle; a yellow pixel inside it is not part of one.
        border = [p for p in range(W * H) if p % W in (0, W - 1) or p // W in (0, H - 1)]
        frame = PLAIN
        for p in border:
            frame = put(frame, p, (255, 210, 0))
        png(studio_on, frame)
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "overlays, ROI on"), "ACCEPTED")
        png(studio_on, put(frame, W + 2, (255, 210, 0)))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "overlays, ROI on"), "DIFF")
        png(studio_on, frame)
        # YOFO's style is red: another colour on its outline is not D2.
        yofo_on = self.dir / "yofo/overlays/valid_000000_m0_roi1.ppm"
        red = PLAIN
        for p in TOP_ROW:
            red = put(red, p, (255, 0, 0) if p else (0, 255, 0))
        ppm(yofo_on, red)
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "overlays, ROI on"), "DIFF")
        png(self.dir / "studio/overlays/valid_000000_m0_roi0.png", put(PLAIN, 5, (0, 0, 0)))
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "overlays, ROI off"), "DIFF")
        (self.dir / "yofo/overlays/valid_000000_m0_roi1.ppm").unlink()
        self.assertEqual(result(self.rows(), "MIB Studio ↔ YOFO", "overlays, ROI on"), "DIFF")

    def test_unaccepted_studio_difference_fails(self):
        self.edit("studio/charts.json", lambda v: v.update(histogram=["3", "0", "6"] + ["0"] * 17))
        rows = self.rows()
        self.assertEqual(result(rows, "MIB Studio ↔ Qt", "histogram bins"), "DIFF")
        self.assertEqual(result(rows, "MIB Studio ↔ Qt", "histogram y range"), "DIFF")
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
