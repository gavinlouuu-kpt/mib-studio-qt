# Dot-grid wafer localization

Absolute localization of the camera field of view on a microfluidic wafer (or
a PDMS chip moulded from it) from a fiducial pattern fabricated on the chip
itself, in the style of the dot patterns on smart-pen notebooks. The camera
sees a small patch of dots; the decoder returns the wafer coordinates of the
image centre, the rotation, the measured scale, whether the view is mirrored
(chip viewed through the glass side), and which chip the view is on.

| Piece | Where |
|---|---|
| Codebook (pattern definition, shared with the mask generator) | `include/backend/processing/DotGridCodebook.h`, `src/backend/processing/DotGridCodebook.cpp` |
| Decoder + synthetic renderer (Qt-free, OpenCV) | `include/backend/processing/DotGridDecoder.h`, `src/backend/processing/DotGridDecoder.cpp` |
| Live service (samples FrameStore, publishes poses) | `include/backend/services/DotGridService.h`, `src/backend/services/DotGridService.cpp` — [vault note](../../knowledge_map/services/DotGridService.md) |
| Preview overlay + "Wafer Grid" toggle | `src/frontend/system/PlaybackPanel.cpp` |
| Mask generator, reference decoder, simulator (Python) | `scripts/dot_grid/` — see [howto/dot-grid-mask-generation.md](../howto/dot-grid-mask-generation.md) |
| Shipped codebook for the Wafer_soRT design | `resources/defaults/dot_grid/wafer_soRT_2025-03-16_seed7_p30.json` |
| Tests | `processing.dot_grid_codebook`, `processing.dot_grid_decoder`, `backend.dot_grid_service`, `scripts.dot_grid_reference` |
| Decision record | [ADR 0006](../decisions/0006-dot-grid-localization.md) |

## Pattern

A square lattice with pitch `P` covers the whole wafer in the mask coordinate
frame (origin = DXF origin, micrometres). Node `(i, j)` sits at
`origin + (i·P, j·P)`. Every node carries one dot of diameter `D`, displaced
by `S` from the node in one of four directions. The direction encodes two bits:

| x-bit | y-bit | displacement |
|---|---|---|
| 0 | 0 | +x |
| 1 | 0 | −x |
| 0 | 1 | +y |
| 1 | 1 | −y |

The two bit planes are separable codes:

- **x-plane** — column `i` holds the period-63 m-sequence `MNS`
  (LFSR `x⁶ + x⁵ + 1`) cyclically shifted by `phi[i]`:
  `xbit(i, j) = MNS[(j + phi[i]) mod 63]`.
- **y-plane** — row `j` holds the same sequence shifted by `psi[j]`:
  `ybit(i, j) = MNS[(i + psi[j]) mod 63]`.

`phi` and `psi` are cumulative sums of *delta* sequences over the alphabet
`0..62`. The deltas come from SplitMix64 hashes of `(seed, axis, index,
attempt)`, with rejection so that every window of two consecutive deltas is
unique along the axis. Consequently:

- any six known dots in one column pin its phase `q_i = phi[i] + J0 (mod 63)`
  (`J0` = absolute row of the first visible row), because every non-zero
  6-bit window of an m-sequence is unique; fewer dots leave ≤ 4 candidates;
- three consecutive columns with known phase give two deltas, which the
  codebook lookup maps to the absolute column `i`; the same holds for rows.

So a patch of roughly 6 × 3 dots decodes each axis, and a 6 × 6 patch decodes
both. Cross-checks make false decodes vanishingly rare: the column window
also yields `J0 mod 63`, which must agree with the row decode, and vice
versa; every additional line in a window must reproduce the same cross
phase. The codebook is fully determined by `(seed, columns, rows, P, D, S,
origin)` and is regenerated identically by the C++ and Python
implementations (`processing.dot_grid_codebook` pins golden values). The
delta sequence is prefix-stable, so a codebook with more columns/rows is a
superset of a smaller one.

### Parameters chosen for Wafer_soRT (DC sorting chip, 30 µm channels)

| Parameter | Value | Why |
|---|---|---|
| Pitch `P` | 30 µm | A 20x view (0.293 µm/px, 562 × 351 µm) still shows ~6 usable rows on each side of a channel + 50 µm keep-out band. 50 µm pitch fails that case. |
| Dot diameter `D` | 12 µm | 41 px at 20x, 8 px at 4x; SU-8 posts of aspect ratio 2.5 at 30 µm height are fabricable; 25 µm features already exist in the design. |
| Displacement `S` | 5 µm (P/6, Anoto's ratio) | 17 px at 20x, 3.4 px at 4x; dots never touch (`2S + D < P`). |
| Keep-out | 50 µm from channel edges, 200 µm from dicing lines, 300 µm from the existing crosses, 3 mm from the wafer edge | Bonding wall around channels; no dots under the dicing saw; keeps the existing alignment marks clean. |
| Seed | 7 | Any value; must match between mask and app. |
| Lattice | 3501 × 3501 nodes (0..105 mm) | Covers the 100 mm wafer; config default uses 3700 (superset). |

The design is pre-enlarged by 1.5 % for PDMS shrinkage. The dots live in the
same enlarged frame, so the decoder reports **mask** coordinates; the
measured `umPerPx` already reflects the real (shrunk) chip.

## Decoder

`backend::dotgrid::Decoder::decode(gray, config)`:

1. **Dots** — background-subtract (Gaussian of 6× the expected dot diameter),
   threshold, connected components; keep blobs of 0.3–3× the expected area,
   aspect < 1.8, fill > 0.5, not touching the border (clipped centroids are
   biased). `umPerPxHint` only sizes this step.
2. **Lattice basis** — axis direction from the circular mean of 4× the
   nearest-neighbour angles; pitch from the mean projection of pair vectors
   along each axis (nearest-neighbour *distances* are biased by the
   displacements, projections are not). The 400 dots nearest the cloud
   centre bound the O(N²) cost.
3. **Indexing** — integer lattice indices relative to a central dot, then
   four rounds of: least-squares affine (index → pixel) with the assigned
   displacements removed, residual classification into ±x/±y, re-indexing.
4. **Code** — for each of the 8 dihedral transforms (rotations and mirrors:
   the glass-side view is a mirror image), build the bit grids, compute
   column/row phase candidates, vote over every 3-line window with the
   cross-phase checks, combine column and row votes with the mod-63
   consistency checks. The winner needs ≥ `minVotes` (3) and more than twice
   the runner-up.
5. **Verify** — every dot's direction is compared with the codebook at its
   decoded absolute index; `agreement` must be ≥ 0.9 (guards against index
   drift on large grids). Agreeing nodes fit the final pixel → wafer affine,
   from which centre, θ, scale, mirror flag and chip come.

Failure modes are reported as `reason` strings (`too few dots`, `no
consistent code window`, `ambiguous code windows`, `bit agreement too low`,
…) and never as a stale or bogus pose.

### Synthetic benchmark (Python reference, 1920 × 1200 frames)

| Case | Pass rate | Position error |
|---|---|---|
| 20x clean, any rotation, both handednesses | 30/30 | 0.004 µm median (sub-pixel; real accuracy is set by optics and lithography) |
| 20x channel + 50 µm keep-out band through the view, along or across | 28/30, 29/30 | 0.004 µm |
| 20x same + 10 % random dropouts | ~31/40 | |
| 20x 25 % random dropouts | ~27/40 | |
| 10x channel band + 10 % dropouts | 29/30 | 0.002 µm |
| 4x clean | 10/10 | |
| Blank / noise images | never decode | |

The C++ decoder runs the same algorithm; `processing.dot_grid_decoder` prints
its per-frame time (about 80 ms for a 1920 × 1200 frame with ~200 dots on the
dev machine, Release), well inside the 250 ms sampling interval.

## Live service and UI

`DotGridService` owns one thread that, every `intervalMs` (250 ms default),
reads the latest committed `FrameStore` frame — skipping straight to the
newest like the realtime drop-frames mode — converts it to 8-bit grey
(16-bit containers are normalised), decodes, and publishes the `Pose`
through `getLatestPose()` (mutex snapshot) and an optional callback. It
never touches the capture or realtime processing threads and decodes a
given frame index at most once. `MIB_DISABLED_SERVICES=dot_grid` skips it.

Configuration lives under `dot_grid` in `config.json` (applied by
`AppConfigWatcher`, live-reloaded): `enabled`, `interval_ms`,
`um_per_px_hint` (0 = use `pixel_to_micron_factor`), `min_votes`,
`min_agreement`, `codebook_path` (a `codebook.json` from the generator, with
the chip table) or `codebook` (`seed`, `columns`, `rows`, `pitch_um`,
`dot_diameter_um`, `displacement_um`, `origin_x_um`, `origin_y_um`).

The Preview page's **Wafer Grid** button toggles `enabled` at runtime. While
on, the canvas draws the detected dots, a cross at the image centre and a
text box with X/Y (µm), θ, µm/px, direct/mirrored, chip, dot and vote counts
and decode time, or the failure reason.

## Fabrication notes

- The dots are on the **channel layer** mask: they become 30 µm tall SU-8
  posts on the mould and sealed 30 µm pits in the bonded PDMS. No extra
  layer or alignment step. If pits near channels ever hurt bonding, the
  fallback is a thin (2–5 µm) second SU-8 layer aligned to the existing edge
  crosses.
- Focus: pits have vertical walls, so their cross-section is the same circle
  at any height; focusing mid-channel (15 µm above the glass) images them as
  sharply as the channel walls.
- Use a chrome mask (film masks are marginal for 12 µm dots with 5 µm
  offsets). Layer 10 in the GDS, cells `DOT` → `DOTGRID`.
- Wafer_soRT output: 6.4 M dots, 180 MB GDS. Keep bulk outputs out of git
  (HDD/shared exports); only the 45 KB codebook is committed.
