# ADR 0004 — Bridge contract governance and serialized operation state

- Status: accepted
- Date: 2026-07-16
- Issue: BE-1 (#271), epic #246
- Extends: [ADR 0003](0003-rust-cxx-bridge.md)

## Context

The bridge command/event surface is represented in four places: the C++
`BackendFacade` types, the `cxx` FFI schema in `crates/mib-bridge/src/lib.rs`,
the Tauri command layer, and the TypeScript client. ADR 0003 fixed the
transport rules (poll-drained events, binary frame path); it did not fix how
the *identities* (enum values, event kinds, error sources) stay in sync, how
long-running actions are tracked, or what happens when the event queue is not
drained. BE-2…BE-9 multiply every one of those surfaces, so the foundation
must come first.

## Decision

### 1. One machine-checked contract

`crates/mib-bridge/contract/bridge-contract.json` is the single source of
truth for the ABI version and every cross-language enum (event kinds, command
types, error sources, operation kinds/states, camera/recording states).

Each language ties to it mechanically, and drift fails CI:

| Layer | Mechanism |
|---|---|
| C++ | `static_assert`s in `crates/mib-bridge/src/shim.cpp` pin enum values and the `BackendEvent` variant order — drift fails the build |
| Rust | `rust_enums_match_contract_json` in `crates/mib-bridge/tests/contract.rs` parses the JSON and asserts the `cxx` enum values |
| TypeScript | `desktop/src/bridgeContract.ts` is generated from the JSON; `python3 scripts/gen_bridge_contract.py --check` fails the desktop CI lane when stale |

### 2. Additive-only evolution

Values are append-only: never renumbered, never repurposed. New event kinds,
command types, error sources, and DTO fields append. Consumers must treat
unknown kinds/fields as ignorable ("Unknown"), never as fatal. The ABI version
(`bridge_abi_version()`, `abi_version` in the JSON) bumps whenever the public
command/event surface changes.

### 3. Serialized operation state

Long-running actions are tracked operations with an explicit lifecycle owned
by `BackendFacade`:

- `beginOperation(kind)` allocates a monotonic non-zero `operationId`, emits
  `OperationStatus(Started)`, and hands the runner a shared cancel flag.
- Progress is reported with `reportOperationProgress` (`Progress` events).
- Exactly one terminal state per operation: `Completed`, `Failed`,
  `Cancelled`, or `TimedOut`. `finishOperation` is idempotent per ID — the
  first terminal state wins and later reports are dropped (late-event policy).
- Commands that start an operation return its `operationId` in the command
  result so the shell can correlate events and cancel.
- `cancel_operation(id)` requests cancellation; it fails safely (`ok=false`)
  for unknown or finished IDs. Runners observe the flag at their own
  boundaries; timeouts are enforced by the runner and reported as `TimedOut`.
- `BackendFacade::shutdown()` cancels every active operation (flag + a
  `Cancelled` event) before tearing services down, so no operation outlives
  the facade silently and no callback fires after destruction.

Duplicate commands must be idempotent or fail without desynchronizing state
(covered by `duplicate_start_stop_cannot_desynchronize`).

### 4. Bounded event queue

The shim's event queue is bounded (default 4096, `MIB_BRIDGE_MAX_QUEUE`
override, floor 4) with **drop-oldest** coalescing: under a stalled poller the
newest state survives, memory does not grow, and the loss is observable — the
next `poll_events` batch is prefixed with a synthetic `QueueOverflow` event
(`u0` dropped since last poll, `u1` dropped total) and
`queue_overflow_total()` exposes the running counter.

### 5. Binary payloads stay out of events

Unchanged from ADR 0003, restated as contract: pixel/mask/chart payloads move
through dedicated binary pulls (`frame_bytes` today); events and command
results carry metadata and handles only. New error sources (`Experiment`,
`Monitoring`, `Hardware`, `ConfigCore`, `Review`, `Export`, `Platform`) exist
so every migrated workflow reports structured failures.

## Consequences

- BE-2…BE-9 add their commands/events by appending to the JSON contract and
  the pinned enums; CI catches forgetting any of the three mirrors.
- The Qt-era pattern of ad-hoc long-running work is replaced by one
  operation-lifecycle primitive that BE-4 (experiment) and BE-6 (export/
  reanalysis jobs) build on.
- A slow or frozen webview can no longer grow backend memory unboundedly; it
  sees an explicit overflow marker instead of silent loss.
