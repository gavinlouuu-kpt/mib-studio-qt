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
`tools/dot_grid_portal/demo/README.md`. The corrected example generated 85,060 dots; no fabricated
camera result or fabrication claim. Tailnet example has a separate four-step Driver.js tour.

## Import recovery (2026-11-04)

`generator/cad_import.py` checks ACIS payload preservation and strictly extracts
planar straight-edge REGIONs; optional ODA executable selection is operator-owned.
The real Wafer_soRT fixture preserves 88/88 REGIONs through the configured route.
`generator/domains.py` derives unclassified closed-area candidates for user selection;
open boundary fragments are reported, not guessed. Annotations stay in delivered CAD.
The area-selection Driver.js tour resets confirmation on edits and includes feedback.
`src/inspection.ts` stores large geometry in private object storage, not large D1 rows;
only server-validated selection IDs enter the confirmation record. Review geometry is kept separate from source entities. The subsequent batching and
post-cut contracts below supersede the initial import-only and wafer-only workflow.

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

## Post-cut chip placement

`generator/placement.py` requires explicit chip inclusion domains independently of
fluid exclusions. It clips complete dots inside cut boundaries plus clearance and
rejects overlapping, empty or dot-starved chips. Fluid candidate holes are preserved;
cut outlines use the exterior. The UI shows these roles separately, supports manual
rectangular boundaries, verifies translated face copies before group assignment, and
shows one generated detail per chip. Geometry without this placement contract cannot
be presented as a cut-chip-ready export. Source geometry is not edited by review roles.

Actual wafer review draft: 20 smaller rectangular envelopes and 20 fluid-network
faces, with all open fragments covered by the proposed exclusions. The measured
local output contains 2,689,252 dots inside those envelopes; no fluid-interior,
fluid-clearance or cut-clearance violations were found by the independent checker.
The intended physical saw edge and real microscope decoding are not thereby proven.

Delivered tailnet review run: **Wafer_soRT — post-cut review draft**, 2,689,289 dots
across all 20 chips. Both downloaded formats and preservation of all 9,171 normalized
device entities passed independent checks. Source geometry and review roles remain
separate. See the dated portal report for the live receipt and verification scope.

### Compact delivery (2026-11-04)

The flat ASCII DXF is a compatibility export, not the primary wafer download.
`generator/compact.py` uses KLayout 0.30.12 to losslessly compact the dots-only
GDS hierarchy into OASIS; the provisioned ODA converter additionally creates a
combined device+dots ACAD2018 DWG. Without ODA, DWG is not advertised. No points
are decimated and the codec/identity is unchanged. The worker returns an explicit
export list with byte sizes, all listed files are stored before readiness, and
the customer allowlist excludes maps. The UI puts compact files first and hides
DXF/GDS under Other file formats. DWG failure fails generation, not a broken link.

Actual wafer: 2,689,289 dots; combined DWG 44,037,408 bytes; dots-only OASIS
1,517,576 bytes. DWG was reopened via ODA and round-tripped to DXF with all dot
coordinates/radii and 9,171 device geometry/annotation records preserved within
1e-8 drawing units (serialization precision). OASIS was loaded/rendered in
KLayout and every dot position matched the original spool. This is not an
AutoCAD, supplier acceptance, physical fabrication or optical-recognition test.
