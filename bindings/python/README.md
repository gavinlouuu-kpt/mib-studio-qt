# mib-processing

pybind11 bindings over mib-studio-qt's Qt-free deformability-cytometry
processing core (`mib_processing`, see `../../src/backend/CMakeLists.txt`).
Lets a non-Qt consumer -- e.g. Biowork's `services/mib-processing` runtime --
run the exact same algorithm the desktop app runs.

## Install

```bash
pip install mib-processing
```

Wheels are published to a mib-studio-qt GitHub Release as `.whl` assets, not
to PyPI or a GitHub Packages registry (GitHub Packages has no native
pip-installable index type). Install a specific release directly:

```bash
pip install "https://github.com/KPT1020/mib-studio-qt/releases/download/<tag>/mib_processing-<version>-<platform-tag>.whl"
```

See `.github/workflows/python-wheel.yml` for how wheels are built and
released.

Linux releases are repaired `manylinux_2_28` wheels for CPython 3.10–3.13 on
`x86_64` and `aarch64`. cibuildwheel installs and imports every
wheel inside its matching architecture container; the ARM64 lane registers
QEMU on the x86_64 GitHub runner. The x86_64 jobs additionally run the full
pytest/conformance suite, and import the CPython 3.12 artifact in a
`python:3.12-slim` container with Biowork's production
`libgl1`/`libglib2.0-0` runtime prerequisites before a tag can publish. These
libraries are in the manylinux system allowlist and therefore remain
OS-provided.

ARM currently means ARM64/aarch64. The pinned cibuildwheel 2.22 release treats
ARMv7 as experimental and has no CPython 3.12 manylinux ARMv7 target, so ARM32
cannot participate in the same complete Python 3.10–3.13 release matrix.

The build uses `MIB_BUILD_PROCESSING_ONLY=ON`, so Qt and the desktop service
graph are not configured into the wheel. The AlmaLinux 8 x86_64 and aarch64
images enable EPEL before installing the OpenCV, HDF5, and spdlog
build packages. `auditwheel` repairs the resulting runtime libraries into each
portable wheel rather than assuming they exist on the target host. Linux
32-bit x86 is intentionally unsupported because NumPy dropped i686 wheels and
the processing workload is not a practical fit for a 32-bit address space.

## Build from source

```bash
pip install .
```

Requires the same C++ toolchain + OpenCV/HDF5/spdlog dev packages as the
`linux-backend-only`/`linux-system-release` CMake presets (see
`docs/howto/linux-build.md`), plus a C++17 compiler. No Qt toolchain is
required -- `mib_processing` is Qt-free by design.

## Contract

Every dict this module returns follows the field names in
[`docs/gold_standard_metrics.md`](https://github.com/KPT1020/mib-studio-qt/blob/main/docs/gold_standard_metrics.md)
("Portable Processing Contract"). `mib_processing.CONTRACT_VERSION` names the
frozen combination of metrics schema + `ProcessingConfig` schema + Young's
modulus LUT format this wheel version was built against.

## Usage

```python
import numpy as np
import mib_processing as mp

config = dict(mp.DEFAULT_PROCESSING_CONFIG)
config["area_threshold_min"] = 60
config["area_threshold_max"] = 290

frames = [np.zeros((512, 96), dtype=np.uint8)]  # grayscale frames
results = mp.process_batch(frames, config, pixel_to_micron=0.4886)
for r in results:
    print(r["frame_type"], r["deformability"], r.get("youngs_modulus"))
```

## Conformance / anti-drift

From the repository root, verify the installed wheel against the committed
full-parity reference:

```bash
python scripts/run_processing_conformance.py
```

The check covers numeric metrics, validity, target-group and tracking metadata,
mask bytes, and ordered multi-image-series bytes. It is the same command run by
the wheel CI and is intended to be called by downstream consumers such as
Biowork after installing a pinned wheel. Install the package's `test` extra (or
`jsonschema>=4`) so the candidate is validated against the committed JSON
Schema before comparison.

The same runner accepts a bounded grayscale dataset directly from an HDF5
recording with `--hdf5`, `--hdf5-dataset`, `--frame-offset`, and
`--frame-limit`. See `docs/gold_standard_metrics.md` for the pinned
`gavinlouuu/z_adjustment-data` real-corpus command and reference provenance.
