# Processing Contract Compatibility

Single source of truth for which profiles, native cores, Python wheels, and
HDF5 files may be combined. Enforced in code by
`backend::processing::contract` (see
[`include/backend/processing/ProcessingContract.h`](../../include/backend/processing/ProcessingContract.h))
and mirrored on the frontend by
`processingcorecatalog::isProcessingContractCompatible`.

See [ADR 0001](../decisions/0006-processing-contract-v2.md) for the rationale.

## Version axes

| Axis | Meaning | v1 | v2 |
|------|---------|----|----|
| `processing_contract_version` | Science contract: metrics, units, subtraction semantics, preprocessing | `1` | `2` |
| `config_schema_version` | Config *document* shape (keys, grouping) | `1` | `2` |
| engine ABI (`MIB_PROCESSING_ENGINE_ABI_VERSION`) | Native plugin call boundary | `1` | `2` (added in V2-5) |

A given artifact declares the versions it was produced with. Consumers select
behavior from the declared versions; they never guess from field presence.

## Rule: contract equality, not ordering

A profile of contract *C* executes **only** against an implementation
(core / bundled kernel / wheel) that advertises the **same** contract *C*.
Contract 2 is not "newer and therefore acceptable" to a Contract-1 core, nor
the reverse — the science differs, so the match must be exact.

## Compatibility matrix

| Profile contract | Native core / bundled kernel | Python wheel | HDF5 file (read) | Result |
|---|---|---|---|---|
| 1 | contract 1, ABI 1 | contract 1 | contract 1 (ring ratio present) | ✅ execute / read |
| 2 | contract 2, ABI ≥ 2, required caps | contract 2 | contract 2 (Laplacian variance, no ring) | ✅ execute / read |
| 1 | contract 2 | contract 2 | — | ❌ refuse (contract mismatch) |
| 2 | contract 1 / ABI 1 | contract 1 | — | ❌ refuse (contract mismatch / missing caps) |
| 2 | contract 2, ABI 2, missing a required capability flag | — | — | ❌ refuse (capability gate) |
| any | — | — | file schema newer than reader | ❌ fail closed with diagnostic |

Capability flags are introduced by ABI v2 (V2-5): full pipeline, absolute
difference, filter chain, per-object Laplacian variance. A Contract-2 profile
may activate a core only if the core advertises the capabilities the profile
needs.

## One contract per shipped core (ADR 0007)

A shipped core implements exactly one contract, fixed at build time. This
covers the kernel bundled into a desktop build (`MIB_PROCESSING_CORE_CONTRACT`,
default `1`) and each native plugin line: `mib_processing_core` is
subtract-ring (Contract 1, exports only `mib_processing_get_api`), and
`mib_processing_core_absdiff_laplacian` is absdiff-laplacian (Contract 2,
exports only `mib_processing_get_api_v2`). A profile's root
`processing_contract_version` is a requirement. `ProcessingService` refuses
to generate masks or empty-frame decisions when the active core does not
serve it (`processingContractMismatch()`), with no fallback. A profile without
the key means Contract 1.

Only the Python wheel is built with `MIB_PROCESSING_CORE_CONTRACT=research`,
which runs either contract, selected per call.

The native loader pairs the engine ABI with the contract: ABI v1 loads only
Contract-1 cores (`mib_processing_get_api`), ABI v2 loads only Contract-2
cores (`mib_processing_get_api_v2` + Contract-2 capabilities). A Contract-2
core owns its object science: the host sends the full config
(`science_config_json`) and receives per-object metrics from
`process_objects`.

## Contract semantics (V2-8)

A config's `ProcessingConfig::processing_contract_version` (default `1`) is set
from a profile's root `processing_contract_version` by `AppConfigWatcher` or
from a Python config dict. A core runs it only when it serves that contract
(see above). Only `backend::processing::contract` interprets it:

| Helper | 1 | 2 |
|---|---|---|
| `contractUsesAbsoluteDifference` | saturating `cv::subtract` | `cv::absdiff` (kernel mask, empty-frame checks, realtime/batch loops) |
| `contractHasRingWidth` | ring ratio computed + gated | ring ratio `NaN`, gate ignored, no `Ring` invalid reason |
| `contractObjectsAreInnerContours` | object = inner contour (hole in the bright halo); `require_single_inner_contour` gates | object = top-level contour (absdiff blob, no halo); inner-contour rule ignored, no `NoContour` reason |
| `isSupportedProcessingContract` | ✅ | ✅ (anything else fails closed: no mask, `ValueError` in Python) |

The Python wheel (0.3.0+) executes both contracts; `CONTRACT_VERSION` stays
`1` (the default when a config omits the key) and
`SUPPORTED_CONTRACT_VERSIONS == (1, 2)`. Result dicts carry
`processing_contract_version`; a Contract-2 record has no `ring_ratio` key and
always has `laplacian_variance` (`NaN` when no object was detected).

## Config-schema handling (loading a profile document)

`classifyConfigSchema(sourceSchema, targetSchema)` drives loading:

| Relationship | Action |
|---|---|
| `source == target` | `Same` — merge missing defaults in memory, do not rewrite the file |
| `source < target` | `UpgradeNeeded` — preserve the source untouched; offer explicit copy-upgrade via the v1→v2 migrator |
| `source > target` or unknown/≤0 | `Incompatible` — fail closed with an actionable diagnostic; never silently rewrite |

An existing schema-1 file is **never** silently rewritten with schema-2 keys.
Upgrading is always an explicit, user-visible copy.

## v1 → v2 migration (`migrateProfileConfigV1ToV2`)

The migrator is the only sanctioned way to turn a v1 config document into a v2
document. It:

1. preserves all unrelated values (target group, multi-image, autofocus, etc.);
2. removes `ring_ratio_min`, `ring_ratio_max`, and
   `filters.enable_ring_ratio_check`;
3. renames `bg_subtract_threshold` → canonical `difference_threshold`;
4. installs a no-op (identity) preprocessing chain
   (`preprocessing.input = [identity]`, `preprocessing.difference = [identity]`);
5. adds `laplacian_variance_min` / `laplacian_variance_max` and
   `filters.enable_laplacian_variance_check = false` (gate disabled until
   calibrated in V2-7);
6. sets `processing_contract_version = 2` and `config_schema_version = 2`;
7. never selects or activates a core.

Recursive merging of v2 defaults onto a v1 profile is prohibited: it would
strand ring configuration and skip the intentional removals.

## Canonical vs legacy keys

| Concept | v2 canonical key | Legacy key (accepted only via adapter) |
|---|---|---|
| Difference threshold | `difference_threshold` | `bg_subtract_threshold` |

`resolveDifferenceThreshold(config)` reads the canonical key, falling back to
the legacy key, so a partially-migrated or hand-authored document still yields
one unambiguous value. New v2 code writes only the canonical key.
