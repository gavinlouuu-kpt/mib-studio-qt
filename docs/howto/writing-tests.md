# Writing Tests — Developer Guideline

How to add tests that actually safeguard MIB Studio's core capabilities
(run experiments, save data, process images in real time). This is the
practical recipe book; the architecture and rationale are in
[`../architecture/testing-strategy.md`](../architecture/testing-strategy.md),
and the per-change gate lives in [`../../AGENTS.md`](../../AGENTS.md).

## TL;DR

1. **Pick the category by failure mode**, not by "unit vs integration" (see
   table below).
2. **Honor the coverage matrix**: a change to a capability lands with its
   required test categories. If the category doesn't exist yet for that area,
   creating it is part of your change.
3. **Reuse `tests/support/`** (assertions, watchdog, temp dirs, frame/stat
   helpers) — don't re-roll them.
4. **Found a bug? Write the failing test first**, watch it fail, then fix.
5. **Never write a thread/pipeline test without a watchdog.**
6. Run it on Windows *and* expect it to run on Linux CI + sanitizers.

## Where tests live, how to register one

```
tests/
  backend/        service + core logic, FrameStore, lifecycle
  camera/         camera implementations
  processing/     processing pipeline
  recording/      HDF5 save/recording/round-trip
  integration/    full capture->process->store e2e, stress, latency
  support/        shared header-only test utilities (include as "support/<x>.h")
  sanitizer/      TSan/LSan suppression files
```

Register in `tests/CMakeLists.txt`:

```cmake
mib_add_backend_test_executable(my_thing_test backend/my_thing_test.cpp)
# Threads::Threads only if you spawn std::thread directly
target_link_libraries(my_thing_test PRIVATE Threads::Threads)
add_test(NAME backend.my_thing COMMAND $<TARGET_FILE:my_thing_test>)
set_tests_properties(backend.my_thing PROPERTIES LABELS "backend" TIMEOUT 60)
```

A test is a plain `int main()` linked to `mib_backend` (no framework). Return
non-zero to fail. **Labels matter** — they drive CI lane selection:

| Label | Effect |
|---|---|
| `backend` / `recording` / `processing` / `camera` | runs in the sanitizer lane |
| `integration` / `e2e` | excluded from fast Windows presets |
| `performance` | excluded from the sanitizer lane (timing skews under TSan) |
| `integration` (network) | excluded from offline runs |

Run it:

```powershell
cmake --build build --config Debug --target my_thing_test
ctest --test-dir build -C Debug -R backend.my_thing -V
```

## The shared support library (`tests/support/`)

```cpp
#include "support/assert.h"     // MIB_REQUIRE / MIB_EXPECT, mib::test::exitCode()
#include "support/watchdog.h"   // mib::test::Watchdog
#include "support/tempdir.h"    // mib::test::TempDir (RAII)
#include "support/frames.h"     // ringFrame(), writeFrames()
#include "support/stats.h"      // summarize(), percentile()
#include "support/faultinject.h"// longPath(), makeReadOnly()
```

- `MIB_REQUIRE(cond, msg)` — fatal (`_Exit(1)`); `MIB_EXPECT(cond, msg)` —
  records a failure and continues. End `main` with `return mib::test::exitCode();`
- `Watchdog wd(20); wd.mark("step");` — if no `mark()` lands within N seconds it
  prints the stuck step and `_Exit(99)`s. **Use this for anything with threads.**
- `TempDir td("prefix"); auto p = td / "file.h5";` — auto-removed.

## Category recipes

### 1. Round-trip (save data)

For every persistence path: write → close → reload → verify. Verify totals,
metadata, ROI, **and** pixel content (not just that it opened).

```cpp
mib::test::TempDir td("roundtrip");
const std::string path = (td / "experiment.h5").string();
{ Hdf5Service h; MIB_REQUIRE(h.openFile(path), "open");
  /* write frames + metadata + info */ h.closeFile(); }
Hdf5Service r; MIB_REQUIRE(r.loadFile(path), "reload");
/* MIB_EXPECT(totals, ROI, metadata, a pixel value round-trip) */
return mib::test::exitCode();
```
See `recording/experiment_roundtrip_test.cpp`.

### 2. Fault-injection (save data robustness)

Exercise the failure paths a user actually hits: nonexistent parent dir, a
file-as-parent, read-only target, path > `MAX_PATH`. Assert it **fails
cleanly** (returns false, no crash) — reaching the line after the call proves
no crash.

```cpp
const fs::path blocker = base / "blocker"; { std::ofstream(blocker) << "x"; }
std::string why;
MIB_EXPECT(!saveRoundTrip(blocker / "exp.h5", why), "file-as-parent fails cleanly");
```
See `integration/e2e_storage_destinations_test.cpp`.

### 3. Pipeline e2e (run experiments)

Drive capture → process → store with the mock camera; **assert frame
accounting is conserved** (captured == processed + explicitly dropped — no
silent loss). See `integration/kin6_mib_app_capture_proof.cpp`.

### 4. Concurrency / lifecycle stress (catch deadlocks)

Repeated start/stop/restart and toggles, **with a watchdog**, asserting no
crash/hang and data integrity. Use a `mark()` per operation so the watchdog
report names the stuck step.

```cpp
mib::test::Watchdog wd(20);
for (int i = 0; i < cycles; ++i) {
    wd.mark("start"); backend.capture().start();
    wd.mark("stop");  backend.capture().stop();
}
```
See `integration/e2e_pipeline_stress_test.cpp`. Pass a `[cycles]` arg for heavy
local stress; keep the CI default light.

### 5. Invariant / property (FrameStore-style)

Assert structural invariants under concurrency: a ring never returns a torn,
stale, or evicted-aliased frame; indices monotonic; counts conserved. Tag each
frame with a value derived from its index so a wrong frame is detectable.
See `backend/frame_store_concurrency_test.cpp`, `frame_store_bounds_test.cpp`,
`frame_store_resize_under_load_test.cpp`.

### 6. Performance / latency budget (real-time)

**Gate on steady-state or ratios, never absolute milliseconds** (CI machines
vary). Report the numbers for visibility; gate on robust invariants: progress
made, steady-state lag bounded, queue bounded, accounting conserved.

```cpp
// good: machine-independent
MIB_EXPECT(result.lateLag < 2000.0, "steady-state lag bounded");
MIB_EXPECT(stats.maxQueueDepth <= maxQueued, "queue bounded");
// bad: flaky
// MIB_EXPECT(latencyMs < 5.0, ...);   // absolute timing -> don't
```
See `integration/e2e_realtime_throughput_test.cpp`,
`e2e_live_view_latency_test.cpp`, `e2e_batch_backpressure_test.cpp`.

### 7. Hardware-present tests

Tests that need a real device (camera, syringe pump, nanopositioner) are labeled
`hardware`, **excluded from every default preset**, and **self-skip** (CTest
`SKIP_RETURN_CODE 77`) when their device env var is absent — so a normal run on a
dev box without hardware is clean. Use `tests/support/hardware.h`:

```cpp
#include "support/hardware.h"
const int port = std::atoi(mib::test::requireDeviceEnv("MIB_TEST_PUMP_PORT")); // skips if unset
const int baud = mib::test::envInt("MIB_TEST_PUMP_BAUD", 115200);
// ... drive the real service, MIB_REQUIRE/MIB_EXPECT on results ...
```

Register with the `hardware` label and the skip code:
```cmake
add_test(NAME hardware.syringe_pump COMMAND $<TARGET_FILE:hw_syringe_pump_test>)
set_tests_properties(hardware.syringe_pump PROPERTIES LABELS "hardware" SKIP_RETURN_CODE 77 TIMEOUT 60)
```

Run the hardware suite (with devices attached + env set):
```powershell
$env:MIB_TEST_PUMP_PORT="6"; $env:MIB_TEST_NANOPOSITIONER_PORT="7"; $env:MIB_TEST_CAMERA="1"
ctest --preset windows-hardware-test --output-on-failure   # only label "hardware"
# or: ctest --test-dir build -C Debug -L hardware
```

Device env vars: `MIB_TEST_PUMP_PORT` (+ `_BAUD`, `_ADDR`),
`MIB_TEST_NANOPOSITIONER_PORT` (+ `_BAUD`, `_ADDR`), `MIB_TEST_CAMERA` (the
operator also sets `MIB_CAMERA_MODE` + selection envs as the app does), and
`MIB_TEST_EGRABBER_SCRIPT` (+ `MIB_TEST_EGRABBER_IF`/`_DEV`) — the EGrabber LED /
strobe is driven by a camera script via `applyCameraScriptFromFile`. Existing:
`tests/hardware/hw_{syringe_pump,nanopositioner,camera,egrabber_script}_test.cpp`.
The hardware-independent guards of that script path are covered off-device by
`backend.camera_script_apply`.

Keep device-independent protocol/logic in a plain unit test instead (e.g. the
Modbus framing lives in `backend::services::modbus` and is covered by
`backend.modbus_rtu` with zero hardware).

### 8. Headless Qt frontend tests on Windows

`frontend.*` tests that create a `QApplication` force `QT_QPA_PLATFORM=offscreen`.
On Windows the packaged tree only carries `qwindows.dll` and the offscreen
platform has no font database, so `cmake/MIBQtOffscreenTests.cmake`
(`mib_apply_qt_offscreen_plugin_path()`, called at the end of every
`CMakeLists.txt` that registers `frontend.*` tests) sets
`QT_QPA_PLATFORM_PLUGIN_PATH` to the Conan Qt plugin directory and
`QT_QPA_FONTDIR` to the system fonts for those tests. Without it the tests
fail-fast (`0xc0000409`) and the Windows crash dialog holds the process until
the CTest timeout. Register new frontend tests with the `frontend.` prefix so
the helper picks them up.

## Mandatory rules

- **Watchdog, never a naked `join()`/`wait()`.** A deadlocked test must fail
  fast with a diagnostic, not hang the ctest timeout. Use `std::_Exit`, **not**
  `abort()` — the linked crash handler intercepts `abort()` and can itself hang.
- **Timing tests gate on steady-state/ratios.** Mark probabilistic tests.
- **Regression-first.** A bug fix lands with a test proven to fail before the
  fix and pass after (verify-by-revert when feasible).
- **Conserve frame accounting** in pipeline tests.
- **Determinism first.** Only fall back to load-induced probabilistic
  reproduction (background CPU threads) when a race genuinely needs scheduling
  pressure — and say so in a comment.

## Sanitizers

The `sanitizers.yml` lane builds the backend with TSan and ASan+UBSan and runs
the non-timing tests. Run locally on Linux:

```bash
cmake --preset linux-backend-only -DMIB_SANITIZER=thread        # or address+undefined
cmake --build --preset linux-backend-only-build
ctest --test-dir build/linux-backend -L "backend|recording|processing|camera" -LE "integration|performance"
```

- **Never suppress our own code.** Suppressions in `tests/sanitizer/tsan.supp`
  are for uninstrumented third-party libraries only (OpenCV/GDAL, glib). A race
  in `backend::*` is a real bug — fix it.
- `called_from_lib:` must name exactly one library (an ambiguous substring is a
  fatal TSan error).
- Avoid Qt-network at startup in tests (it pulls in glib/libproxy threading TSan
  can't model): CI sets `MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL` to a `file:` URL.

## Lessons baked in (real bugs this framework caught)

- **Condition-variable lost wakeup:** always set the predicate variable **under
  the same mutex** the consumer holds in `wait()` before notifying. A lock-free
  store + notify can be lost (caused the trigger variable-delay and the
  capture-shutdown deadlock).
- **Lifecycle flags must be atomic:** a plain `bool running_` written by `stop()`
  and read by a worker thread is a data race (MockCamera).
- **Lock-free reads racing a locked write:** if a member is read without the
  lock in some paths but written under it (because a writer calls those readers
  while holding the lock), make the member `std::atomic` (FrameStore `capacity_`).
- **HDF5 on Windows locks the open file:** `copy_file` of an open HDF5 file fails
  unless file locking is disabled on the FAPL (recovery checkpoint).
- **`std::this_thread::sleep_for` is coarse on Windows (~1–15 ms):** don't use it
  to pace high-rate producers in tests; use a tight loop with periodic `yield()`.

## PR checklist

- [ ] Required categories for the touched capability are present (see the matrix
      in [`AGENTS.md`](../../AGENTS.md)).
- [ ] Bug fixes include a regression test proven to fail before the fix.
- [ ] Any thread/pipeline test has a watchdog and uses `_Exit`.
- [ ] Timing tests gate on steady-state/ratios, not absolute ms.
- [ ] New test registered in `tests/CMakeLists.txt` with correct labels.
- [ ] Passes locally (`ctest`), and is expected to pass on Linux CI + sanitizers.
- [ ] `python scripts/check_docs.py` passes if docs/vault changed.
