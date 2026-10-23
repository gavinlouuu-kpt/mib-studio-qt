# I production integration checkpoint — 2026-10-22

## Branch and incorporated slices

`feat/807-ique-workspace-core`, worktree `mib-807-ique`. Starting HEAD `c7fcae7d`
already includes annotation scope `71b82074`, QC trends `569ab14c`, recipe pairing
`605cb4fb` and SQLite core `55b2849d` (draft PRs #811/#813/#814/#815).
No main/develop merge, reset, #722 change, or edits to other worktrees.

## Real production path

AppBackend → SqliteService (app.sqlite3) → WorkspaceStore; BackendFacade → cxx
bridge → shared mib-app-commands → Tauri / WebSocket → Experiment / Plan /
**Saved workspace plan**. ABI 45 is additive over this branch's ABI 44, with
regenerated TypeScript version and updated contract assertions. There are no new
package dependencies or alternate persistence authorities.

- Enter/retain Workspace ID, then **Save new workspace** (create-only CAS).
- **Open saved plan** reads the latest revision; persisted annotation groups appear.
- **Save new revision** uses the read revision as CAS. Another writer causes a
  visible conflict and clears the obsolete readback; it never overwrites blindly.
- An unknown write keeps its exact command for **Retry same save** while mounted.
- Navigation and mounting do not invoke metadata commands. Save/Open never applies
  a method, starts capture or replaces the active experiment identity.
- The recorded operator is a declaration, not authentication.

QA URL: `http://127.0.0.1:18088/` (production build served by yofo-studio-server).
This is distinct from the preserved `:18087/ique-demo.html` fixture-only runtime.
Runtime uses its own `data/qa-807-production`, mock camera selection and disabled
capture, processing, autofocus, trigger, dot-grid, playback and HDF5 boot services.
There is no connected hardware and no scientific worker in this runtime.

## Validation

- Production C++ backend and Rust server builds; TypeScript/Vite production build.
- 555 workspace tests and 21 adapter/version tests passed; existing jsdom canvas
  diagnostics remain non-fatal and are not rendered-browser evidence.
- `backend.workspace_host`: real save/reopen, immutable CAS revisions, lost-reply
  replay after DB reopen, injected receipt rollback, restore and read-only refusal.
- Real WebSocket through the running app: init, save, exact replay, stale CAS,
  read-endpoint mutation refusal, viewer write denial and viewer reads passed. Restarting only this newly owned
  QA server preserved the original revision through the same native endpoint.
- 10 Rust shared-dispatch tests passed; the updated server control-policy test passed.
- Host test ASan/UBSan passed. TSan rebuilt but failed at process startup with
  `unexpected memory mapping` (exit 66); it did not execute tests.
- Browser navigation was blocked by tool policy. Rendered click-through unverified.

## Concrete unfinished integration and acceptance

1. Save/Open currently persists and inspects the sample sheet. Loading a saved
   experiment as a fresh runnable plan, workspace listing/history/restore controls,
   recipe editing/persistence and finding export/relink are not implemented in UI.
   Backend history/restore already exist; no complete I12 acceptance claimed.
2. Recipe method/analysis refs remain immutable model data; no backend scientific
   compatibility evaluator is connected. Compatibility must remain separate from
   physical readiness and may never auto-apply/start.
3. F inspected at `4f384c5645b026cb2aecbaa960a354721fb2a81c` exposes native
   `PopulationPlan{id,revision,nodes}` and validated exact-key evaluation, not a
   public application plan lookup/compatibility facade. E inspected at
   `a09230c6a80f1e8a6bce067f636f97332459931a`. Neither is incorporated by this
   checkpoint. Next adapter must resolve recipe refs to persisted F plans and
   immutable E source identity, not treat a missing provider as compatibility.
4. The metadata mutex executes on the shared bridge command path (2-second busy
   timeout), not a dedicated metadata worker. Long metadata work and acquisition
   responsiveness still need stress/performance qualification.
5. Retry receipt survives backend restart but client pending-command state does
   not survive panel unmount/reload. Reopen the workspace ID to inspect persisted
   state; do not claim durable frontend command reconciliation.
6. Tauri wiring compiles only as source here; native webview packaging and full
   TSan acceptance remain unverified. Scientific parity/hardware acceptance remain
   separate. No ownership/allocation gate is asserted.

## 2026-10-23 recovery continuation

Implemented on the same I branch:

- Production revision history (bounded first 1000) and expected-head CAS restore;
  restoration appends rather than overwrites historical records.
- Explicit saved-sheet load into a new experiment UUID; queue/reference/result
  reset, with locked/pending/in-progress plan guards. No method apply/start call.
- Exact pending write bytes persisted before dispatch in per-tab sessionStorage.
  Reload/remount offers explicit retry; storage write failure sends nothing.
- Editable pinned recipe association persisted/reopened in the same SQLite
  document, retaining immutable method/analysis references and parent lineage.
  Compatibility is explicitly unknown, not inferred from well-formed references.

Validation: 558 workspace tests passed before two further focused regression tests;
10 focused tests pass including reload replay, CAS restore, recipe round-trip,
storage failure and actual controller fresh-identity/detached-sheet/viewer guards. TypeScript/Vite production build and docs checks pass. Existing
native SQLite host/core harnesses rerun successfully for rollback/restart/replay,
restore and concurrent CAS. Production tailnet HTTP returns 200. A live WSS write
probe was denied VIEWER_ONLY because another client controls the runtime; control
was not taken from that client. Rendered browser interaction remains unverified.

The previously stopped owned QA process was relaunched using its existing data
and production dist, with the same mock/offline disabled services. Existing
Tailscale handlers were inspected and left unchanged.

Still incomplete: scientific resolver/provider binding (no public exact-plan
lookup/evaluation adapter exists on this branch), finding/artifact export/relink,
full workspace lifecycle beyond the saved plan/recipe document, pagination beyond
1000 history entries, cross-tab/closed-tab pending recovery, TSan qualification.
This is a recovery checkpoint, not full revamp acceptance. Coordinator was asked
for current F/E application contracts/commits; no generic shared-file gate asserted.

## 2026-10-23 pinned providers and full metadata lifecycle

The continuation ledger now supersedes earlier missing lifecycle/provider notes:
[requirement and evidence ledger](CONTINUATION-LEDGER.md). Production library,
archive/history/CAS restore, full-document save/reopen, pending-write recovery,
export/import with exact analysis dependencies, source relink and native recipe
resolution/evaluation are integrated. See F's pinned
[production API](../flowjo-analysis/production-api.md) for authority and bounds.

QA (tailnet-only): https://gavin-p5-ubuntu.himalayan-coho.ts.net:18489/
Experiment → Plan → Saved workspace plan → Workspace library. Open a workspace;
source links can be verified/relinked, then **Open linked source in Review** and
**Evaluate pinned recipe**. Local methods require their host profile directory;
method configurations are not embedded in exports. Save finding is explicit.
Use classic Review's native analysis workspace to create the pinned strategy first.
Mock/offline runtime, no hardware. This is the real application/SQLite backend,
with synthetic qualification data; not an alternate fixture UI.
