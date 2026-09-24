Title: Bridge contract source of truth + operation state + bounded queue (BE-1, issue #271)

Context:
- First backend slice of the React/Tauri issue breakdown (#266–#279, epic
  #246). Establishes the contract/state foundation the remaining BE slices
  build on. Design record: `docs/decisions/0004-bridge-contract-and-operation-state.md`.

Implementation Notes:
- **Contract source of truth** — `crates/mib-bridge/contract/bridge-contract.json`
  pins the ABI version and every cross-language enum. C++ ties to it via
  static_asserts in `crates/mib-bridge/src/shim.cpp` (including the
  `BackendEvent` variant order), Rust via
  `rust_enums_match_contract_json` in `crates/mib-bridge/tests/contract.rs`,
  TypeScript via the generated `desktop/src/bridgeContract.ts`
  (`scripts/gen_bridge_contract.py`, `--check` wired into
  `.github/workflows/desktop-ci.yml`).
- **Operation state (facade)** — `BackendFacade` gains
  `beginOperation/reportOperationProgress/finishOperation/requestOperationCancel`
  with a registry of active operations and shared cancel flags.
  `OperationStatusEvent` (id, kind, state, progress/total, message) appended
  to `BackendEvent`; `OperationCommand{Cancel}` appended to `BackendCommand`;
  `BackendCommandResult.operationId` added. RecordingLoad is the first tracked
  operation. `shutdown()` cancels all active operations before service
  teardown. First terminal state wins; late finishes/progress are dropped.
- **Bounded queue (shim)** — the event queue is drop-oldest bounded
  (default 4096, `MIB_BRIDGE_MAX_QUEUE` override, floor 4). Overflow is
  observable: poll batches are prefixed with a synthetic `QueueOverflow`
  event (u0 dropped-since-last-poll, u1 total) and `queue_overflow_total()`.
- **Error sources** — appended `Experiment, Monitoring, Hardware, ConfigCore,
  Review, Export, Platform` to `BackendErrorSource`.
- **ABI v4** (additive). Tauri commands `cancel_operation`,
  `queue_overflow_total`; `CmdResult.operation_id`; UI logs operation
  failures/cancellations and queue overflows, warns when the bridge ABI is
  older than the UI contract.

Verification:
- `cargo test` (mib-bridge): 9 tests including duplicate start/stop safety,
  cancel-unknown fail-safe, recording-load operation lifecycle, bounded-queue
  overflow observability, JSON↔Rust contract pinning.
- `cargo test` (desktop): 4 tests incl. unknown-event-kind fail-safe.
- `npm run build`, `gen_bridge_contract.py --check`, `check_docs.py` green.
- Backend CTest lane green.

Follow-ups:
- BE-4 (#274) experiment coordinator and BE-6 (#276) export/reanalysis jobs
  run on this operation-lifecycle primitive.
