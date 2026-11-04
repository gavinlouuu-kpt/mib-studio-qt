## 2026-11-04 — Invite-only dot-grid CAD portal, local implementation

Added the Cloudflare Worker/Containers portal in `tools/dot_grid_portal/`, with
Access JWT plus tenant membership, private uploads/downloads, confirmation gating,
queued native CAD generation and MIB registry package exports. Reuses frozen codec 1;
no processing ABI or desktop behavior changed. Includes a state-gated Driver.js tour.
See [[services/DotGridPortal]] and `docs/reports/2026-11-04-dot-grid-portal.md` for actual
verification and remaining deployment/full-wafer/fabrication limitations.

### Review correction: proprietary customer surface and fluid interiors

Customer delivery is now combined device/dots DXF plus dots-only GDS. The API denies
map downloads for every customer role and omits seeds and internal registry records.
Registration, decoder and map configuration explanations were removed from the portal/tours.
Internal artifacts remain preserved, not deleted. Legacy ready exports lacking the new
no-dot contract are shown as `needs_review` and cannot be downloaded.

The previous demo was invalid: sampled wall-distance exclusion left dots inside wide
channels. Portal inputs now require explicit closed `NO_DOT` polylines/circles. Generation
excludes their filled interiors plus the whole dot radius and 50 µm clearance; no automatic
fluidic interpretation of open walls is claimed. Preview shades declared regions and asks
for completeness confirmation. The illustrative sample was rebuilt as a connected filled
fluidic region. Independent exported-DXF/GDS verification uses polygon offsets.

Actual source DWG was retrieved read-only from the recorded Drive file on 2026-11-04.
LibreDWG reported conversion errors; its output contains 9171 entities, including 88 REGION
objects and one MTEXT, all on layer 0. Inspection rejects it. No original-design generation,
fluidic validation or fabrication qualification has passed. Preserve the source and obtain
a reliable conversion plus confirmed fluidic domains before claiming actual-chip coverage.
