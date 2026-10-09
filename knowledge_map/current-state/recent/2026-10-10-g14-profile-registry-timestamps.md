## 2026-10-10 — Profile registry and cache rows are stamped with the synced wall clock (#651 G14 follow-up)

`ProfileCache` no longer uses SQLite's `CURRENT_TIMESTAMP` when it writes: revision `downloaded_at`/`synced_at`, validation `validated_at` and draft `created_at`/`updated_at` are bound from
`WallClock` (#671), in the same UTC `YYYY-MM-DD HH:MM:SS` format, so old and new rows still sort together. The PZ7035 has no RTC, so the board's own clock reads whatever the boot left.
`ProfileRegistryWorker` takes `confirmed_at_utc` and the published `lastSuccessfulRefresh` from the same clock. Without a client sync the system clock is used, as before. Token expiry
stays on the system clock (it is relative to "now"). Test: `profiles.cache_wall_clock`.
