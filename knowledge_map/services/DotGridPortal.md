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
