## 2026-11-03 — Review statistics fixture floating-point tolerance (MIBQT-17)

The `review_bridge` parity test now compares the selected run statistics and reference statistics
recursively with an absolute tolerance of `1e-12` for floating-point values. Object keys, optional
field presence, array shape/order, integers, decimal-string counts, booleans, strings and nulls remain
exact. Focused controls cover the reported overlay roundoff and reject numeric and structural changes.

The shared golden fixture and C++ production calculations are unchanged. Validation in this worktree:
the Linux backend-only configure/build and offline `review_bridge` Cargo test passed (8 tests), as did
`python3 scripts/check_docs.py` and `git diff --check`. Linux validation cannot establish the macOS
arm64 result; the reported compiler contraction cause remains unconfirmed. The effect on MIBQT-18
also remains unverified.
