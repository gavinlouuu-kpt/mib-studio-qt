## 2026-10-11 — SSD export slot passed on the board (bundle 2e215049, runs 18 and 19) and the three #690 converter nits
The export slot ran the export bundle (Studio server 365d58bc, pzrec = the S2 binary) on the PZ7035: `pre` → `fetch` → `post`, downloads through the tunnel, then the host-side checks (`export_check.py`, 21/21 PASS).

| Run | Records | Bytes | Download | Rate | Converter | Rows valid / invalid |
|---|---|---|---|---|---|---|
| 18 | 27,119 | 1,610,651,648 | 138.32 s | 11.64 MB/s | rc 0, 15.2 s | 48,020 / 6,215 |
| 19 | 10,892 | 646,897,664 | 55.73 s | 11.61 MB/s | rc 0, 8.7 s | 14,567 / 7,217 |

- **Exclusion:** a Start attempted from Studio while run 18 was downloading was refused (`storage.ssd: export in progress: a download is reading the SSD — wait for the download to finish`, readiness reply `not ready: instrument.mode, storage.ssd`); a second concurrent GET got 409 BUSY. The instrument was never in Run.
- **Rate:** 11.6 MB/s over the tunnel against 28.2 MB/s for the board-side read: the route/HTTP/tunnel path costs more than half. Not yet explained (issue filed, no work started).
- **Cross-check ("ring-vs-SSD", reference windows + headers):** the three S2 reference windows are byte-identical slices of the downloads; all 27,119 run-18 FRAME headers equal the S2 `run18.heads.bin` (index, epoch, frame_id, ticks, drops_before); record counts, first/last frame ids and ticks equal the run table; frame ids strictly ascending, ticks monotonic, one epoch per run (5, 7), `drops_before == min(gap, 255)` for every record. A live-ring comparison is deferred: the S2 ring was RAM and is gone; it needs a slot where Studio's playback is read back (M1 gate 5 follow-up).
- **Converter nits (#690) fixed:** a cell of an EMPTY/INVALID frame now has `isValid = inRange = 0` (it already sat in `/invalid_frames`); a 0-record window and a response without Content-Length get their own error texts; the README says a garbled `X-Run-*` value is only seen after the body was downloaded (the `.bin` is kept, not quarantined, and no report is written; re-run `convert` with `--runs`).
