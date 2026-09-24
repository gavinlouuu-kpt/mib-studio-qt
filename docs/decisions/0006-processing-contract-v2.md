# 0006. Processing Contract v2

Date: 2026-07-21
Status: accepted

## Context

The portable processing contract (v1) is frozen and shared across the desktop
app, the `mib_processing` native cores, the Python wheel, and HDF5 files
(`docs/gold_standard_metrics.md`, ADR-less but documented under
`docs/architecture/`). A set of scientific changes are intentionally
**incompatible** with v1 and cannot be introduced by mutating the frozen
contract in place:

- Background comparison must use `cv::absdiff` rather than saturating
  subtraction, so bright-on-dark and dark-on-bright objects behave
  symmetrically.
- A deterministic preprocessing filter stage (identity, invert,
  linear-contrast, gamma, CLAHE) must live inside the versioned pipeline
  instead of being applied ad-hoc in the frontend.
- Ring width / ring ratio is derived from outer/inner contour topology and is
  being **removed** as a science metric, replaced by a per-detected-object
  Laplacian variance focus metric.

Changing subtraction semantics, replacing a metric, adding preprocessing
stages, and changing persisted output at the same time is a breaking change to
every consumer of the contract. Doing it by editing v1 would silently
invalidate existing golden references, HDF5 files, published cores, and Python
wheels, with no way for a reader to tell which semantics produced a given
result.

## Decision

Introduce **Processing Contract v2** as a new, explicitly versioned pipeline
that coexists with v1 rather than replacing it.

1. **Two independent version axes, bumped together for v2.**
   - `processing_contract_version` — the *science* contract (metrics, their
     units, subtraction semantics, preprocessing). v1 = `1`, v2 = `2`.
   - `config_schema_version` — the *config document* shape. v1 = `1`, v2 = `2`.
   A profile/core/file declares both; a consumer refuses to execute a profile
   against an implementation whose contract does not match.

2. **v1 stays executable.** Contract 1 remains a first-class, byte-for-byte
   reproducible path. ABI-v1 cores keep serving Contract-1 profiles. No v1
   golden output changes as part of the v2 work.

3. **Canonical v2 keys, legacy accepted only through migration.** The v2 config
   uses `difference_threshold` (canonical) where v1 used
   `bg_subtract_threshold`. Legacy keys are accepted **only** through an
   explicit compatibility/migration adapter, never as live members of the v2
   public contract.

4. **Contracts do not silently rewrite documents.** Schema-aware loading:
   - same schema → merge missing defaults in memory;
   - older schema → preserve the source untouched and offer an explicit
     copy-upgrade;
   - newer/unknown schema → fail closed with a diagnostic.

5. **Explicit, lossless v1→v2 migration.** A migrator produces a v2 config from
   a v1 config that: preserves unrelated values, removes ring thresholds and
   their enable flags, installs a no-op (identity) preprocessing chain, leaves
   the Laplacian-variance gate disabled, and never selects or activates a core.

6. **A single compatibility matrix** governs which
   profiles / native cores / Python wheels / HDF5 files may be combined. It is
   defined once (`docs/architecture/processing-contract-compatibility.md`) and
   enforced in code rather than re-derived per call site.

This ADR is the umbrella decision for the epic tracked as issues V2-1…V2-7.
V2-1 establishes the schema, versioning, migration, and compatibility boundary;
later issues add the shared absdiff path and preprocessing (V2-2), the
per-object Laplacian metric (V2-3), focus-score autofocus (V2-4), ABI v2
(V2-5), persistence/UI migration (V2-6), and calibration/validation (V2-7).

## Consequences

- Every persisted or transported artifact must carry both version numbers.
  Readers select behavior from the declared versions, not from heuristics.
- The v1 path must remain untouched; changes that would alter v1 golden output
  are bugs, not intended v2 behavior.
- New scientific fields (Laplacian variance, preprocessing config) are added to
  the v2 contract surface only; they must not leak into the v1 struct/schema as
  "always present" members.
- The migrator is the *only* sanctioned way to turn a v1 document into a v2
  document. Recursive key-merging of v2 defaults onto a v1 profile is
  prohibited because it would strand ring configuration and skip the
  intentional field removals.
- Future agents adding a v2 field must extend the migrator, the compatibility
  matrix, and the schema version in lockstep, with tests for each
  compatibility-matrix cell.
