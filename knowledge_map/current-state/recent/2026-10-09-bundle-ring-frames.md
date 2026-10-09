## 2026-10-09 — The bundle sets the frame ring and the boot args together (results13)

`bundle_assemble.sh` takes `PZ_RING_FRAMES` (frames the PL's every-frame ring keeps; 0 or unset = no ring) next to `PZ_BOOTARGS`. With a ring
it checks that the boot args' `mem=` stops below the ring base (the ring ends at the PL's 0x3F000000 area: 5000 frames are 283.2 MiB and
start at about 724 MiB, so `mem=720M` fits and the default `mem=1008M` does not), refuses a bundle where they disagree, writes
`Environment=MIB_PZ_RING_FRAMES=<n>` into the staged unit (once, also when the bundle is assembled again), and adds one BUILD_INFO line:
the ring, its placement, the `mem=` limit and the unit setting. The unit in the repository carries no ring setting, so older bundles keep
today's behaviour. Studio still validates the placement against `/proc/iomem` before every ARM, so a bundle and a kernel that disagree
refuse the run with the reason. `scripts.yofo_standing_package` covers a matching bundle, a clash, a missing `mem=`, a ring larger than the DDR
and an unchanged bundle without a ring.
