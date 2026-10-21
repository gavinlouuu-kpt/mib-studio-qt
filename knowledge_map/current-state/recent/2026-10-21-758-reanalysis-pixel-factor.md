## 2026-10-21 — Reanalysis validates at the file's recorded pixel factor (#758)

Found during B3b (#756). The shared reanalysis engine
([[services/ReviewSession]], `Reanalysis.h`) ran `processBatch` on a
`ProcessingService` whose factor was never set to the source's. Area validity
and the target group were decided at the kernel's own factor, while review
converted areas with the file's:

- YOFO Review's Regenerate masks: 0.4886 µm/px (its bundled kernel);
- MIB Studio's `review_reanalysis_json` and the Qt `BatchMaskDialog`: the live
  factor.

A 1000 px object of a 0.25 µm/px recording was validated as 238.7 µm² but
reported as 62.5 µm².

**Per-call inputs, never the shared instance.** `processBatch` takes
`BatchAnalysisInputs{pixelToMicronFactor, channelBand}`, which replace the live
factor and the live channel band for that call only
([[services/ProcessingService]]). `offlineAnalysisInputs(config, background,
factor)` builds them. The band is detected from the run's own background, and
with `auto_roi_from_background` on it is written into the recorded config;
before, the facade gated objects by the live background's band. The facade's
kernel is the live service, so a job never sets anything on it: it cannot race
a live run or the export lane, and one source's factor cannot reach the next
job.

**Factor policy** (`ReanalysisRequest::useRecordedPixelToMicron` /
`pixelToMicron`; not in the JSON request, no contract change):

- An HDF source's recorded factor (`ReviewSession::recordedPixelToMicron`)
  wins.
- Otherwise, and for folder / AVI sources, the host's: the facade's live
  factor; YOFO's fallback preference with the recorded settings, and 0.4886
  with the default settings or AVI / folder sources.
- The Qt dialog uses the HDF source's recorded factor, else the live one.
- A batch of sources follows batch export's rule: each source is resolved on
  its own.

**What outputs record.** `saveMasksToHdf5` writes `/experiment_info
@pixel_to_micron`, the factor the objects were validated at
([[services/Hdf5Service]], [[data-model/HDF5-Storage]]).
`recordedPixelToMicron` reads it when the file has no run snapshot factor,
so a regenerated file reopens at its factor instead of "(fallback)". Outputs
also record the applied config document in `@config_json`. YOFO's completion
message ends with "px→µm 0.2500 recorded | default | fallback: the file records
none".

**Also found: the recorded target group was never read.** No
`processing_config_*` attribute carries the target group, so YOFO's
regenerate with the recorded settings selected none. `readRecordedProcessingConfig`
now takes it from the file's `@config_json` (`image_processing.target_group`;
captures write it, and so do regenerated outputs). Files without the document
keep the default (off).

**Tests.** The fixture is one disc per frame, valid and in the target group at
0.25 µm/px but not at 0.4886.

- `review.reanalysis_pixel_factor` covers `processBatch` inputs, the engine
  (recorded, default, fallback, folder, refused zero factor), the recorded
  target group, and four sources with different factors on one kernel. It also
  covers a live caller running beside six jobs, the run's own band against a
  live walled band, and YOFO's regenerate job (recorded, default, fallback, two
  files in turn, reopen).
- `backend.facade_reanalysis_pixel_factor` covers the same through
  `review_reanalysis_json`, with the live factor 0.7 unchanged during and after
  each job.
- `review.jobs` now expects the factor in the completion message and in the
  output.

Each of these mutations fails a test:

- the factor not applied;
- the first source's factor reused for later ones;
- the shared kernel set and restored around the call;
- YOFO's recorded factor dropped;
- the output records no factor;
- the reader ignores it;
- the target group not read;
- the facade's live fallback dropped;
- the band not passed.

**Notice.** A pending notice in `docs/release-notes/README.md` names the files
affected and asks to regenerate them. The manuals carry it too
(`docs/manual/yofo-review.md` and `docs/manual/review-and-postprocess.md`,
Regenerate masks). The S6 parity harness was not rerun: it runs no reanalysis,
and its four inputs (0.25 or 0.4886 in the run snapshot, or no factor) carry no
`@pixel_to_micron`.

**Left as is.** The wheel's `process_batch(pixel_to_micron=…)` still validates
at a fresh service's 0.4886 and uses the argument only for `area_um2`. The
engine does not load the E-modulus LUT a file names: only its identity is
recorded. See [[services/BatchMaskSources]], [[frontend/YofoReview]] and
[[frontend/Dialogs]].
