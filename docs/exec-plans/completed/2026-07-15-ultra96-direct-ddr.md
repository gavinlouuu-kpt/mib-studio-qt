# Ultra96 Direct-DDR Image Pipeline

Status: completed

## Goal

Remove full-frame `/dev/mem` transfers through AXI BRAM from the verified
Ultra96 4-PPC/250-MHz pipeline. The accelerator will read and write
CMA-backed DDR buffers through a Zynq UltraScale+ high-performance AXI port,
while the ARM keeps MIB Studio-equivalent contour, metric, validation, and
tracking stages. The resulting run must remain exact across the same 171
frames and expose enough stage timing to choose the next acceleration target.

## Acceptance criteria

- [x] XRT can allocate, map, and synchronize CMA-backed buffers on the current
      Ultra96 image without replacing the known-good boot image.
- [x] A timing-clean 4-PPC/250-MHz Vivado design connects all three HLS AXI
      masters directly to PS DDR and keeps only the control interface on MMIO.
- [x] The board runner uses physical addresses from XRT buffer objects and
      reports input sync, FPGA kernel, output sync, contour extraction,
      metrics/filtering, and tracking separately.
- [x] All 171 frames match the MIB reference masks, contours, records, and
      target decisions exactly.
- [x] Steady-state results are captured on the HDD and compared with the
      existing AXI-BRAM baseline.
- [x] The board is left running the fastest exact, timing-clean image; the
      prior 4-PPC/250-MHz image remains available for rollback.

## Decision log

- 2026-07-15: Prioritize direct DDR over a wider PPC core because BRAM MMIO
  accounts for roughly 3.19 ms of the 3.84-ms baseline, while the verified
  FPGA kernel itself is about 54 us.
- 2026-07-15: Reuse the exact verified 4-PPC/250-MHz HLS IP from the HDD. This
  isolates the transport change and preserves the fair MIB Studio comparison.
- 2026-07-15: Use the existing XRT 2.6/ZOCL stack and its CMA-backed BOs instead
  of changing the PetaLinux boot image or adding an out-of-tree kernel module.
- 2026-07-15: Store generated builds and result payloads on
  `/mnt/fpga-workspace`; keep reproducible source and documentation in git.
- 2026-07-15: Pin the PL-control loop to A53 CPU 2 and the persistent ARM
  post-processing worker to CPU 3. The honest two-buffer throughput interval
  is 744.21 us (1,344 fps); validation is not subtracted from overlapped wall
  time.
- 2026-07-15: Keep the direct-DDR image loaded. It is exact, timing-clean
  (+1.059 ns setup, +0.010 ns hold), and 5.16x faster than the BRAM baseline
  in throughput mode.

## Progress

- [x] Confirm local route, Tailscale exit node, board access, CMA reservation,
      XRT/ZOCL device, libraries, and headers.
- [x] Prove XRT BO allocation/mapping/synchronization on the current board.
- [x] Implement and validate the direct-DDR block design.
- [x] Implement the XRT runner and detailed ARM-stage timing.
- [x] Build, deploy, and run exact 171-frame verification.
- [x] Record steady-state measurements and close the plan.
