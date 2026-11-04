## 2026-10-22 — Native analysis composition and durable Cytoflow jobs (#808)

Added Qt-free native inclusive scalar predicates and three-valued Boolean
parent/QC composition, exact key/revision validation, plus a bounded actual
Cytoflow 1.3.2 polygon subprocess. The Rust host composes scalar membership via
cxx into the same C++ authority; it does not reimplement mechanics/science.

The private atomic checksummed job journal persists inputs and terminal success,
failure or interrupted states with exclusive scheduler ownership, idempotency,
conflict and corruption refusal. It is not the shared user workspace store.
Fresh-process demo query reproduces all four synthetic event results.

Native tests pass normally and under ASan/UBSan; 8 Python and 6 Rust tests pass,
including real Cytoflow, missing-value/equality behavior, process failures and
journal fault/reopen cases. See [[../../../knowledge_map/services/AnalysisWorker]]
and `tools/flowjo_acceptance/README.md`. No public ABI, source reader, UI mount,
Windows execution, distribution or full F01–F16 acceptance is claimed.
