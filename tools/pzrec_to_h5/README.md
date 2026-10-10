# pzrec_to_h5: SSD run records to a MIB Studio HDF5 experiment (#667)

Converts what Studio's `GET /ssd/runs/{id}/records` streams (a `pzrec read` stream: whole 59,392 B stored records of one closed SSD run) into the experiment layout YOFO Review
and MIB Studio open: `/valid_frames` and `/invalid_frames` with `images`, `masks`, `metadata` and `/experiment_info`.

```bash
# from a downloaded file (the run table entry from `pzrec runs` or the Studio runs listing)
python3 tools/pzrec_to_h5/pzrec_to_h5.py convert run18.bin --runs runs.json --run 18 --out run18.h5 --expect-records 27119
# download and convert in one go (token as for the Files view); the run table entry comes from the X-Run-* headers of the download
# (the self-describing route, #691 d3fe7421; header block pinned in testdata/route-headers-691.txt), --runs runs.json overrides it
python3 tools/pzrec_to_h5/pzrec_to_h5.py fetch http://192.168.137.2:8427 --token T --run 18 --out-dir exports/
python3 -m pytest tools/pzrec_to_h5          # synthetic records always, the S2 reference records when the HDD is mounted
```

## What is verified (and what is not)

While converting, every record is checked with the vendored pz7035 reference decoder (`third_party/pz7035-decoder`, pinned and sha256-verified against `PROVENANCE.json`; layout in its
`tools/pzrec/pzrec_records.md`): the stream is a whole number of records and has the expected count (`--expect-records`, or `X-Record-Count` of a download), every wire record's CRC, magic,
length and sequence, the set structure (FRAME first, IMAGE blocks inside the record, padding and tail zero), the run id, ascending frame ids and timestamps, and `drops_before` against the
frame id gap (`<= min(gap, 255)`; exactly equal when the run table proves every gap frame was a drain drop: filter ALL and `seen` equal to the frame id span).
**The MONO8 and MASK1 blocks are not covered by any CRC** (pz7035 issue #65): this tool checks their structure, not their pixels. M1 substitute: compare with Studio's ring playback by
(epoch, frame_id) for the frames that are in both (see the recent note).

A failed input produces **no HDF5 file** (the output is written as `.partial` and renamed only when everything passed). The input is reported in `quarantine/<name>.report.json` (stream
problems, the bad records with index and code, sha256); a downloaded file is moved there, a file you gave is left where it is. A download that is short, refused (HTTP 503 with the
server's reason) or interrupted is quarantined the same way. `--salvage` converts the intact records of a damaged stream instead, says so in `/experiment_info` (`salvaged`, `config_json.skipped_records`) and still writes the report.

## Layout

| | |
|---|---|
| `/valid_frames/images`, `/masks` | uint8 (N, 96, 512): the raw MONO8 window; the U-Net mask as 0/255. One row per RESULT, the image and mask repeated on every row of a frame (as Studio does). Rows are routed **per object**, as Studio's `appendExperimentFrame(row, isValid)`: a valid cell of a good frame goes to `/valid_frames`; a rejected cell (reason != NONE), a cell of an EMPTY/INVALID frame and a frame without results go to `/invalid_frames`. `total_valid_frames` counts rows. |
| `/valid_frames/metadata` | Studio's `ProcessedFrameMetadataRecord` (fields by name). `index` = **frame_id**, `timestampNs` = wall unix ns, one row per RESULT: area = hull area, centroid, bbox, deformability, E-modulus (kPa), Laplacian variance, brightness mean/variance, contour area, pixel and blemish counts, `isValid` = reason NONE. A payload word the profile flags invalid is NaN (Studio's rule); the two areas (`area`, `contourArea`) are 0.0 then, as in Studio. `brightness_mean`, `brightness_variance`, `contourArea`, `pixelCount`, `blemishCount` and `degenerateContour` are members Studio's record has too (appended); the compound is the full Studio type, field order tolerated by name. |
| `/valid_frames/ssd_meta` | parallel to `metadata`: `ssd_run_id`, `frame_id`, `record_index` (ordinal in the stream), `ticks`, `wall_unix_ns`, `drops_before`, `epoch`, `frame_flags`, `result_index`, `result_count`. |
| `/invalid_frames/...` | the same for the rows above that are not valid cells. |
| `/experiment_info` | `start_time_ns`/`end_time_ns` (wall), totals, ROI 0,0,512,96, `processing_core_source`, `ssd_run_id`, `salvaged`, `config_json` (run table values, filter, tick rate, input sha256, decoder commit, converter version). |

Wall time = `start_unix_ms` x 1e6 + (ticks - first_ticks) / tick_hz from the run table (accurate to the Start-to-first-frame delay; `wall_source` 0 means the board's client clock was not synced, flagged in `config_json`).
Centroids are unsigned q16.16 as in the vendored profile (the RTL yields non-negative ROI coordinates); Studio's own decoder reads them signed, which only differs above 32768 px. A record whose ticks are before the run table's `first_ticks` is refused (`TIME_BEFORE_RUN`: the entry does not belong to the stream).
Results of a profile other than U-Net cells v3 keep the platform envelope only (bbox, validity); numbers are NaN, a warning is reported.

## Size

Rows are duplicated per RESULT and nothing is compressed by default: a 1.6 GB run (27,119 records) becomes about 3-4 GB of HDF5 (`--gzip`, level 4 with shuffle, is about 7x slower but much smaller). Keep that much free next to the input; the output is written as `.partial` first.

`convert` expects the run table's count for the whole run (`--expect-records`, or `--from/--count` for a window); a short stream of a longer run is refused. `fetch` checks the response headers (X-Run-Id equals `--run`, X-Record-Bytes 59392, X-First-Record equals `--from`, Content-Length equals X-Record-Count x 59392) before it reads the body; garbled `X-Run-*` values are a clean error.

## Review check

`tests/review/ssd_h5_review_test.cpp` (registered when `MIB_SSD_H5_REVIEW="<file.h5> <valid rows> <invalid rows> [<first frame id>]"` is set at configure time) opens a converted file with
`backend::review::ReviewSession`, the one implementation behind the Qt tab, MIB Studio and YOFO Review: counts, image and mask shapes, mask values, metadata `index`, overlay, scatter.
