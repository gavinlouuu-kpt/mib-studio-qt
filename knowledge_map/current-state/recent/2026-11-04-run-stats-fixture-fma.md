## 2026-11-04 — Run statistics parity fixture exact on arm64

`run_statistics_twin_over_the_parity_fixture` (`crates/mib-bridge/tests/review_bridge.rs`) failed
on every macOS arm64 Review bundle run. The parity fixture's deformability,
`0.02 + 0.003 * i`, rounds twice on x86-64 but once where clang fuses it into a multiply-add
(arm64), so the golden reply generated on Linux (`0.038000000000000006`) never matched the Mac's
`0.038`. `RunStatisticsFixture.cpp` now writes `(20 + 3 * i) / 1000.0`, one correctly rounded
division with the same result everywhere, and `contract/fixtures/run-quick-look-v1.json` was
regenerated from the bridge's reply (only the compared fields; its `/data/runs` paths kept).
Lesson for new fixtures: build test values from integers with one division, never a multiply-add.
See [[services/RunQuickLook]].
