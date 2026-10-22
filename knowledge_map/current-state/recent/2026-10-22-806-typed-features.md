## 2026-10-22 — Isolated typed feature control (#806)

Added [[camera/FeatureControl]] with exact session fencing, dynamic scalar
validation, requested-versus-readback outcomes, and a borrowed eGrabber adapter.
No camera ownership, shared bridge ABI, acquisition behavior or UI changed.
Standalone tests distinguish fake-port behavior, fake-adapter behavior, and
compile-only compatibility against the operator-provided eGrabber 26.09 headers.
E02 is partial pending capture-owner integration and physical qualification.

A path-filtered GitHub workflow runs the SDK-free core with UBSan; it does not
claim SDK/hardware coverage. SDK adapter checks remain an explicit local gate.
