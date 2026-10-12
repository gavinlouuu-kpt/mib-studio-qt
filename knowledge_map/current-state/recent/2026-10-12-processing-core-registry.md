## 2026-10-12 — Processing-core registry compatibility

The installed 1.2.0 desktop cannot load the published Contract-1 cores: their
descriptors cap app support at 1.0.99. The repository release variables were
still 1.0.6–1.0.99. Contract 2 has no published registry (HTTP 404).

Release configurations now reject compatibility bounds excluding the current
desktop version; regression coverage checks stale upper and lower bounds.
The promotion workflow accepts either processing line and verifies the matching
public registry path. The manifest verifier accepts native-only Contract-2
manifests and checks their v2 ABI/entrypoint without requiring a Python wheel.
New signed releases of both lines are needed; immutable
historical descriptors must not be widened without validating their binaries.
