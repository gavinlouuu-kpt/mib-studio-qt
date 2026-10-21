## 2026-10-22 — Reprocessing uses the source run's own E-LUT and px→µm (#764)

The #758 follow-ups ([[services/ReviewSession]], [[services/ProcessingService]],
[[data-model/HDF5-Storage]], [[frontend/YofoReview]]).

**The source run's E-LUT.** Reanalysis (MIB Studio's `review_reanalysis_json`),
YOFO Review's Regenerate masks and the one-record `review_reprocess_record`
used the live LUT (the facade), none (YOFO Review) or none (reprocess). Now a
source whose run snapshot v3 records its `emodulus_lut` is reprocessed with
that table (`SourceEModulusLut.h`):

- found by its recorded sha256 in the kernel's loaded LUT or the bundled LUT,
  which `mib_review_core` compiles in (`cmake/GenerateEmbeddedText.cmake`;
  YOFO Review ships no resources folder);
- recorded without a hash, or not available here: refused before anything is
  written, with a reason naming the LUT and the hashes present;
- `loaded` false: no E (E 0, an E-gated target group cannot pass), reported
  as not computed;
- no record (no snapshot, a snapshot before v3, folder / AVI sources): the
  behaviour before (Legacy), said in the reply: the facade's `warnings`, YOFO's
  completion message, the reprocess `settings_label`.

The table goes per call through `BatchAnalysisInputs::eModulusLut`; the
kernel's own table is never replaced, so a job cannot affect a live run.

**Review ABI 4.** `review_reprocess_record` names the LUT it used:
`emodulus_lut` (lut id, revision, the hash's first 12 hex digits) and
`emodulus_lut_sha256`, `null` in the shell's JSON when E was not recomputed.
4 was free on develop. The workspace UI (#774) still caveats the reprocessed
E; B3d can show the named LUT instead.

**A fallback px→µm is no longer "recorded".** `saveMasksToHdf5` writes
`/experiment_info @pixel_to_micron_source` beside the factor: `recorded` (the
source's own), `operator` (supplied for this output;
`ReanalysisRequest::pixelToMicronExplicit`, no host sets it yet) or `fallback`
(a host default or fallback). `ReviewSession::recordedPixelToMicron` takes
only the first two; files regenerated between #758 and #764 have no source
attribute and read as before. The Qt `BatchMaskDialog` marks its live factor
`fallback`.

**Metrics CSV.** For a run whose snapshot records `emodulus_lut.loaded ==
false`, Export Metrics to CSV, Export All and Batch Metrics leave every E cell
empty (the export's missing-value convention), not the stored 0. Legacy files
are written as stored ([[services/HdfExportService]]).

**Wheel.** `process_batch(pixel_to_micron=None, channel_band=None)`: a given
factor now decides area validity and the target group as well as `area_um2`
(it was only used for `area_um2`); a given band `(x, y, w, h)` gates objects by
centroid row for the call. Omitted, both behave as before. No version bump:
the wheel version is the processing-core release identity and is bumped at
release.

**One string-attribute reader** in `Hdf5Service.cpp` (`readGroupStringAttribute`
serves `readScalarStringAttribute` and the config document; caps unchanged).

Tests: `review.source_elut` (new), updated `review.inspect`,
`review.reanalysis_pixel_factor`, `review.jobs`, `tests/review_bridge.rs`,
the wheel's `TestProcessBatchCalibration`, `exports.test.ts`.
