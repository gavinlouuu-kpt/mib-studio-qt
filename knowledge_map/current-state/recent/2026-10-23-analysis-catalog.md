## 2026-10-23 — Bounded saved-record discovery

Added internal metadata-only namespace catalog through the existing serialized SQLite
owner. Literal prefix range, keyset cursor, max100 entries plus lookahead; exact head
revisions, no moving-head evaluation. Generic workspace endpoint cannot invoke it.
Production library has exact-open discovery; strategy/source references are visible.
Tests cover namespace isolation, continuation, generic rejection and exact UI reopen.

Validation: native3 persistence/host suites,4 sanitizer suites, UI20 and builds pass.
Tailnet keyset pages distinct; existing study-1792757005646 revision1 reopens with
unchanged content hash. F-owned server only restarted; original recording restored.
