# PZ7035 result records

> Decoding the records the PZ7035 FPGA (PL) writes to the PS DDR result
> ring: FRAME per frame, RESULT per cell, plus EVENT / COUNTERS / PREVIEW /
> DESCRIPTOR / IMAGE (#447 E3, plan W3.B1).

**Source:** `include/backend/pz/PzRecords.h`, `src/backend/pz/PzRecords.cpp`
(in `mib_processing`, Qt-free)
**ABI bundle:** `third_party/pz7035-abi/` (pz7035-imx426 `abi/` at tag
`abi-v1.2.0`, commit c726d338, pinned by `PROVENANCE.json`, which records the
tag and commit; `scripts/vendor_pz7035_abi.py --from <checkout> --tag <tag>`
refreshes it and refuses a tag that does not resolve to the checkout,
`--check` verifies it; `.gitattributes` marks it `-text` so a Windows checkout
keeps the exact bytes the sha256 check expects)
**Tests:** `processing.pz_records`, `processing.pz_unet_cells_host`,
`scripts.pz7035_abi_vendor`
**Related:** [[HDF5-Storage]], [[../services/ProcessingService]]

## Wire format

- Every record has a 16-byte header: magic, type, version, length, sequence
  and CRC-32. Records are little-endian and 8-byte aligned.
- `decodeRecord` applies the bundle's checks in the reference order: TRUNCATED,
  BAD_MAGIC, OVERSIZED, BAD_LENGTH, BAD_CRC, UNKNOWN_TYPE, BAD_VERSION, type
  minimum, then type rules (RESULT_LIMIT, payload bounds, BAD_STATE).
  Trailing bytes inside `length` are additive and ignored.
- Layouts come from the vendored `pz_mib_abi.h`; its `_Static_assert`s pin
  every offset.
- `StreamDecoder` checks one ring's sequence (a duplicate is rejected; a gap
  is counted and the record still delivered), its epoch (STALE_EPOCH; a FRAME
  with FIRST_OF_EPOCH advances it) and the descriptor generation.
- A RESULT may carry zero payload words. Encoder and decoder both skip the
  payload copy then (copying into an empty vector's null `data()` is undefined
  behaviour; caught by UBSan). `processing.pz_records` round-trips one.

## Ring and frames

- `ResultRingReader` drains `[tail, head)` from a `RingMemory`, handling the
  wrap, then releases `tail = head`.
- HEAD and TAIL are free-running byte counters. When `head - tail` exceeds the
  ring, the consumer fell behind: the reader reports an overrun and
  resynchronises at head.
- At 5 kHz the 1 MiB ring fills in about 1 s, so the consumer must drain it
  continuously.
- `FrameAssembler` groups each FRAME with the RESULTs that follow it. A frame
  is complete at `result_count`; an early next FRAME marks it `incomplete`.

## Profile `unet_cells_v2`

`decodeUnetCellsV2` turns a RESULT of science profile 2, version 2 into a
`UnetCell`: plain host-unit values with the profile reason, flags, shape,
brightness, focus metric, counts and centroid. Coordinates are in ROI 1.

- The module does not depend on the processing contracts, so the PZ7035
  application line can take it as is.
- Any other profile, or a short payload, gives `nullopt`: the payload is
  unavailable, never zero.
- `processing.pz_unet_cells_host` checks that the PL vectors' payloads,
  passed through encode, decode and the profile decoder, equal the host
  Contract 3 science on the same frames.

## Platform monitor (#501)

`include/backend/pz/PzPlatformMonitor.h` reads the PZ7035 read-only, from
pz7035-imx426 `docs/YOFO_HOST_INTERFACE.md` (PR #10, 5b30761):

- **Identity:** `BUILD_ID`/`PROFILE_ID`, ID3 first. The build is checked
  against `/etc/yofo/expected-core.json` (missing = "unknown"). The weights
  are checked against the pinned `unet-c4-multiline-v1` `.npz` sha256, whose
  first 32 hex CMake compiles in from `env/assets.json`.
- **LED strobe window:** `S[0..2]` (presets run 7/60 µs, align 0/125 µs), and
  `S[12]` bit 4 / `S[13]` for the guard.
- **Live-page counters:** `P[12]` ingress errors, `P[14]` resyncs, `P[7]` bad
  frames, `P[6]` dropped. Rates come from deltas; a decrease is a reset.
- **Latency:** `S[47..51]`, 175 MHz clocks.

**Bus guard.** Every sample, and the provider's identity, configure and
start, first check DEVCFG `INT_STS` (`0xF800700C`) bit 2, PCFG_DONE
(`pzPlConfigured`). A read of the PL window while the PL is blank (power-up,
JTAG reload) stalls the AXI bus. The register is only read; it is
write-1-to-clear. The provider creates its mapping once, under a mutex,
because identity polls and configure/start run on different threads.

It never writes: sensor timing, the command word and the LED belong to the
single mode owner (P0b). `AppBackend` creates it beside the `pz-devmem`
provider. Test `processing.pz_platform_monitor` uses fake registers.

## Camera modes and the register writer (#501 P1)

`include/backend/pz/PzInstrumentControl.h` is the one writer of the live
registers the UI drives. Every access first checks PCFG_DONE and the cell
image (`S[41]` = `'CEL2'`), and one mutex serialises them all:

- **LED** (`S[0..5]`): written off first (`S[0]=0`), delay and width in
  `S[10]`-kHz cycles, `S[4]=S[5]=0`, then `S[0]=1` (XVS sync).
- **Cell path:** `S[46]` together with `P[8]` bit 4. That bit is a level;
  the other `P[8]` bits are kept.
- **Latency clear:** a write to `S[47]`.
- **One-frame cell capture:** arm with `S[36]` and wait for `S[42]` bit 0.
  `S[43]` is the frame id, `S[44]` = listed / flags / dropped, `S[45]` =
  cells / blemishes. The window at `0x400E0000` holds gray in words
  0-12287 and the mask in 12288-13823, then 18 words per cell at
  `13824+32i` (`{y,x}`, `{h,w}`, `{count,rank,valid}` in words 15-17).

`AppBackend::setInstrumentMode` runs the agreed sequence. The GenTL producer
applies ROI, timing, `S[9]` and the receiver reset at AcquisitionStart, and
every capture start reopens it and rewrites rate and exposure, so its mode is
always re-applied.

| Mode | Producer request | Result | After AcquisitionStart |
|---|---|---|---|
| Align | full field, 500 fps, 1899.7 µs | HMAX 116 / VMAX 1280 / SHS 64 | LED 0/125 µs; the camera keeps streaming |
| Run | 512×96 at (x%8, y%4), 5000 fps, 150.0 µs | 58 / 256 / 64 | producer stopped; cell path on; latency cleared; LED 7/60 µs |

Both modes start the same way: LED off, then cell path off.

**Producer rule (PL owner, 2026-10-05).** The producer's command pulses
write `P[8]` = mask, then 0. Every AcquisitionStart therefore clears the
U-Net enable, so the producer never runs while the cell path is on. In Run:

- `start_capture` and the overview switch are refused;
- the Aravis camera factory vetoes any other start;
- readiness replaces the live-camera gates (session, delivery mode,
  geometry, overview, transport loss) with `instrument.mode`, which passes
  only in Run.

With the cell path on, the grabber window holds the cell capture, so Run
previews come from `captureCell`, not from the producer.

Test: `backend.instrument_modes`. It covers the write order, no access while
the PL is blank, the cell path never on while the camera streams, the
snapped window, the gates, and the Service-mode LED.

## Execution providers (YOFO S1)

`include/backend/processing/IExecutionProvider.h` is the seam: one
`ProviderFrame` per sensor frame on the provider's thread. It carries the
frame identity, flags, the `UnetCell`s, and their `FilterResult` view
(`filterResultFromUnetCell`). `include/backend/processing/pz/PzExecutionProviders.h`:

- `PzRecordPipeline` — stream checks, then assembly into ProviderFrames,
  with status counters for decode errors, gaps, overruns, incomplete frames,
  orphans and unknown profiles.
- `ReplayExecutionProvider` — replays a record stream at a frame rate. The
  stream can be `pzres capture` files from the board, or encoded vectors.
- `PzDevMemExecutionProvider` (Linux) — the PS path as `pzres` does it.
  - Bridge registers at `0x40101000`; ring at `0x3F000000`, 1 MiB, mapped
    uncached with `O_SYNC` (Linux booted with `mem=1008M`).
  - start: ring base, `tail = head` (HEAD is not reset at ARM), run id, ARM.
  - The reader thread drains every 500 µs, writes TAIL back raw, and drains
    once more after STOP.
  - **Only one reader at a time:** the TAIL register is shared, so stop
    `pzres` first.
  - `identity()` reads the core: `BUILD_ID0-3` hold the first 128 bits of the
    PL build's git commit, and `PROFILE_ID0-3` the first 128 bits of the
    weights `.npz` sha256 (0 when none). Both are formatted ID3 first, as
    `pzres id` prints them (`formatCoreId`). They go into the run snapshot's
    `pl_core`, and `pz_provider_probe` prints them.

Test `processing.pz_execution_provider` covers vector replay, a corrupted
RESULT (counted; its frame incomplete; the gap reported) and a paced
replay's rate. With `MIB_PZ_RING_CAPTURE=<ring.bin>` it replays a board
capture. On 2026-10-04 these captures decoded with 0 errors, 0 sequence or
frame-id gaps and 0 incomplete frames:

| Capture | Frames | Cells |
|---|---|---|
| `results-hw-20261003/retrain-lit-run3` | 100,090 | 100,086 (all cut off) |
| `results-hw-20261003/dark-120s` | 600,157 | 0 |
| `results-hw-20261004/results4-dark` | 100,091 | 0 |

## Ingest and lifecycle (YOFO S1)

- **Selection** — `MIB_EXECUTION_PROVIDER` (`ExecutionProviderFactory`):
  `pz` for `/dev/mem`, `replay:<file>[@fps]`, or `none`. Unset means `pz` in
  a `MIB_PL_SCIENCE` build (#501) and none elsewhere. With `MIB_PL_SCIENCE`,
  `AppBackend` creates the provider, sets its sink to
  `ProcessingService::ingestProviderFrame`, and stops it at shutdown before
  the service.
- **Ingest** — `ProcessingService::ingestProviderFrame` feeds what the inline
  loop feeds after metrics:
  - run accounting: an ingress-error frame (FRAME.INVALID/PARTIAL) is booked
    `StoreMalformed`, not a processing failure;
  - the identification funnel, with the PL's own reasons; the histogram has 8
    codes, matching develop (Laplacian, Channel);
  - monitoring rows without images, carrying the configured p2m.
  - It never calls the target-group callback: the PL owns the trigger.
- **Readiness** — `science.pl` passes with the provider named, and warns
  without one.
- **Start** — the coordinator arms the provider after the run's accounting
  started. If the provider cannot start, the Start rolls back (`NotReady`,
  file removed).
- **Stop** — the provider stops before the final drain, so the frames the
  device wrote before STOP are ingested.
- **Tests:**
  - `processing.pz_provider_ingest`: accounting, funnel, monitoring, no
    trigger;
  - `backend.pl_science_provider`: 300 replayed frames through a real
    start/stop, accounting reconciles.

Recording (S3): `ingestProviderFrame` appends one experiment row per cell:
valid cells always, invalid ones sampled. The rows carry the PL values
(Laplacian, brightness mean/variance, contour area, pixel and blemish
counts) and no images. See [[HDF5-Storage]].

## Profile compiler (S2)

`include/backend/processing/pz/PzProfileCompiler.h` builds the 32-word
`unet_cells_v2` page and the 80,000 B E-modulus table (`PROFILE_TABLE0`).
`compileUnetCellsV2` takes the `ProcessingConfig`, p2m, the store policy and
the `EModulusLut`.

- The config gained develop's `laplacian_variance_*`, `channel_band_*`,
  `min_cell_area_px` and `laplacian_kernel_size`.
- `EModulusLut` gained develop's `loadGrid` plus grid getters.
- **Page format:** the field table equals the vendored profile, which a test
  checks. A value outside a field's range is a compile error, except
  deformability 1.0, which is held as 65535.
- **Table format:** Q8.8 kPa, `0xFFFF` = no value.
- **Board equality:** the host defaults compile to exactly the page the board
  ran (`page.bin`, 2026-10-03). The board's grid compiles to its `lut.bin`
  byte for byte (`MIB_PZ_BOARD_LUT=`).
- **Device commit:** providers take the profile with `configure()`. The
  `/dev/mem` provider does what `pzres config` does: geometry 512x96 MONO8,
  preview off, the page, then `CONFIG_COMMIT`. It waits for ACKED, then loads
  the table through `TABLE_*`.
- **Readiness:** gate `processing.profileCompile` fails with the compile
  errors.
- **Start:** compile, `configure`, then arm. Any failure rolls the Start back.
- **Tests:** `processing.pz_profile_compiler`; `backend.pl_science_provider`
  (gate pass and fail, profile committed before arming).
- **Board check:** `tools/pz_provider_probe --configure` commits these
  defaults through the provider before reading.

## Not yet

- The store drain (images from the PL frame store).
Channel band (T3.7): with `auto_roi_from_background`, the band is detected
from the background (`ChannelRoiDetect`, identical to develop). It goes into
the effective config (`getEffectiveProcessingConfig`), and from there into
page word 1 of the compiled profile.

- **Where the background comes from:** with the science on the PL, background
  calibration (`startPreviewBackgroundCalibration`, the bridge's calibration
  command in PL mode) takes the per-pixel median of N preview frames from the
  FrameStore. Cells passing through are removed by the median.
- **Status, cancel and publication** are the same as the host calibration's.
- **Test:** `processing.pl_preview_background` (median removes a moving cell,
  band, page word, cancel, timeout).

Settings: `config.json` `image_processing` carries develop's
`laplacian_variance_min/max` and `filters.enable_laplacian_variance_check`,
plus `min_cell_area_px` and `laplacian_kernel_size` (top level, new). They
apply through the same config transaction as the other settings. The
transaction rejects ksize other than 1/3, a size gate over u16, and an
inverted Laplacian range (`backend.processing_config_transaction`). The React
JSON editor edits them like any `image_processing` key.
- `scripts/export_hdf5.py` on this line still exports the quartiles, which are
  NaN for PL rows. develop's contract-aware exporter comes with the merge.
**Board check (2026-10-04, `tools/pz_provider_probe`).** The probe was
cross-built with `build.sh` and the Yocto SDK, and run with `results4`, Linux,
5 kHz and the strobe at 7/60 µs. Both runs pass:

| Run | Frames | Cells | Invalid frames | Errors |
|---|---|---|---|---|
| 20 s | 100,129 (5,001/s) | 100,118 (all cut off) | 11 (LED ingress errors) | 0 |
| 120 s | 600,251 | 600,192 | 59 | 0 |

"Errors" covers decode errors, sequence and frame-id gaps, overruns and
incomplete frames. `pzres monitor` straight after (120 s) agrees: 600,159
FRAME and 600,124 RESULT, all zero, latency max 104.8 µs. Log:
`/mnt/hdd/developer-data/IMX426/results-hw-20261004/pz_provider_probe_120s.log`.
Stop `pzres` before running the probe: one reader at a time.
