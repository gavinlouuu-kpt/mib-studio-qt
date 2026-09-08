# Ultra96 FPGA image pipeline

## Outcome

The Ultra96 now runs the same fixed MIB Studio comparison pipeline with a
direct-DDR transport. Across all 171 `gavinlouuu/512x96stream` frames, the
board produced byte-identical masks and identical ordered contours, records,
valid-object decisions, tracking results, and target decisions.

The timing-clean image processes 512x96 frames at 1,251 fps serially and
1,344 fps with two-buffer FPGA/ARM overlap. The earlier AXI-BRAM image reached
260.5 fps, so the direct-DDR architecture is 4.80x faster serially and 5.16x
faster in throughput mode.

## Architecture

```text
Mono8 frame -> CMA/XRT input BO -> S_AXI_HP0_FPD -> 4-PPC HLS mask core
                                                       |
MIB records <- ARM tracking/metrics/findContours <- CMA/XRT output BO
```

The PL core remains the already verified 4-PPC/250-MHz implementation:
Gaussian filtering of current and background frames, saturated background
subtraction, threshold 8, and four cross-morphology stages. Only transport and
scheduling changed.

- XRT allocates physically contiguous buffers from the boot image's 512 MiB
  CMA reservation and exposes their device addresses.
- The HLS input, background, and output AXI masters share the PS 128-bit HP0
  low-DDR aperture. The PS HPM0 master keeps the small accelerator control
  register interface at `0xA0030000`.
- Two input/output BO slots allow the FPGA control path on A53 CPU 2 to overlap
  the previous frame's ARM science stages on CPU 3.
- The boot image, microSD contents, and network/exit-node configuration were
  not changed.

## Exactness evidence

| Check | Result |
|---|---:|
| Frames | 171 |
| Mask bytes/pixels compared | 8,404,992 |
| Differing mask pixels | 0 |
| Ordered contours | 920, exact |
| Batch records | 176, equivalent |
| Candidate / valid / target records | 48 / 21 / 7 |
| Total white pixels | 85,878 |
| 5,000-frame serial steady mask checks | all exact |

The canonical result and verification JSON are under
`/mnt/fpga-workspace/results/ultra96-direct-ddr`. The strict verifier is
[`verify_direct_ddr.py`](../../tools/fpga_exploration/benchmark/verify_direct_ddr.py).

## Performance

All values below are steady-state means over 5,000 frames. Exact-mask scan
time is excluded from processing latency and reported separately by the
runner.

| Stage | Direct DDR, us/frame |
|---|---:|
| CPU copy into input BO | 64.33 |
| Input cache sync | 8.59 |
| Buffer-select MMIO | 0.44 |
| FPGA kernel including DDR access | 80.01 |
| Output cache sync | 5.29 |
| `findContours` extraction | 377.83 |
| Hierarchy/inner pairing | 8.05 |
| Metrics and filtering | 247.18 |
| Tracking | 5.85 |
| ARM post-processing total | 640.46 |
| Serial end-to-end | 799.12 (1,251 fps) |
| Two-buffer throughput interval | 744.21 (1,344 fps) |

The old BRAM input write plus output read cost 3,193.76 us/frame. Direct DDR's
CPU copy, cache synchronization, and buffer-select overhead totals 78.65 us,
a 40.6x transport improvement. DDR access raises the kernel itself from about
54.3 us to 80.0 us, but removing the frame-sized BRAM MMIO dominates the net
gain.

The workstation MIB `processBatch` reference was 164.57 us/frame. The Ultra96
throughput interval is still 4.52x slower because exact contour extraction and
metrics remain on the Cortex-A53. The direct-DDR result sustains about
66.0 MPixel/s, or 0.53 Gbit/s of Mono8 payload; it does not approach a 10-GbE
camera's theoretical line rate.

## Timing and artifacts

- Vivado 2026.1, device `xczu3eg-sbva484-1-e`
- 4 PPC at 250 MHz
- Worst setup slack: +1.059 ns
- Worst hold slack: +0.010 ns
- Bitstream SHA-256:
  `77b07c1b7f78a1ca81d4cf12ebdc8a4d70b246d7f1de0d46c48a2755dcd835d5`
- Build root:
  `/mnt/fpga-workspace/builds/mib-fpga-direct-ddr/ultra96-ppc4-250`

## Next bottleneck

Transport is no longer the dominant cost. The next high-value hardware work is
an exact streaming contour/connected-component and feature extractor, or a
reformulation of the science contract that avoids `cv::findContours` while
preserving ordered outlines and record semantics. That is a larger design
change than transport because contour hierarchy, geometry, brightness
quantiles, target gates, and tracking must remain equivalent.

For real 10-GbE acquisition, the Ultra96 also needs external ingress hardware
(for example an FMC/mezzanine NIC or a separate capture/packet-processing
device). The on-board interfaces cannot directly accept a 10-GbE camera.

## Reproduction

Build, board-compile, and strict verification commands are in
[`tools/fpga_exploration/README.md`](../../tools/fpga_exploration/README.md).
