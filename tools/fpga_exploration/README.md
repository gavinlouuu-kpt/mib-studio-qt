# FPGA image-pipeline exploration

Reproducible source for the Ultra96 image-processing experiments lives here;
generated Vivado projects and measured payloads live on the workstation HDD
under `/mnt/fpga-workspace`.

The current best design is the 4-pixel-per-clock, 250-MHz direct-DDR path.
It preserves the MIB Studio mask, contour, metric, validation, and tracking
outputs while replacing full-frame AXI-BRAM `/dev/mem` traffic with XRT
CMA-backed buffers over the Zynq UltraScale+ HP0 DDR port.

See [the integration report](../../docs/integration/ultra96-fpga-image-pipeline.md)
for architecture, exactness evidence, performance, and remaining limits.

## Source

- `ultra96/build_direct_ddr_design.tcl` — timing-gated Vivado block design.
- `ultra96/ultra96_direct_ddr_runner.cpp` — XRT buffer management, PL control,
  MIB-equivalent ARM post-processing, detailed profiling, and two-buffer
  overlap.
- `ultra96/xrt_bo_probe.cpp` — non-destructive test of the board's XRT/CMA
  allocation, mapping, physical-address, and cache-sync APIs.
- `benchmark/verify_direct_ddr.py` — strict 171-frame mask, outline, record,
  summary, and configuration comparison.

## Build

The design reuses the already verified PPC4 HLS IP on the HDD:

```bash
MIB_ULTRA96_BUILD_ROOT=/mnt/fpga-workspace/builds/mib-fpga-direct-ddr/ultra96-ppc4-250 \
MIB_VIVADO_JOBS=12 \
/mnt/fpga-workspace/tools/AMD/2026.1/Vivado/bin/vivado \
  -mode batch \
  -source tools/fpga_exploration/ultra96/build_direct_ddr_design.tcl
```

Set `MIB_VALIDATE_ONLY=1` to stop after block-design validation. A full build
fails if either setup or hold slack is negative.

## Board runner

The current PetaLinux image provides OpenCV 3.4, XRT 2.6, ZOCL, and the
`nlohmann/json` header previously staged under
`/home/root/mib-fpga-full/include`:

```bash
g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra \
  -I/usr/include/opencv4 \
  -I/home/root/mib-fpga-full/include \
  ultra96_direct_ddr_runner.cpp \
  -o ultra96_direct_ddr_runner \
  -lopencv_imgproc -lopencv_core -lxrt_core -pthread
```

The runner is intentionally fixed to the verified comparison contract:
512x96 Mono8, background subtraction enabled, threshold 8, 4 PPC, and
250 MHz. It pins FPGA control to A53 CPU 2 and ARM post-processing to CPU 3
during the two-buffer throughput pass.

## Verification

```bash
python3 tools/fpga_exploration/benchmark/verify_direct_ddr.py \
  --reference-masks /mnt/fpga-workspace/results/ultra96-full-pipeline/mib_masks.raw \
  --reference-json /mnt/fpga-workspace/results/ultra96-full-pipeline/mib_full_pipeline.json \
  --board-masks /mnt/fpga-workspace/results/ultra96-direct-ddr/direct_ddr_masks.raw \
  --board-json /mnt/fpga-workspace/results/ultra96-direct-ddr/direct_ddr_result.json \
  --output /mnt/fpga-workspace/results/ultra96-direct-ddr/verification.json
```

Success prints `MIB_DIRECT_DDR_EXACT`. The runner also compares every mask in
its 5,000-frame serial steady-state pass with the exact 171-frame result; that
validation time is reported separately and excluded from processing latency.
