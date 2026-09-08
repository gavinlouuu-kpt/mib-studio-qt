# Architecture Decision Records

Short, dated records of decisions that shape the codebase. Write one whenever
a choice would otherwise need re-explaining in review: a new threading
pattern, a dependency, a storage schema change, a boundary exception.

## Format

Name files `NNNN-<slug>.md` (e.g. `0001-backend-frontend-bridge.md`):

```markdown
# NNNN. <Title>

Date: YYYY-MM-DD
Status: accepted | superseded by NNNN

## Context
What forced the decision.

## Decision
What we chose.

## Consequences
What becomes easier/harder; what future agents must respect.
```

## Index

| ADR | Title | Status |
|-----|-------|--------|
| [0001](0001-react-tauri-migration.md) | Migrate the desktop application from Qt to React + Tauri | accepted |
| [0002](0002-lut-catalog-http-seam.md) | E-modulus LUT catalog fetches through an injected HTTP seam | accepted |
| [0003](0003-rust-cxx-bridge.md) | Rust ↔ C++ bridge uses `cxx` over the `BackendFacade` seam | accepted |

Decisions made before this index existed live implicitly in
[`../architecture/`](../architecture/) and the vault
(`knowledge_map/architecture/`). When you rediscover one, backfill it here.
