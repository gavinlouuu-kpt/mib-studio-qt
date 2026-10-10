## 2026-10-10 — SSD run download (#667): `GET /ssd/runs/{id}/records`, idle only, verified (bridge ABI 36)

Studio's HTTP service (same listener, token, Origin and Sec-Fetch-Site gate as `/files/download`; no new port, no change to the exposure) streams the raw records of one closed SSD
run: the stdout of `pzrec read RUN IMG <the same options as start/stop> --summary [--from F] [--count N]` (pz7035 #61, the bounce path, never `/dev/yofoblk0` directly), whole 59,392 B
records in order. The S1 runs listing stays the index; `?from=` and `?count=` select a window that must lie inside the run (refused with 416, never clipped). Headers: `Content-Length`
= records × 59,392 (the run table's `written`), `X-Run-Id`, `X-Record-Count`, `X-Record-Bytes`, `X-First-Record`. The PC converter (length, per-record CRC/framing, quarantine) is the next task.

**Idle only, two ways.** `ssd_export_begin` takes one lease for the whole download (before `pzrec read` is spawned, released after it has exited). It refuses (503 `BUSY`) while anything
records or is armed: Studio's own run, a Start in progress (`prepareRun` .. `startRun`), STOPPING, an open run, or a drain window that is not IDLE (`pzrec snapshot --window-only`, registers only,
no disk; a window that cannot be read refuses too). And while the lease is held `prepareRun` and the `storage.ssd` gate refuse with "export in progress", `refresh()` stands down (no pzrec call
beside the reader). One lease at a time (a second request is 409 at once); a lease also expires on its own (`max_seconds` = 2 × 25 s + 35 s + bytes / 5 MB/s, after the reader must be dead).
If a run starts behind a download anyway, `pzrec read` ends with ACTIVE and the response ends as an error.

**Verified, never silently short.** The route reads the first chunk before it answers, so a refusal before any byte (ACTIVE, HW, NO_SUCH_RUN, RUN_DELETED, ARGS, IO, BAD_STATE/WEDGED) is a proper
status (503 / 503 / 404 / 410 / 400 / 502 / 503) with `{"error", "pzrec": {"name", "code", "exit_code", "stderr"}}`; pzrec's own recovery in its open (closed_at_open_run) is its business and the
route takes no recovery action. After the first byte a failure can only end the transfer: pzrec's exit code, its `--summary` byte count, the bytes sent and records × 59,392 must all agree,
otherwise the response is aborted and the reason (code, name, bytes) is logged and put in the lease's release line. The **last byte of the body is held back** until that verdict, so even a
failure after the last record leaves the client short of its `Content-Length`. A reader quiet for 90 s, or beyond the lease bound, is terminated (504 before the first byte).

**Cancellation bound.** A client that disconnects gets its reader SIGTERM (pzrec ends between two disk commands with exit 3), then SIGKILL after 35 s; the lease is released only after the
process has exited. On the bounce path (this bundle) pzrec stops after at most one block command, about 25 s; a pzblk-backed IMG could wait up to the driver's 30 min admit timeout. Until then
Studio refuses a new Start ("export in progress").

**Bridge ABI 36**, two additive commands, called only by this route (not in the WebSocket table): `ssd_export_begin {run, from, count}` → `{ok, lease, run_id, records, bytes, max_seconds, argv}`
or `{ok: false, code, reason}` (`code`: BUSY, UNAVAILABLE, NO_SUCH_RUN, RUN_DELETED, NOT_OFFERED, BAD_RANGE; argv is a vector, no shell string), and `ssd_export_end {lease, bytes_sent, outcome}`
(idempotent). Tests: `ssd_record_test` (lease, both orders, window states, expiry, ranges), `crates/mib-bridge-server/tests/ssd_export.rs` (a fake pzrec behind the real route: streaming,
gate, refusals, every pzrec code, failure inside a record, counts that disagree, cancel with SIGTERM seen and lease released, SIGKILL of a reader that ignores SIGTERM, a second download refused,
exclusion both ways), `crates/mib-bridge/tests/contract.rs`. Not tested without the board: the real read rate (about 27 MB/s expected on the bounce path).
