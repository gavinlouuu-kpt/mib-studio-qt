## 2026-10-23 — Native axis-aligned ellipse populations

Extended the existing native predicate evaluator, not a browser membership engine.
Exact named raw/derived X/Y domains, finite positive radii, inclusive hypot boundary,
missing-coordinate unknown, same parent/QC/Boolean composition and source pins.
Production graph form supports ellipse parameters; template interval overrides stay
explicitly interval-only. No new dependency/ABI or rotated/covariance-fit claim.

Validation: native host axis-boundary/outside/missing/extreme/invalid-radius tests,
all4 ASan/UBSan suites, focused UI19 and frontend/server builds pass.

Real tailnet ellipse-e2e-1792760361901 revision1 selects7, mean105, exact7 images;
Cytoflow job79b4da5a33c3210de534f53717365f939599a8f4ffca396e13ef4da0ef1144f0
selects same7 with source/parameter parity. Exact save replay passes.
