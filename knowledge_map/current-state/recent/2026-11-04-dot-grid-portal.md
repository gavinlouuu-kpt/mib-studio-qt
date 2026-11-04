## 2026-11-04 — Invite-only dot-grid CAD portal, local implementation

Added the Cloudflare Worker/Containers portal in `tools/dot_grid_portal/`, with
Access JWT plus tenant membership, private uploads/downloads, confirmation gating,
queued native CAD generation and MIB registry package exports. Reuses frozen codec 1;
no processing ABI or desktop behavior changed. Includes a state-gated Driver.js tour.
See [[services/DotGridPortal]] and `docs/reports/2026-11-04-dot-grid-portal.md` for actual
verification and remaining deployment/full-wafer/fabrication limitations.
