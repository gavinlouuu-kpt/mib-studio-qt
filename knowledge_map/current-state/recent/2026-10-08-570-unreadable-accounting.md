## 2026-10-08 — Report unreadable run accounting as Unknown (#570)

[[services/Hdf5Service]] checks every required accounting attribute and returns
an explicit read diagnostic for partial/corrupt writes, with Unknown completion
and cleared counters. [[services/ReviewSession]] and [[frontend/HdfReviewTab]]
show the short reason. Legacy files retain their accounting-absent behavior;
BackendFacade exposes the diagnostic through existing JSON fields without an
ABI change. React Review also preserves Unknown instead of promoting it to
Failed solely because unreadable accounting is not reconciled. `review.session` covers deleted counter, flag and string attributes,
round-trip accounting and legacy absence; the deletion regression failed before
the fix.
