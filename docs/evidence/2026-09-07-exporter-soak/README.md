# Exporter repeated-run soak evidence (issue #344)

Captured 2026-09-07 on the Linux backend-only environment (Ubuntu, Python
3.13, PySide6 6.11, h5py 3.16, OpenCV 4.13 headless; system Qt 6.4.2 for the
native build). All three runs execute **50 consecutive exports in one
process** against the synthetic fixture (`scripts/export_test_fixture.py`:
60 valid + 30 invalid frames, 3-image series, 96x512 px → 271 output files
per round) and apply the #344 acceptance gates.

| Surface | Command | Round 50 / median(2–5) | RSS after round 2 → 50 | Peak RSS | Lifecycle | Result |
|---|---|---|---|---|---|---|
| PySide GUI (`ExportWindow` + `ExportWorker` + `QThread`) | `python3 scripts/exporter_soak.py --cycles 50 --mode gui` | 0.140 s / 0.145 s (0.97×) | 124.0 MB → 124.3 MB (slope 2.2 kB/round) | 133.3 MB | 50 started = 50 finished = 50 workers destroyed = 50 threads destroyed; no Qt thread warnings | pass (`exporter-soak-gui-50.json`) |
| Python engine only (`run_export_job`) | `python3 scripts/exporter_soak.py --cycles 50 --mode engine` | 0.121 s / 0.120 s (1.01×) | 79.6 MB → 79.8 MB (slope 1.1 kB/round) | 80.9 MB | n/a | pass (`exporter-soak-engine-50.json`) |
| Native `HdfExportService` (Qt-free C++) | `MIB_EXPORT_SOAK_CYCLES=50 ./hdf_export_service_test` | 11.4 ms / 11.7 ms (0.97×) | HDF5 open-object count back to baseline after every round | — | output manifests identical across rounds | pass (`native-soak-50.txt`) |

Every run also verified that the output SHA-256 manifest is identical
across completed rounds and that the source HDF5 hash is unchanged. The
GUI lifecycle, cancellation (per phase), close-during-export, worker
exception, write-failure and partial-output cases are covered by
`scripts/test_hdf5_export_app_lifecycle.py`,
`scripts/test_export_hdf5_streaming.py` and
`tests/recording/hdf_export_service_test.cpp` (also run under TSan).

Re-run with:

```bash
QT_QPA_PLATFORM=offscreen python3 scripts/exporter_soak.py --cycles 50 --mode gui --report exporter-soak-gui-50.json
ctest --test-dir build/linux-backend -R 'recording.hdf_export_soak|scripts.exporter_soak' --output-on-failure
```

Not yet exercised here: the packaged Windows executable and the Windows
native build (the reported production platform); `.github/workflows/exporter-soak.yml`
runs the same harness on a schedule/manually so that evidence accumulates
per platform.
