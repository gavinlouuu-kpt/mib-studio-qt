# Dot-grid wafer localization

Absolute localization of the camera field of view on a microfluidic wafer (or
a PDMS chip moulded from it) from a fiducial pattern fabricated on the chip
itself, in the style of the dot patterns on smart-pen notebooks. The camera
sees a small patch of dots; the decoder returns the wafer coordinates of the
image centre, the rotation, the measured scale, whether the view is mirrored
(chip viewed through the glass side), which **design** the chip is (from the
design registry) and which chip (die) of that wafer the view is on.

| Piece | Where |
|---|---|
| Codebook (pattern definition, shared with the mask generator) | `include/backend/processing/DotGridCodebook.h`, `src/backend/processing/DotGridCodebook.cpp` |
| Decoder + synthetic renderer (Qt-free, OpenCV) | `include/backend/processing/DotGridDecoder.h`, `src/backend/processing/DotGridDecoder.cpp` |
| Codec cores (contract, `ICodec`, bundled contract-1 core, per-contract routing) | `include/backend/processing/DotGridCodec.h`, `src/backend/processing/DotGridCodec.cpp`; version `scripts/dot_grid/dotgrid/VERSION` |
| Codec gold reference (contract 1) | `scripts/dot_grid/gold/codec-contract1.json`, `scripts/dot_grid/dotgrid/gold.py` |
| Design registry (Qt-free; every design, one seed each) | `include/backend/processing/DotGridRegistry.h`, `src/backend/processing/DotGridRegistry.cpp`, `scripts/dot_grid/dotgrid/registry.py` |
| Live service (samples FrameStore, publishes poses) | `include/backend/services/DotGridService.h`, `src/backend/services/DotGridService.cpp` — [vault note](../../knowledge_map/services/DotGridService.md) |
| Overview overlay + "Wafer Grid" toggle (Overview tab only) | `src/frontend/tabs/OverviewTab.cpp`, `src/frontend/utils/SimpleImageCanvas.cpp` |
| Mask generator, reference decoder, simulator (Python) | `scripts/dot_grid/` — see [howto/dot-grid-mask-generation.md](../howto/dot-grid-mask-generation.md) |
| Bundled registry (compiled into the app as `:/defaults/dot_grid_registry.json`) | `resources/defaults/dot_grid/registry.json` |
| Archived full codebook of the Wafer_soRT design | `resources/defaults/dot_grid/wafer_soRT_2025-03-16_seed7_p30.json` |
| Tests | `processing.dot_grid_codebook`, `processing.dot_grid_decoder`, `processing.dot_grid_registry`, `backend.dot_grid_service`, `scripts.dot_grid_reference` |
| Decision records | [ADR 0008](../decisions/0008-dot-grid-localization.md) (pattern, decoder, service), [ADR 0009](../decisions/0009-dot-grid-design-registry.md) (design registry), [ADR 0010](../decisions/0010-dot-grid-codec-cores.md) (codec cores) |

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

## Design registry

Every chip design that carries a grid is registered once
(`resources/defaults/dot_grid/registry.json`, [ADR 0009](../decisions/0009-dot-grid-design-registry.md)):

```json
{"version": 1, "designs": [
  {"id": "wafer-sort-rt", "name": "Wafer_soRT DC sorting chip (30 um channels)",
   "revision": "2025-03-16", "status": "active", "codec_contract": 1, "seed": 7,
   "columns": 3501, "rows": 3501, "pitch_um": 30.0, "dot_diameter_um": 12.0,
   "displacement_um": 5.0, "origin_um": [0.0, 0.0], "design_scale": 1.015,
   "keepout": {"channel_um": 50.0, "...": "..."},
   "source": {"file": "Wafer_soRT.dxf", "sha256": null},
   "chips": [{"name": "R0C1", "x_min_um": 30053.6, "...": "..."}]}]}
```

- **The seed is the design identity.** The codebook is regenerated from the
  entry, so the file holds parameters only (about 4 KB per design). Seeds and
  ids are unique; a seed is never reused (new = highest + 1; retired designs
  keep theirs). Both loaders reject the whole file when that is violated.
- **Decoding against all designs.** A frame decodes under the codebook it was
  made from and under no other: the vote, the cross-phase checks and the
  bit-agreement verification all have to agree (0 of 120 cross-seed attempts
  decoded in synthetic trials; `processing.dot_grid_registry` and the
  `register` cross-check keep proving it). The decoder groups designs by dot
  geometry (dot diameter, shift/pitch ratio): detection and the lattice fit run
  once per group, the code vote and verification once per design. Exactly one
  design must decode; two would be `ambiguous design (a, b)`.
- **Cost.** Designs sharing the geometry add no measurable time (1, 2, 4 and
  8 designs all decode in 120–160 ms on the cloud container). Each extra
  geometry adds one detection pass (two geometries took 360–420 ms there), so
  keep 30/12/5 µm unless the process needs otherwise; `register` warns.
- **Where the app gets it.** The bundled file is compiled in; `dot_grid.
  registry_path` merges a local registry (relative to the config directory)
  for designs not yet shipped. Clashing local entries are skipped with a
  warning.
- **Adding a design** is `dotgrid_cli.py register` + a PR; see the
  [how-to](../howto/dot-grid-mask-generation.md).

## Codec cores

The encoder and decoder form a versioned **codec core**, modelled on the
processing cores ([ADR 0010](../decisions/0010-dot-grid-codec-cores.md);
plan: [exec-plans/active/2026-10-02-dot-grid-codec-cores.md](../exec-plans/active/2026-10-02-dot-grid-codec-cores.md)).

- **Codec contract** = what the dots mean (this document's *Pattern* section
  is contract 1, line `mseq63-delta2`). Frozen forever: masks are permanent.
  Every registry design declares `codec_contract` and records the `encoder`
  core that made its mask.
- **Core version** = a build of one contract's encoder + decoder
  (`scripts/dot_grid/dotgrid/VERSION`, `MIB_DOTGRID_CORE_VERSION`). Better
  detection or speed is a new core version, never a contract change.
- **Routing.** `CodecSet` holds at most one active core per contract (a bench
  can mix wafer generations); `DesignDecoder` sends each design only to the
  core of its contract and keeps the exactly-one-design rule across contracts.
  A design whose contract has no active core is *unsupported*: logged, never
  decoded, still counted for id/seed uniqueness. `Decoder` (the contract-1
  algorithm) skips other contracts itself.
- **Identity.** Every `DecodeResult` / `Pose` carries `codecContract`,
  `coreVersion`, `coreSource`; `DotGridService::activeCodecs()` lists the
  active cores. Phase 1 has one: the bundled contract-1 core.
- **Gold.** `scripts/dot_grid/gold/codec-contract1.json`: exact encode
  references (SHA-256 of the full `phi`/`psi` arrays for five seeds including
  a 64-bit one, heads, probe bits) and twelve decode cases judged against the
  rendered truth (20x/10x/4x, rotations, mirror, channel band, 10 % dropouts,
  blank and foreign-seed rejects). Met by the C++ core
  (`processing.dot_grid_codec_gold`) and the Python reference
  (`dotgrid_cli.py gold`, `scripts.dot_grid_reference`); changes only with the
  `gold-reference-change` label.
- **Next phases** (planned): a pure-C plugin ABI and signed loader shared
  with the processing cores, the wheel as the mask generator's encoder, and a
  catalog + release line `mib-dotgrid-<line>-v<semver>`.

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
`min_agreement`, and the codebook source, first match wins:
`codebook_path` (one `codebook.json` from the generator, an explicit
override), the design registry (bundled + `registry_path`, the normal case),
or `codebook` (`seed`, `columns`, `rows`, `pitch_um`, `dot_diameter_um`,
`displacement_um`, `origin_x_um`, `origin_y_um`) when the registry is empty.

Localization belongs to the **Overview** tab, where the operator navigates
the chip; the Experiment tab never shows it and never pays for it. The
Overview's **Wafer Grid** button toggles `enabled` at runtime, and the
Overview pauses the service (`DotGridService::setPaused`) whenever it is not
on screen — another tab current or the window minimised — so nothing is
decoded next to a running experiment even with `enabled` on. Showing the
Overview again resumes at once (the wait is woken, the newest frame decoded).
While on, the Overview canvas draws the detected dots, a cross at the image
centre and a text box with X/Y (µm), θ, µm/px, direct/mirrored, design name
and chip, dot and vote counts and decode time, or the failure reason.
`frontend.dot_grid_overview` covers the tab gating.

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
