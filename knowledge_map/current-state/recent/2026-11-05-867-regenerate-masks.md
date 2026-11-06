## 2026-11-05 — Tauri Review: Regenerate masks matches the Qt BatchMaskDialog (#867 PR 2)

Both React/Tauri Review surfaces get the Qt `BatchMaskDialog`'s groups (matrix REVIEW-057,
-059, -060, -061, -062, -063, -064, -067, -069), on one set of shared components
(`desktop/src/review/regenerate/`: `regenerateForm.ts` rules, `RegeneratePanels.tsx`
`RoiPreviewPanel` + `ProcessingConfigGroup`).

- **Engine** (`backend/review/ReviewJobs.h`, review ABI 8): `RegenerateMasksRequest` gains the
  dialog's overrides (`imageProcessingJson` merged onto the recorded / default config,
  `overrideRoi`, `background` Recorded / None / Frame); `parseRegenerateMasksJson`;
  `ReviewJobs::regenerateSetup` (preview set and frame count, recorded config / ROI /
  background, Qt's core-mismatch text) and `regeneratePreview`, both gated on the lanes;
  `batch_masks::countSourceFrames` (AVI / folder). A completed `review_reanalysis_json` status
  adds `pixel_to_micron`, `pixel_to_micron_source` and `emodulus_lut`.
- **YOFO Review** (`RegenerateMasks.tsx`): the three Qt groups over
  `fetch_review_regenerate_setup` / `fetch_review_regenerate_preview` /
  `review_regenerate_masks_json`; Count 100 by default; the synthetic box on by default;
  `<base>_remasked.h5` beside the source; the factor and E-LUT named before Run; the
  core-mismatch warning before Run.
- **MIB Studio** (`reanalysisControls.tsx`, `areas/review/`): the same groups over the facade
  (`fetch_review_reanalysis_preview`, the live config for "Reset to live defaults"); the
  entire-file box and Start / Count 0 / 100; the input budget defaults to the contract maxima;
  `<base>_remasked.h5` beside the source; the core-mismatch warning before Run; the completed
  status names the factor and E-LUT; the new file opens in the Review stage.

Decisions (PR body and #867): the backend never overwrites (#512 D6), so no overwrite prompt;
YOFO keeps the Preferences fallback factor (no live processing), MIB Studio follows the live
factor as Qt; YOFO's reset goes to the recorded config (the defaults for AVI / folder); both
keep the file's saved background by default (Qt uses none); the core warning is shown in the
dialog before Run rather than as a second modal; YOFO's From stays 1-based.

Tests: `review.jobs` ("regenerate dialog inputs", mutation-checked), `review.reanalysis`
(status bytes), `tests/review_bridge.rs`, `regenerateForm.test.ts`, `exports.test.ts`,
`reanalysisControls.test.tsx`, the `reviewIpc` golden (the two regenerate flows changed on
purpose, one flow added). See [[services/ReviewSession]], [[frontend/YofoReview]],
[[architecture/Rust-Bridge]], [[architecture/Desktop-Shell]].
