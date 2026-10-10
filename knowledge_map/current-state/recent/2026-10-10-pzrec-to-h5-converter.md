## 2026-10-10 — PC-side converter from a downloaded SSD run to a Review-readable HDF5 (#667, `tools/pzrec_to_h5`, no ABI change)

`tools/pzrec_to_h5/pzrec_to_h5.py` turns what `GET /ssd/runs/{id}/records` streams into a MIB Studio experiment file (`/valid_frames`, `/invalid_frames` with images, 0/255 masks and Studio's
metadata record, `/experiment_info`), plus a parallel `ssd_meta` dataset (`ssd_run_id`, `frame_id`, `record_index`, `ticks`, `wall_unix_ns`, `drops_before`, epoch, flags) so every row can
be traced to its record. `index` of the metadata is the frame id; wall time is `start_unix_ms` + ticks since `first_ticks` at `tick_hz` from the run table (`pzrec runs` or the Studio runs listing).
Decoding uses the pz7035 reference decoder vendored byte for byte in `third_party/pz7035-decoder` (pz7035 main 6d1937d1, #64; `PROVENANCE.json` holds the sha256 of every file, the
converter refuses a modified copy).

Verified while converting: whole records and the expected count, every wire record's CRC and structure, run id, ascending frame ids and timestamps, `drops_before` against the gap. A failing
input yields no HDF5 (written as `.partial`, renamed on success), is reported and quarantined (`quarantine/<name>.report.json`); `--salvage` keeps the intact records and says so in the file.
The MONO8/MASK1 blocks have no CRC (pz7035 #65, ABI 1.6): the tool checks their structure only. **M1 gate 5 substitute:** compare Review's image and mask for a `frame_id` with Studio's ring
playback of the same `(epoch, frame_id)`: after a graceful Stop the newest `ring_records - GUARD` (about 4744) passing frames before the drain's STOP are on the SSD and in the ring
(pz7035 `pzrec_records.md`, "Frames in both the ring and the SSD"); older ring frames and frames a filter skipped may be missing on the SSD, and after an ABORT there is no guarantee.

Tests: `python3 -m pytest tools/pzrec_to_h5` (synthetic records built with the vendored encoder: layout, values, truncation, CRC, run id, order, drops, salvage, unknown profile, tampered
decoder, fetch against a stand-in server: ok, short body, 503 with reason, bad stream; the fixed S2 reference records on the HDD: image, mask, wall time and metadata equal the decoder's
`rows()`), and `ssd_h5_review_test` (a converted real window opens in `ReviewSession`: 20 records of run 18 = 40 valid rows, images 96x512, masks 0/255, metadata index = frame id).
Speed: 3,000 synthetic records (178 MB) convert in 0.7 s with 124 MB RSS.
