## 2026-10-14 — Experiment/run/chapter identity and P7 Compare for the eGrabber workspace

A study of plate-based high-throughput cytometry software was folded into the
`feat/egrabber-workspace` plan as content for Prepare / Align / Experiment / Review,
not as a parallel navigation (`docs/design/egrabber-workspace/FORECYT.md`).

Owner decisions are recorded in ADR 0015 (`docs/decisions/0015-experiment-run-chapter-identity.md`):

- An experiment groups runs, and a sample queue writes one file per sample.
- Chapters mark in-run sample changes. They are recorded at acquisition, never inferred.
- The planned annotation is written into each run at Start; corrections live in the workspace store.
- Auto-stop is a per-run option: valid-object count, duration, or both.
- The Cytoflow worker (#523) computes statistics, comparisons and fits.
- A new P7 Compare work package was added.
- Not adopted: bead QC, e-signatures / Part 11, and plate formats until an autosampler device class exists.

Schema plans are noted in [[data-model/HDF5-Storage]], and the terms in [[domain/Glossary]].
The design note also lists ten independent audit findings. The most important are that pumps
are not stopped when the last remote client disconnects, and that the E-LUT identity is
missing from run provenance.

Documentation only: no code, ABI or schema changes yet.
