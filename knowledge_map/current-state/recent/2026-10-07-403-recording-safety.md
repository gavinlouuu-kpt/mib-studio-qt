## 2026-10-07 — Re-port recording safety to develop (#403)

Ported incomplete multi-image Stop handoff, pressure wakeups, destination
roundtrip and host buffer feasibility gates, stopped-queue rejection,
serialized joins, and callback exception containment. Failed-write accounting
survives queue destruction and fatal outcomes are persisted using #589's
backend approach. Kept #407's existing byte-pressure and time-backstop policy.
Condition-driven `e2e.recording_403_*` regressions cover partial series with
capture running/stopped and restart, plus the already-landed byte-pressure fix.
See [[architecture/ExperimentCoordinator]], [[services/ProcessingService]],
[[services/Hdf5Service]], and [[architecture/AppBackend]].

Validation: eight focused tests passed; seven readiness/queue/e2e scenarios
also passed 20 consecutive runs each. Disabling the Stop handoff reproduced
a falsely Complete run with zero persisted series; the stopped-queue
regression also failed on the unfixed header. The full offscreen suite ran
226 cases: 216 passed, seven skipped, and three unrelated local-HTTP tests
failed/timed out under socket restrictions (`backend.profile_catalog`,
`frontend.registry_http_transport`, `backend.crash_reporter_pending_upload`).
The standalone adversarial queue test passed with TSan in a non-PIE binary;
other TSan starts failed in this host runtime, so the full TSan lane remains
unverified. No hardware or external destination throughput was qualified.
