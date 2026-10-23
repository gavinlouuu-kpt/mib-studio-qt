## 2026-10-22 — Revision-bound population statistics

F/#808 continued during QA with bounded raw population reductions in
`include/backend/analysis/PopulationStatistics.h` and matching source/tests.
Exact-key columns retain feature/unit and revision identity; missing selected
measurements and unknown membership remain separate. Empty reductions are null.
Three native tests pass under ASan/UBSan, including extreme-value cases.
See [[services/AnalysisWorker]]. This does not expose analysis in Studio/Review;
shared host/provider integration and full scientific acceptance remain open.
