"""Generate the dot-grid mask layer for a wafer design (DXF in, DXF + codebook out)."""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Callable, List, Optional, Sequence, Tuple

import numpy as np

from .codebook import Chip, Codebook, generate_codebook


@dataclass
class KeepOut:
    channel_um: float = 60.0     # distance from any channel edge
    dicing_um: float = 200.0     # distance from chip dicing outlines
    wafer_edge_um: float = 3000.0
    existing_marks_um: float = 300.0


def _sample_entity_points(msp, step_um: float = 10.0) -> Tuple[np.ndarray, List[Tuple[float, float, float, float]], List[np.ndarray]]:
    """Sample channel geometry to points. Returns (points, chip rectangles, mark polylines)."""
    pts: List[np.ndarray] = []
    chips: List[Tuple[float, float, float, float]] = []
    marks: List[np.ndarray] = []
    def xy(v):
        return np.array([v.x, v.y], float)
    for e in msp.query("LINE"):
        a, b = xy(e.dxf.start), xy(e.dxf.end)
        n = max(2, int(np.linalg.norm(b - a) / step_um) + 1)
        pts.append(np.linspace(a, b, n))
    for e in msp.query("ARC"):
        r, c = e.dxf.radius, xy(e.dxf.center)
        a0, a1 = math.radians(e.dxf.start_angle), math.radians(e.dxf.end_angle)
        if a1 < a0:
            a1 += 2 * math.pi
        t = np.linspace(a0, a1, max(3, int(r * (a1 - a0) / step_um) + 1))
        pts.append(c + r * np.stack([np.cos(t), np.sin(t)], 1))
    for e in msp.query("CIRCLE"):
        r, c = e.dxf.radius, xy(e.dxf.center)
        if r >= 10000:
            continue  # wafer outline
        t = np.linspace(0, 2 * math.pi, max(8, int(2 * math.pi * r / step_um)))
        pts.append(c + r * np.stack([np.cos(t), np.sin(t)], 1))
        # fill the interior so ports/reservoirs never get dots
        if r > 100:
            rr = np.arange(step_um, r, step_um)
            for radius in rr:
                tt = np.linspace(0, 2 * math.pi, max(8, int(2 * math.pi * radius / step_um)), endpoint=False)
                pts.append(c + radius * np.stack([np.cos(tt), np.sin(tt)], 1))
    for e in msp.query("LWPOLYLINE"):
        p = np.array(e.get_points("xy"), float)
        w, h = np.ptp(p[:, 0]), np.ptp(p[:, 1])
        if len(p) == 4 and w > 5000 and h > 5000:
            chips.append((p[:, 0].min(), p[:, 1].min(), p[:, 0].max(), p[:, 1].max()))
        elif w < 5000:
            marks.append(p)
        closed = e.closed or len(p) == 4
        seg = np.vstack([p, p[:1]]) if closed else p
        for a, b in zip(seg[:-1], seg[1:]):
            n = max(2, int(np.linalg.norm(b - a) / step_um) + 1)
            pts.append(np.linspace(a, b, n))
    return np.vstack(pts) if pts else np.zeros((0, 2)), chips, marks


def load_design(dxf_path: str, step_um: float = 10.0):
    import ezdxf  # optional dependency, only needed for pattern generation
    doc = ezdxf.readfile(dxf_path)
    msp = doc.modelspace()
    wafer = None
    for e in msp.query("CIRCLE"):
        if e.dxf.radius >= 10000 and (wafer is None or e.dxf.radius < wafer[2]):
            wafer = (e.dxf.center.x, e.dxf.center.y, e.dxf.radius)
    pts, chips, marks = _sample_entity_points(msp, step_um)
    return doc, pts, chips, marks, wafer


def chip_table(chips: Sequence[Tuple[float, float, float, float]]) -> List[Chip]:
    """Name chips by row/column from the inner (smaller) outlines, e.g. R3C2."""
    inner = sorted(chips, key=lambda c: (c[3] - c[1]) * (c[2] - c[0]))
    if not inner:
        return []
    min_area = (inner[0][3] - inner[0][1]) * (inner[0][2] - inner[0][0])
    inner = [c for c in inner if (c[3] - c[1]) * (c[2] - c[0]) < 1.05 * min_area]
    ys = sorted({round(c[1], -2) for c in inner})
    xs = sorted({round(c[0], -2) for c in inner})
    out = []
    for c in inner:
        r = ys.index(round(c[1], -2))
        col = xs.index(round(c[0], -2))
        out.append(Chip(f"R{r}C{col}", c[0], c[1], c[2], c[3]))
    return sorted(out, key=lambda c: c.name)


def build_pattern(cb: Codebook, geometry_pts: np.ndarray, chips, marks, wafer, keepout: KeepOut,
                  progress: Optional[Callable[[str], None]] = None) -> np.ndarray:
    """Return Nx4 array [i, j, x_um, y_um] of dots that survive the keep-out rules."""
    from scipy.spatial import cKDTree
    tree = cKDTree(geometry_pts) if len(geometry_pts) else None
    mark_pts = np.vstack(marks) if marks else np.zeros((0, 2))
    mark_tree = cKDTree(mark_pts) if len(mark_pts) else None
    ii, jj = np.meshgrid(np.arange(cb.columns), np.arange(cb.rows), indexing="ij")
    ii = ii.ravel(); jj = jj.ravel()
    x = cb.origin_um[0] + ii * cb.pitch_um
    y = cb.origin_um[1] + jj * cb.pitch_um
    keep = np.ones(len(ii), bool)
    if wafer is not None:
        cx, cy, r = wafer
        keep &= np.hypot(x - cx, y - cy) < r - keepout.wafer_edge_um
    if progress: progress(f"{keep.sum()} nodes inside wafer")
    if tree is not None:
        d, _ = tree.query(np.stack([x[keep], y[keep]], 1), distance_upper_bound=keepout.channel_um + cb.displacement_um + cb.dot_diameter_um)
        sub = keep.copy(); sub[keep] = d > keepout.channel_um + cb.displacement_um + cb.dot_diameter_um / 2
        keep = sub
    if progress: progress(f"{keep.sum()} nodes after channel keep-out")
    for (x0, y0, x1, y1) in chips:
        m = keepout.dicing_um
        near = ((np.abs(x - x0) < m) | (np.abs(x - x1) < m)) & (y > y0 - m) & (y < y1 + m)
        near |= ((np.abs(y - y0) < m) | (np.abs(y - y1) < m)) & (x > x0 - m) & (x < x1 + m)
        keep &= ~near
    if mark_tree is not None:
        d, _ = mark_tree.query(np.stack([x, y], 1), distance_upper_bound=keepout.existing_marks_um)
        keep &= d > keepout.existing_marks_um
    if progress: progress(f"{keep.sum()} nodes after dicing/mark keep-out")
    sel = np.nonzero(keep)[0]
    dots = np.zeros((len(sel), 4))
    for n, k in enumerate(sel):
        i, j = int(ii[k]), int(jj[k])
        dx, dy = cb.dot_um(i, j)
        dots[n] = (i, j, dx, dy)
    return dots


def write_dxf(doc, dots: np.ndarray, cb: Codebook, out_path: str, layer: str = "DOTGRID") -> None:
    """Add dots as circles on a new layer of the design and save a copy."""
    if layer not in doc.layers:
        doc.layers.add(layer, color=1)
    msp = doc.modelspace()
    r = cb.dot_diameter_um / 2
    for _, _, x, y in dots:
        msp.add_circle((x, y), r, dxfattribs={"layer": layer})
    doc.saveas(out_path)


def write_dots_only_dxf(dots: np.ndarray, cb: Codebook, out_path: str, layer: str = "DOTGRID") -> None:
    import ezdxf
    doc = ezdxf.new("R2010")
    doc.header["$INSUNITS"] = 13  # micrometres
    doc.layers.add(layer, color=1)
    msp = doc.modelspace()
    r = cb.dot_diameter_um / 2
    for _, _, x, y in dots:
        msp.add_circle((x, y), r, dxfattribs={"layer": layer})
    doc.saveas(out_path)


def write_gds(dots: np.ndarray, cb: Codebook, out_path: str, layer: int = 10) -> bool:
    """Write a GDSII with one dot cell referenced per dot (compact). Returns False if gdstk is missing."""
    try:
        import gdstk
    except ImportError:
        return False
    lib = gdstk.Library(unit=1e-6, precision=1e-9)
    dot = lib.new_cell("DOT")
    dot.add(gdstk.ellipse((0, 0), cb.dot_diameter_um / 2, layer=layer, tolerance=0.05))
    top = lib.new_cell("DOTGRID")
    for _, _, x, y in dots:
        top.add(gdstk.Reference(dot, (float(x), float(y))))
    lib.write_gds(out_path)
    return True


def generate_for_design(dxf_path: str, seed: int, *, pitch_um: float = 50.0, dot_diameter_um: float = 12.0,
                        displacement_um: float = 8.0, keepout: Optional[KeepOut] = None,
                        design_name: str = "", design_scale: float = 1.0,
                        progress: Optional[Callable[[str], None]] = None):
    """Full pipeline: design DXF -> (codebook, dots array, design doc)."""
    keepout = keepout or KeepOut()
    doc, pts, chips, marks, wafer = load_design(dxf_path)
    if wafer is not None:
        x_max, y_max = wafer[0] + wafer[2], wafer[1] + wafer[2]
    else:
        ext = doc.header.get("$EXTMAX", (110000.0, 110000.0, 0.0))
        x_max, y_max = ext[0], ext[1]
    columns = int(math.ceil(x_max / pitch_um)) + 1
    rows = int(math.ceil(y_max / pitch_um)) + 1
    cb = generate_codebook(seed, columns, rows, pitch_um=pitch_um, dot_diameter_um=dot_diameter_um,
                           displacement_um=displacement_um, origin_um=(0.0, 0.0), chips=chip_table(chips),
                           design_name=design_name, design_scale=design_scale)
    dots = build_pattern(cb, pts, chips, marks, wafer, keepout, progress)
    return cb, dots, doc
