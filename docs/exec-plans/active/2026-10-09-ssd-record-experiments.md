# YOFO Studio Record to the SATA SSD: experiments, export, UI (design note for #667)

Status: active (design, no code)

## Goal

[#667](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/667): on the PZ7035, Record writes the experiment to the
SATA SSD through the PL writer (coordinator decisions 1 to 6 in the issue), Studio lists the SSD runs as experiments and exports
one as the PC experiment HDF5, which YOFO Review opens unchanged. This note is the Studio side. It builds on the parity spec
([2026-10-08-playback-save-parity.md](2026-10-08-playback-save-parity.md), #653) and on the board owner's
`docs/FRAME_RING_FEASIBILITY.md` (pz7035 #33). Order of work stays: #649 v1 (ring playback and Save clip to the eMMC) first.

Facts used (all from repository files, none newly measured): store record 59,392 B (4 KiB record set + 49,152 B image +
6,144 B packed mask); SSD sustained about 110 MB/s rested and about 37 to 46 MB/s after the SLC cache is spent; eMMC
`yofo-data` 7 GiB free and about 22 MB/s (#666); uncached `/dev/mem` read 68 MB/s; run table of 29 slots with 16-byte entries
`{start_lba, records, end_lba, flags}` and a `PZRT` trailer sector per run (`firmware/pz7035/sata_rec.h`, `store_rec.h`).

## Findings that shape the design

1. **The PC has no experiment list.** There is no list widget in `desktop/src`: an experiment is a file the user opens in Review
   (or finds in the Files view, #661). "Shown like the PC experiment list" therefore means: a list whose rows carry what
   `/experiment_info` and `accounting_*` carry in the HDF5 (start, end, valid and invalid counts, completion state), so a row
   matches what Review shows after the export. It is a new view, modelled on the Files view.
2. **PC Record and PC Experiment are different things.** PC "Record" is the raw recording (images only, no mask, no rows,
   no gates); "Experiment" is images, masks, metadata rows, readiness gates, provenance and accounting. Decision 1 (image, mask,
   RESULT rows) is the Experiment content. See D1 below for the control name.
3. **The existing converter is not the product exporter.** `pz_store_to_mib.py` is a PC tool: it holds a whole run in
   memory, derives most metadata columns from the mask (the PL's own cell words are ignored), and writes no
   `accounting_*`, `run_provenance` or pixel-to-micron. It stays the reference for the *layout* (images and masks stacked per
   RESULT row, mask as 0/255, `/valid_frames` and `/invalid_frames`, `/experiment_info`), and the test oracle. The in-box
   exporter reuses Studio's own `Hdf5Service` writer and `filterResultFromUnetCell` (the same mapping a live PZ experiment uses),
   so a row is identical whichever way the run was made.
4. **The run table is too small for decision 6.** It has 29 slots (older runs are overwritten by `run % 29`) and no start time,
   duration, drop count or reason in the entry (the reason is in the trailer sector). Requests R4 to R6 below.
5. **The board has no trustworthy wall clock** (RAM root, JTAG boot, no RTC): the start time must come from Studio at RUN_START
   (the browser or server clock, flagged when not synchronised), not from the PL.
6. **The idle rule collides with long recordings.** The server stops and saves when the last client has been gone for about
   6 s (`crates/mib-bridge-server/src/lib.rs`). For a PC experiment that is the parity behaviour; for an unattended SSD run
   it would end the recording. See D4.

## Decisions (coordinator, 2026-10-09: D1 to D4 accepted, with the changes noted)

| # | Question | Recommendation |
|---|---|---|
| D1 | One control or two? "Record" (Gavin's word) vs the existing Start/Stop Experiment. | **Accepted.** **One control: the existing Experiment start/stop with a storage target `ssd`**, labelled "Record to SSD" while the SSD is ready, "Record to eMMC (metadata only)" otherwise. It keeps the gates, provenance, accounting and idle handling the PC experiment has. PC raw "Record" stays hidden on the PZ. |
| D2 | Where does the export land? | **Accepted with a change.** Staged on the eMMC with the space check (section 3). A run bigger than the free space exports automatically in consecutive parts by frame-id range (section 3.2); the user downloads a part, deletes it, then exports the next. Raw-run download stays a later phase. v1 is uncompressed (the PC writer has no deflate); measuring compression is a follow-up (section 3.3). |
| D3 | Does Stop during a recording close the run and freeze the ring for review? | **Accepted** (section 5). |
| D4 | Idle rule for SSD runs. | **Accepted with bounds.** The idle stop stays unchanged for every other mode. The opt-in "keep recording when I leave" applies only to `storage=ssd` runs and requires a **maximum duration entered at start**: there are no unbounded unattended runs. The run ends through the normal Stop path at that time, so LED and strobe shutdown is identical. The run card shows "unattended until HH:MM". |

## 1. Run control through the PL register window (a)

Linux owns it (decision 4): Studio, the single `/dev/mem` owner, starts and stops runs, reads status and the run table, and
reads runs back. The window is the board owner's to define; Studio needs the following from it. These are requirements
(R-numbers are used in the message to the board owner), not register addresses.

| R | Need | Why |
|---|---|---|
| R1 | **State** word: ABSENT, INITIALISING (IDENTIFY, superblock), RECOVERING (mount-time recovery, or a core-reset recovery during a run), READY, RECORDING, STOPPING (draining), WEDGED (drive not answering; the A400 is self-powered, the board cannot cycle it), FULL; plus a last-error code. | UI states (section 4); start gate. |
| R2 | **Start / stop commands** with arguments: filter mode (all frames, frames with a cell, frames with a valid cell), include-INT8 off, and a 64-bit **start time (unix ms)** and a 32-bit **client tag** written before start. Stop is graceful (drain to disk, write trailer, close run) and returns a run id; an *abort* is separate. | D1, section 1.1. |
| R3 | **Live counters** (read-only, coherent snapshot): frames seen, frames filtered as empty, frames passed by the filter, records on disk, records dropped by overload, records failed (not recoverable), drain recoveries, bytes written, ring fill (records waiting) and drain rate. | Honest accounting (section 4). |
| R4 | **A larger run table (hard requirement: no silent overwrite)**: at least 256 entries (a table of 64-byte entries in sectors after the superblock), no `run % N` overwrite while there is free space; an explicit "run table full" state. | Experiment list. |
| R5 | **Run entry fields**: run id, start unix ms and client tag (from R2), first and last frame id, first and last PL timestamp ticks and the tick rate (duration is derived), records, frames seen, frames empty, dropped, failed, drain recoveries, completion reason (0 clean, 1 drain fault, 5 not stopped, 6 length limit, 7 recovered at mount, as in `store_rec.h`), flags. | Experiment list and export. |
| R6 | **Drop placement**: each stored record carries `lost_before` (a u32 in its FRAME record: records dropped since the previous stored record). | Studio can reconstruct exactly where the run has gaps and book them (`sequenceGaps`), without a per-run list that can overflow. |
| R7 | **Read DMA**: `read(lba, sectors)` into a PS buffer in the PL carve-out (size and base reported by the window; at least 16 MiB, double-buffered or a ring of chunks), completion by status or IRQ, error code on a failed read. Refused (clear status) while RECORDING. | Export (section 3); replaces the 512 B port. |
| R8 | **Ring and drain coexistence**: the drain is a pure consumer of the shared store ring; stopping a run does not clear the ring and freezes it as #649 specifies. A ring mode in which overwrite is loss (SSD drain) or not loss (review only) is set per run. | Section 5. |
| R9 | **Mount-time recovery** reports what it closed: count of runs closed, their ids, reason 7, in the window and in the run entry. | Section 4. |

### 1.1 Studio side

New bridge commands (JSON, additive, listed in `bridge-contract.json` when implemented; the ABI number is decided then):
`ssd_status`, `ssd_runs {offset, limit}`, `ssd_run_info {run}`, `ssd_export_start {run, first?, last?}`, `ssd_export_status`,
`ssd_export_cancel`, `ssd_export_delete {run}`; `experiment_start` gains `storage: "ssd" | "emmc"` and `keep_recording`.
A platform capability `ssd_store` is true only when the PL image reports the writer, so the bundle and UI degrade to today's
behaviour on an older image. The PS code is one class, `SsdStore`, behind an interface (`ISsdDevice`: window registers and the
read DMA) with two implementations: the `/dev/mem` one and a file-backed fake for tests (section 6).

Run start order (per the established Run-entry sequence): the experiment gates pass (new gate `storage.ssd`: READY, not
RECOVERING, free space for at least 60 s at the typical rate, run table not full, clock flag), Studio writes R2 arguments, sets
START, waits for RECORDING with a timeout, then books the run in the experiment coordinator exactly as a PS experiment (the
provider already ingests PL results, so the live results session and the Run preview keep working). Stop is the reverse; the
coordinator's finalize waits for STOPPING to end or for a timeout, reads the run entry back and compares it with its own
counters (a mismatch is shown, not hidden).

## 2. The experiment list (b)

Source: the SSD run table (R4, R5), read through the status path (no DMA needed; each entry is a few words). Columns:

| Column | Source |
|---|---|
| Id, name (default `run-<id>-<local start time>`) | run id, start unix ms |
| Started, duration | R5 start time; ticks / rate |
| Frames seen, stored, empty | R5 |
| Dropped (count and percent of frames that passed the filter) | R5 |
| Completion | clean; clean, ended at the length limit; partial (declared drops); recovered after power loss; failed |
| Size on SSD, estimated HDF5 size | records x 59,392 B; records x about 98.5 KB |
| Export | none, exporting (progress), ready on the eMMC (size, Download, Delete), failed (reason) |

Actions: Export (full or range), Download, Delete export, Open in Review (only after the export, using the existing review open
on the board's path, or download and open on the PC). **No delete of the SSD run in v1** (the run table is append only; erasing
a run is a separate decision for the board owner and Gavin). Paging and permits reuse the Files view machinery
(`filesView.ts`, page of 2,000 entries); at 256 runs one page is enough.

## 3. Export of an SSD run to the PC experiment HDF5 (c)

### 3.1 Mapping

Per record in the run, in frame-id order:

1. Decode the store record (the wire decoder the PS already has for the result ring, plus the IMAGE records for the blocks).
2. For each RESULT record (cell): one row. Row fields from `filterResultFromUnetCell` (the live PZ path, so identical rows),
   frame index = frame id, timestamp ns = ticks x 1e9 / tick rate (the run entry's rate; 59.4 MHz today).
   Image = the MONO8 block, mask = the MASK1 block unpacked LSB first to 0/255, stacked once per cell exactly as the PC does and
   as `pz_store_to_mib.py` does.
3. A record whose cells are all invalid goes to `/invalid_frames`, other records to `/valid_frames`; the invalid group holds
   whatever the filter mode stored (with mode "valid cell" it is empty; with mode "any cell" it holds every invalid-only frame, with no
   sampling, unlike the PC's sampled invalid group; documented in `config_json`).
4. `/experiment_info`: start and end time (unix ns from R5 for the start; end = start + duration), totals, ROI = full 512 x 96,
   processing-core provenance from the run's `core.json`, `config_json` with the profile and the exporter version. `/run_provenance`
   with `pixel_to_micron` (from the config of the run; if unknown the Review fallback applies and is labelled so), `science_placement:
   "pl"`, PL build id. All written by the same `Hdf5Service` calls the live path uses.
5. `accounting_*` (schema 1) is filled from the run entry and the `lost_before` fields (table below). Missing the PC-only terms
   (ring ratio, brightness quartiles) stay NaN, as in a live PZ experiment.

| Run entry | Accounting |
|---|---|
| frames seen | `admitted` |
| frames empty | `empty` |
| records on disk | `persistenceCommitted` |
| dropped by overload (declared, decision 2) | `cancelledByPolicy` and `persistenceCancelledByPolicy`, `policyAllowsDrops = true` |
| failed, not recoverable | `persistenceFailed` |
| `lost_before` positions | `sequenceGaps`, `sequenceGapFrames`, gap list |
| reason 0 or 6, no drops | `complete` |
| reason 0, declared drops only | `intentionallyPartial` |
| reason 1 or 5, failed records | `incompleteLoss` (`failed` if the run produced nothing) |
| reason 7 (recovered at mount) | `incompleteLoss`, `reconciled = false`: the true totals are unknown, the file says so |

### 3.2 Where the file lands, and why not streamed

- **Stage on the eMMC data partition**, `<data-dir>/exports/run-<id>-<start>.h5`, written as `.run-<id>.part` and renamed when
  complete, so a crash, cancel or power loss never leaves a file that looks finished. Stale `.part` files are removed at server start.
  Served afterwards by the Files route (#661, Range, 2 concurrent downloads), which already confines paths and checks Origin.
- **Streaming the HDF5 into a download is not feasible**: the HDF5 library needs a seekable file (chunk index and superblock are
  rewritten at close). Hand-writing a streamable layout would be a second writer, with its own parity risk. Rejected.
- **A raw-run download is the streaming path** (phase S3): the store records from the SSD DMA straight into the HTTP body
  (`/ssd/runs/<id>/raw`, Range, same gates), no staging, no eMMC limit. The PC then runs `pz_store_to_mib.py` (extended to read a
  stream with the run header). It is the answer for runs that do not fit the eMMC, at the price of a PC-side step.
- **Space check, before the export starts and again every 64 MiB written**: required = estimated HDF5 size x 1.05 + 512 MiB
  reserve (settings, saved clips, logs). Estimate = records x 98.5 KB (image 49,152 + mask 49,152 + a 192-byte row, plus HDF5
  overhead). Refused with the numbers and the largest frame range that would fit. Mid-export `ENOSPC` aborts, deletes the part file,
  and says so. Existing exports are never auto-deleted.
- **Runs larger than the free space export in consecutive parts** (decision D2). Studio computes the part size from the free space
  (free minus the 512 MiB reserve, divided by 98.5 KB per record, rounded down to whole records) and splits the run by frame-id
  range into N parts; the UI says "part 1 of N". An export job produces one part; the user downloads it, deletes it, and starts the
  next part (the list row offers "Export part k of N" and shows which parts are on the eMMC). No range arithmetic for the user.
  Each part is a complete experiment file: the full run provenance, `accounting_*` for its own frame range (the run totals appear
  in `config_json` as `run_totals`, so a part is not mistaken for the whole run), and its frame range and part number in
  `/experiment_info` (`export_part`, `export_parts`, `export_first_frame`, `export_last_frame`). A part boundary never splits a
  frame, and a gap (`lost_before`) that falls on a boundary is booked in the part that contains the following record. A run that
  fits is a single file without part attributes.
- Only one export runs at a time, and not while a run is RECORDING (R7 refuses the DMA); the UI says "Export available after the
  run is stopped".

### 3.3 How long it takes (estimates, to be replaced by a board measurement)

Rates used: typical scene 86 valid frames/s (issue decision 1); eMMC write 22 MB/s (measured, #666); PS read of the DMA buffer
68 MB/s (measured uncached `/dev/mem`, feasibility note); SSD read about 130 MB/s (measured, `SATA_BRINGUP.md`). The eMMC write is the
slowest stage, so the stages overlap and the time is about the HDF5 size / 22 MB/s.

| Run | Records | SSD | HDF5 | Export time | Fits the eMMC (7 GiB)? |
|---|---|---|---|---|---|
| Typical, 1 min | 5,160 | 306 MB | about 0.51 GB | about 25 s | yes |
| Typical, 10 min | 51,600 | 3.06 GB | about 5.1 GB | about 4 min | yes (70 %) |
| Typical, 13 min | about 67,000 | 4.0 GB | about 6.6 GB | about 5 min | limit; longer needs a range or S3 |
| Large: typical, 1 h | 309,600 | 18.4 GB | about 30.5 GB | about 23 min if it fitted | **no**: range export (about 13 min at a time) or raw download (about 4.5 min of SSD read) |
| Dense, 60 s (SSD-limited at about 37 MB/s, about 620 records/s) | about 37,000 | 2.2 GB | about 3.7 GB | about 3 min | yes |

Compression (gzip on the chunks) could roughly halve the file but is not in the PC layout (the PC writer has no deflate) and its ARM CPU cost is
unmeasured; v1 stays uncompressed. **Follow-up item:** measure gzip level 1 and LZF on the mask dataset on the A9 (the mask is mostly zeros, so
it should shrink far more than the image). If the eMMC write rate is the bottleneck, a cheaper filter may cut the export time; h5py and Review read
filtered datasets transparently, so only the ARM build's HDF5 filter support and the CPU cost need checking. Download to the PC then adds file size / min(eMMC read about 23 MB/s, network): 5.1 GB about 4 min.

## 4. UI states (d)

The Run page gets a storage strip (also shown in Diagnostics) and the Experiments view gets row badges. States:

| State | Where | Shown | Behaviour |
|---|---|---|---|
| **SSD absent** (R1 ABSENT) | strip, start gate | "No SSD detected" | Record to SSD disabled with the reason; the eMMC metadata-only experiment stays available. No spinner. |
| **Initialising / recovering** (INITIALISING, RECOVERING) | strip | "SSD starting" / "SSD recovering (n runs closed)" with elapsed time | Start disabled; auto-clears. After a mount-time recovery (R9) a notice names the runs it closed and links to them in the list. |
| **Ready** | strip | model, free space, "about N h at the typical rate" (explicitly "dense scenes use more") | Start enabled. |
| **Recording** | strip, run card | stored records, empty frames skipped (neutral, not an error), dropped (amber when above zero, red above 1 % of passed frames), recoveries so far (informational: 1 to 2 per GB is normal), write rate, ring fill, elapsed | A drop count is cumulative and never resets inside a run; the card shows the rate over the last 10 s so a burst is visible. |
| **Stopping** | strip | "Writing the last N records, about s s" | Controls locked; from the ring fill and the drain rate. |
| **Wedged / error** | strip, banner | "SSD not responding. The drive is self-powered; power-cycle it." | The run (if any) is closed on a best-effort basis; the list shows it as failed or recovered; start disabled. |
| **Full / run table full** | strip, start gate | "SSD full" | Start disabled; the list explains. |
| **Run closed by mount-time recovery** | list badge, detail | "Recovered after power loss: N records found, true totals unknown" | Selectable and exportable; the export marks `incompleteLoss`, `reconciled=false`. |
| **Drops, finished run** | list badge | "Partial: N dropped (x %)" | The same counts as in the exported accounting, so Review's accounting panel agrees. |

All text follows the existing honest-state rules: no state is hidden, no count is estimated when the hardware reports it.

## 5. Record, buffer playback and Save clip (e)

Premise (decision 3, R8): one store ring in PS DDR; the SSD drain is a consumer of it; Stop freezes the ring.

- **Yes: record and still Stop and review the ring.** Stop first closes the run (STOPPING: the drain finishes the records that passed
  the filter and the trailer is written), and then the ring is frozen. The review buffer is the last 0.9 to 2.1 s of **every** frame
  (feasibility note: 256 to 600 MiB), whether or not the SSD run kept them. A cell frame that the SSD dropped under overload is
  still in the ring if it is recent, and the playback marks it ("not on the SSD").
- **Save clip** (to the eMMC, #649) works after Stop exactly as without a run; it is independent of the SSD run, and a clip and an
  export write to the same eMMC (22 MB/s shared), so Studio runs them one at a time and says which one is waiting.
- **While recording** the ring is moving, so playback and Save clip are not offered (the live results view continues). Reading
  the ring range while running is possible (feasibility 4.2) but not needed.
- **Ring mode conflict to settle with the board owner (R8):** with the drain active, overwriting a record the drain has not
  consumed is a loss; with no recording it is the review buffer working. Studio sets the mode per run; the cost is that a run
  with a slow drain can lose a record the ring would otherwise have kept for review.
- **Idle rule (D4):** a run ends with the last client by default (parity). The opt-in (SSD runs only) needs a maximum duration entered at start
  and keeps the run going without clients until that time; the run then ends through the normal Stop path, so LED and strobe shutdown is identical
  to a manual Stop. The run card shows "unattended until HH:MM"; the idle stop is unchanged for every other mode.

## 6. Tests without the board (f)

1. **Fake SSD image**: a generator (Python, from the pz7035 platform model and `store_records`, which already create byte-exact
   store records) writes a sparse image with a superblock, run table and runs: clean, with `lost_before` gaps, reason 7, a torn
   last record, a corrupt CRC mid-run, a full table. The C++ `ISsdDevice` fake reads this file. Fixtures are small (tens of records).
2. **Exporter golden test**: the same fixture converted by `pz_store_to_mib.py` and by the C++ exporter. Compared with h5py: images,
   masks, frame index, timestamp, bbox, `isValid`, group sizes, `/experiment_info` counts. Columns the exporter takes from the PL
   cell words (deformability, area, Young's modulus) are checked against `filterResultFromUnetCell` directly.
3. **Review opens it**: the exported file goes through the bridge's review open in the existing backend test harness (frames > 0,
   metrics table, accounting panel states for `complete`, `intentionallyPartial`, `incompleteLoss` / not reconciled).
4. **Accounting table test**: every row of the section 3.1 table as a table-driven unit test, including `reconcile` true/false.
5. **Space, parts, unattended runs and cleanup**: part sizing and boundaries (a gap on a boundary, a run that fits in one file), no opt-in without a maximum duration, stop at the deadline through the normal path, injected `statvfs` (full, almost full, ample), ENOSPC at the 64 MiB check, cancel mid-export, a stale
   `.part` at start-up, rename atomicity, two exports refused, export refused while RECORDING.
6. **State machine**: a fake register window (a small model of R1 to R9) drives `SsdStore` through every transition, including
   RECOVERING during a run, WEDGED, a drop burst and a mount-time recovery; asserts the gates, the counters and the strip view
   model. The UI view models (`ssdView.ts`, pure functions like `filesView.ts`) are unit-tested; a Playwright pass runs against the
   server with the fake window to click through the strip, the list and the export.
7. **Security**: the export download goes through the existing Files route tests (Origin, confinement, Range, permits); a new
   route (`/ssd/runs/<id>/raw`, S3) gets the same.
8. **Throughput on the dev PC**: a 50,000-record synthetic image measures the CPU-bound part of the exporter (decode, unpack,
   HDF5 write to a tmpfs) so a regression is visible without the board; the eMMC and DMA parts are measured once on the board.

Not testable here, and to be run in a board slot: the DMA read (R7), real drops and recoveries, mount-time recovery after a real
power loss, the eMMC write rate during an export, and the Run page with real counters.

## 7. Phasing

| Phase | Content | Needs |
|---|---|---|
| S0 | This note; agree R1 to R9 with the board owner | now |
| S1 | `SsdStore`, fake device and window, `ssd_status`, `ssd_runs`, strip and the Experiments list (read only), gate `storage.ssd` | the window (R1 to R6) |
| S2 | Record to SSD (`experiment_start storage=ssd`), live counters, accounting, idle option | results13 plus the writer image |
| S3 | Export to the eMMC and download; Review opens it | R7 (read DMA) |
| S4 | Raw-run download, range export, SSD-run delete (if wanted) | decisions |

S1 can be built and tested against the fake before any new PL image exists. Nothing starts before #649 v1 is merged.

## Open questions for the board owner

R1 to R9 above, and: the filter predicate (any RESULT, or any valid RESULT) and whether it can sample invalid-only frames
like the PC; whether `lost_before` fits the FRAME record without an ABI bump; the DMA buffer size and where it sits relative to the ring
carve-out (it reduces the ring); and the measured rate of the read DMA into PS DDR.
