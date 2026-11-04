## 2026-10-30 — Lossless analysis manifest transport

Adopted I90dd17cc's float_roundtrip parser feature and native double-bit regression.
F additionally exports opaque manifest_json/artifact_json and keeps manifest_json
unchanged through browser download, recovery journal and native import. Native
checksums retain authority; both payload forms together are refused. Tests exercise
integral-valued doubles (5.0), one-ULP decimal examples and lossy-object refusal.
Exact API and real WebSocket reproduction are in production-api.md and
 tools/flowjo_acceptance/manifest-transport-parity.mjs. No I checkout changes.

Verified native analysis.host, Rust float-bit regression, UI10/10 and real tailnet
WSS: lossy object refused, opaque native manifest imports revision2 with5.0 intact.
