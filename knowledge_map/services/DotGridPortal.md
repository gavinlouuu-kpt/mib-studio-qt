# DotGridPortal

Invite-only Cloudflare CAD preparation service in `tools/dot_grid_portal/`.
Related: [[DotGridService]], [[../task/2026-10-01-dot-grid-design-registry]].

The Worker verifies Access JWT identity and D1 membership, enforces tenant ownership,
stores CAD privately in R2 and queues conversion/generation. Private Python containers
reuse the existing codec and output combined DXF, dots-only GDS and a registry/codebook ZIP.
The unchanged desktop loader consumes the registry through `dot_grid.registry_path`.

See `tools/dot_grid_portal/README.md` for deployment preparation, verification, resource
bounds and source-to-mask transforms. One consumer is a registration invariant.
The initial adapter rejects unsupported geometry and full-wafer jobs above its node bound.
Registration is not fabrication approval. No CAD-overlay viewer is added.

Engineering plan: `docs/exec-plans/active/2026-11-04-dot-grid-portal.md`.
Canonical operating record: ENG-MIBQT tracker, MIBQT-28 (MIBQT-27 is fabrication).

Tailnet review adapter: `tools/dot_grid_portal/review/README.md`. Separate shared review
identity and persistent local data; never a production Access bypass or production registry.

Realistic demo: original 12 × 8 mm microfluidic concept, actual exported dot geometry,
before/after toggle and sample-loading controls. Provenance and generation steps:
`tools/dot_grid_portal/demo/README.md`. Review seed 11 generated 87,907 dots; no fabricated
camera result or fabrication claim. Tailnet example has a separate four-step Driver.js tour.

## Import recovery (2026-11-04)

`generator/cad_import.py` checks ACIS payload preservation and strictly extracts
planar straight-edge REGIONs; optional ODA executable selection is operator-owned.
The real Wafer_soRT fixture preserves 88/88 REGIONs through the configured route.
`generator/domains.py` derives unclassified closed-area candidates for user selection;
open boundary fragments are reported, not guessed. Annotations stay in delivered CAD.
The area-selection Driver.js tour resets confirmation on edits and includes feedback.
`src/inspection.ts` stores large geometry in private object storage, not large D1 rows;
only server-validated selection IDs enter the confirmation record. Helper exclusion
polygons are removed before device export. Full-wafer inspection now works, but the
one-million-node generation cap remains: wafer mask acceptance is still open.

## Full-wafer batching (2026-11-04)

The portal adapter now batches lattice generation without changing codec contract 1,
indexes filled-region clearance, and streams DXF circles / GDS cells. Bounds are
14 million candidates, 3,700 per axis, 3 GiB address space and per-file size, and
five minutes. The review adapter streams object uploads/downloads as well.
The private wafer produced 6,387,278 dots in a capacity-only benchmark; a separate
conservative all-closed-polygons test produced 3,452,492 dots, with both exported
formats independently checked and zero whole-chip clearance violations across
20 chips. These are tests, not approval of fluid semantics or placement inside chips.
Ready designs expose actual-dot overview/detail previews; maps and seeds remain private.
