## 2026-10-07 — Qt experiment configuration lock and Review outcomes (#582)

Closed the Qt ROI/background bypasses: preview controls and context actions follow
experiment ownership, direct processing setters use the idle transaction, and
watched configuration reloads defer until idle. Nested service setters reuse the
same-thread enclosing idle transaction. See [[frontend/System-Utilities]],
[[services/ProcessingService]] and [[architecture/ExperimentCoordinator]].

Also fixed #584: experiment-file Review retains Valid/Invalid counts and appends
the run accounting summary, including incompleteLoss/failed outcomes. See
[[frontend/HdfReviewTab]]. Regression coverage extends the backend readiness and
offscreen config-apply/Review tests. No Rust bridge ABI changes.
