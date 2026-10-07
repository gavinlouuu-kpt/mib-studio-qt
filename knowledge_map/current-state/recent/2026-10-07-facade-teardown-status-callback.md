## 2026-10-07 — BackendFacade detaches the experiment status callback on destruction

`~BackendFacade` now calls `experiment().setStatusCallback({})`. The callback captures the facade, and the coordinator outlives it. If a test or client destroyed the facade without calling `shutdown()`, `AppBackend::shutdown()` would finalize the live run and publish into freed memory. `setStatusCallback` waits out in-flight invocations (#538), so after destruction nothing reaches the dead facade.

Found as a ThreadSanitizer heap-use-after-free on PR #539: `e2e.experiment_coordinator` timed out silently on a slow runner and returned early. Regression: the "Facade destroyed while a run is live" block in `tests/backend/experiment_coordinator_test.cpp` fails with return 75 without the fix.

The timeout itself was the test's "frames keep flowing after a refused recording" wait. It required the buffered count to exceed its earlier value, but the blank mock frames are all invalid and only every 100th is buffered. The background flush drains that, so on a slow TSan runner the count sits at 0..1 (#539 twice, #555). The wait now uses persistence-admitted plus buffered, which only grows. Under `CPUQuota=50%` the old predicate fails and the new one passes 3 of 3.
