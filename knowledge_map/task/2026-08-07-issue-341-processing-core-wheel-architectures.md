# Processing-core wheel architecture matrix

**Date:** 2026-08-07

**Issue:** [#341](https://github.com/KPT1020/mib-studio-qt/issues/341)

## Outcome

The processing-core package version advances from `0.2.0` to `0.2.1` so the
new architecture matrix can be published as a fresh immutable release and R2
registry entry.

`python-wheel.yml` publishes `manylinux_2_28` wheels for CPython 3.10–3.13 on
`x86_64` and `aarch64`. The four x86_64 jobs preserve the full
pytest, publisher/version-tooling, scientific-conformance, and Biowork
production-base gates. One aarch64 job builds all four Python versions;
cibuildwheel installs and imports every repaired wheel in its
matching container, with QEMU registered for ARM64 on the x86_64 Actions
runner.

ARM in this release means ARM64/aarch64. cibuildwheel 2.22 marks Linux ARMv7
support experimental, uses a separate Ubuntu-based `manylinux_2_31` image,
and does not offer a CPython 3.12 manylinux ARMv7 build identifier. ARM32 is
therefore outside this coherent CPython 3.10–3.13 release matrix.

Release staging and immutable-release reuse require the exact set of 8
Python/architecture wheel pairs. An incomplete, duplicate, mis-tagged, or
unexpected wheel set cannot reach the GitHub Release or R2 registry.

Linux i686 is intentionally unsupported. NumPy dropped Linux i686 wheels due
to low demand, and microscopy image processing is not a practical fit for a
32-bit address space. Supporting it would create a wheel whose core dependency
cannot be installed normally on the supported Python versions.

## Verification

- `test_processing_core_wheel_architectures.py` guards the build matrix,
  matching-container smoke test, release allowlist, and QEMU setup.
- A CPython 3.12 aarch64 wheel is built, repaired, installed, and imported in
  the actual cibuildwheel 2.22 `manylinux_2_28_aarch64` image.
- `python3 scripts/check_docs.py` verifies vault and Markdown links.

**Related:** [[2026-07-13-processing-core-registry]] ·
[[../build-and-run/Build]] · [[../current-state/Recent-Work]]
