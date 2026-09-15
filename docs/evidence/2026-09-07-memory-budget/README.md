# Byte-budgeted ownership evidence (issue #370)

Captured 2026-09-07 on the Linux backend-only environment (4 threads,
OpenCV 4.13, bundled processing core 0.2.1) with
`MIB_MEMORY_BENCH_REPORT=… ./build/linux-backend/memory_budget_bench`
(`performance.memory_budget`, ~2 s wall). Synthetic Mono8 frames, 512×96
unless stated; RSS from `/proc/self/status`. Raw numbers:
`memory-budget-bench.json`.

| Scenario | Gate | Result |
|---|---|---|
| A. per-object copies (`processBatch`, 200 frames × {0, 1, 8} objects) | objects of one frame share the source + mask; ≤ 1 source and 1 mask allocation per frame | 0 clones per object (allocations per result 1.0 / 1.0 / 0.83; the former clone-per-object cost is reported alongside) at 0.22–0.42 ms/frame |
| B. slow persistence consumer (20 000 frames, never flushed, 64 MiB budget) | peak ≤ budget, every frame stored or reported evicted | peak 63.9 MiB, 682 retained, 19 318 evicted (all accounted), 2.7 µs/append, RSS +36 MB |
| C. larger frames (2048×1088, 4.4 MB/frame, 256 MiB budget, 1000-frame cap) | byte budget bounds before the frame cap | 60 frames retained (cap would allow 1000), 240 evicted, RSS +198 MB |
| D. long monitoring session (6000 frames, rings never read) | bytes at 6k == bytes at 3k | 98.3 MB at both samples, 1000 entries, 5000 replacements, ~4400 frames/s |
| E. FrameStore ring, EveryFrame consumer then drop-to-latest (512 slots, 16 000 pushes) | retained bytes constant | 25.2 MB plateau in both modes, peak == current |
| F. 1M-frame metadata stress (`RecordingAccounting`) | exact, bounded | 13 ns/frame, RSS +0 MB, 1 000 000 admitted with zero sequence gaps |
| G. multi-image series (count 4) | series retention ≤ multi_image_count | max 4 images per result |

Process RSS over the whole run: 59.8 MB → 222.8 MB (peak 330 MB), the
sum of the budgets exercised, not growth with input size.

Deterministic policy coverage (accountant semantics, eviction order,
takeAll, FrameStore reserve/plateau/resize, batch-queue byte budget drops,
per-object sharing with identical science, realtime plateau + experiment
byte budget reconciled with the run accounting) is `processing.memory_budget`
(`tests/processing/memory_budget_test.cpp`), also run under ThreadSanitizer.

Re-run with:

```bash
ctest --test-dir build/linux-backend -R 'processing.memory_budget|performance.memory_budget' --output-on-failure
MIB_MEMORY_BENCH_REPORT=$PWD/docs/evidence/2026-09-07-memory-budget/memory-budget-bench.json ./build/linux-backend/memory_budget_bench
```

Not yet exercised here: hardware camera SDK buffer counts (the
`capture.sdkBuffers` owner is reported as *unknown* on this mock-only
environment) and the Windows RSS helper.
