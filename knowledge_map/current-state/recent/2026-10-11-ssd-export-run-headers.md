## 2026-10-11 — The SSD download is self-describing: run table in `ssd_export_begin` and `X-Run-*` headers (#691, additive inside bridge ABI 36)
`ssd_export_begin` also returns the run table entry (`run`: start_unix_ms, tick_hz, first_ticks, last_ticks, first/last_frame_id, seen, filter, written, client_tag, wall_source,
reason, size_bytes, exact_drops) and the route sends it as headers `X-Run-Start-Unix-Ms`, `X-Run-Tick-Hz`, `X-Run-First-Ticks`, `X-Run-Last-Ticks`, `X-Run-First-Frame-Id`, `X-Run-Last-Frame-Id`, `X-Run-Seen`,
`X-Run-Filter` (0 all, 1 any, 2 valid), `X-Run-Written`, `X-Run-Client-Tag`, `X-Run-Wall-Source`, `X-Run-Reason`, `X-Run-Bytes`, `X-Run-Exact-Drops` (1 when every frame id gap was a drain drop). They describe the
whole run, also for a `?from&count` window. Additive fields of the same two ABI 36 commands; `tools/pzrec_to_h5 fetch` needs no `--runs` file then.


(#688 P3s) The begin runs in a detached task and the handler holds it through a guard: a client that leaves before the answer arrives, or just after it was parked in the channel, gets its lease ended at once, not at the C++ expiry. `kExportKillMarginSeconds` (60 s) is static_asserted in C++ against 35 + 10 + 5 s and mirrored by a const assert in the route.
