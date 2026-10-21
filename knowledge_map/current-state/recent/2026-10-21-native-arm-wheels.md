## 2026-10-21 — ARM64 wheels build on the native arm64 runner; EPEL install retried

**Why:** the Python wheel workflow's ARM64 job built all four CPython wheels under QEMU on an x86_64
runner. It took 70–95 minutes on every PR touching the processing or recording code (five of them
on 2026-10-21), while every other check finished in 3–15 minutes, so it decided how long each
backend PR waited. The CPython 3.13 x86_64 job also failed twice that day on one EPEL mirror's SSL
certificate (`mirrors.wcupa.edu`): a 2-minute failure that could only be rerun after the 90-minute
ARM64 job had finished.

**Change (`.github/workflows/python-wheel.yml`):**
- Each matrix entry names its `runner`: x86_64 on `ubuntu-24.04`, ARM64 on `ubuntu-24.04-arm`
  (GitHub's arm64 runner, free for public repositories). The QEMU setup step is gone, and the
  ARM64 job cap drops from 120 to 30 minutes. The coverage is the same: the same cibuildwheel
  selectors, the same in-container import test, and the same 8-wheel release gate.
- `CIBW_BEFORE_ALL_LINUX` retries the EPEL and dependency install up to four times, running
  `dnf clean all` and waiting 20 s between attempts so fresh metadata picks another mirror. It
  still fails if every attempt fails.
- `tests/release/test_processing_core_wheel_architectures.py` now pins the native runner, the
  absence of QEMU and the retry.

The docs that described the QEMU lane were updated: [[build-and-run/Build]],
`docs/architecture/testing-strategy.md`, `docs/portable-processing-sync.md` and
`bindings/python/README.md`. The 2026-10-07 runtime-bounds notes stay as the record of that time;
they had already named a native ARM runner as the follow-up.
