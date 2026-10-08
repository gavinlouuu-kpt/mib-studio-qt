# 2026-10-07 — CI runtime bounds and stalling review

## Scope

The Linux backend, sanitizer, and soak lanes were reviewed for stalls and
minute waste. The Python wheel `build-and-test` matrix was included because
ARM64 QEMU is a larger measured runtime consumer than the sanitizer lane. No
test selection, coverage, failure semantics, or release behavior was removed.

## Evidence

| Run | Observation |
|---|---|
| [37583515274](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37583515274) | Backend configure 17 s, build 5 m 48 s, suite 3 m 49 s, PL replay 3 s, advisory Conan probe 17 s. |
| [37582104010](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37582104010) | ASan build about 21 m with a 4 m 13 s test portion; TSan build about 5 m 35 s with a 3 m 49 s test portion. ASan build output was continuous; no proven deadlock. |
| [37562107549](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37562107549) | 60 m overall wall time, with ASan runtime about 26 m and TSan runtime about 14 m. Matrix runtimes overlap or stagger and are not additive queue or billing totals. |
| [37456065522](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37456065522) | Soak compile 12 m 48 s and test 12 m 30 s. |
| [37568147020](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37568147020) | ARM64 QEMU built all four Python wheels in about 69.5 m; x86_64 entries took 3.0–3.8 m. |

The evidence supports hard bounds and cancellation/retry control, but does not
establish a deadlock or a complete billing total. The audit used current `origin/develop`, not the older dirty main checkout.
Queued/cancelled attempts must be separated from executing job time.

## Implemented controls

- Sanitizer matrix: 60-minute job; setup step cap 10, configure command/step caps 4/5, build 35/36, and
  test 12/13 minutes. C++ path gating
  remains step-level so required check names continue to expand on docs-only
  pull requests.
- Backend CI: 45-minute job; setup step cap 10, configure command/step caps 4/5, combined backend
  plus `mib_processing` build 20/21, full suite 12/13, and PL replay 4/5 minutes. The Conan probe
  is advisory and bounded at 180 seconds; timeout remains a warning. Existing
  PL lower-bound and all test options remain.
- Nightly soak: 60-minute job; repeats validated before setup with no direct
  shell interpolation, 1–100 accepted and 40 default; setup step cap 10,
  configure command/step caps 4/5, build 20/21, and full existing suite 30/31 minutes. Concurrency cancels stale same-ref
  runs. Heavy memory-budget and HDF export tests remain selected.
- Python wheels: unchanged cibuildwheel/test coverage with a 20-minute
  x86_64 job cap and 120-minute ARM64-QEMU cap. A native ARM runner is a
  future optimization, not part of this patch.
- All three Linux lanes use `mozilla-actions/sccache-action@v0.0.11`, explicit
  sccache `v0.16.0`, `SCCACHE_GHA_ENABLED=true`, and the existing CMake
  launcher reuse. Logs, CTest failure files, and cache stats upload on every
  attempted run with seven-day retention and bounded paths.

## Remaining risks and follow-up

Hosted sccache hit rates and artifact sizes are unverified until a fresh hosted
run completes. A hard job timeout can still prevent artifact upload. Bridge,
desktop, and review workflows independently rebuild native archives; they do
not share this Linux compiler-cache change, and their current semantics were
left unchanged. The Windows candidate's non-cancelling 120-minute behavior and
the wheel workflow's separate native-plugin jobs are intentional scope for a
later review.

See [[build-and-run/Build]] and
the [testing strategy](../../docs/architecture/testing-strategy.md) for the
operational budgets.
