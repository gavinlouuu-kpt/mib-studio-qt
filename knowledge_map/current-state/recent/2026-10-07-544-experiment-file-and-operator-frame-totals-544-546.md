## 2026-10-07 — Experiment file and operator frame totals (#544, #546)

The shared coordinator writes run-wide valid/invalid committed counts instead of
the stop-time remainder into HDF5 headers. Bridge events and pull status retain
the saved split and report actual policy drops; the desktop separately labels
pending saves and writer failures using existing ABI fields. A deterministic
mixed-run regression checks header totals against datasets and accounting;
desktop tests cover labels and exact pending arithmetic. Older-file recovery is
documented in [[services/Hdf5Service]]. See [[architecture/ExperimentCoordinator]]
and [[architecture/Rust-Bridge]].
