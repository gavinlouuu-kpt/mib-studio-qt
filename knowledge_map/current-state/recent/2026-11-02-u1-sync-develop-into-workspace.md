## 2026-11-02 — develop synced into the workspace branch (fold-in slice U-1)

Slice U-1 of the fold-in realignment plan
(`docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md`): branch
`claude/sync-develop-into-workspace-1102` starts at `feat/egrabber-workspace` and merges
`develop` (six commits: #792 WebSocket requests with duplicate JSON keys are refused, #793 the
monitoring strip pins own a copy of their cell). The merge was clean; the bridge ABI stays 44 and
the review ABI 6. No goal row (E01–E09, I01–I12, F01–F16) changes state: U-1 only puts the
integration branch on `develop`'s head so #810 (F) and the I stack can be merged onto one base
(slice U-2). See [[architecture/Desktop-Shell]].
