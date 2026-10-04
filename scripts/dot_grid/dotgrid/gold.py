"""Frozen gold references of a dot-grid codec contract (ADR 0010).

A codec contract fixes what a fabricated mask means, so every core that
claims the contract must reproduce the same references:

- **encode** - exact: the m-sequence and, for several seeds and lattice sizes,
  SHA-256 digests of the full phase arrays (``phi``/``psi`` joined as decimal
  text with commas), their first entries and probe bits. A core whose
  codebook differs by one symbol would mis-locate every chip ever made.
- **decode** - behavioural: synthetic views whose truth is the rendered pose
  (not a recorded decoder output), so each implementation renders with its
  own renderer. A ``decode`` case must decode within tolerance, a ``reject``
  case must never decode. A newer core may decode more, never less.

The file is ``scripts/dot_grid/gold/codec-contract<N>.json``. It changes only
in a PR labelled ``gold-reference-change`` (gold-reference-guard.yml). Check a
core with ``dotgrid_cli.py gold`` (Python reference) or CTest
``processing.dot_grid_codec_gold`` (C++ core).
"""
from __future__ import annotations

import hashlib
import math
from typing import Dict, List, Optional, Tuple

from .codebook import CODEC_CONTRACT, CODEC_LINES, generate_codebook

GOLD_SCHEMA_VERSION = 1

# Lattice the decode cases render from (Wafer_soRT geometry).
DECODE_CODEBOOK = {"seed": 7, "columns": 3700, "rows": 3700, "pitch_um": 30.0,
                   "dot_diameter_um": 12.0, "displacement_um": 5.0}

ENCODE_CASES = [
    {"seed": 7, "columns": 3700, "rows": 3700},
    {"seed": 1, "columns": 64, "rows": 64},
    {"seed": 8, "columns": 3501, "rows": 2900},
    {"seed": 1001, "columns": 1200, "rows": 1200},
    {"seed": 1099511627779, "columns": 2000, "rows": 2000},  # 2^40 + 3: 64-bit seed arithmetic
]
PROBES = [(0, 0), (1, 0), (0, 1), (62, 62), (100, 200), (63, 64), (1999, 1)]


def _pose(x, y, theta, um_per_px, mirrored):
    return {"centre_um": [x, y], "theta_deg": theta, "um_per_px": um_per_px, "mirrored": mirrored,
            "width": 1920, "height": 1200}


DECODE_CASES = [
    {"name": "20x-clean-r0", "pose": _pose(40000.0, 52000.0, 0.0, 0.293, False), "noise_seed": 0},
    {"name": "20x-clean-r37-mirror", "pose": _pose(41234.5, 51012.75, 37.5, 0.293, True), "noise_seed": 1},
    {"name": "20x-clean-r-120", "pose": _pose(42469.0, 50025.5, -120.0, 0.293, False), "noise_seed": 2},
    {"name": "20x-clean-r91-mirror", "pose": _pose(43703.5, 49038.25, 91.0, 0.293, True), "noise_seed": 3},
    {"name": "20x-clean-r179", "pose": _pose(44938.0, 48051.0, 179.0, 0.293, False), "noise_seed": 4},
    {"name": "20x-clean-r-45-mirror", "pose": _pose(60000.0, 30000.0, -45.0, 0.293, True), "noise_seed": 5},
    {"name": "10x-clean-r12-mirror", "pose": _pose(35000.0, 61000.0, 12.0, 0.586, True), "noise_seed": 6},
    {"name": "4x-clean-r-70", "pose": _pose(52000.0, 47000.0, -70.0, 1.465, False), "noise_seed": 7},
    {"name": "20x-channel-band", "pose": _pose(30000.0, 30000.0, 2.0, 0.293, False), "noise_seed": 8,
     "channel": {"from_um": [27000.0, 30040.0], "to_um": [33000.0, 30040.0], "width_um": 30.0,
                 "band_um": 71.0}},
    {"name": "10x-dropout-10pct", "pose": _pose(47000.0, 39000.0, 63.0, 0.586, True), "noise_seed": 9,
     "missing_fraction": 0.10},
    {"name": "blank-frame", "pose": _pose(40000.0, 52000.0, 0.0, 0.293, False), "blank": True,
     "expect": "reject"},
    {"name": "foreign-seed-99", "pose": _pose(40000.0, 52000.0, 25.0, 0.293, True), "noise_seed": 10,
     "pattern_seed": 99, "expect": "reject"},
]


def phases_digest(phases) -> str:
    return hashlib.sha256(",".join(str(int(v)) for v in phases).encode("ascii")).hexdigest()


def build_reference() -> dict:
    """The gold document, computed by the Python reference core of this contract."""
    encode = []
    mns = None
    for case in ENCODE_CASES:
        cb = generate_codebook(case["seed"], case["columns"], case["rows"], pitch_um=30.0,
                               dot_diameter_um=12.0, displacement_um=5.0)
        mns = "".join(map(str, cb.mns))
        probes = [{"i": i, "j": j, "bits": list(cb.bits(i, j))}
                  for (i, j) in PROBES if i < cb.columns and j < cb.rows]
        encode.append({**case, "phi_sha256": phases_digest(cb.phi), "psi_sha256": phases_digest(cb.psi),
                       "phi_head": cb.phi[:12], "psi_head": cb.psi[:12],
                       "phi_last": cb.phi[-1], "psi_last": cb.psi[-1], "probes": probes})
    cases = []
    for c in DECODE_CASES:
        c = dict(c)
        c.setdefault("expect", "decode")
        if c["expect"] == "decode":
            fine = c["pose"]["um_per_px"] < 0.4
            c.setdefault("tolerance", {"position_um": 1.0 if fine else 2.0, "theta_deg": 0.3,
                                       "um_per_px_rel": 0.01})
        cases.append(c)
    return {
        "gold_schema_version": GOLD_SCHEMA_VERSION,
        "codec_contract": CODEC_CONTRACT,
        "line": CODEC_LINES[CODEC_CONTRACT],
        "encode": {"pitch_um": 30.0, "dot_diameter_um": 12.0, "displacement_um": 5.0,
                   "phase_digest": "sha256 of the decimal phases joined with ','",
                   "mns": mns, "codebooks": encode},
        "decode": {"codebook": DECODE_CODEBOOK, "um_per_px_hint_factor": 1.06, "cases": cases},
    }


def render_case(case: dict, cb_for_seed):
    """Render one decode case with the Python reference renderer."""
    import numpy as np
    from .render import ViewPose, render_view
    p = case["pose"]
    pose = ViewPose(centre_um=tuple(p["centre_um"]), theta_deg=p["theta_deg"], um_per_px=p["um_per_px"],
                    mirrored=p["mirrored"], width=p["width"], height=p["height"])
    if case.get("blank"):
        return np.full((pose.height, pose.width), 180, np.uint8), pose
    cb = cb_for_seed(case.get("pattern_seed", DECODE_CODEBOOK["seed"]))
    kwargs = {"seed": case.get("noise_seed", 0)}
    ch = case.get("channel")
    if ch:
        a, b = np.array(ch["from_um"]), np.array(ch["to_um"])
        d = (b - a) / np.linalg.norm(b - a)
        n = np.array([-d[1], d[0]])
        kwargs["channel_lines_um"] = [(tuple(a), tuple(b), ch["width_um"])]
        kwargs["keepout_mask"] = lambda x, y: abs((np.array([x, y]) - a) @ n) >= ch["band_um"]
    frac = case.get("missing_fraction", 0.0)
    if frac > 0:
        rng = np.random.default_rng(case.get("noise_seed", 0) + 1000)
        i0 = int((p["centre_um"][0] - 1000) / cb.pitch_um)
        j0 = int((p["centre_um"][1] - 1000) / cb.pitch_um)
        kwargs["missing"] = [(i, j) for i in range(i0, i0 + 70) for j in range(j0, j0 + 70) if rng.random() < frac]
    return render_view(cb, pose, **kwargs), pose


def check_encode(gold: dict, generate) -> List[str]:
    """generate(seed, columns, rows) -> object with mns, phi, psi, bits(i, j)."""
    problems = []
    for case in gold["encode"]["codebooks"]:
        cb = generate(case["seed"], case["columns"], case["rows"])
        tag = f"seed {case['seed']} {case['columns']}x{case['rows']}"
        if "".join(map(str, cb.mns)) != gold["encode"]["mns"]:
            problems.append(f"{tag}: m-sequence differs")
        if phases_digest(cb.phi) != case["phi_sha256"] or list(cb.phi[:12]) != case["phi_head"]:
            problems.append(f"{tag}: phi differs")
        if phases_digest(cb.psi) != case["psi_sha256"] or list(cb.psi[:12]) != case["psi_head"]:
            problems.append(f"{tag}: psi differs")
        for pr in case["probes"]:
            if list(cb.bits(pr["i"], pr["j"])) != pr["bits"]:
                problems.append(f"{tag}: bits at ({pr['i']},{pr['j']}) differ")
    return problems


def check_decode_result(case: dict, ok: bool, centre: Optional[Tuple[float, float]], theta: float,
                        um_per_px: float, mirrored: bool) -> Optional[str]:
    """None when the result meets the case, else a description."""
    name = case["name"]
    if case.get("expect", "decode") == "reject":
        return f"{name}: must not decode" if ok else None
    if not ok:
        return f"{name}: did not decode"
    tol = case["tolerance"]
    p = case["pose"]
    err = math.hypot(centre[0] - p["centre_um"][0], centre[1] - p["centre_um"][1])
    dtheta = abs((theta - p["theta_deg"] + 180.0) % 360.0 - 180.0)
    if err > tol["position_um"]:
        return f"{name}: position off by {err:.3f} um"
    if dtheta > tol["theta_deg"]:
        return f"{name}: rotation off by {dtheta:.3f} deg"
    if abs(um_per_px - p["um_per_px"]) > tol["um_per_px_rel"] * p["um_per_px"]:
        return f"{name}: scale {um_per_px:.5f} vs {p['um_per_px']}"
    if mirrored != p["mirrored"]:
        return f"{name}: mirror flag wrong"
    return None


def check_reference_decode(gold: dict, progress=None) -> List[str]:
    """Run the decode cases through the Python reference decoder."""
    from .decode import decode_image
    dc = gold["decode"]["codebook"]
    cache: Dict[int, object] = {}

    def cb_for_seed(seed):
        if seed not in cache:
            cache[seed] = generate_codebook(seed, dc["columns"], dc["rows"], pitch_um=dc["pitch_um"],
                                            dot_diameter_um=dc["dot_diameter_um"],
                                            displacement_um=dc["displacement_um"])
        return cache[seed]

    gold_cb = cb_for_seed(dc["seed"])
    problems = []
    for case in gold["decode"]["cases"]:
        img, pose = render_case(case, cb_for_seed)
        hint = case["pose"]["um_per_px"] * gold["decode"]["um_per_px_hint_factor"]
        r = decode_image(gold_cb, img, hint)
        msg = check_decode_result(case, r.ok, r.centre_um, r.theta_deg, r.um_per_px, r.mirrored)
        if progress:
            progress(f"{case['name']}: {'ok' if msg is None else msg}")
        if msg:
            problems.append(msg)
    return problems
