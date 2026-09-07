# FrameStore

> Thread-safe ring buffer for in-memory frame history. Shared by
> [[../services/CaptureService]] (producer), [[../services/ProcessingService]]
> (realtime consumer), and [[../services/PlaybackService]] (UI consumer).

**Source:** `src/backend/playback/FrameStore.cpp`,
`include/backend/playback/FrameStore.h`
**Related:** [[../architecture/AppBackend]], [[../services/PlaybackService]]

## Capacity

- Header default constructor: `FrameStore(capacity = 512)`.
- **Actual runtime capacity**: `5000`, set in
  `AppBackend::initialize` (`src/backend/AppBackend.cpp:97`).
- Resizable at runtime via `resize(newCapacity)`.

## Frame type

```cpp
struct Frame {
    uint64_t width, height;
    uint64_t pixelFormat;   // PFNC code
    size_t linePitch;       // bytes per line
    uint64_t timestamp;     // device ticks OR nanoseconds (source-dependent)
    uint64_t hostTimestampUs; // host monotonic acquisition stamp (0 = unknown)
    std::vector<uint8_t> data;
};
```

`hostTimestampUs` is stamped by [[../services/CaptureService]] at grab time
and carried through `pushFrame`/`getByWriteIndex[ROI]` so consumers can
measure acquisition→stage latency on one clock
([[../diagnostics/PipelineTimingRecorder]]).

## Absolute indexing

Write-index is monotonic (never wraps). Consumers must query the current
window:

- `earliestAvailableIndex()` / `latestAvailableIndex()`
- `availableCount()`, `totalWritten()`
- `getAvailableRange() → IndexRange { start, end }`
- `getAvailableTimestampRange(TimestampRange&)`

## Push / query APIs

```cpp
void pushFrame(src, size, w, h, linePitch, pixelFormat, timestamp,
               hostTimestampUs = 0);

// Event-driven consumer wake (issue #282): block until totalWritten()
// exceeds lastSeenTotal, a frame is pushed, or timeout elapses.
// pushFrame notifies after the slot copy via a Dekker-guarded waiter
// counter (one relaxed load per push while nobody waits, no lost
// wakeups). Replaced the realtime loops' fixed 2 ms sleep-poll, which
// put a uniform 0-2 ms wait in front of every frame.
uint64_t waitForFrame(uint64_t lastSeenTotal, std::chrono::microseconds timeout);

bool getLatest(Frame& out) const;                 // latest COMMITTED frame
bool getByWriteIndex(uint64_t idx, Frame& out) const; // wrapper: Available only
FrameReadOutcome readByWriteIndex(uint64_t idx, Frame& out) const; // typed (issue #367)
uint64_t committedCount() const;   // publication count (<= totalWritten())
uint64_t latestCommittedIndex() const;
void setCommitHookForTests(std::function<void(uint64_t)>); // barrier between reserve and commit
bool getByWriteIndexROI(uint64_t idx, int roiX, int roiY, int roiW, int roiH,
                        Frame& out) const;  // avoids full-frame copy

bool saveFramesToDisk(dir, filterFn = nullptr) const;       // all frames (TIFF)
bool saveFramesToDisk(dir, startIdx, endIdx, filterFn);     // by index
bool saveFramesToDisk(dir, startTs, endTs, /*useTs=*/true, filterFn);

// Single-file uncompressed AVI export. fps only affects playback
// metadata, not the captured frame rate. Per-frame timestamps are
// NOT preserved in AVI.
bool saveFramesToAvi(path, fps = 30.0, filterFn = nullptr) const;
bool saveFramesToAvi(path, startIdx, endIdx, fps, filterFn);
bool saveFramesToAvi(path, startTs, endTs, /*useTs=*/true, fps, filterFn);

bool resize(size_t newCapacity);
size_t estimateMemoryBytesForCapacity(size_t capacity) const;
// Issue #370: measured retained bytes (slot allocations, resident even
// when stale), peak, retained frame count, capacity x reserved frame
// bytes as the declared bound, overwrites as evictedByBudget. Lock-free.
backend::diagnostics::MemoryOwnerStats memoryStats() const;
```

### AVI codec choice

`saveFramesToAvi` tries `fourcc('Y','8','0','0')` (single-channel Mono8)
first. If the backend can't open that codec it falls back to
`fourcc('D','I','B',' ')` (uncompressed BGR) and per-frame
`cv::cvtColor(GRAY2BGR)`. The fallback triples file size. Which path
actually runs depends on the platform's OpenCV backend (FFmpeg / VFW /
MSMF) — check the log line `"Writing AVI ... ({Y800/GRAY|DIB/BGR})"`.

Y800 is compact and bit-exact but **not playable in Windows Media
Player / Movies & TV** — those consumer decoders don't support 8-bit
grayscale AVIs and can crash trying. Confirmed to play in **VLC**, and
it round-trips cleanly through `cv::VideoCapture` and ImageJ, so mask
regeneration is unaffected. Use VLC (or ImageJ) to preview saved
buffers visually.

## Publication contract (issue #367)

`pushFrame` **reserves** the next index first (`totalWritten()` advances, the
`CrashStateMirror` mirrors it) and only after the image + metadata copy under
the slot lock **commits** it (`committedCount()` advances, release-ordered).
With the single capture producer, commit order equals reservation order, so
every index below `committedCount()` is readable unless evicted.
`readByWriteIndex` returns one of `Available` (slot holds exactly this index,
usable geometry), `NotYetCommitted` (index reserved or beyond the reservation
but not published; also the slot still holding the *previous* occupant), 
`Overwritten` (evicted: a newer frame occupies the slot), `Malformed` (zero
geometry / pitch < width / payload shorter than `(h-1)*pitch + w`), or
`OutOfRange` (empty store). `getLatest()` uses `latestCommittedIndex()` and the
same identity check, so it can never expose a slot whose copy is in progress;
`getByWriteIndex` is `readByWriteIndex(...) == Available`. Consumers that must
account for every index (the raw-recording loop, experiment accounting) switch
on the outcome: `NotYetCommitted` → wait/retry without claiming the index;
`Overwritten`/`Malformed` → claim and count as store loss. Guarded by
`tests/backend/frame_store_commit_test.cpp` (`backend.frame_store_commit`),
which uses `setCommitHookForTests` as a deterministic barrier inside the
reserve→commit window.

## Threading

Two-tier locking so the producer and consumers don't serialise on one lock
(the previous single-`std::mutex` design forced capture and every reader to
take turns *while holding the lock across the full-frame `memcpy`*, which
throttled both capture and the realtime pipeline — see
[[../current-state/Recent-Work]]):

- **`structureMutex_`** (`std::shared_mutex`) guards the *identity* of the
  ring (`ring_` / `slotMutexes_` / `capacity_`). The hot path (`pushFrame` /
  `getLatest` / `getByWriteIndex` / `getByWriteIndexROI`) takes it in
  **shared** mode; only whole-ring ops (`resize`, `saveFrames*`,
  `estimateMemoryBytesForCapacity`) take it **exclusive**.
- **`slotMutexes_`** — one `std::mutex` per ring slot. The actual frame copy
  in/out is done holding only that slot's lock, so the producer writing slot
  A never blocks a consumer reading slot B. Producer vs. consumer on the
  *same* slot (a wrap-around overwrite of the frame being read) is still
  serialised — correctness preserved.

`resize` rebuilds `slotMutexes_` alongside `ring_` under the exclusive lock;
this is safe because every hot-path op acquires the shared structural lock
*before* any slot lock, so no slot lock is held when `resize` runs.

## Gotchas

- `getByWriteIndex` / `getByWriteIndexROI` reject indices that are too new
  (`idx >= totalWritten`) **and** indices already evicted from the ring
  (`idx < totalWritten - capacity`). The eviction check is essential: without
  it, `idx % capacity` aliases to a live slot holding a *newer* frame and the
  call would silently return the wrong frame instead of failing. All callers
  treat `false` as "skip this frame" (`continue`) or clamp to the available
  range first, so rejecting is safe. Guarded by
  `tests/backend/frame_store_bounds_test.cpp` (`backend.frame_store_bounds`).
- **Identity is re-verified under the slot lock** via `slotWriteIndices_`
  (parallel to `ring_`; written by `pushFrame` after the copy, remapped by
  `resize`). The snapshot eviction check above races the producer two ways:
  a wrapping producer can overwrite the slot between check and lock (newer
  frame under the requested index), and because `totalWritten_` is
  incremented *before* the slot data is copied, a reader can also lock the
  slot before the producer and see the previous occupant. One
  `slotWriteIndices_[idx] != writeIndex → false` comparison closes both, so
  success now guarantees the returned frame IS the requested write index.
  Guarded by the identity assertions in
  `tests/backend/frame_store_concurrency_test.cpp`.
- `getByWriteIndexROI` validates `data.size() >= (h-1)*pitch + w` before its
  strided row walk (a producer-supplied `linePitch > width` with a
  tightly-sized buffer would otherwise read past the vector); the AVI/TIFF
  export paths use the same pitch-aware bound. Guarded by
  `frame_store_bounds_test`.
- If a consumer holds onto a `getByWriteIndex` copy for longer than
  `capacity / fps` seconds, its data is still valid (copy) but the index
  may fall off the available range.
- `capacity_` is `std::atomic<size_t>`. The lock-free guard reads
  (`earliest/latestAvailableIndex`, `availableCount`, and the top-of-function
  `capacity_ == 0` checks in `getLatest`/`getByWriteIndex`) cannot take
  `structureMutex_` because `resize()` calls them while holding it exclusively —
  so atomicity, not a lock, prevents the data race against `resize()`'s write.
  Surfaced by the ThreadSanitizer lane via `frame_store_resize_under_load_test`.
- `pushFrame` copies bytes. For high-speed capture, call
  `reserveFrameBytes(frameBytes)` at capture start ([[../services/CaptureService]]
  does this on the first frame / on geometry growth) so the first pass through
  the ring does not allocate — without it the first `capacity` pushes each
  allocate a slot buffer mid-stream. The empty-frame filter path reuses a
  `thread_local` scratch `Frame` rather than allocating a temp per frame.
- Retained-byte accounting (issue #370, [[../diagnostics/MemoryBudget]]):
  `slotBytes_` mirrors each slot's `data.capacity()` (updated under the slot
  lock in `pushFrame`, under the exclusive lock in `reserveFrameBytes` /
  `resize`) and `retainedBytes_` / `peakRetainedBytes_` are relaxed atomics,
  so `memoryStats()` never takes a lock. Guard: `processing.memory_budget`
  (reserve → plateau across overwrites → resize re-account).
- `saveFramesToAvi` serialises frames while holding the mutex only long
  enough to snapshot the range; the VideoWriter loop runs outside the
  lock. Same pattern as `saveFramesToDisk`.
- With the empty-frame filter enabled, AVI frame count is smaller than
  the requested buffer range — 1:1 buffer-index-to-AVI-index mapping is
  lost. Acceptable for mask regen; don't depend on it elsewhere.
- OpenCV 5 changed `cv::Exception::code` from an integer-like value to an enum;
  the TIFF error log casts it explicitly so the Qt-free wheel remains buildable
  with both OpenCV 4 and 5 (no behavior change to frame storage).
- Portable manylinux builds may use fmt 6 through the distribution spdlog;
  exception logging passes `cv::Exception::func/file` as C strings so both
  that older formatter and current Conan builds compile identically.
