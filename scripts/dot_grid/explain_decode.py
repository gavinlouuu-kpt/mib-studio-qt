"""Generate a step-by-step visual explanation of the dot-grid decoder.

Runs the real reference decoder (dotgrid.decode) on a synthetic 20x frame and
saves one annotated PNG per pipeline stage under /tmp/decode_steps/, using the
actual numbers the decoder computed (no fabricated illustration values).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Circle
from matplotlib.lines import Line2D

from dotgrid import Codebook, render_view
from dotgrid.render import ViewPose
from dotgrid.codebook import DIRECTIONS, DIRECTION_TO_BITS, MNS_ORDER, MNS_PERIOD, WINDOW_SYMBOLS, WINDOW_DOTS
from dotgrid.decode import detect_dots, fit_lattice, decode_grid, DIHEDRAL, _transform_indices

OUT = "/tmp/decode_steps"
os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- setup: a clean, legible synthetic frame
cb = Codebook.load("/mnt/hdd/shared/exports/dot_grid/codebook.json")

# Small image, low um/px so pitch is ~60 px on screen -> individual dots and
# their displacement are clearly visible (a real 20x frame is 0.293 um/px;
# here we use 0.5 um/px over a smaller sensor so ~15x10 dots fill the frame).
pose = ViewPose(centre_um=(50000.0, 60000.0), theta_deg=0.0, um_per_px=0.5, mirrored=False,
                width=1000, height=700)
img = render_view(cb, pose, seed=3, noise_sigma=5.0, blur_sigma_px=1.2)

diam_px = cb.dot_diameter_um / pose.um_per_px
disp_px = cb.displacement_um / pose.um_per_px
disp_ratio = cb.displacement_um / cb.pitch_um

DIR_COLOR = {(1, 0): "#e6194B", (-1, 0): "#4363d8", (0, 1): "#3cb44b", (0, -1): "#f58231"}
DIR_LABEL = {(1, 0): "+x  (bits 0,0)", (-1, 0): "-x  (bits 1,0)", (0, 1): "+y  (bits 0,1)", (0, -1): "-y  (bits 1,1)"}


def new_fig(title):
    fig, ax = plt.subplots(figsize=(11, 7.7), dpi=140)
    ax.imshow(img, cmap="gray", vmin=0, vmax=255)
    ax.set_xlim(0, img.shape[1])
    ax.set_ylim(img.shape[0], 0)
    ax.set_axis_off()
    ax.set_title(title, fontsize=13, loc="left", pad=10)
    return fig, ax


# ================================================================== STEP 1: raw frame
fig, ax = new_fig("Step 1 — Raw camera frame\nA 20x field of view sees only a small patch of dots: no position is obvious yet.")
fig.savefig(f"{OUT}/1_raw_frame.png", bbox_inches="tight")
plt.close(fig)

# ================================================================== STEP 2: blob detection
pts = detect_dots(img, diam_px)
fig, ax = new_fig(f"Step 2 — Blob detection\nBackground-subtract + threshold + connected components -> {len(pts)} dot centroids (red).")
ax.scatter(pts[:, 0], pts[:, 1], s=70, facecolors="none", edgecolors="#e6194B", linewidths=1.4)
fig.savefig(f"{OUT}/2_blob_detection.png", bbox_inches="tight")
plt.close(fig)

# ================================================================== STEP 3: lattice fit
M, idx, dirs, rms = fit_lattice(pts, disp_ratio)
# nominal (undisplaced) node grid from the fitted affine
i0, i1 = idx[:, 0].min(), idx[:, 0].max()
j0, j1 = idx[:, 1].min(), idx[:, 1].max()
fig, ax = new_fig(f"Step 3 — Fit one lattice to every dot\nPitch + orientation estimated from nearest-neighbour vectors (unbiased by the dot offsets); rms fit residual = {rms:.2f} px.")
for i in range(i0, i1 + 1):
    p0 = M[:, :2] @ [i, j0] + M[:, 2]
    p1 = M[:, :2] @ [i, j1] + M[:, 2]
    ax.plot([p0[0], p1[0]], [p0[1], p1[1]], color="#42d4f4", lw=0.8, alpha=0.8)
for j in range(j0, j1 + 1):
    p0 = M[:, :2] @ [i0, j] + M[:, 2]
    p1 = M[:, :2] @ [i1, j] + M[:, 2]
    ax.plot([p0[0], p1[0]], [p0[1], p1[1]], color="#42d4f4", lw=0.8, alpha=0.8)
origin_px = M[:, :2] @ [0, 0] + M[:, 2]
ex_px = M[:, :2] @ [1, 0] + M[:, 2]
ey_px = M[:, :2] @ [0, 1] + M[:, 2]
ax.add_patch(FancyArrowPatch(origin_px, ex_px, color="yellow", arrowstyle="-|>", mutation_scale=18, lw=2))
ax.add_patch(FancyArrowPatch(origin_px, ey_px, color="yellow", arrowstyle="-|>", mutation_scale=18, lw=2))
ax.text(*(ex_px + (5, -5)), "lattice +u\n(pitch %.1f px)" % np.hypot(*(ex_px - origin_px)), color="yellow", fontsize=9)
ax.text(*(ey_px + (5, 10)), "lattice +v", color="yellow", fontsize=9)
fig.savefig(f"{OUT}/3_lattice_fit.png", bbox_inches="tight")
plt.close(fig)

# ================================================================== STEP 4: direction / bit classification
fig, ax = new_fig("Step 4 — Every dot's displacement encodes 2 bits\nEach dot sits pitch/6 off its lattice node, in one of 4 directions -> (x-bit, y-bit).")
node_px = idx @ M[:, :2].T + M[:, 2]
for k in range(len(pts)):
    d = tuple(dirs[k])
    col = DIR_COLOR[d]
    ax.add_patch(Circle(pts[k], diam_px / 2 * 0.55, fill=False, edgecolor=col, linewidth=1.6))
    ax.add_patch(FancyArrowPatch(node_px[k], pts[k], color=col, arrowstyle="-|>", mutation_scale=8, lw=1.2))
legend = [Line2D([0], [0], marker="o", color="w", markerfacecolor="none", markeredgecolor=c, markeredgewidth=2,
                 markersize=10, label=DIR_LABEL[d]) for d, c in DIR_COLOR.items()]
ax.legend(handles=legend, loc="lower right", fontsize=9, framealpha=0.85)
fig.savefig(f"{OUT}/4_direction_bits.png", bbox_inches="tight")
plt.close(fig)

# ================================================================== STEP 5: phase / codebook lookup (pick the winning transform)
best, total, runner_up = decode_grid(cb, idx, dirs)
T, (I0, J0), votes, tmin = best
ti, td = _transform_indices(idx, dirs, T)
ti = ti - ti.min(0)
W = ti[:, 0].max() + 1
H = ti[:, 1].max() + 1
xb = -np.ones((H, W), int)
yb = -np.ones((H, W), int)
px_at = {}
for k, ((u, v), dv) in enumerate(zip(ti, td)):
    bits = DIRECTION_TO_BITS.get((int(dv[0]), int(dv[1])))
    if bits is None:
        continue
    xb[v, u], yb[v, u] = bits
    px_at[(u, v)] = pts[k]

# find a column with >= MNS_ORDER consecutive known x-bits, near the middle
chosen = None
for u in range(W):
    col = xb[:, u]
    known_rows = [v for v in range(H) if col[v] >= 0]
    for s in range(len(known_rows) - MNS_ORDER + 1):
        run = known_rows[s:s + MNS_ORDER]
        if run == list(range(run[0], run[0] + MNS_ORDER)):
            chosen = (u, run)
            break
    if chosen:
        break
u_col, rows_run = chosen
bit_seq = [int(xb[v, u_col]) for v in rows_run]
q = cb.mns_phase(bit_seq)

fig, ax = new_fig(f"Step 5 — Column {u_col}: six known bits pin the phase\nbits {''.join(map(str,bit_seq))} match the 63-bit sequence at exactly one place -> phase q = {q}")
for v in rows_run:
    p = px_at.get((u_col, v))
    if p is not None:
        ax.add_patch(Circle(p, diam_px / 2 * 0.7, fill=False, edgecolor="#42d4f4", linewidth=2.2))
xs = [px_at[(u_col, v)][0] for v in rows_run if (u_col, v) in px_at]
ys = [px_at[(u_col, v)][1] for v in rows_run if (u_col, v) in px_at]
if xs:
    ax.plot(xs, ys, color="#42d4f4", lw=1.5, ls="--", zorder=0)
    for v, (x, y) in zip(rows_run, zip(xs, ys)):
        ax.annotate(str(xb[v, u_col]), (x, y), xytext=(x + diam_px * 0.6, y), fontsize=11, color="#42d4f4", weight="bold")
fig.savefig(f"{OUT}/5a_column_phase.png", bbox_inches="tight")
plt.close(fig)

# three consecutive columns -> codebook lookup for the absolute column index
cols_for_lookup = list(range(max(0, u_col - 1), max(0, u_col - 1) + WINDOW_DOTS))
phases = []
for u in cols_for_lookup:
    col = xb[:, u]
    known_rows = [v for v in range(H) if col[v] >= 0]
    ph = None
    for s in range(len(known_rows) - MNS_ORDER + 1):
        run = known_rows[s:s + MNS_ORDER]
        if run == list(range(run[0], run[0] + MNS_ORDER)):
            seq = [int(col[v]) for v in run]
            p = cb.mns_phase(seq)
            if p is not None:
                ph = (p - run[0]) % MNS_PERIOD  # phase referenced to row 0
                break
    phases.append(ph)
deltas = [(phases[i + 1] - phases[i]) % MNS_PERIOD for i in range(WINDOW_SYMBOLS)]
I0_lookup = cb.lookup_column(deltas)

fig, ax = plt.subplots(figsize=(11, 4.2), dpi=140)
ax.set_axis_off()
ax.set_title(f"Step 5b — Three neighbouring columns -> absolute column index\ncolumn phases (mod 63) = {phases}   ->   deltas = {deltas}   ->   codebook lookup: column {I0_lookup} is the only place these deltas occur", fontsize=12, loc="left")
y0 = 0.5
for n, (u, ph) in enumerate(zip(cols_for_lookup, phases)):
    x = 0.08 + n * 0.30
    ax.add_patch(plt.Circle((x, y0), 0.09, fill=False, edgecolor="#4363d8", lw=2))
    ax.text(x, y0, f"col u={u}\nphase={ph}", ha="center", va="center", fontsize=10)
    if n > 0:
        ax.annotate("", xy=(x - 0.09, y0), xytext=(x - 0.21, y0), arrowprops=dict(arrowstyle="->", lw=1.5))
        ax.text(x - 0.155, y0 + 0.13, f"delta={deltas[n-1]}", ha="center", fontsize=9)
ax.text(0.5, 0.08, f"Lookup table has exactly one entry for deltas {deltas}: absolute column i0 = {I0_lookup}\n(the codebook was built so every 2-delta window over 3,700 columns is unique)",
        ha="center", fontsize=10, style="italic")
ax.set_xlim(0, 1)
ax.set_ylim(0, 1)
fig.savefig(f"{OUT}/5b_codebook_lookup.png", bbox_inches="tight")
plt.close(fig)

# ================================================================== STEP 6: verify + final pose
a, b, c, d = T
R = np.array([[a, b], [c, d]])
abs_idx = (idx @ R.T) - tmin + np.array([I0, J0])
agree = np.zeros(len(idx), bool)
for k, (ij, dv) in enumerate(zip(abs_idx.astype(int), (dirs @ R.T).astype(int))):
    i, j = ij
    if 0 <= i < cb.columns and 0 <= j < cb.rows:
        agree[k] = cb.direction(i, j) == tuple(dv)

from dotgrid import decode_image
result = decode_image(cb, img, pose.um_per_px * 1.1)

fig, ax = new_fig("Step 6 — Verify every dot, then solve the final pose\nGreen = dot's direction matches the codebook at its decoded index; that agreement fits pixel -> wafer.")
ax.scatter(pts[agree, 0], pts[agree, 1], s=40, facecolors="none", edgecolors="#3cb44b", linewidths=1.8)
ax.scatter(pts[~agree, 0], pts[~agree, 1], s=40, facecolors="none", edgecolors="#e6194B", linewidths=1.8)
cx, cy = img.shape[1] / 2, img.shape[0] / 2
ax.plot([cx - 22, cx + 22], [cy, cy], color="#00e5ff", lw=2.5)
ax.plot([cx, cx], [cy - 22, cy + 22], color="#00e5ff", lw=2.5)
info = (f"Wafer X = {result.centre_um[0]:.1f} um   Y = {result.centre_um[1]:.1f} um\n"
        f"theta = {result.theta_deg:.2f} deg   scale = {result.um_per_px:.4f} um/px   mirrored = {result.mirrored}\n"
        f"chip = {result.chip}   agreement = {agree.mean()*100:.0f}%   votes = {result.votes}   dots = {result.dots}\n"
        f"(true pose was X={pose.centre_um[0]:.1f} Y={pose.centre_um[1]:.1f} theta={pose.theta_deg:.1f})")
ax.text(0.02, 0.02, info, transform=ax.transAxes, fontsize=10, color="white",
        bbox=dict(facecolor="black", alpha=0.65, boxstyle="round"), va="bottom")
fig.savefig(f"{OUT}/6_verify_and_pose.png", bbox_inches="tight")
plt.close(fig)

print("agreement:", agree.mean(), "n_agree:", agree.sum(), "/", len(agree))
print("decode result ok:", result.ok, result.reason)
print("centre:", result.centre_um, "theta:", result.theta_deg, "chip:", result.chip)
print("files:", sorted(os.listdir(OUT)))
