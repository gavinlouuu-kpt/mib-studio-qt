## 2026-10-07 — BackendFacade detaches the experiment status callback on destruction

`~BackendFacade` now calls `experiment().setStatusCallback({})`. The callback captures the facade, and the coordinator outlives it. If a test or client destroyed the facade without calling `shutdown()`, `AppBackend::shutdown()` would finalize the live run and publish into freed memory. `setStatusCallback` waits out in-flight invocations (#538), so after destruction nothing reaches the dead facade.

Found as a ThreadSanitizer heap-use-after-free on PR #539: `e2e.experiment_coordinator` timed out silently on a slow runner and returned early. Regression: the "Facade destroyed while a run is live" block in `tests/backend/experiment_coordinator_test.cpp` fails with return 75 without the fix.
