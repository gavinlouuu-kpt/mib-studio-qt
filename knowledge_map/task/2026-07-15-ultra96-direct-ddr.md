# Ultra96 Direct-DDR FPGA Pipeline

## Outcome

The verified 4-PPC/250-MHz Ultra96 mask core now accesses XRT/CMA buffers in
PS DDR through `S_AXI_HP0_FPD`, eliminating full-frame AXI-BRAM MMIO. The ARM
runner preserves the complete MIB comparison path—ordered contours, hierarchy,
metrics, validation, target classification, and tracking—and overlaps it with
the next FPGA frame using two BO slots and dedicated A53 cores.

The strict 171-frame run has zero differing pixels and identical 920 contours,
176 records, 21 valid objects, and 7 targets. A 5,000-frame steady serial pass
also asserts every output mask exactly. Measured throughput improved from
260.5 fps to 1,344 fps; serial latency improved from 3,838.5 us to 799.1 us.

## Decisions

- Reuse the already exact PPC4 HLS core so the experiment isolates transport
  and scheduling.
- Use the boot image's XRT 2.6/ZOCL/CMA support instead of changing PetaLinux
  or adding an out-of-tree DMA allocator.
- Map all three HLS masters to the 2-GiB low-DDR HP0 aperture and reject any BO
  whose physical address falls outside it.
- Keep control on `/dev/mem`; sub-microsecond address-register MMIO is not a
  meaningful bottleneck.
- Exclude mask-verification work from timed processing and report it
  separately.
- Pin PL control to CPU 2 and ARM post-processing to CPU 3 for repeatable
  overlap.

## Evidence

- [[../../docs/integration/ultra96-fpga-image-pipeline|Ultra96 FPGA image pipeline]]
- Source: `tools/fpga_exploration/`
- Results: `/mnt/fpga-workspace/results/ultra96-direct-ddr`
- Build: `/mnt/fpga-workspace/builds/mib-fpga-direct-ddr/ultra96-ppc4-250`

## Remaining constraint

`findContours` and metrics consume roughly 625 us/frame on the A53 and now
dominate. Exact hardware contour/feature extraction is the next major FPGA
boundary; 10-GbE acquisition additionally requires external physical ingress.
