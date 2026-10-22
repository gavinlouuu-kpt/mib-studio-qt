# EventSnapshot consumer contract — F proposal 1

For the E/F/I coordinator; logical provider contract, **not an allocated ABI**.
F owns the adapter and ReviewSession/facade integration in its own dev branch; E
supplies live source facts. Shared-file naming is not an allocation gate. No second HDF5 reader or acquisition
policy. Existing #722 exclusion applies before acquiring a snapshot lease.

## Identity and immutable content

- `schema`: EventSnapshot/1; opaque snapshot ID, provider generation and source
  identity/fingerprint. Source fingerprint identifies immutable content, not just
  pathname/mtime. Legacy missing run UUID remains missing, never synthesized as
  an authenticated run identity. A generated local source ID is labelled local.
- Snapshot kind: offline frozen file or live frozen watermark. Ordered source
  manifest and parameter schema ID/revision; native core/config, calibration,
  mechanics LUT/provenance and QC-policy revisions pinned as available. Missing
  provenance is explicit and cannot pass a compatibility check by empty equality.
- Exact event identity tuple: source ID, dataset/set, uint64 row ordinal serialized
  as decimal text at JSON boundaries. Valid and invalid sets and multiple sources
  cannot collide. Stored frame index/object ID/track grouping are separate lineage
  fields, not substitutes for set row ordinal;
  per-detection does not silently become per-cell.
- Parameter IDs and units are explicit: canonical area in µm², dimensionless
  deformability and E in kPa only when provided by the existing native authority.
  Raw pixels/units, calibration origin and recorded-versus-fallback state remain
  inspectable. F does not calculate a second LUT or infer zero for missing E.
- Missing measurements are nullable with reason (not recorded, invalid/nonfinite,
  unavailable calibration/LUT, outside domain). QC is a separate tri-state mask;
  processing validity is a source fact, not automatically scientific QC.
- Image identity/locator and availability are optional independent facts. The
  scalar analysis snapshot must retain events without images. Retention failure
  is explicit; a stale ring slot is never relabelled the original image.

## Acquisition and pagination lifecycle

`beginSnapshot(sourceSelection, parameterSelection, budget)` returns an immutable
manifest plus a lease/token. It must capture both-set totals, metadata/calibration
and generation atomically, or reject. A live provider freezes an inclusive source
watermark and captures unbiased metrics through that watermark; monitoring ring
retention/sample selection is not a substitute.

`readPage(token, set, cursor, limit)` returns token/generation, exact ordered keys,
columns, total for that frozen set and next cursor. Limits are explicit; no hidden
truncation, sampling, omission or duplicate keys. Successful empty is distinct
from unreadable metadata. Cursor is bound to token/set, not a mutable file path.
The last page provides complete coverage for the advertised count.

`finishSnapshot(token)` validates generation/coverage and releases the lease.
Any source reopen (including the same path), close, fallback-calibration edit,
source mutation or relevant settings/LUT revision invalidates an unfrozen token;
**all partial pages are discarded**, no usable partial analysis result is emitted.
Cancellation and failure release exactly once. A stable copy provider may release
the original source earlier after its immutable copy is fully established.

Implementation options to allocate centrally:

1. Owner-held read lease excludes source lifecycle/calibration mutation for the
   bounded extraction, with existing acquisition/job admission exclusion; or
2. Atomic monotonically unique generation + manifest capture, generation checked
   on every page and at finish. Generation changes on every relevant mutation,
   including A→B→A; content mutation is detected or prohibited for the lease.

Current ReviewSession API supplies neither for both sets: validSet() is atomic
but valid-only; framesPage(), metadata() and pixelToMicron() lock independently.
Path-before/after equality is insufficient. F will consume the allocated seam,
not add shared locking or change #722 policy without ownership handoff.

## Required proof (not yet claimed)

- Two recordings with overlapping uint64 IDs above 2^53, both sets, differing
  calibration/LUT/provenance; exactly conserved source/set coverage.
- Page sizes 1, boundary-sized and multi-page produce identical keys, masks,
  counts and numeric statistics; empty sets and unreadable metadata distinct.
- Same-path reopen and A→B→A between pages, fallback/LUT changes, missing/wrong
  token, duplicate/drop/reorder, out-of-budget and cancellation all refuse or
  recover without publishing partial/stale results. Reordering only where an
  exact keyed join is specified, never implicit ordinal remapping.
- #722 active capture/export/reanalysis admission test: no HDF5 access outside
  the existing authority; release on every error and source close path.
- Frozen live watermark versus equivalent persisted offline rows: identical
  source-normalized keys, missingness/QC, native E values, memberships and
  statistics for the same plan/application/override revisions. Events arriving
  after watermark cannot alter prior results; retention loss is explicit.
- Input missing x/y or E cannot be admitted by NOT of an unevaluable gate;
  parent/QC clipping, null denominator and per-detection/per-cell policies shown.
- Image lookup maps exact EventRef to original asset or explicit unavailable,
  including ring wrap/eviction. No screenshot is numerical identity proof.
- Concurrent mutation/read tests and TSan on any new shared synchronization;
  bounded extraction off acquisition/UI threads, with measured latency budgets.

The shipped native/worker tests exercise synthetic identity and missingness
parts only. They do not satisfy these real provider/parity acceptance gates.
