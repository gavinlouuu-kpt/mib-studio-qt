## 2026-10-19 — Start outcome code and expected run on experiment_stop (#741)

Bridge ABI 40, additive. **Reply outcome:** every `experiment_start` reply the backend evaluated
carries `start_outcome` (an `experiment_start_outcomes` value, JSON number) and every
`experiment_stop` / `experiment_cancel` reply `stop_outcome` (an `experiment_stop_outcomes` value),
through C++ `BackendCommandResult` → cxx `BridgeCommandResult` (`has_start_outcome` /
`start_outcome`, `has_stop_outcome` / `stop_outcome`) → `CmdResult` (Tauri and the WebSocket server)
→ `eventAdapter.ts` `CmdResult`. Other replies omit both fields; messages are unchanged, so a refused
start request is `start_outcome: 7` (`InvalidRequest`) and still reads `invalid start request: …`.
**Expected run:** `experiment_stop` takes an optional `expectedStartGeneration` (a canonical decimal
string; strict, anything else is an error and nothing is sent). The coordinator compares it with the
run it holds under the mutex that accepts the stop: another run (active or finalizing) or none is the
new stop outcome `RunMismatch` (3) with a plain reason (`stop refused: expected run E, but run N is
active; nothing was stopped`) and nothing changes; a match behaves as before, and so does a stop
without it. A stale generation cannot stop a newer run (the accepted stop is consumed by the run it
named; no run starts before that finalization ends). The develop shell is unchanged (it neither
parsed the prefix nor passes a generation); the workspace queue and Plan switch separately on
`feat/egrabber-workspace`. See [[architecture/Rust-Bridge]], [[architecture/ExperimentCoordinator]],
[[architecture/Desktop-Shell]]. Guards: `e2e.experiment_stop_generation` (every start outcome, the
stop semantics, a stress test of stale stops racing start/stop cycles), `contract.rs`
`experiment_lifecycle_end_to_end` and `experiment_command_reply_and_stop_request_are_described`,
`dispatch.rs` `experiment_outcomes_and_expected_generation_through_dispatch`, `ws.rs`
`experiment_stop_names_its_run_over_the_websocket`, `eventAdapter.test.ts`, `eventTransport.test.ts`.
