# Promotion plan: `feat/egrabber-workspace` → `develop`

Status: **on hold, by owner decision (2026-10-21).** The branch is not merged into `develop`.
Every qualifying `develop` push cuts a MIB Studio beta (Build Windows is the auto-beta gate),
so a merge there would put the workspace in beta users' hands at once, even behind an
opt-in. Promotion happens only when Gavin explicitly approves it. This page records how the
branch is kept ready until then and what the promotion would involve.

## Until then: the branch stays separate and current

- **Syncs only flow one way.** `develop` is merged into the branch regularly (the egw-sync PRs), and nothing flows back.
- **What still lands on `develop`, and why.** Backend and bridge work the workspace needs keeps landing on `develop` as it has since the first sync. That work is additive: commands, contract fields and fixes that the classic view either doesn't call or that fix it. Examples are A4a (the calibration commands, bridge ABI 42) and #762.
  - A `develop` PR must not change what a beta user sees in the classic view, unless it is a fix the classic view needs anyway (for example #763's F17 and F19).
  - Anything that exists only for the workspace UI stays on the branch.
- **Testing without the beta.** Gavin and the coordinator test the branch from builds made outside the beta feed:
  - the browser UI served by `yofo-studio-server` on P5, over the tailnet;
  - local Tauri builds on P5.
  - If a Windows build is wanted, it is a CI **workflow artifact** from the branch: unsigned, with no release, tag or updater feed. That workflow doesn't exist yet and needs Gavin's go-ahead. The Build Windows workflow is never dispatched for this, because it cuts a release.

## What the branch carries (2026-10-21)

The branch was 94 commits ahead of `develop`: 192 files, about 23k lines. It holds no backend, bridge ABI or HDF5 change.

| Area | Paths | Shipped behaviour if merged |
|---|---|---|
| Operation workspace (track A) | `desktop/src/workspace/`, hooks in `desktop/src/App.tsx`, `experimentStart.ts`, `preflight.ts`, `confirmationSignature.ts`, `cameraWindow.ts`, `serialReads.ts` | The **Workspace preview** button in the classic header (session-only) |
| Review app shell (track B) | `desktop/src/review/` (`ReviewApp.tsx`, `open/`, `inspect/`, `shell/`) | Replaces YOFO Review's shell. Inspect is the existing `ReviewPanel`. Overview and Export say they are not available yet |
| UI language kit `@mib/ui` | `desktop/ui/`, aliases in `desktop/vite.config.ts` and `desktop/tsconfig.json` | None by itself |
| Design records | `docs/design/egrabber-workspace/`, `EGRABBER_WORKSPACE.md`, `knowledge_map/` | None |
| CI triggers | `backend-ci`, `bridge-ci`, `desktop-ci` and `docs-ci` add `feat/egrabber-workspace` | None |

YOFO Review releases only on a `review-v*` tag (`review-release.yml`), so its shell reaches users only through a deliberate review release.

## Readiness checklist (to have answers when Gavin asks)

1. #762 (S1: a relative output path was written into the server's cwd) is merged and synced. The workspace Plan prompt shows the full default path.
2. #763's polish has merged (the workspace PR and the develop PR). F3, F4 and F5 are tracked under #754, B4 and #761.
3. A re-walk on the mock camera of F1, F2, F6, F9–F15 and F21 shows each one fixed.
4. Tauri smoke tests:
   - MIB Studio with the mock camera opens the workspace and runs one sample;
   - YOFO Review opens a run, goes Open → Inspect, and exports metrics.
5. The S13 parity harness is green on the branch head.
6. A4 (calibration run) and A7 (quick look) are in, if Gavin wants them before promotion.

## If and when Gavin approves: the options

- **(a) Merge behind a default-off setting.** The PR is a merge commit from the branch to `develop`. "Show workspace preview (experimental)" in Settings defaults to off, so beta users don't see the button unless they turn it on.
- **(b) Merge right after a stable cut.** The merge goes in right after a `develop` → `main` release, so the workspace has a full beta cycle before the next stable.
- **(c) A separate preview channel.** The workspace ships to a separate preview updater channel and stays off the beta channel. This needs release-pipeline work.

Whichever is chosen:
- YOFO Review hides the Overview and Export stage tabs until their views exist (B2, B4).
- Every release carries curated notes.
- Codex reviews the promotion PR, at most 2 rounds. Its focus is anything in the classic view that changes while the workspace is closed, and anything that actuates on navigation.

## What it does not decide

- Making the workspace the default view, or retiring the classic view.
- Cutting a YOFO Review release with the new shell.
- The PZ7035 instrument UI. S4–S6 run there separately.
