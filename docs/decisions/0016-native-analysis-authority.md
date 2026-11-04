# 0016. The native evaluator is the production analysis authority; Cytoflow is a comparator

Date: 2026-11-02
Status: accepted

## Context

The FlowJo fold-in (#808) recorded Cytoflow as the production analysis engine,
isolated behind a worker adapter, with its distribution decision still open.
The implementation on #810 went another way without recording it:

- Every population (interval, polygon, ellipse, quadrant, Boolean, E interval)
  is evaluated natively in C++ (`src/backend/analysis/PopulationGraph.cpp`) with
  tri-state missing-value logic and exact membership.
- Statistics, overlays, comparison and reports are native as well.
- Cytoflow 1.3.2 runs only `flow.PolygonOp` in a supervised Python worker, as
  a parity check. t-SNE uses scikit-learn, not Cytoflow.

Other requirements point the same way:

- The fold-ins need **one** evaluator serving Studio, the standalone Review
  host and the iQue live views (I06–I08, F13).
- Live analysis must read committed snapshots without blocking the recorder.
- The specification forbids "arbitrary Python or TS science".

Cytoflow is copyleft-licensed (GPL-2.0; confirm in the distribution review) and
needs a Python runtime, which leaves the distribution gate open for as long as
it sits in the production path.

## Decision

1. **The native C++ analysis code is the single production authority** for
   populations, statistics, transforms, overlays, comparison, fits and reports
   (F02–F11, F13), for every host.
2. **Cytoflow and FlowKit are independent comparators.** They run in CI parity
   tests against shared fixtures and are not part of the production evaluation
   path. Production installers do not bundle Cytoflow.
3. **The supervised worker (`crates/mib-analysis-worker`) stays** for long jobs
   that have no native implementation yet (currently t-SNE, F07). Each such
   algorithm must meet all of these:
   - it is named in the F ledger with its library and version;
   - its results are bound to that runtime version;
   - it uses only permissively licensed dependencies cleared in a distribution
     review.

   Moving any worker algorithm into native code later needs no new ADR.
4. **New scientific computation goes into the native authority.** Adding a
   Python algorithm to the production path needs an amendment to this ADR.

## Consequences

- F work concentrates on native scale and fidelity. Removing the
  10,000-event / 64 MiB limits (F-1) and running the FlowJo reference through
  native import (F-2) become the priority, not Cytoflow integration.
- The Cytoflow distribution gate no longer blocks shipping analysis. It
  applies only if a Cytoflow-backed algorithm is ever proposed for production.
- Parity CI must keep running the real Cytoflow engine (`analysis-ci.yml`, run
  with ignored tests included), so the native evaluator is checked against an
  independent implementation.
- iQue features (criteria, hit finding, metrics) call this authority through
  the bridge and never compute in TypeScript. See
  [`../architecture/fold-in-backend-goals.md`](../architecture/fold-in-backend-goals.md).
