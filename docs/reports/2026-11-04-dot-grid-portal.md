# MIBQT-28: dot-grid portal local implementation

Reviewed 2026-11-04. Branch: `feat/dot-grid-portal-v1`, based on `699e9e82`.
Canonical status remains **In progress** in the ENG-MIBQT tracker. No Cloudflare deployment,
resource provisioning, invitation email, fabrication submission or desktop ABI change occurred.

## Delivered

Invite-only Worker application with verified Access JWTs, live D1 membership checks,
tenant-scoped private R2 storage, bounded DXF/DWG uploads, explicit units and normalized
geometry confirmation. Queued private Python/LibreDWG containers reuse codec contract 1.
Exports are combined device/dots DXF, dots-only GDS, and registry/codebook/manifest ZIP.
D1 reserves immutable identities; failed output writes do not publish registry entries.
The real UI includes a state-gated Driver.js walkthrough, restart/skip and feedback.

See [implementation](../../tools/dot_grid_portal/README.md) for architecture, setup,
limits, supported geometry and deployment preparation. The UI also presents before/after
flow diagrams. [Test boundaries](../../tools/dot_grid_portal/tests/README.md) distinguish
real subsystem execution from fixture-backed browser behavior.

## Verified locally

- TypeScript checks for Worker and browser passed; Vite production build passed.
- Nine Vitest tests passed: seven API/JWT cases, actual local workerd D1/R2 streaming
  upload, and a real Python job round-trip with output-write failure/retry/replay.
- Host Python suite: seven passed, one DWG test skipped (converter is container-only).
- Playwright: one actual browser walkthrough passed, including Driver.js controls.
  API responses are fixtures; screenshot is UI evidence, not cloud/CAD execution evidence.
- Wrangler deployment dry-run passed with container rollout disabled; no account write.
- Docker image built with hash-pinned LibreDWG 0.14; runtime is non-root and tested offline.
  All eight container Python tests passed, including genuine DWG conversion through map export.
- Focused native C++ registry/codebook/codec/decoder harness accepted the exported registry
  and localized a synthetic observation at `(2499.998, 2500.002)` micrometres.
  This is not full desktop GUI import or real-camera verification.
- Repository documentation checker passed.

The seed-8 negative fixture intentionally fails the existing decoder discrimination gate;
no registry or map is published. Positive fixtures use independently validated seed 9.
A faulty DXF-to-DWG writer round-trip was rejected during test development. The final
DWG test instead uses the genuine `2000/Line.dwg` fixture from the pinned GNU source
archive; conversion must pass strict DXF audit, normalization and map generation.

## Remaining acceptance work

1. Select an authorized Cloudflare account/hostname and establish Access policy, D1/R2/queues
   and container eligibility. Import existing production registry identities before uploads.
2. Prove live authentication, two-tenant isolation, revoke behavior, queue/container lifecycle,
   representative customer DWGs and downloaded map import in the actual MIB desktop.
3. Implement and validate chunked full-wafer generation: current one-million-node bound
   deliberately rejects a 100 mm wafer at 30 micrometre pitch.
4. Keep fabrication qualification separate under MIBQT-27. Synthetic decoding does not
   establish optical readability, manufacturing DRC, mask compensation or fabrication approval.

There is no running background deployment or promise of automatic production completion.
