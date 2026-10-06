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
| 3 | a PL core: bitstream plus weights, reporting `science_profile` 2 / `profile_version` 2, through an execution provider (PZ7035) | — | contract 3 (brightness mean and variance, contour area, pixel and blemish counts; no quartiles, no ring) | ✅ the instrument executes it, the host reads the recording |
| 3 | any host core, bundled kernel or loader (`isSupportedProcessingContract(3)` is false) | contract 1/2 only (`SUPPORTED_CONTRACT_VERSIONS == (1, 2)`) | — | ❌ refuse, fail closed (no mask, no fallback) |
| any | — | — | file schema newer than reader | ❌ fail closed with diagnostic |

The ABI encodes Contract 3 as `science_profile` 2 / `profile_version` 2
(pz7035-imx426 `abi/profiles/unet_cells_v2.json`); the two map 1:1 (ADR 0011).

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

The third line is **`unet-cells`** (Contract 3, registered here as ADR 0007
requires). On the PZ7035 its core is a PL bitstream plus its model weights
(ADR 0011, decision 12): a new bitstream or retrained weights is a new core
version of the same contract. A host `unet-cells` plugin for desktop
reprocessing is optional and is not an instrument dependency.

Only the Python wheel is built with `MIB_PROCESSING_CORE_CONTRACT=research`,
which runs either contract, selected per call.

The native loader pairs the engine ABI with the contract: ABI v1 loads only
Contract-1 cores (`mib_processing_get_api`), ABI v2 loads only Contract-2
cores (`mib_processing_get_api_v2` + Contract-2 capabilities). A Contract-2
core owns its object science: the host sends the full config
(`science_config_json`) and receives per-object metrics from
`process_objects`.

## Reference per contract

What "correct" means differs by line, and so does how it may change.

| Contract | Line | Reference | Check | A change is |
|---|---|---|---|---|
| 1 | `subtract-ring` | the frozen host science: `scripts/conformance/focus-50v-real-contract1.json` | host and native-core gold gates | a gold-reference change, with the `gold-reference-change` label (ADR 0007) |
| 2 | `absdiff-laplacian` | the Contract 2 gold: `scripts/conformance/focus-50v-real-contract2.json` | the same, plus `scripts/run_native_core_conformance.py` against the built core | the same |
| 3 | `unet-cells` | **the PL specification**, owned by pz7035-imx426: `abi/profiles/unet_cells_v2.json` and its vectors, vendored from a tag (ADR 0011, decision 10) | `processing.contract3_cells_conformance` and `processing.pz_unet_cells_host` against `scripts/conformance/unet-cells-v2-pl-vectors.json`; `scripts.pz7035_abi_vendor` pins the bundle | a new profile version in pz7035-imx426, then a new contract here |

The tests that exercise the PL path without hardware carry the ctest label
`pl`. CI runs them as the named "PL replay lane" step of `backend-ci`
(`ctest -L pl`).

## Contract semantics (V2-8)

A config's `ProcessingConfig::processing_contract_version` (default `1`) is set
from a profile's root `processing_contract_version` by `AppConfigWatcher` or
from a Python config dict. A core runs it only when it serves that contract
(see above). Only `backend::processing::contract` interprets it:

| Helper | 1 | 2 | 3 (defined, not yet served) |
|---|---|---|---|
| `contractUsesAbsoluteDifference` | saturating `cv::subtract` | `cv::absdiff` (kernel mask, empty-frame checks, realtime/batch loops) | — (masks come from the U-Net) |
| `contractHasRingWidth` | ring ratio computed + gated | ring ratio `NaN`, gate ignored, no `Ring` invalid reason | as 2 |
| `contractObjectsAreInnerContours` | object = inner contour (hole in the bright halo); `require_single_inner_contour` gates | object = top-level contour (absdiff blob, no halo); inner-contour rule ignored, no `NoContour` reason | as 2 |
| `contractObjectsAreUnetCells` | — | — | U-Net cell rules, below |
| `isSupportedProcessingContract` | ✅ | ✅ (anything else fails closed: no mask, `ValueError` in Python) | ❌ until a core serves it |

The helpers match each contract by equality, so a new contract inherits no
behaviour from an older one.

### Contract 3 — `unet-cells` (defined 2026-10-04)

The science of the PZ7035 PL cell stage (pz7035-imx426 profile
`unet_cells_v2`), so host and PL report the same cells from the same U-Net
mask. `science::filterUnetCellObjects`:

- an object is a top-level 8-connected mask component (components inside a
  hole are ignored). With at least `min_cell_area_px` pixels it is a cell;
  smaller ones are blemishes, counted per frame (`FilterResult::blemishCount`);
- cells are ordered by bounding box (x, then y) and numbered from 1;
- cut-off: a component pixel on the ROI edge (1 px rule, always checked);
  brightness, Laplacian and centroid are still reported;
- a contour that encloses no area is degenerate: reason `NoContour`, centroid
  at the bounding-box centre;
- otherwise the Contract 2 outer-contour metrics and gates, with no ring width.
  Brightness is the mean and population variance over the filled contour
  (`brightnessMean`, `brightnessVariance`), and the Laplacian aperture is
  `laplacian_kernel_size` (1 or 3);
- the reasons follow the PL's single-reason order: Border, NoContour, Channel,
  Area, Deform, AreaRatio, Laplacian.

The gold reference is `scripts/conformance/unet-cells-v2-pl-vectors.json`, a
subset of the PL conformance vectors (`scripts/build_unet_cells_vector_subset.py`).
`processing.contract3_cells_conformance` checks every payload word within the
profile tolerances. The full PL set (45 frames, 181 cells) also passes.

`scripts/compare_metrics.py` compares Contract 3 documents: no quartiles,
plus `brightness_mean`, `brightness_variance`, `contour_area`, `pixel_count`
and `blemish_count`. A null brightness matches only null.

No host core, wheel or loader serves Contract 3 yet. On the PZ7035 the PL core
does, through an execution provider (ADR 0011). Its recordings carry the core
that produced them: the run snapshot's `pl_core` holds the PL build id (the git
commit prefix), the weights sha256 prefix, the ABI version and the science
profile (see [HDF5-Storage](../../knowledge_map/data-model/HDF5-Storage.md)). The science JSON adds
a `unet_cells` block (`min_cell_area_px`, `laplacian_kernel_size`) only for
Contract 3, so Contract 1/2 documents are unchanged.

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
