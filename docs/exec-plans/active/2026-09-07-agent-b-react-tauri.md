# Agent B: shared-backend React/Tauri integration

Status: active

## Candidate inventory (2026-09-07)

Issue #372 is authoritative over older parity language. Remote refs refreshed
with `git ls-remote`; no accepted backend handoff has been supplied.

| Reference | SHA | Acceptance |
|---|---|---|
| develop | bb82cf19a6e4ae44e766e4d787594ff76344f26c | Integration destination, not migration base |
| dev/react-tauri | 5c7918c6410d4594a49f1deaf1e7035a3ba6d307 | Independent transport staging base |
| claude/host-sdk-reliability-qt-ui-g03ubd | 921f48dbc6eb546e241d34b305298954cf542cea | Agent A candidate; not accepted handoff |
| PR #321 | 910fde754b4bc6cce99e63bc0d39a0bf70ac5555 | Open dashboard slice, not merged |

Source inspection and historical reported tests are distinct from executed
verification. No hardware validation is asserted by this inventory.

| Capability | Implementation branch/PR | Owner | Bridge | Qt | React | Evidence | Remaining gap |
|---|---|---|---|---|---|---|---|
| Frame pulls | migration ABI 11 | B transport / A snapshots | Owned BridgeFrame, split mutable IPC caches | Existing preview | Live/indexed/review | New deterministic client regression fails with B pixels for A metadata | Atomic IPC; source/session/config identity |
| Contracts | migration #271 | B | cxx, JSON, generator, enum tests | Facade | Generated enums, generic slots | Historical issue comment; inspect contract.rs | Payload meaning, lossless integers, compatibility |
| Readiness/run snapshot | reliability #369 | A | Migration facade lacks readiness request | Reliability coordinator consumed by Qt | Old preconditions | Candidate header + readiness tests, not executed here | Reviewed public facade binding and generation rules |
| Finalization | migration #274 / reliability #369 | A | Migration experiment commands | Reliability caller owns finalization | Migration status/start/stop | Historical e2e.experiment_coordinator and cargo lifecycle test | One converged coordinator; Qt-free finalization |
| Config application | migration #273 / reliability work | A | JSON merge/config_version; no checked persistent transaction | Existing config UI | Editor | Historical processing_config_roundtrip_and_core_status | Baseline revision, conflict/save/apply/verification outcomes |
| Monitoring/accounting | migration #275 / reliability #367 | A | Bounded metrics snapshots; UI activation switch | Existing monitoring | Visibility-gated collection | Historical monitoring_and_trigger_contract | Backend-owned run collection and authoritative accounting |
| Review/export | migration #276 / develop #344 plan | A service / B adapter | Paged review, read-only CSV worker | Existing exporter/review | Review table/image/export | Historical review_metadata_pages_images_and_export_job | Reuse accepted HdfExportService; provenance/accounting exposure |
| Integrated workspace | PR #321 | B | Uses migration snapshots | Reference only | Metrics/alerts slice exists elsewhere | PR reports 8 tests; not native E2E proof | Persistent header/alerts, truthful freshness, layout/drafts |
| Pump/autofocus/commissioning | migration ABI 10/11, PR #320 | Existing services A / B adapter | Existing bindings | Existing controls | Commissioning guards | Existing test files | Preserve; no milestone expansion |
| Native cross-shell experiment | #372 M4 | A contracts / B integration | No accepted revision | Required comparison | Not validated | No execution evidence | Native Linux/Windows setup and handoff |

## Convergence and minimum dependencies

The same ExperimentCoordinator paths contain incompatible authorities:
`backend::ExperimentCoordinator` on migration owns a worker, periodic flushing,
requestStop, finalize and shutdown. `backend::app::ExperimentCoordinator` on
reliability owns evaluateReadiness, generation-checked start, frozen activeRun,
and finish; finish explicitly leaves finalization to its caller. The reliability
facade lacks most migration command/query declarations (the facade diff removes
roughly 1,900 lines). Selecting either whole file is not convergence.

Agent A/maintainer must provide one committed handoff covering:

| Dependency request | Proposed narrow public contract | Owner | Gate / regression to enable |
|---|---|---|---|
| Readiness/start | Existing readiness/start/run types through facade, atomic generation validation | A | M3/M4 stale readiness then blocked→resolved Start |
| Finalization | Owned async Stop plus queryable exact terminal result/accounting; no Qt timer | A | M4 navigate/close during saving, HDF5 reopen |
| Configuration | Checked patch with baseline revision, persistence/apply/readback outcomes | A | M3/M4 external edit conflict and saved-not-applied |
| Frame identity/time | Immutable frame source/session/config IDs plus validity, timestamp unit/domain | A | M1/M4 source switch and overlay matching; unavailable until supplied |
| Recovery snapshot | Session + event watermark and retained operation/fault outcomes | A | M2 gap/snapshot race and timeout reconciliation |
| Reusable export | Reviewed #344 service operation/progress/cancel/partial result | A | M4 repeated export/cancel/close without another exporter |

These are contract requests, not fabricated implementations. Tests depending on
absent production contracts remain open, not passed via fixtures. Handoff must
name accepted SHA, executed commands/results, units, revisions, retention expiry,
operation/cancellation rules, and known gaps. No shared backend files are edited.

## Decision log

- 2026-09-07: separate clone/worktree/feature branch; preserve existing dirty
  local checkout and Agent A's branch. Build outputs stay in this worktree.
- 2026-09-07: PR #321 is reusable presentation source, but not yet cherry-picked.
  Its change-based freshness, unknown-as-zero metrics, and frontend-controlled
  collection are unsuitable under #372. No new dashboard prototype.
- 2026-09-07: first regression deliberately executes A pull, B indexed pull,
  A pixels through the existing client and an invoke-boundary fake. Baseline
  fails: expected [11,11,11,11], received [22,22,22,22]. No sleeps.
- 2026-09-07: choose a single versioned binary response, not retained tokens.
  Unknown backend identities/time semantics remain explicitly unavailable.

## Progress and acceptance

- [x] Refresh branch refs; read #372 and inspect PR #321/current frame path.
- [x] Inventory coordinator/facade differences and ownership dependencies.
- [x] Execute first deterministic regression against pre-fix client.
- [ ] Atomic packet implementation and bounded malformed/interleaving tests.
- [ ] M2 operation recovery and bounded ownership.
- [ ] M3 integrated workflow/layout and drafts.
- [ ] M4 accepted-backend native experiment and Qt comparison.

## Environment limits

Node/npm and frontend dependencies are available. Rust/Cargo, CMake, pkg-config,
and the GTK/WebKit native development stack are absent at inventory time.
System package setup failed on restricted UID/group operations. No native,
CTest, sanitizer, or Qt-free build result is claimed from frontend tests.
