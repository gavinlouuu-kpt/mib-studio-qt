# MIBQT-28 — invite-only dot-grid portal

Status: implementation under review; deployed to the shared tailnet review portal only.
Production cloud tenancy and physical microscope validation remain outstanding.
Canonical task status: ENG-MIBQT Google tracker, MIBQT-28. This document is engineering
scope, not a competing task tracker. MIBQT-27 remains separate fabrication verification.

Gavin approved DXF/DWG inputs, explicit units/converted preview, combined DXF,
dot-layer-only GDS on 2026-11-04. Subsequent direction keeps all maps, seeds and
registration details internal; customers receive device/dots artifacts only.

Implementation and local commands: [portal README](../../../tools/dot_grid_portal/README.md).

## Acceptance

- Verified identity plus active per-tenant membership on every request.
- No cross-tenant design/file/member access; no public R2 URLs.
- Units and geometry explicitly confirmed before generation.
- Conversion and malformed/unsupported geometry fail closed.
- Immutable, centrally allocated identity; checksum/codec metadata round-trip.
- No registered/ready row after a failed export; replay does not create another identity.
- Actual in-app Driver.js tour with action gates and feedback.
- Cloud end-to-end and real MIB desktop import remain required before production acceptance.
- Full-wafer execution uses bounded batches and streaming exports, tested at actual wafer scale.
- Post-cut chip inclusion and fluid exclusion are separately confirmed; holes are preserved.
- Every chip retains dots, with complete-dot clearance from both cuts and fluid regions.

## Decisions

- Reuse existing Python codec and registry; do not alter frozen encoding or C++ ABI.
- Cloudflare Access supplies verified email identity; D1 supplies workspace membership.
- R2 holds original/normalized/output files; D1 holds ownership/state/registry.
- Native Python/LibreDWG processing runs in private Containers, not ordinary Worker memory.
- One queue consumer serializes registration and cross-design checks in v1.
- No live invitations, fabrication sending or production cloud provisioning in review validation.
- Tailnet review deployment is separate from production; old outputs without the post-cut contract remain retained but unavailable as chip-ready downloads.
