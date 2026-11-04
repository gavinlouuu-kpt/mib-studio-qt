## 2026-10-23 — Pinned multi-source histogram overlays

AnalysisOverlay persists exact retained-source histogram references with native
numeric-domain validation. Counts/normalized values retain each source denominator;
explicit display offsets never change scientific values. Report runs freeze overlays.
Normal/ASan/UBSan host tests and production tailnet14/22-observation proof pass.
The second fixture includes20 valid plus2 invalid observations; all-policy correctly
includes both sets. Documentation/test expectation corrected, not the counting code.
