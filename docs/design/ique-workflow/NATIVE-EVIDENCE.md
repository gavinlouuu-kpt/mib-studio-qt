# Native qualification — 2026-10-22

Source revision: `71b820745b45751719056ffc03ce8042ebde55b1`; subsequent QC-history
changes are frontend-only. Native build and tests ran locally against the real
C++ backend with synthetic/mock inputs and temporary HDF5/SQLite files. This is
**real-backend evidence, not physical hardware or complete operational proof**.

## Reproduction

```sh
cmake -S . -B build/ique-system -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMIB_BUILD_BACKEND_ONLY=ON -DBUILD_TESTING=ON \
  -DMIB_ENABLE_HARDWARE_SDKS=OFF -DMIB_ENABLE_MINDVISION=OFF -DMIB_USE_SENTRY=OFF
cmake --build build/ique-system --target mib_backend_tests -j 6
ctest --test-dir build/ique-system --output-on-failure \
  -R 'recording.run_start_identity|backend.calibration_record|e2e.run_quick_look|review.run_statistics|e2e.experiment_stop_generation' -j 2
ctest --test-dir build/ique-system --output-on-failure \
  -R '^e2e.(calibration_run|calibration_profile_apply|experiment_coordinator|method_gate)$' -j 2
```

This host's first configuration mixed system spdlog with Homebrew fmt 12 and
failed compilation. A separate clean configuration excluding the Homebrew
prefix with `CMAKE_IGNORE_PREFIX_PATH` and `CMAKE_SYSTEM_IGNORE_PREFIX_PATH`
used consistent system dependencies and compiled all 332 build steps. No host
packages, global configuration or other worktree builds were changed.

## Results: 9/9 passed

| Native test | Capability evidence |
|---|---|
| `recording.run_start_identity` | I01/I04 snapshot annotation round-trip, bounded identity and fault paths |
| `e2e.experiment_stop_generation` | I04 expected-run stop generation and lifecycle stress |
| `e2e.experiment_coordinator` | I04 existing coordinator lifecycle with mock acquisition |
| `review.run_statistics` | I06 existing native descriptive statistics; not the missing F population/fit evaluator |
| `e2e.run_quick_look` | I06 closed-run cache/evaluation and acquisition exclusion |
| `backend.calibration_record` | I11 native derivation, append-only store, read/reload and write-fault/CAS refusal coverage |
| `e2e.calibration_run` | I11 native calibration derivation at close |
| `e2e.calibration_profile_apply` | I11 explicit method/application interaction |
| `e2e.method_gate` | I03/I11 native method readiness policy |

No production native code was changed for these checks. TSan was not run and
is not claimed. These checks strengthen reuse evidence but do not by themselves
accept I03–I06 or I11: full workflow persistence, live analysis and physical
acquisition qualification remain separate requirements.
