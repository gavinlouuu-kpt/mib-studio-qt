## 2026-10-08 — YOFO Studio release = instrument bundle

The PZ7035 instrument is the PL and the PS together, so the standing package becomes one bundle:
`package_studio.sh --bundle` adds the pz7035-imx426 PL image (default ref `pl-results9`: bitstream,
`ps7_init.tcl`, `core.json`), the firmware, the Linux boot set, the producer and the slot tools, all
in one `MD5SUMS`, with one `BUILD_INFO` line naming both commits, the PL BUILD_ID and the ABI.
`install.sh` installs the producer and the bundle's `core.json` as the expected PL identity and
checks only the board side of `MD5SUMS` (`host/` stays on the PC). See [[Build]] and the plan
`docs/exec-plans/active/2026-10-08-yofo-studio-bundle.md`. The board owner's restore script is
the next step.
