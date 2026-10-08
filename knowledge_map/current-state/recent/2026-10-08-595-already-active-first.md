## 2026-10-08 — A start during an active run is AlreadyActive without taking the lock (#595)

`backend.experiment_readiness` failed now and then at "AlreadyActive while running" (Windows CI,
attempt 1 of the first v1.2.0 build). The staleness check was not the cause: the active-state check
already came before it. `ExperimentCoordinator::start` took the mutex with `try_lock` first and
answered `Busy` when another thread (a status poll, the worker) held it for a moment, which is what
the log showed (no "start refused" line). `state_` is now atomic and written under the mutex as
before; `start()` answers `AlreadyActive` from it before the try-lock. A stress case (a thread polling
`status()` while 3000 starts with a stale generation are offered during an active run) fails every time
without the change and passes with it. See [[ExperimentCoordinator]].
