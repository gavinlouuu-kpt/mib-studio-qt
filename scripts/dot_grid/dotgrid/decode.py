"""Reference decoder: image -> absolute wafer pose. Mirrors the C++ DotGridDecoder."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import cv2
import numpy as np

from .codebook import Codebook, DIRECTION_TO_BITS, MNS_ORDER, MNS_PERIOD, WINDOW_SYMBOLS, WINDOW_DOTS

# The eight dihedral transforms of lattice indices (u, v) -> (u', v'): (a, b, c, d) with u' = a*u + b*v, v' = c*u + d*v.
DIHEDRAL = [(1, 0, 0, 1), (0, -1, 1, 0), (-1, 0, 0, -1), (0, 1, -1, 0),
            (-1, 0, 0, 1), (1, 0, 0, -1), (0, 1, 1, 0), (0, -1, -1, 0)]


@dataclass
class DecodeResult:
    ok: bool
    reason: str = ""
    centre_um: Tuple[float, float] = (0.0, 0.0)
    theta_deg: float = 0.0
    um_per_px: float = 0.0
    mirrored: bool = False
    votes: int = 0
    candidates: int = 0
    dots: int = 0
    chip: Optional[str] = None
    pixel_to_wafer: Optional[np.ndarray] = None  # 2x3
    lattice_px: Optional[np.ndarray] = None      # detected dot centroids
    residual_px: float = 0.0
    debug: Dict[str, object] = field(default_factory=dict)


# ---------------------------------------------------------------- blob detection
def detect_dots(gray: np.ndarray, expected_diameter_px: float) -> np.ndarray:
    """Return Nx2 float32 centroids of dark, roughly round blobs near the expected size."""
    if gray.ndim == 3:
        gray = cv2.cvtColor(gray, cv2.COLOR_BGR2GRAY)
    if gray.dtype != np.uint8:
        gray = cv2.normalize(gray, None, 0, 255, cv2.NORM_MINMAX).astype(np.uint8)
    k = int(max(3, expected_diameter_px * 6)) | 1
    bg = cv2.GaussianBlur(gray, (k, k), 0)
    diff = cv2.subtract(bg, gray)  # dark dots -> positive
    thr = max(8, int(diff.max() * 0.4)) if diff.max() > 0 else 8
    _, binary = cv2.threshold(diff, thr, 255, cv2.THRESH_BINARY)
    n, _, stats, cents = cv2.connectedComponentsWithStats(binary, connectivity=8)
    area_expected = np.pi * (expected_diameter_px / 2) ** 2
    out = []
    for idx in range(1, n):
        a = stats[idx, cv2.CC_STAT_AREA]
        w, h = stats[idx, cv2.CC_STAT_WIDTH], stats[idx, cv2.CC_STAT_HEIGHT]
        if a < area_expected * 0.3 or a > area_expected * 3.0:
            continue
        if max(w, h) > 1.8 * min(w, h):
            continue
        fill = a / float(w * h)
        if fill < 0.5:
            continue
        x0, y0 = stats[idx, cv2.CC_STAT_LEFT], stats[idx, cv2.CC_STAT_TOP]
        if x0 <= 0 or y0 <= 0 or x0 + w >= binary.shape[1] or y0 + h >= binary.shape[0]:
            continue  # clipped by the image border: centroid would be biased
        out.append(cents[idx])
    return np.asarray(out, np.float32).reshape(-1, 2)


# ---------------------------------------------------------------- lattice fit
def _estimate_basis(pts: np.ndarray) -> Optional[Tuple[np.ndarray, np.ndarray]]:
    """Estimate the lattice basis (a, b) in pixels, unbiased by the dot displacements.

    Nearest-neighbour distances are biased (displacements spread them by +-2d), so the
    pitch is taken as the mean projection of pair vectors that lie along each axis.
    """
    if len(pts) < 8:
        return None
    if len(pts) > 400:  # bound the O(N^2) pair search: keep the 400 dots nearest the cloud centre
        c = pts.mean(0)
        pts = pts[np.argsort(((pts - c) ** 2).sum(1))[:400]]
    diff = pts[:, None, :] - pts[None, :, :]
    d2 = (diff ** 2).sum(-1)
    np.fill_diagonal(d2, np.inf)
    nn = np.argsort(d2, axis=1)[:, :4]
    vecs = (pts[nn] - pts[:, None, :]).reshape(-1, 2)
    lens = np.linalg.norm(vecs, axis=1)
    p0 = np.median(lens)
    keep = (lens > 0.6 * p0) & (lens < 1.4 * p0)
    v = vecs[keep]
    ang = np.arctan2(v[:, 1], v[:, 0])
    theta = np.arctan2(np.sin(4 * ang).mean(), np.cos(4 * ang).mean()) / 4  # axis direction mod 90 deg
    ex = np.array([np.cos(theta), np.sin(theta)])
    ey = np.array([-np.sin(theta), np.cos(theta)])
    # all pair vectors with length ~ one pitch (both orientations)
    allv = diff.reshape(-1, 2)
    alll = np.sqrt(d2.reshape(-1))
    allv = allv[(alll > 0.6 * p0) & (alll < 1.4 * p0)]
    px = allv @ ex
    py = allv @ ey
    def axis_pitch(par, perp):
        sel = (np.abs(perp) < 0.35 * p0) & (np.abs(par) > 0.6 * p0)
        return float(np.abs(par[sel]).mean()) if sel.sum() >= 4 else p0
    pa = axis_pitch(px, py)
    pb = axis_pitch(py, px)
    return pa * ex, pb * ey


def fit_lattice(pts: np.ndarray, displacement_ratio: float, iterations: int = 4):
    """Assign integer lattice indices and per-dot direction to detected dots.

    Returns (affine 2x3 mapping (u,v)->px, indices Nx2 int, dirs Nx2 int, rms residual).
    """
    basis = _estimate_basis(pts)
    if basis is None:
        return None
    a, b = basis
    ref = pts[len(pts) // 2]
    B = np.stack([a, b], 1)
    Binv = np.linalg.inv(B)
    idx = np.round((pts - ref) @ Binv.T).astype(int)
    dirs = np.zeros_like(idx)
    M = None
    for _ in range(iterations):
        nominal_target = pts - (dirs @ B.T) * displacement_ratio  # remove assigned displacement
        X = np.hstack([idx.astype(float), np.ones((len(idx), 1))])
        M, *_ = np.linalg.lstsq(X, nominal_target, rcond=None)  # 3x2
        M = M.T  # 2x3
        B = M[:, :2]
        node = idx @ B.T + M[:, 2]
        res = pts - node
        # residual in lattice units
        rl = res @ np.linalg.inv(B).T / displacement_ratio
        dirs = np.zeros_like(idx)
        ax = np.abs(rl[:, 0]) >= np.abs(rl[:, 1])
        sx = np.where(rl[:, 0] >= 0, 1, -1)
        sy = np.where(rl[:, 1] >= 0, 1, -1)
        dirs[ax, 0] = sx[ax]
        dirs[~ax, 1] = sy[~ax]
        # re-index in case of drift
        idx = np.round((pts - M[:, 2]) @ np.linalg.inv(B).T - dirs * displacement_ratio).astype(int)
    node = idx @ M[:, :2].T + M[:, 2] + (dirs @ M[:, :2].T) * displacement_ratio
    rms = float(np.sqrt(((pts - node) ** 2).sum(1).mean()))
    return M, idx, dirs, rms


# ---------------------------------------------------------------- code decoding
def _transform_indices(idx: np.ndarray, dirs: np.ndarray, T) -> Tuple[np.ndarray, np.ndarray]:
    a, b, c, d = T
    R = np.array([[a, b], [c, d]])
    return idx @ R.T, dirs @ R.T


MAX_PHASE_CANDIDATES = 4


def _line_phases(cb: Codebook, bits: np.ndarray, along_axis: int) -> List[List[int]]:
    """For each line (column if along_axis==0) the phases q consistent with all known bits.

    q is defined so that bit at position s equals mns[(s + q) mod 63]. A line with fewer known
    bits may have several candidates; an inconsistent line (bit error) has none.
    """
    H, W = bits.shape
    n_lines = W if along_axis == 0 else H
    mns = np.asarray(cb.mns)
    out: List[List[int]] = []
    for k in range(n_lines):
        line = bits[:, k] if along_axis == 0 else bits[k, :]
        known = np.nonzero(line >= 0)[0]
        if len(known) < MNS_ORDER:
            out.append([])
            continue
        cands = []
        for q in range(MNS_PERIOD):
            if np.array_equal(mns[(known + q) % MNS_PERIOD], line[known]):
                cands.append(q)
        out.append(cands if len(cands) <= MAX_PHASE_CANDIDATES else [])
    return out


def _vote_axis(phases: List[List[int]], lookup, table_phase: List[int], n_lines_total: int) -> Dict[Tuple[int, int], int]:
    """Votes for (absolute index of line 0, cross-axis index-0 phase) from runs of WINDOW_DOTS lines."""
    import itertools
    votes: Dict[Tuple[int, int], int] = {}
    for k0 in range(0, len(phases) - WINDOW_DOTS + 1):
        run = phases[k0:k0 + WINDOW_DOTS]
        if any(len(p) == 0 for p in run):
            continue
        for combo in itertools.product(*run):
            deltas = [(combo[i + 1] - combo[i]) % MNS_PERIOD for i in range(WINDOW_SYMBOLS)]
            K0 = lookup(deltas)
            if K0 is None or K0 + WINDOW_DOTS > n_lines_total:
                continue
            cross0 = (combo[0] - table_phase[K0]) % MNS_PERIOD
            if any((combo[i] - table_phase[K0 + i]) % MNS_PERIOD != cross0 for i in range(1, WINDOW_DOTS)):
                continue
            key = (K0 - k0, cross0)
            votes[key] = votes.get(key, 0) + 1
    return votes


def decode_grid(cb: Codebook, idx: np.ndarray, dirs: np.ndarray):
    """Try all 8 dihedral transforms. Returns (T, (I0, J0), votes, tmin, candidates) for the best.

    I0/J0 are the absolute codebook indices of transformed lattice index (0, 0), i.e. after
    subtracting tmin from the transformed indices.
    """
    best = None
    total = 0
    runner_up = 0
    for T in DIHEDRAL:
        ti, td = _transform_indices(idx, dirs, T)
        tmin = ti.min(0)
        ti = ti - tmin
        W = int(ti[:, 0].max()) + 1
        H = int(ti[:, 1].max()) + 1
        if W < WINDOW_DOTS or H < MNS_ORDER:
            continue
        xb = -np.ones((H, W), int)
        yb = -np.ones((H, W), int)
        for (u, v), dvec in zip(ti, td):
            bits = DIRECTION_TO_BITS.get((int(dvec[0]), int(dvec[1])))
            if bits is None:
                continue
            xb[v, u], yb[v, u] = bits
        col_q = _line_phases(cb, xb, 0)   # per column: (J0 + phi[I]) mod 63
        row_q = _line_phases(cb, yb, 1)   # per row:    (I0 + psi[J]) mod 63
        col_votes = _vote_axis(col_q, cb.lookup_column, cb.phi, cb.columns)  # (I0, J0 mod 63)
        row_votes = _vote_axis(row_q, cb.lookup_row, cb.psi, cb.rows)         # (J0, I0 mod 63)
        combined: Dict[Tuple[int, int], int] = {}
        for (I0, j0m), nc in col_votes.items():
            for (J0, i0m), nr in row_votes.items():
                if I0 % MNS_PERIOD == i0m and J0 % MNS_PERIOD == j0m:
                    if 0 <= I0 < cb.columns and 0 <= J0 < cb.rows:
                        combined[(I0, J0)] = combined.get((I0, J0), 0) + nc + nr
        total += sum(combined.values())
        for key, n in combined.items():
            if best is None or n > best[2]:
                runner_up = best[2] if best is not None else 0
                best = (T, key, n, tmin)
            else:
                runner_up = max(runner_up, n)
    return best, total, runner_up


def decode_image(cb: Codebook, image: np.ndarray, um_per_px_hint: float, *, min_votes: int = 3) -> DecodeResult:
    """Decode a camera frame. um_per_px_hint is only used to size the blob detector."""
    diam_px = cb.dot_diameter_um / um_per_px_hint
    pts = detect_dots(image, diam_px)
    if len(pts) < WINDOW_DOTS * MNS_ORDER:
        return DecodeResult(False, f"too few dots ({len(pts)})", dots=len(pts))
    disp_ratio = cb.displacement_um / cb.pitch_um
    fit = fit_lattice(pts, disp_ratio)
    if fit is None:
        return DecodeResult(False, "lattice fit failed", dots=len(pts))
    M, idx, dirs, rms = fit
    best, cands, runner_up = decode_grid(cb, idx, dirs)
    if best is None or best[2] < min_votes:
        return DecodeResult(False, "no consistent code window", dots=len(pts), candidates=cands,
                            votes=0 if best is None else best[2], residual_px=rms, lattice_px=pts)
    if runner_up * 2 > best[2]:
        return DecodeResult(False, f"ambiguous ({best[2]} vs {runner_up} votes)", dots=len(pts),
                            candidates=cands, votes=best[2], residual_px=rms, lattice_px=pts)
    T, (I0, J0), n, tmin = best
    a, b, c, d = T
    R = np.array([[a, b], [c, d]], float)
    abs_idx = (idx @ R.T) - tmin + np.array([I0, J0])
    # Verify every dot's direction against the codebook; index drift on large grids shows up
    # here as a block of disagreeing dots. Only agreeing dots feed the final pose fit.
    tdirs = dirs @ R.T
    agree = np.zeros(len(idx), bool)
    for k, ((I, J), dv) in enumerate(zip(abs_idx.astype(int), tdirs)):
        if 0 <= I < cb.columns and 0 <= J < cb.rows:
            agree[k] = cb.direction(I, J) == (int(dv[0]), int(dv[1]))
    if agree.mean() < 0.9 or agree.sum() < 12:
        return DecodeResult(False, f"bit agreement {agree.mean():.2f} too low", dots=len(pts),
                            candidates=cands, votes=n, residual_px=rms, lattice_px=pts)
    node_um = (np.array(cb.origin_um) + cb.pitch_um * abs_idx)[agree]
    node_px = (idx @ M[:, :2].T + M[:, 2])[agree]
    X = np.hstack([node_px, np.ones((len(node_px), 1))])
    P, *_ = np.linalg.lstsq(X, node_um, rcond=None)
    P = P.T  # 2x3 pixel -> um
    A = P[:, :2]
    mirrored = bool(np.linalg.det(A) < 0)
    sx = float(np.linalg.norm(A[:, 0]))
    sgn = -1.0 if mirrored else 1.0
    theta = float(np.degrees(np.arctan2(sgn * A[1, 0], sgn * A[0, 0])))
    h, w = image.shape[:2]
    centre = A @ np.array([w / 2, h / 2]) + P[:, 2]
    chip = cb.chip_at(*centre)
    return DecodeResult(True, "", centre_um=(float(centre[0]), float(centre[1])), theta_deg=theta,
                        um_per_px=sx, mirrored=mirrored, votes=n, candidates=cands,
                        dots=len(pts), chip=chip.name if chip else None, pixel_to_wafer=P,
                        lattice_px=pts, residual_px=rms, debug={"agreement": float(agree.mean())})
