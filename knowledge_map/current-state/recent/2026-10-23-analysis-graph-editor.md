## 2026-10-23 — Production graph editing and revision identity

Added interval/Boolean draft forms with reference-safe rename/delete and undo/redo.
Fixed same-plan edits clearing the expected backend revision. Exact saved-graph
loading restores method, transforms, QC, plot and template lineage; explicit detach
only affects the draft. Native evaluation keys graph revisions by content hash.

Evidence: UI14, frontend build, normal analysis.host and all4 ASan/UBSan suites pass.
Tailnet plan graph-revision-1792759318653 proves14→0 selected across immutable
revisions1→2, old revision unchanged, exact replay and stale CAS rejection.
No new browser click-through or full F acceptance claim.

Follow-up: selected/restored/template-applied revisions must be loaded before draft
save. This prevents unloaded method/application defaults or an old draft graph from
silently replacing a newly selected strategy. Own successful saves retain the edit
base. A UI regression verifies refusal before any backend write.

Rectangular draft creation reuses two explicit native inclusive intervals plus AND,
with collision/bounds refusal. Tailnet rectangle-1792759502927 selects7 and returns7
exact images, checked against frozen rows. No browser-side membership authority.
