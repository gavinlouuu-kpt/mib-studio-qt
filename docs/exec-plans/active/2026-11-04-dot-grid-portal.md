# MIBQT-28 — invite-only dot-grid portal

Status: local implementation and verification in progress; no deployment.
Canonical task status: ENG-MIBQT Google tracker, MIBQT-28. This document is engineering
scope, not a competing task tracker. MIBQT-27 remains separate fabrication verification.

Gavin approved DXF/DWG inputs, explicit units/converted preview, combined DXF,
dot-layer-only GDS and a MIB registry/codebook map package on 2026-11-04.

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
- Full-wafer execution requires chunked generation beyond the current bounded v1 adapter.

## Decisions

- Reuse existing Python codec and registry; do not alter frozen encoding or C++ ABI.
- Cloudflare Access supplies verified email identity; D1 supplies workspace membership.
- R2 holds original/normalized/output files; D1 holds ownership/state/registry.
- Native Python/LibreDWG processing runs in private Containers, not ordinary Worker memory.
- One queue consumer serializes registration and cross-design checks in v1.
- No live invitations, fabrication sending, cloud provisioning or deployment in local validation.
