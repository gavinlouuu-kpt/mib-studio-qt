## 2026-10-20 — `backend.facade_review_jobs` passes on Windows (#512 S7 follow-up)

The test compares the facade's event trace with the pre-S7 oracle after
replacing each run's temporary directory with `<DIR>`. Status JSON carries that
directory JSON-escaped, and on Windows the escaping doubles its backslashes. So
the replacement missed it, and the oracle and facade lines differed only by the
directory name. `Build Windows` on develop failed at 464a18fc. The normalization
now replaces the escaped spelling as well. Test-only; no behaviour changes. See
[[services/ReviewSession]].
