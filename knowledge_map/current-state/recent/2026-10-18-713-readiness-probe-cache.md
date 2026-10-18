## 2026-10-18 — Readiness no longer writes to the destination on every evaluation (#713)

Every readiness evaluation with an output path created and removed a `.mib_write_probe_*` file next
to the target (`storage.output`), and the HDF5 round trip of `storage.roundtrip` ran again every 30 s.
Polling readiness from Prepare would have churned the SSD. Both probes now keep a pass per (output
path, readiness generation): repeated evaluations with the same path and generation create no file,
and a path change (or anything else that bumps the generation) probes again. A failed probe is not
kept, so a destination the operator fixes passes at the next evaluation without a path change. The
metadata checks of `storage.output` (parent a directory, path not a directory, 64 MiB free) write
nothing and still run every time. Gate ids, statuses and messages are unchanged. Only finding 5 of
#713; the Prepare UI changes are a later step. See [[architecture/ExperimentCoordinator]]. Guard:
`backend.readiness_probe_cache`.
