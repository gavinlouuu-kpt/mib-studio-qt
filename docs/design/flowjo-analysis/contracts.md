# Shared analysis contract proposals

Status: proposed for #806/#807/#808 and existing #512/#717 owners; not an ABI reservation.

## Ownership and versioning

Reuse the generated `bridge-contract.json` / `review-contract.json` authority.
Current fetched backend: bridge 44, Review 6. #803 reserves bridge 45;
#804 reserves Review 7. Do not consume those numbers for analysis.
The coordinator assigns subsequent additive ABI changes after agreement.
Proposal revision `F-contracts/1` is a document version, not a wire version.

- E (#806) owns source generations, acquisition/finalization, unbiased committed
  event supply and retained image delivery. It must not derive a second population evaluator.
- F (#808/#523) owns analysis definitions, dependency invalidation, qualified worker
  evaluation, membership, metrics, comparison and scientific artifact persistence.
- I (#807/#314) owns experiment questions, recipe association and next-run UX;
  existing method/journal services remain the persistence/application authorities.
- #512/#717 retain the shared ReviewSession and host transport/mount ownership.

## Proposed logical records

All counts/ordinals/frame IDs/object IDs/track IDs/ticks crossing JSON use canonical
decimal strings or existing typed binary u64 fields, never JavaScript numbers.
Unknown fields are explicit null/status, never fabricated identifiers or zero.

1. **EventSnapshot**: source ID + verified fingerprint, snapshot ID, run/session/epoch,
   source kind and event mode, committed watermark, finalized/provisional state,
   declared universe/coverage/gaps, parameter registry, units and processing,
   calibration, mechanics and clock revisions. Page by immutable snapshot plus cursor.
   An EventRef adds dataset/set + row ordinal; frame/object/track are lineage,
   not sufficient keys. Reopening or relinking verifies the fingerprint.
2. **EvidenceResolver**: existing image owner resolves EventRef + asset kind + pinned
   processing revision into a bounded binary handle, dimensions and provenance, or
   unavailable/expired/not-recorded/incompatible reason. Request generation must match
   the current selection before pixels display; clear old pixels on selection change.
3. **AnalysisPlan**: expected workspace revision, stable strategy/application IDs and
   revisions, per-sample override/base revisions, parent universe, explicit QC and
   missing-value policies, gate DAG and parameter/evaluation-space bindings.
   Validate cycles, dangling references, units, geometry and source capabilities
   before dispatch. MIB composes populations; real Cytoflow supplies qualified operations.
4. **AnalysisResult**: job/result IDs, complete input revision key, exact membership
   handle, evaluable/unknown/excluded counts and denominator, metric definitions/units,
   engine/dependency pins and validity reasons. Reject stale results rather than
   attaching them to current definitions. Display transforms never alter evaluation.
5. **ComparisonSpec**: explicit historical/retrospective mode, pinned result refs,
   endpoint compatibility, experimental units, run/sample membership, pairing,
   technical-repeat weighting, missing/excluded units and model/contrast definition.
   Report event/run/unit counts separately. Twelve technical runs in the two-donor
   paired fixture remain two biological pairs. Descriptive support is distinct from
   qualification for inference; overlapping live windows are not replicates.
6. **Finding/Recipe**: references to immutable comparison/result/evidence and existing
   finding/method records; attributed interpretation, question, parent method and
   next-run draft lineage. A draft never applies itself or mutates acquisition.
7. **Workspace/ExportJob**: portable schema-versioned definitions, source links and
   content-addressed artifacts; expected revisions for mutations; reversible command
   history; atomic publication with no replacement of earlier outputs. Queryable
   terminal state survives progress loss/restart. Cache keys include all scientific
   dependencies; caches cannot stand in for missing evidence.

## Scientific semantics retained

Area is calibrated X, deformability Y. E and contours use the existing native LUT
authority; no Python/TypeScript mechanics duplicate. E bounds are inclusive and
absent endpoints unbounded. Missing E remains unknown under negation. Primitive
membership is parent AND QC eligibility AND predicate with three-valued logic.
Zero denominator yields null. Per-cell analysis requires verified lineage and
a declared representative observation, not objectId-based deduplication.

## Required peer decisions before shared implementation

- #806: agree EventSnapshot/EvidenceResolver fields, generation ownership and
  committed live provider seam. The current valid-first display ring is insufficient.
  Keep the #722 HDF5 exclusion until a separately qualified safe route exists;
  independent reader handles alone do not make non-thread-safe HDF5 concurrent.
- #807: agree AnalysisResult/ComparisonSpec/Finding reference consumption and the
  minimum experimental-unit metadata adapter; do not independently compute fits.
- #512/#717: confirm ownership windows for bridge/session/job edits and #803/#804
  sequencing. F's current change owns only its new documentation and qualification
  harness, not shared service/bridge files or App.tsx.
- Owner/release maintainer: record a Cytoflow distribution plan for the pinned
  GPL-2.0-or-later candidate and dependencies. Process isolation is not an approval.
  This blocks shipping, not isolated internal qualification. No engine substitution
  or FCS-only fallback is proposed.

The F coordinator has acknowledged the compatible logical E/I proposals on #808.
Exact shared public ABI and file ownership remains coordinated separately.
F-owned native composition, internal worker protocol and private job artifact
journal are now implemented in isolated new paths; this does not create another
workspace/recipe/finding store. See [AnalysisWorker](../../../knowledge_map/services/AnalysisWorker.md).
