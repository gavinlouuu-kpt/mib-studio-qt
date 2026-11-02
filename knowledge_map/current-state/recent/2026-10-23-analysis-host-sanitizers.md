## 2026-10-23 — Native analysis host sanitizer coverage

Standalone flowjo_acceptance can now build the complete Qt-free source/persistence
host using MIB_ANALYSIS_HOST_TESTS=ON. All four tests pass under ASan/UBSan locally,
including application/QC/transform/report/library/namespace persistence cases.
CI configuration now includes this target and real embedding/scheduler tests; no
updated remote CI success is claimed. TSan remains a separate host-startup blocker.
