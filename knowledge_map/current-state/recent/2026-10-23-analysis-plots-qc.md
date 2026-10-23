## 2026-10-23 — Persisted native plots and attributed QC

Source-bound strategies can retain histogram/overlay PlotSpecs and manual
scientific QC assertions. Native fixed-bin counts, normalization, outside-range
accounting and tri-state QC feed the real app and Cytoflow eligibility. The
renderer exports pinned SVG without recomputing science. Metadata initialization
now fails safely across the Review FFI and cannot silently retarget its authority.
See [API](../../../docs/design/flowjo-analysis/production-api.md).
