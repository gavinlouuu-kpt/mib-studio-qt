# 0009. Dot-grid design registry: the seed is the design identity

Date: 2026-10-01
Status: accepted

## Context

ADR 0008 put one dot-grid pattern (Wafer_soRT, seed 7) on one design and
gave the app one codebook. More chip designs are coming, each with its own
mask, and on the bench the operator may put any of them under the objective.
The app must know *which design* it is looking at, not only where, and
developers need one repeatable way to add a design and get its mask layer
without editing app code or config by hand.

Options considered:

- **Encode a design id in the pattern** (reserve code space per design):
  the 2-symbol delta windows already use ~3500 of 3969 possible values per
  axis on a 100 mm wafer, so there is no room left without longer windows,
  which ADR 0008 rejected for the 20x mid-channel case.
- **Operator picks the design in the UI**: error-prone, and the whole point
  of the grid is that the frame tells you.
- **One seed per design, decode against all of them**: a frame rendered from
  one seed failed to decode under every other seed in synthetic trials
  (0 of 120 cross-seed attempts at 4x–20x, any rotation and handedness; the
  true seed decoded in 39 of 40). The code vote, the cross-phase checks and
  the ≥ 90 % bit-agreement verification all have to agree, so a wrong
  codebook does not produce a pose.

## Decision

- A **registry** (`resources/defaults/dot_grid/registry.json`, schema
  version 1) lists every design: `id` (slug), `name`, `revision`, `status`
  (`active` | `retired`), `seed`, lattice size, pitch/dot/shift, origin, chip
  table, keep-outs and the source DXF's sha256, plus the required
  `codec_contract` and the `encoder` core that made the mask
  ([ADR 0010](0010-dot-grid-codec-cores.md)). It stores parameters, not
  the codebook arrays; the core of the design's contract regenerates the
  codebook from the seed, exactly as the gold references require.
- **Seeds are unique and never reused** (new design = highest seed + 1;
  retired designs keep theirs because their wafers still exist). Ids are
  unique. Both loaders reject a registry that breaks this.
- Developers add a design with `dotgrid_cli.py register` (DXF in → registry
  entry + GDS/DXF/CSV mask layer out, after a synthetic cross-design check)
  and commit the registry, i.e. the PR *is* the upload. `mask` regenerates
  a registered design's layer, refusing a DXF whose hash differs.
- The registry is compiled into the app (`:/defaults/dot_grid_registry.json`);
  `dot_grid.registry_path` merges a local file on top for designs not yet
  shipped (clashing entries are skipped, the bundled one wins).
- `backend::dotgrid::Decoder` decodes against every registered design: dot
  detection and the lattice fit run once per dot geometry, the vote and
  verification once per design. Exactly one design must decode; two is
  `ambiguous design (...)`, never a pose. `DecodeResult` / `Pose` carry
  `designId` and `designName` next to `chip`.
- Source precedence in `DotGridService`: `codebook_path` (explicit override)
  → registry → inline `codebook` params.

## Consequences

- Easier: adding a design is one command plus a PR; the app identifies the
  design and the die from any decodable frame; mask regeneration is
  reproducible and tied to the source file.
- Cost: designs sharing the dot geometry (dot diameter and shift/pitch
  ratio) add no measurable decode time. Each *distinct* geometry adds a
  dot-detection pass, which roughly doubles to triples the per-frame time,
  so `register` warns when a design introduces one.
- To respect: never change or reuse a registered seed or geometry. A
  changed layout is a new id (and seed). The two implementations must keep
  parsing the same schema; `processing.dot_grid_registry` and
  `scripts.dot_grid_reference` both load the bundled file and check that
  the Wafer_soRT entry regenerates the archived codebook.
- Not done: an in-app "upload design" dialog (the CLI + PR flow covers
  developers), and per-design CAD overlays.
