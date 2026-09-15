# Lossless desktop event JSON v1 (bridge ABI 13)

The cxx ABI and desktop JSON protocol are versioned independently from science.
ABI 12 appends exact integer companions to BridgeEvent while preserving every
legacy slot, enum value, ring metric and Contract-v1 processing meaning:

| Native field | Legacy slot retained | Exact companion |
|---|---|---|
| Experiment endTimeNs | f0 | experiment_end_time_ns |
| Experiment droppedValid | f1 | experiment_dropped_valid |
| Experiment droppedInvalid | f2 | experiment_dropped_invalid |
| Frame byteSize | f0 | frame_byte_size |

The production C++ converter fills both representations. The desktop consumes
only the exact companions for integer data. Public backend/domain files are
unchanged. This is a transport ABI extension, not a second experiment authority.

`poll_events_exact` returns `{transport_version:1, events:[...]}`. Unsigned
slots and companions are canonical unsigned decimal strings through u64::MAX.
Command results, experiment snapshot integers, queue-overflow totals, seek
indices and cancellation IDs use the same representation. Requests reject
already-rounded JavaScript numbers. The old poll_events command returns
EVENT_PROTOCOL_UPGRADE_REQUIRED; an old shell cannot silently decode a new
representation. ABI negotiation refuses mismatched frontend/backend builds.

`eventAdapter.ts` is the sole generic-slot interpreter. Components receive named
discriminated event variants. The machine-readable mappings live in the existing
bridge-contract.json and generated TypeScript mirror. Unknown optional fields
are ignored. An unknown event kind produces a reconciliation-required notice;
unknown required enums, malformed identities, incompatible versions and invalid
payload types fail closed. Neither behavior invents successful run state.

## Metrics and truth

Non-finite processing values are explicitly null. Reported zero remains zero;
null is never coerced to zero. The adapter records unavailable freshness because
these snapshots do not contain an accepted sample clock/validity contract.
Finite means reported, not scientifically verified. The legacy timestamp_ns
values remain exact but their clock/unit verification is unavailable. React no
longer subtracts such a timestamp from Date.now() to invent elapsed time.

Command ok is acceptance according to that command's native contract, not proof
of operation completion. The matching operation notification reports its own
state. Notification loss is reported separately from camera/data loss.
Acknowledging/printing a notification cannot resolve a fault. Recovery snapshots,
watermarks, request deduplication and retained terminal outcomes still require
the accepted Agent A facade handoff. This slice does not implement recovery by
caching notifications or synthesizing terminal results.

Experiment Start remains disabled with an explanation while the staging backend
lacks the accepted authoritative readiness seam. Connection is insufficient.
Idle plus saved rows no longer marks a workflow Complete. The backend needs an
explicit retained terminal outcome. The raw Review slider refuses indices above
JavaScript's exact integer range; the exact identities remain visible and seek
commands accept them losslessly. Full bounded Review navigation remains M3 work.

## Bounds

The native event count override is clamped to 4..4096 entries. A poll may include
one additional overflow marker. The JSON boundary caps each text detail at 4096
UTF-8 bytes, preserving character boundaries and adding text_truncated. The
frontend refuses more than 4097 events or oversized text. The bound applies to
JSON output; native input-string byte budgets, coalescing and current-fault
retention are still M2 work. It does not imply lost acquisition data.

## Tests and production isolation

A test-only `contract-fixtures` Cargo feature feeds domain events/frames through
the **production** C++ converter. Rust serialization and frame encoding compare
against committed JSON/binary-hex golden fixtures; TypeScript decodes those same
fixtures. Cases include 2^53+1, u64::MAX, non-finite metrics, and background-source
identity. The feature is a desktop dev-dependency only. There is no Tauri fixture
command, demo fallback, service access shortcut or fake experiment success path.

The desktop native tests execute the C++→cxx→Rust→JSON/binary producer checks;
frontend tests execute JSON/binary→typed adapter checks. This is cross-language
contract evidence, not a native webview experiment workflow.

The prior frame PR's Desktop CI run 34085079050 passed on remote commit
0e2d92b7d097997ef62c5944e948cfda5e08b01e: Qt-free archives, Tauri build,
12 desktop Rust tests, frontend build/tests, drift check and Xvfb smoke.
The logs include legacy missing-HDF5-attribute diagnostics; no provenance
completeness is inferred from those tests. Full CTest/Qt/native E2E, scientific
comparison, Windows, sanitizer and resource acceptance remain open.

## ExperimentStatus since ABI 13 (shared backend, issue #372)

The experiment lifecycle is owned by the shared `ExperimentCoordinator`;
the bridge carries its status on the legacy slots plus exact typed
companions, all present on every event (zero/false for other kinds):

| Field | Meaning |
|---|---|
| `u0` | `experiment_states` value |
| `u1`, `u2` | valid / invalid frames buffered (live) |
| `u3` (`validSaved`) | persistence committed |
| `u4` (`invalidSaved`) | always `0` (the saved split is no longer tracked) |
| `u5` | start wall-clock ns |
| `experiment_end_time_ns` (`f0`) | end wall-clock ns |
| `experiment_dropped_valid` (`f1`) | persistence pending = admitted − committed − failed |
| `experiment_dropped_invalid` (`f2`) | persistence failed |
| `experiment_start_generation` | run identity (decimal string) |
| `experiment_persistence_admitted` / `_committed` / `_failed` | exact persistence terms |
| `experiment_completion` | `run_completion_states` value; `Unknown` until `experiment_terminal` |
| `experiment_terminal` | finalization finished (Idle or Failed); the outcome is final |
| `experiment_finalization_ok` | every finalize step succeeded |

`fetch_experiment_status` returns the full status (generations, completion
reason, fault code/message) and `fetch_experiment_readiness(outputPath)` the
gate list (`readiness_gate_statuses` values; Fail and Unavailable block) with
the generation a Start must present. `experiment_start` evaluates readiness
itself and presents that generation; the backend refuses a stale one.
