# YOFO Studio Record to the SATA SSD: experiments, export, UI (design note for #667)

Status: active (design, no code)

## Goal

[#667](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/667): on the PZ7035, Record writes the experiment to the
SATA SSD through the PL writer (coordinator decisions 1 to 6 in the issue, as amended by Gavin on 2026-10-09: all recorded data and clips go
to the SSD, the eMMC is only for firmware and internal data), Studio lists the SSD runs as experiments and exports one as the PC
experiment HDF5 on the SSD's ext4 area, which YOFO Review opens unchanged. This note is the Studio side. It builds on the parity spec
([2026-10-08-playback-save-parity.md](2026-10-08-playback-save-parity.md), #653) and on the board owner's
`docs/FRAME_RING_FEASIBILITY.md` (pz7035 #33). Order of work: #649 v1 (ring playback first; Save clip arrives with the SSD path, section 5) first.

Facts used (all from repository files, none newly measured): store record 59,392 B (4 KiB record set + 49,152 B image +
6,144 B packed mask); SSD sustained about 110 MB/s rested and about 37 to 46 MB/s after the SLC cache is spent; the eMMC
`yofo-data` partition is for Studio's settings and state only (#666); uncached `/dev/mem` read 68 MB/s; run table of 29 slots with 16-byte entries
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
| D1 | One control or two? "Record" (Gavin's word) vs the existing Start/Stop Experiment. | **Accepted.** **One control: the existing Experiment start/stop with a storage target `ssd`**, labelled "Record to SSD"; disabled with the reason when the SSD is not ready (there is no eMMC fallback for user data, section 3.4). It keeps the gates, provenance, accounting and idle handling the PC experiment has. PC raw "Record" stays hidden on the PZ. |
| D2 | Where does the export land? | **Changed by Gavin (2026-10-09, #667 comment).** All recorded data goes to the SSD, clips included; the eMMC holds only firmware and internal data (settings, state, logs, OTA later). The SSD has two areas: the raw record area (PL drain) and an ext4 area that Linux mounts through a block device. An export is raw area to HDF5 file on the ext4 area (section 3); there is no eMMC staging and no 13-minute cap. Parts stay an *option* for downloads (section 3.2). Compression follow-up stays (section 3.3). v1 is uncompressed (the PC writer has no deflate). |
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
| R4 | **A larger run table (hard requirement: no silent overwrite)**: at least 256 entries (a table of 64-byte entries in sectors after the superblock), no `run % N` overwrite while there is free space; an explicit "run table full" state; a `deleted` flag per entry (section 3.5). | Experiment list. |
| R5 | **Run entry fields**: run id, start unix ms and client tag (from R2), first and last frame id, first and last PL timestamp ticks and the tick rate (duration is derived), records, frames seen, frames empty, dropped, failed, drain recoveries, completion reason (0 clean, 1 drain fault, 5 not stopped, 6 length limit, 7 recovered at mount, as in `store_rec.h`), flags. | Experiment list and export. |
| R6 | **Drop placement**: each stored record carries `lost_before` (a u32 in its FRAME record: records dropped since the previous stored record). | Studio can reconstruct exactly where the run has gaps and book them (`sequenceGaps`), without a per-run list that can overflow. |
| R7 | **Linux block access to the SSD** (replaces the read DMA of the first version of this note): the raw record area is readable by Linux, read only, as a block range (a partition or an offset window) of the same block device that carries the ext4 area, with large sequential reads (at least 1 MiB per request) at the rate the board owner measures; the partition table and the PL superblock must not collide at LBA 0; refused or throttled while RECORDING so that filesystem I/O can never cause a drain drop (arbitration is the board owner's, drain writes have priority). | Export (section 3), clips and Files on the ext4 area (section 3.4). |
| R8 | **Ring and drain coexistence**: the drain is a pure consumer of the shared store ring; stopping a run does not clear the ring and freezes it as #649 specifies. A ring mode in which overwrite is loss (SSD drain) or not loss (review only) is set per run. | Section 5. |
| R9 | **Mount-time recovery** reports what it closed: count of runs closed, their ids, reason 7, in the window and in the run entry. | Section 4. |

### 1.1 Studio side

New bridge commands (JSON, additive, listed in `bridge-contract.json` when implemented; the ABI number is decided then):
`ssd_status`, `ssd_runs {offset, limit}`, `ssd_run_info {run}`, `ssd_export_start {run, first?, last?}`, `ssd_export_status`,
`ssd_export_cancel`, `ssd_export_delete {run}`, `ssd_run_delete {run, confirm_unexported}`; `experiment_start` gains `storage: "ssd"` (the only target once S2 ships; the eMMC metadata-only target is transitional) and `keep_recording` with `max_duration_s`.
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
| Export | none, exporting (progress), ready (size, path on the SSD, Download, Delete), failed (reason) |

Actions: Export (full, parts or range), Download, Delete export, Delete run, Open in Review (only after the export, using the existing
review open on the board's path, or download and open on the PC). **Delete run** follows the coordinator's lifecycle policy (section 3.5).
A row carries an "exported" badge once an export of the run exists on the ext4 area. Paging and permits reuse the Files view machinery
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

### 3.2 Where the file lands

- **On the SSD's ext4 area**, `<ssd>/yofo-studio/experiments/run-<id>-<start>.h5`, written as `.run-<id>.part` and renamed when
  complete, so a crash, cancel or power loss never leaves a file that looks finished. Stale `.part` files are removed at server start.
  Served by the Files route (#661, Range, 2 concurrent downloads), which already confines paths and checks Origin, with the SSD mount as
  a second root (section 3.4). Nothing is staged on the eMMC.
- **The export reads the raw record area through the block device (R7) and writes the HDF5 to the ext4 area of the same drive.**
  Both go over the one SATA link, so the cost per record is 59.4 KB read plus 98.5 KB written (3.3).
- **Streaming the HDF5 into a download is still not feasible** (the HDF5 library needs a seekable file), and is no longer needed: the file
  is on the SSD and downloads with Range from there.
- **Space check, before the export starts and every 64 MiB written**: required = estimated HDF5 size x 1.05 + 1 GiB reserve on the ext4
  area. Estimate = records x 98.5 KB (image 49,152 + mask 49,152 + a 192-byte row, plus HDF5 overhead). Refused with the numbers when
  it does not fit; mid-export `ENOSPC` aborts and deletes the part file. Existing exports and clips are never auto-deleted. The
  ext4 area has a fixed size set when the drive is prepared (open question below), so a long recording can still outgrow it.
- **Parts are an option, not a necessity** (D2 as accepted before the storage change): "Export in parts of K frames" splits a run by
  frame-id range into N complete experiment files (full run provenance, `accounting_*` for the part's own range, the run totals in
  `config_json.run_totals`, and `export_part`, `export_parts`, `export_first_frame`, `export_last_frame` in `/experiment_info`; a part
  boundary never splits a frame, and a gap that falls on a boundary is booked in the part holding the following record). It is
  offered for downloads to a PC with little disk space, and as the way out when the ext4 area cannot hold the whole run (then
  "part 1 of N" is sized to the free space, the user downloads and deletes it, and exports the next). A run that is exported whole is
  a single file without part attributes.
- **A raw-run download** (`/ssd/runs/<id>/raw`, Range, same gates, phase S4) stays an option: store records straight to the HTTP body, the
  PC converts with `pz_store_to_mib.py`. It avoids the ext4 write and is the fastest way to move a very large run.
- One export runs at a time, and **none while a run is RECORDING**: Studio refuses it and the UI says "Export available after the run is
  stopped". This is Studio's rule on top of the board owner's drain-priority arbitration; it can be relaxed to a rate-limited read if
  the board test shows no drops with concurrent I/O.

### 3.3 How long it takes (estimates, to be replaced by a board measurement)

The limit is no longer the eMMC. It is the block path (the PL block port plus whatever serves it to Linux: a kernel driver or an NBD/ublk
server over UIO, the board owner's choice), shared by the 59.4 KB read and the 98.5 KB write of each record, so time per record is
158 KB / B, with B the sustained block throughput (unmeasured). The three columns assume B = 30, 60 and 100 MB/s. The drive itself is
about 110 MB/s rested, 37 to 46 MB/s after the SLC cache is spent (A400, Gen1), so a multi-GB write can fall into the slow region; a Gen2
link and a DRAM TLC SSD (Gavin's plan) lift that ceiling. Typical scene = 86 valid frames/s.

| Run | Records | Raw on SSD | HDF5 | B = 30 MB/s | B = 60 MB/s | B = 100 MB/s |
|---|---|---|---|---|---|---|
| Typical, 1 min | 5,160 | 0.31 GB | 0.51 GB | 27 s | 14 s | 8 s |
| Typical, 10 min | 51,600 | 3.1 GB | 5.1 GB | 4.5 min | 2.3 min | 1.4 min |
| Typical, 1 h | 309,600 | 18.4 GB | 30.5 GB | 27 min | 14 min | 8 min |
| Dense, 60 s (drain-limited at about 37 MB/s, about 620 records/s) | about 37,000 | 2.2 GB | 3.7 GB | 3.2 min | 1.6 min | 1 min |

No size cap other than the ext4 area. Downloading the file to the PC adds file size / min(SSD read, network): 5.1 GB in about 1 to 2
minutes at 50 to 100 MB/s. The PS ceiling is also unmeasured: the HDF5 writer on the A9 (chunk (1, H, W), no filter) and the block driver
share the CPU; the earlier `/dev/mem` read of 68 MB/s is an upper bound for any PS copy.

Compression (gzip on the chunks) could roughly halve the file but is not in the PC layout (the PC writer has no deflate) and its ARM CPU
cost is unmeasured; v1 stays uncompressed. **Follow-up item:** measure gzip level 1 and LZF on the mask dataset on the A9 (the mask is
mostly zeros, so it should shrink far more than the image). If the block path or the drive is the bottleneck, a cheaper filter may cut the
export time; h5py and Review read filtered datasets transparently, so only the ARM build's HDF5 filter support and the CPU cost need checking.

### 3.4 Storage layout and the data directory

| What | Where | Why |
|---|---|---|
| Studio settings, state, logs, the run window, future OTA | the eMMC data partition, `/var/lib/yofo-studio` (#666, unchanged) | Gavin: the eMMC is for firmware and internal data. Survives without the SSD. |
| Raw run records | the SSD raw record area, written by the PL drain | full-rate path, not a Linux file |
| Experiment HDF5 exports, clips (Save from the ring), anything the user saves | the SSD ext4 area, `<ssd>/yofo-studio/{experiments,clips}/` | all recorded data goes to the SSD |

- **Mount.** A `mount-ssd.sh`, in the style of `mount-data.sh`, mounts the ext4 area by label (`yofo-ssd`) at a fixed mount point
  (for example `/mnt/ssd`) with `noatime,commit=30,errors=remount-ro`, after the block device exists. The unit does not wait for it:
  the server polls the mount (every 2 s) and the state becomes "SSD absent" until it appears and "SSD ready" when it does (a SATA
  link is not hot-plugged, but the block driver and the PL link come up after Studio starts). After an unclean power-off the journal is
  replayed by the mount; if a check is needed the script runs `e2fsck -p` first and reports it. The block driver and the label are the board
  owner's to provide.
- **Studio configuration.** `--data-dir` stays on the eMMC. A new `--storage-dir` (the SSD mount) holds user data. Default output
  paths (`resolveRemoteDefault`) move to the storage directory when it is mounted and there is **no default when it is not**. The Files
  view and the download route (#661) get the storage directory as a second confined root (own `openat2` confinement, same Origin and
  Range rules); the data directory root stays for logs and the run window.
- **Storage status.** `recordingTarget` classifies the target as `ssd` (ext4 on the SSD device), `emmc`, `ram` or `absent`. The gate
  `storage.persistent` (warn on RAM) is replaced for experiments by a **blocking** gate `storage.ssd`: SSD READY, ext4 area mounted,
  writable (the existing probe), free space for the minimum run. Diagnostics shows both: "Settings: eMMC (persistent)" and
  "Data: SSD, 212 GB free" or "Data: no SSD".
- **Without the SSD nothing can be saved, and the UI says so explicitly.** Start Experiment, Save clip, Export and the Files "save" targets
  are disabled with the same reason ("No SSD: nothing can be saved"). A run in progress when the SSD disappears is closed with the best-effort
  trailer and listed as failed or recovered. The eMMC is never used as a fallback for user data. Today's metadata-only experiment to the eMMC
  is the transitional behaviour until S2 ships; at S2 it is removed from the UI (the coordinator is asked to confirm).

### 3.5 SSD layout and raw-area lifecycle (coordinator policy, 2026-10-09)

- **One GPT on the SSD, two partitions**: the raw record partition and the ext4 partition. The PL drain is confined to the raw partition's
  LBA range, read from its superblock. The split is chosen when the SSD is formatted, by a format tool (the board owner's); the default is
  raw 40 % and ext4 60 %, because HDF5 is about 1.67 times the raw size of the same records. Studio shows the free space of both areas
  (strip, Diagnostics, list header).
- **The raw area is a log.** Runs append at the head; space is reclaimed from the tail when the oldest runs are deleted. Deleting a middle
  run only marks it deleted (the `deleted` flag in its run entry); its space returns once everything older has been freed. Studio therefore
  shows two numbers for a deleted middle run: it is gone from the list, but the raw free space does not grow until the older runs are
  freed (the list header says "N GB waiting for older runs").
- **Nothing is ever auto-deleted.** Only the user deletes, from the experiment list. Deleting a run that has no export asks for an explicit
  confirmation that names the run and says it cannot be recovered; a run with an export on the ext4 area shows an "exported" badge, and
  its delete asks the ordinary confirmation. Each export has an optional "delete the raw run after a verified export" (default off). The
  verification is: the file closed and renamed, then re-opened read-only, with the frame count, the first and last frame id and the
  `accounting_*` terms equal to the run's; only then the run is marked deleted.
- **Raw area full** (or the run table full): Start refuses with "SSD raw area full: export/delete old runs". **ext4 full**: the export (and
  Save clip) refuses up front from the size estimate (section 3.2).
- **Studio checks, not the PL, enforce the policy**: the delete command and the confirm flag are Studio's (`ssd_run_delete`), the head and
  tail bookkeeping and the superblock update are the board owner's.

## 4. UI states (d)

The Run page gets a storage strip (also shown in Diagnostics) and the Experiments view gets row badges. States:

| State | Where | Shown | Behaviour |
|---|---|---|---|
| **SSD absent** (R1 ABSENT, or the ext4 area not mounted) | strip, banner, start gate | "No SSD: nothing can be saved" | Experiments, exports and Save clip are all disabled with that reason (section 3.4). Run, previews, live results and ring playback keep working. No spinner, no fallback to the eMMC. |
| **Initialising / recovering** (INITIALISING, RECOVERING) | strip | "SSD starting" / "SSD recovering (n runs closed)" with elapsed time | Start disabled; auto-clears. After a mount-time recovery (R9) a notice names the runs it closed and links to them in the list. |
| **Ready** | strip | model, free space, "about N h at the typical rate" (explicitly "dense scenes use more") | Start enabled. |
| **Recording** | strip, run card | stored records, empty frames skipped (neutral, not an error), dropped (amber when above zero, red above 1 % of passed frames), recoveries so far (informational: 1 to 2 per GB is normal), write rate, ring fill, elapsed | A drop count is cumulative and never resets inside a run; the card shows the rate over the last 10 s so a burst is visible. |
| **Stopping** | strip | "Writing the last N records, about s s" | Controls locked; from the ring fill and the drain rate. |
| **Wedged / error** | strip, banner | "SSD not responding. The drive is self-powered; power-cycle it." | The run (if any) is closed on a best-effort basis; the list shows it as failed or recovered; start disabled. |
| **Raw area full / run table full** | strip, start gate | "SSD raw area full: export/delete old runs" | Start refuses with exactly that reason; the list shows the free space of the raw area and offers Delete run. |
| **ext4 area full** | strip, export and clip gates | "SSD files area full" (free space shown) | Export and Save clip refuse up front using the size estimate; the user deletes exports or clips. |
| **Run closed by mount-time recovery** | list badge, detail | "Recovered after power loss: N records found, true totals unknown" | Selectable and exportable; the export marks `incompleteLoss`, `reconciled=false`. |
| **Drops, finished run** | list badge | "Partial: N dropped (x %)" | The same counts as in the exported accounting, so Review's accounting panel agrees. |

All text follows the existing honest-state rules: no state is hidden, no count is estimated when the hardware reports it.

## 5. Record, buffer playback and Save clip (e)

Premise (decision 3, R8): one store ring in PS DDR; the SSD drain is a consumer of it; Stop freezes the ring.

- **Yes: record and still Stop and review the ring.** Stop first closes the run (STOPPING: the drain finishes the records that passed
  the filter and the trailer is written), and then the ring is frozen. The review buffer is the last 0.9 to 2.1 s of **every** frame
  (feasibility note: 256 to 600 MiB), whether or not the SSD run kept them. A cell frame that the SSD dropped under overload is
  still in the ring if it is recent, and the playback marks it ("not on the SSD").
- **Save clip** (from the ring, after Stop) writes an HDF5 to the SSD's ext4 area, `<ssd>/yofo-studio/clips/`, with the same
  `Hdf5Service` writer and the same layout as an export (content as decided in #649). It arrives with the SSD path: in #649 v1 the
  control is **disabled** with "Saving needs the SSD (not available yet)", never silently missing and never written to the eMMC. With the SSD
  absent it stays disabled with "No SSD: nothing can be saved". A clip and an export write to the same drive, so Studio runs them one at a
  time and says which one is waiting.
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
5. **Space, parts, unattended runs, storage layout and cleanup**: a missing mount point and a mount that appears later (the fake is a temporary directory plus a parsed `mountinfo` sample), SSD absent disabling every save path, default paths with and without the mount, the second Files root,  part sizing and boundaries (a gap on a boundary, a run that fits in one file), no opt-in without a maximum duration, stop at the deadline through the normal path, injected `statvfs` (full, almost full, ample), ENOSPC at the 64 MiB check, cancel mid-export, a stale
   `.part` at start-up, rename atomicity, two exports refused, export refused while RECORDING.
6. **State machine**: a fake register window (a small model of R1 to R9) drives `SsdStore` through every transition, including
   RECOVERING during a run, WEDGED, a drop burst and a mount-time recovery; asserts the gates, the counters and the strip view
   model. The UI view models (`ssdView.ts`, pure functions like `filesView.ts`) are unit-tested; a Playwright pass runs against the
   server with the fake window to click through the strip, the list and the export.
7. **Security**: the export download goes through the existing Files route tests (Origin, confinement, Range, permits); a new
   route (`/ssd/runs/<id>/raw`, S3) gets the same.
8. **Throughput on the dev PC**: a 50,000-record synthetic image measures the CPU-bound part of the exporter (decode, unpack,
   HDF5 write to a tmpfs) so a regression is visible without the board; the block path and the ext4 write are measured once on the board.

Not testable here, and to be run in a board slot: the block read of the raw area (R7) and the ext4 area (mount, journal replay after power loss, drain priority with concurrent I/O), real drops and recoveries, mount-time recovery after a real
power loss, the export rate end to end, and the Run page with real counters.

## 7. Phasing

| Phase | Content | Needs |
|---|---|---|
| S0 | This note; agree R1 to R9 with the board owner | now |
| S1 | `SsdStore`, fake device and window, `ssd_status`, `ssd_runs`, strip and the Experiments list (read only), gate `storage.ssd` | the window (R1 to R6) |
| S2 | Record to SSD (`experiment_start storage=ssd`), live counters, accounting, idle option | results13 plus the writer image |
| S3 | Storage layout (mount, `--storage-dir`, second Files root, `storage.ssd` gate), export to the ext4 area, download; Review opens it | R7 (block access) and the ext4 area |
| S3b | Save clip from the ring to the ext4 area (#649, enabled once S3 lands) | S3 |
| S3c | Delete run (log reclaim), "exported" badge, optional "delete raw after verified export" | S3 and the board owner's tail reclaim |
| S4 | Raw-run download, parts export option | decisions |

S1 can be built and tested against the fake before any new PL image exists. Nothing starts before #649 v1 is merged.

## Alignment with the board owner's design (pz7035 `docs/SSD_RECORDING_DESIGN.md`, 15603ac9)

The board owner's note settles the interface; where it differs from the requirements above, it wins:

| Topic | Board owner's design | Effect on Studio |
|---|---|---|
| Control path | The PL window at 0x40102000 holds the drain, link and counters; sequencing (run table, start/stop, trailer, recovery) is `libpzrec` (C ABI) and the `pzrec` CLI (JSON), in user space. | Studio calls `libpzrec` (or the CLI) through one `SsdStore` class behind `ISsdDevice`; no register sequencing in Studio. S1 runs against `libpzrec` on a file-backed fake block device. |
| State word (R1) | ABSENT, INITIALISING, RECOVERING, READY, RECORDING, STOPPING, WEDGED, RUN_TABLE_FULL, RAW_FULL, plus `last_error`. | The UI states of section 4 map one to one; RUN_TABLE_FULL and RAW_FULL are separate refusals. |
| Start arguments (R2) | Filter mode (ALL, ANY_RESULT, VALID_ONLY), sampler N, `start_unix_ms`, `client_tag`; graceful stop and abort are separate. | `start_unix_ms` comes from `WallClock` (G14). Studio asks for a one-byte time-source flag in the run entry (`client_sync` or `board_clock_unsynced`) so the export can write `timestamp_wall_source`. |
| Counters (R3) | Coherent snapshot: seen, empty_filtered, invalid_not_sampled, passed, written, dropped, failed, recoveries, bytes, ring fill, drain rate; identities `seen = empty + invalid_not_sampled + passed` and `passed = written + dropped + failed`. | Accounting mapping of 3.1 becomes: `admitted = seen`, `empty = empty_filtered`, `scientificallyRejected = invalid_not_sampled`, `persistenceCommitted = written`, `cancelledByPolicy = dropped` (declared), `persistenceFailed = failed`; the identities are asserted on read and a violation is shown, not hidden. |
| Run table (R4, R5) | 256 entries of 128 bytes (formatted up to 1024), append-only, never wraps; `DELETED` flag; start unix ms, client tag, first/last frame id and ticks, the seven totals, recoveries, reason (0 clean, 1 drain fault, 2 stall, 3 length, 5 not stopped, 6 length limit, 7 recovered at mount, 8 aborted, 9 raw full). Superblock v2 at the first sector of the raw partition (A/B). | The list columns of section 2 come from the entry; reason 9 maps to `complete` (ended at the space limit), 8 to `incompleteLoss`, 7 to `incompleteLoss` and not reconciled. |
| Drop placement (R6) | A saturating `drops_before` byte in FRAME reserved byte 0, patched by the drain with the FRAME CRC recomputed; no FRAME version bump. | The exporter reads it to book `sequenceGaps` and to place a gap on a part boundary; `pz_store_to_mib.py` and `mib_abi.py` accept a non-zero reserved byte. The store sequence is per seen frame, so a sequence gap alone does not mean loss. |
| Block access (R7) | A kernel module `pzblk` (blk-mq) with a scatter-gather DMA in the PL: the disk is `/dev/yofoblk0`, the raw area `/dev/yofoblk0p1` (read only for Studio; writes only through `libpzrec`), the ext4 area `p2`. No read DMA window and no 16 MiB buffer. Expected 110 to 140 MB/s read and 100 to 137 MB/s write at Gen1 (estimate). | The export reads `/dev/yofoblk0p1` with sequential reads of 1 MiB or more and writes the HDF5 to the ext4 mount; the timings of 3.3 are planned with B = 100 MB/s. |
| Arbitration (R8) | The drain wins: a PS command is limited to 1 MiB and admitted only while the drain backlog is under a threshold (25 % of the ring by default). Block I/O waits, the drain does not. The 0-drop test with concurrent ext4 I/O is section 7.4 of their note. | Studio still refuses export and clip saves while RECORDING in v1, and relaxes it only after that test passes. The run list while recording uses the window snapshot, not the disk. |
| Mount recovery (R9) | `libpzrec` returns the runs it closed (count, ids, reason 7). | Shown as the notice of section 4. |
| Layout and lifecycle | One GPT (raw partition 1, ext4 partition 2), default 40/60 chosen by `pzssd-format`; the raw area is a log with head and tail; Start refused with RAW_FULL below a 1 GiB minimum. | As section 3.5. |
| Gen2, SSD | Gen2 is attempted separately; Gen1 is the shipped fallback; the SSD to buy is a DRAM TLC drive (Crucial MX500 500 GB). | No Studio change; the export time scales with the measured B. |

S1 therefore needs only `libpzrec` and the `pzrec` CLI on a file-backed fake block device, which the board owner builds next.

## Open questions for the board owner

R1 to R9 above, and: the filter predicate (any RESULT, or any valid RESULT) and whether it can sample invalid-only frames
like the PC; whether `lost_before` fits the FRAME record without an ABI bump; the DMA buffer size and where it sits relative to the ring
carve-out (it reduces the ring); and the measured rate of the read DMA into PS DDR. After Gavin's storage change (R7): the block device design
(kernel driver or NBD/ublk over UIO) and its sustained throughput B; how the partition table and the PL superblock share LBA 0; the arbitration test
(drain drops with concurrent ext4 I/O). The layout and raw-area lifecycle are decided by the coordinator (section 3.5); the board owner implements the mechanics.
