## 2026-11-02 — Fold-in integration rehearsal and end-to-end scenario (E, I, F)

`claude/fold-in-integration-e2e` rehearses the final merge: `claude/807-i1-recipes`
plus the F stack (#818 → #823 → #824, review ABI 10) and the E stack (#812 → #817 → #819).
One bridge contract carries F (47), I workspace (48), E `camera_features` (49) and I
recipes (50); `abi_version` 50. `crates/mib-bridge-server/tests/fold_in_e2e.rs` (ignored by
default) drives the real `yofo-studio-server` process over the WebSocket through E01, E02,
E04/E06, I02, F04/F05/F08, F09 and an I12/F12 process restart. Evidence:
`docs/evidence/fold-in-e2e-2026-11-02.md`. See [[architecture/Rust-Bridge]],
[[architecture/Desktop-Shell]].
