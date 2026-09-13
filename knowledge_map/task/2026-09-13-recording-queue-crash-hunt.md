# Recording queue crash hunt — 2026-09-13

User requested deliberate crash hunting after issue #403. Continue from PR #404
commit 5d3140b, using Linux mock/backend and isolated queue tests; no hardware
settings or real recordings were touched.

## Confirmed failures (regression-first)

Build the new test against the pre-fix header:

```sh
c++ -std=c++17 -pthread -Iinclude -Itests tests/backend/hdf_write_queue_fault_test.cpp -o /tmp/mib-queue-crash-probe
/tmp/mib-queue-crash-probe throw
/tmp/mib-queue-crash-probe stop
```

- `throw`: injected writer exception is caught, but a subsequent exception from
  the error callback escapes the writer thread and calls terminate. Exit 134;
  stderr names the injected notification failure.
- `stop`: four callers enter Stop while a write is blocked, then the test releases
  the write. Concurrent access to the same std::thread join has undefined behavior;
  observed hang exits 99 with watchdog step `concurrent queue stops` after 10 s.
  Scheduling is adversarial/probabilistic, widened by a 10 ms held writer.

## Fix and contract

Serialize Stop's join through a separate mutex; do not hold the queue mutex while
joining. Contain error-callback exceptions on both worker-failure and submitting
thread overflow paths. Keep the original fatal error and failed Stop result;
never reinterpret a failed/partial drain as success. Callbacks must not stop or
destroy their queue. Queue ownership/destruction still requires callers to finish
using the object first.

New `backend.hdf_write_queue_fault` regression covers both failures, a non-standard
exception from the overflow callback, and 25 rounds of four concurrent Stop
callers. Each clean round asserts 16 accepted batches written exactly once and
all four Stop callers completed. All threaded paths have a 10-second watchdog.

## Scope limits

These are demonstrated public queue API failure mechanisms. Application-level
reachability of simultaneous Stop or a throwing notification callback has not
been established; do not attribute the original filmed Windows/LED incident to
them. No electrical timing or hardware conclusion follows.

## Validation

Baseline capture lifecycle, processing fault injection, storage destinations,
and pipeline lifecycle stress passed (4/4). Post-fix results recorded below.

Post-fix Release: 12/12 passed (queue contracts/faults, capture lifecycle,
processing faults, recording lifecycle/roundtrip, storage destinations,
pipeline stress, four issue-403 recording scenarios). TSan: 9/9 passed (queue,
recording lifecycle/roundtrip, pipeline stress, four issue-403 scenarios) with
existing suppressions and process-local `setarch x86_64 -R`. The standalone
fault regression also passed `-fsanitize=address,undefined` with leak detection
and halt-on-error. Docs, screenshot-manifest and whitespace checks passed.
