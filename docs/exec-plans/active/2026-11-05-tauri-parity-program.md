# Tauri parity program: close the Qt gaps and the parity-check bugs (2026-11)

Status: active

Tracking issue: [#869](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/869), under the
migration epic #246. Area epics: #862–#868.

## Goal

The React/Tauri desktop app reaches parity with the Qt app, and every bug the parity checks found is
fixed. A row of the parity matrix is closed when Tauri behaves as Qt does; where Tauri should
intentionally differ, the decision is recorded on the area epic.

## Sources

- The mock-camera parity matrix of develop `5f906ac2`: PR #844,
  `docs/reports/2026-11-04-tauri-parity-matrix.md` (607 rows; `parity-matrix.json` carries each
  row's Qt and Tauri `file:line`).
- The real-camera Qt vs Tauri test S1–S7 on win11-agent (report on P5:
  `/mnt/hdd/developer-data/mib-studio-qt/handover/tauri-camera-parity/REPORT.md`).

## Gaps by area (395: 112 missing, 283 partial)

| Epic | Gaps | Major or blocker | Bugs |
|---|---:|---:|---|
| #862 Shell | 72 | 12 | #847, #850, #856, #858, #861 |
| #863 Connect and Overview | 45 | 10 | #846, #852 |
| #864 Configuration | 75 | 9 | #849 |
| #865 Experiment Preview | 32 | 11 | #848 |
| #866 Experiment start/stop and Monitoring | 81 | 15 | |
| #867 Review (FCS export blocker) | 47 | 12 | #853, #854 |
| #868 Hardware | 43 | 20 | #855 |
| Platform (no epic) | | | #845, #851 |

Each epic lists its gap rows (matrix ID, Qt and Tauri `file:line`) and the suspicious or stubbed
code found in its area. Qt bugs found on the way are fixed in the Qt app: #857, #859, #860.

## Waves

- **Wave 0 (now)**
  - (A) Split `desktop/src/App.tsx` (2685 lines) and `App.css` into one module per area with no change
    in behaviour, so the area streams do not conflict; write this plan. Owner map:
    [`../../howto/desktop-area-ownership.md`](../../howto/desktop-area-ownership.md).
  - (B) Platform bugs #845 (Tauri side: long commands must not block every invoke; bounded close),
    #850 and #851, on `fix/tauri-platform-bugs`. It owns `desktop/src-tauri/**`, `crates/**`,
    `desktop/src/closeGuard.ts` and `desktop/scripts/windows-runtime.cmake`.
  - (Q) The Qt bugs #857, #859 and #860.
- **Wave 1 (after A merges):** Review #867, Monitoring #866, Connect and Overview #863, Hardware #868.
  These hold the most major and safety gaps.
- **Wave 2:** Shell #862, Configuration #864, Experiment Preview #865.

## Rules for every stream

- One branch and one draft PR per stream into `develop`. The orchestrator reviews each PR; Gavin merges.
- Each PR ships its vault and docs updates (`AGENTS.md`, `knowledge_map/Vault-Maintenance.md`).
- Edit your own area's files ([owner map](../../howto/desktop-area-ownership.md)); change shared
  files (`App.tsx`, `App.css`, `studio/**`, `styles/**`, `bridge.ts`, `transport/**`) only in small,
  additive edits named in the PR.
- Camera, capture or eGrabber paths are built with `scripts/win11_bench_build.sh`
  ([howto](../../howto/win11-bench-build.md)) and tested on the win11-agent camera before merge. The orchestrator schedules the rig: one camera, one stream at a time.
- The backend half of #845 is #842: merge it before re-testing #845.

## Acceptance criteria

- [ ] Every area epic (#862–#868) closed: no missing or partial matrix rows except recorded decisions.
- [ ] The parity matrix re-run on the new develop shows the same.
- [ ] The real-camera Qt vs Tauri test S1–S7 on win11-agent repeated and passing.
- [ ] Platform bugs #845, #850, #851 and Qt bugs #857, #859, #860 closed.

## Decision log

- 2026-11-05: Split `App.tsx` by area before any parity work (#869 A). The state stays in one place
  (per-area state hooks composed by `studio/useStudioController.ts` and provided through one React
  context); the composition keeps the original effect order, because effect order is observable
  (frame-pull invalidation before a recovered review's first frame request). `App.css` becomes an
  `@import` manifest of per-area files in the original order, so the built CSS is byte-identical.
  Existing modules outside `App.tsx` keep their paths; the owner map lists them per area.

## Progress

- [ ] Wave 0 A: App.tsx split (`refactor/tauri-desktop-split`)
- [ ] Wave 0 B: platform bugs (`fix/tauri-platform-bugs`)
- [ ] Wave 0 Q: Qt bugs #857, #859, #860
- [ ] Wave 1: #867, #866, #863, #868
- [ ] Wave 2: #862, #864, #865
- [ ] Acceptance re-runs (matrix, S1–S7)
