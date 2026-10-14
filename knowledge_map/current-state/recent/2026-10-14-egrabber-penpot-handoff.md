## 2026-10-14 — Penpot references for the eGrabber workspace branch

The `feat/egrabber-workspace` branch now includes
`docs/design/egrabber-workspace/PENPOT.md`, linked from `EGRABBER_WORKSPACE.md`.
The handoff identifies the sandbox source, proposed page, all seven context pages,
selected board UUID anchors, named flows, and the full 00–36 view-to-work-package
map. `reference.json` contains the machine-readable `penpot` registry.

Use Penpot for layout/workflow and the checked-in reviewed-v2 HTML for corrected
state semantics, within the real backend contract. See [[architecture/Desktop-Shell]].
Implementation PRs should cite the file/page/board and the revision actually read.

The private origin is supplied locally through the existing team access mechanism;
no private hostname, credentials, recovery-operation identifiers or font binaries
are published. Native routing/font repair and normal-viewer acceptance remain
pending; this documentation update performs no Penpot edits or application changes.
