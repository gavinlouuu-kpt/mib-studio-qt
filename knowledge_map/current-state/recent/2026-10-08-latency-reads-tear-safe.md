## 2026-10-08 — Latency readout reads each word until two reads match

`report_cdc` on results9/10 shows the latency monitor words (S[47..56]) cross from the 175 MHz domain
to the host clock unsynchronised, so a read can tear. `PzPlatformMonitor` now reads the latency
words it shows (last, max, over budget, frames) until two consecutive reads match, as it already
does for the RXH1 auto-reset count. A stopgap until a PL fix (results11). See [[PZ7035-Records]].
