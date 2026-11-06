## 2026-11-04 — Wafer import recovery

The actual Wafer_soRT DWG exposed LibreDWG's modern ACIS-data loss. The optional
preservation-converter adapter recovers all 88 REGION bodies; strict planar polygon
extraction rejects partial/unsupported bodies. Geometry missing its ACIS payload
has a distinct sanitized customer diagnostic instead of an endless generic retry.

Unlabelled drawings can select closed no-dot areas in-app, with zoom/reset,
server-side ID validation, reset-on-edit confirmation, a state-gated Driver.js tour,
and feedback. No fluid semantics are guessed from closure alone. The actual wafer
has 3210 closed candidates and 20 open fragments. Its conflicting mm header and µm
interpretation are surfaced. Generation capacity still blocks the full wafer.

See [[../../services/DotGridPortal]] and `tools/dot_grid_portal/README.md` for runtime,
regression instructions, limits and evidence boundaries. Private CAD stays outside git.
