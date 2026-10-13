## 2026-10-12 — Processing-core registry compatibility

The installed 1.2.0 desktop cannot load the published Contract-1 cores: their
descriptors cap app support at 1.0.99. The repository release variables were
still 1.0.6–1.0.99. Contract 2 has no published registry (HTTP 404).

Release configurations now reject compatibility bounds excluding the current
desktop version; regression coverage checks stale upper and lower bounds.
The promotion workflow accepts either processing line and verifies the matching
public registry path. The manifest verifier accepts native-only Contract-2
manifests and checks their v2 ABI/entrypoint without requiring a Python wheel.
Both 0.3.2 lines were subsequently built, checked, signed and published to the
stable registry with app bounds 1.2.0–1.2.0. Historical descriptors remain
immutable. The Production environment now also permits the specific
`mib-processing-absdiff-laplacian-v*` tag pattern, retaining reviewer approval.
Workstations still need the verified organizational root described in
`deploy/signing/README.md`; missing Windows root trust prevents activation
even when registry compatibility succeeds.
The Windows native-core job allows 60 minutes because cold Conan dependency
builds exhausted its previous 30-minute limit during the release repair.
