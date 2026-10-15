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
- Calibration runs (amended the same day): a reference-sample run produces an instrument-scoped
  calibration record that the operator applies explicitly. Pass/fail-only bead QC is still not adopted.
- Not adopted: e-signatures / Part 11, and plate formats until an autosampler device class exists.

Schema plans are noted in [[data-model/HDF5-Storage]], and the terms in [[domain/Glossary]].
The design note also lists ten independent audit findings. The most important are that pumps
are not stopped when the last remote client disconnects, and that the E-LUT identity is
missing from run provenance.

Documentation only: no code, ABI or schema changes yet.

Update 2026-10-15: the plan now has a single **Delivery sequence** (S0–S9) that orders the
eGrabber workspace packages and the experiment revamp together, with needs and gates per
slice; seven eGrabber-styled fold-in views are registered in `PENPOT.md` and
`reference.json` (`penpot.fold_in`).

Scope trim 2026-10-15: Compare/dose-response, criteria sliders and statistics stay in the
analysis tool (#314, #523); chapters deferred (ADR 0015 Amendment 1); sample sheet is a
fixed schema, the queue a guided UI flow, no hardware auto-revert, calibration history is
a list. Delivery sequence is now S0–S7.
