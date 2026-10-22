# Analysis worker and native population core

Sources: `include/backend/analysis/`, `src/backend/analysis/`,
`crates/mib-analysis-worker/`, `workers/cytoflow/worker.py`.
Tracking: #808/#523. This is an initial F-A/F-B slice, not complete FlowJo support.

## Responsibility and boundaries

The portable C++ population core is the authority for inclusive optional scalar
bounds, three-valued Boolean composition within parent/QC universes, exact keyed
membership validation and counts with null empty denominators. It never computes
E itself: values/validity must come from the existing LUT/source authority.
The Rust host calls this same C++ implementation through an internal cxx seam;
it has no independent scalar or Boolean evaluator.

The Python process uses real pinned Cytoflow 1.3.2 PolygonOp, linear coordinates
and exact opaque string row keys. It has no access to HDF5, images or hardware.
It rejects unsupported fields, expressions, duplicate keys, invalid geometry
and unbounded requests. Missing coordinates are explicit unknowns, never False
placeholders that become admitted by NOT.

## APIs

- C++ `evaluateInterval`, `logicalAnd/Or/Not`, `compose`, `countMembership`,
  `acceptWorkerMembership` in `Population.h`.
- Rust `Worker::run` validates pinned revisions/keys, launches a process group,
  applies byte/time/address-space limits, kills/reaps on failure/cancel, verifies
  complete worker key coverage, then composes an optional native scalar gate.
- `JobStore::begin/finish/query` keeps immutable checksummed request and terminal
  artifacts, no-replace publication, file/directory sync and an exclusive OS lock.
  Reusing a job ID with different input is refused. `recover_interrupted` records
  interrupted rather than replaying uncertain work. It fails before mutation when
  its directory scan budget is insufficient.
- Internal input `mib-analysis-batch/1` includes native scalar configuration;
  projected Python request `mib-cytoflow-polygon/1` excludes it. Neither is a public
  Studio/Review ABI reservation. The current public contracts remain unchanged.

## Threading, storage and limits

`Worker::run` is synchronous and must be scheduled off acquisition/UI threads.
It mutates no shared analysis state and communicates through bounded temporary
files, not pipes that can deadlock on a full output buffer. Unix process groups
and rlimits are implemented; Windows explicitly returns UnsupportedPlatform until
a qualified Job Object supervisor lands. No cross-platform execution claim.

Initial caps: 10,000 events, 128 polygon vertices (optional closing duplicate),
4 MiB request/output, caller deadline and memory cap. No million-event claim.
The job journal is private worker artifact recovery, **not** a second workspace,
recipe, method, finding or run-identity store. Source HDF5 is never modified.
Caller-supplied source fingerprints/revisions must come from the source adapter;
validating their transport does not independently verify a recording.

## Evidence / remaining work

[Module instructions](../../tools/flowjo_acceptance/README.md) include the synthetic
real-engine/native/journal demo. Native tests cover full Boolean truth tables,
scalar edges, invalid bounds, universes, exact keys and stale revisions. Python
tests exercise real engine geometry/identity plus refusals. Rust tests cover
terminal recovery, collision/corruption, cancellation, timeout, crash and budgets;
the separately enabled real-engine test covers C++ equality/unknown composition
and durable reopen. Native ASan/UBSan and Rust clippy pass locally.

Not yet mounted in Studio/Review; no live provider, source reader adapter,
workspace DAG/undo/redo, images, qualified advanced science or reports. Keep
[[ReviewSession]] and acquisition exclusion intact. Cytoflow distribution remains
a release decision, not an excuse to stop internal implementation.

## Bounded native population graph

`PopulationGraph.h` adds plan ID/revision, stable node IDs, explicit parent,
predicate-mask reference or AND/OR/NOT operands. Validation includes parent and
operand edges in the same acyclic dependency graph. Execution is independent of
input node ordering, joins every mask by exact key/revision, rejects missing or
unused predicate inputs, and clips every result to parent and QC. Limits: 256
nodes, 10,000 events and 1,000,000 node-events; excess is an error, never sampling.
Empty populations retain revision identity. This is an internal scientific
contract, not a bridge ABI, serialized workspace document or a gate editor.

`analysis.graph` proves parent/operand cycles, invalid refs/arity, stale revisions,
unknown-safe NOT/QC, exact reordering, empty inputs and deduplicated unions. Both
native tests pass under ASan/UBSan. Overrides, undo/redo and native source adapters
remain separate implementation obligations.

## Exact population reductions

`PopulationStatistics.h` adds bounded (10,000 event) revision-bound feature/unit
columns joined by opaque exact keys. It reports selected/excluded/unknown
membership separately from finite measured/missing selected values, and raw-value
minimum/maximum/mean/median (null when no finite selected measurement exists).
Nonfinite values count as missing; excluded and unknown events never contribute.
Full column coverage is mandatory even for excluded rows. Median uses the middle
value or midpoint of the two middle values; mean uses scaled compensated summation.
These are floating-point raw-data reductions, not histogram estimates or arbitrary
precision arithmetic. Feature conversion and mechanics computation are not performed.

`analysis.statistics` covers reordered >2^53 keys, conservation, stale/foreign/
duplicate/missing-row refusal, units, empty/singleton populations, finite extremes,
subnormal medians and size limits. This is not source completeness certification,
a table UI/export, or F08 acceptance. Existing Review quick-look stays unchanged.
