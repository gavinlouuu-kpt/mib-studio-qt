# YOFO Studio release = instrument bundle

Status: active

## Goal

On the PZ7035 the instrument is the PL (FPGA science, firmware) and the PS (Linux, producer,
Studio) together, and they only work as a matched pair: Studio expects a PL BUILD_ID, an ABI and
a producer. A YOFO Studio release is therefore **one bundle**, not a server binary plus whatever
PL happens to be loaded. The bundle is built from a mib-studio-qt commit and a pz7035-imx426 ref
(a tag such as `pl-results9`), lists every file in one `MD5SUMS`, and names both commits, the PL
BUILD_ID and the ABI on the first line of `BUILD_INFO`. Restoring the board takes the bundle path
and nothing else.

## Definition

Contents (see `scripts/yofo/bundle_assemble.sh`):

| Part | Files | Goes to |
|---|---|---|
| Studio | `yofo-studio-server`, `dist.tar`, `yofo-studio.service`, `pl-ready.sh`, `install.sh` | board (`install.sh`) |
| Identity | `core.json` (the PL build's own) | board `/etc/yofo/expected-core.json` (`install.sh`) |
| Producer | `producer/libpz7035_gentl.cti` (md5 pinned) | board `/usr/lib/genicam` (`install.sh`) |
| Slot tools | `tools/pzcell pzres pzpump page.bin lut.bin` | board `/tmp` (restore script) |
| PL | `host/pl/pz_live.bit`, `host/pl/ps7_init.tcl` | JTAG from the PC (restore script) |
| Firmware | `host/firmware/live_server.elf` | JTAG from the PC |
| Linux | `host/linux/` dtb, RAM root, `bootargs` | JTAG boot from the PC |

`host/` is 70 MB and never copied to the board; `install.sh` checks only the board side of
`MD5SUMS`. The PL side never needs dcp, xpr or reports.

**Boot check.** Studio's preflight already compares the loaded PL's identity with
`/etc/yofo/expected-core.json`. The bundle installs the core.json of *its own* PL there, so a
mismatch means the wrong image is loaded, not that two files disagree.

**Release.** A YOFO Studio release (update list, in-app Help, release notes) is a bundle:
version = `mib-studio-qt <commit> + pz7035-imx426 <commit>`.

## Acceptance criteria

- [x] `package_studio.sh --bundle` (PZ7035_REF, default `pl-results9`) assembles the bundle.
- [x] One `BUILD_INFO` line names both commits, the PL BUILD_ID and the ABI; each part lists its
      source commit.
- [x] `install.sh` installs the producer and `expected-core.json` from the bundle, verifies the board side.
- [x] `scripts.yofo_standing_package` covers assembly against a fixture repo (wrong-image core.json refused).
- [ ] The board owner's restore script takes only the bundle path and loads PL, firmware and Linux from `host/`.
- [ ] One restore of a bundle on the board, preflight green, identity matches (needs a slot and Gavin's yes).

## Decision log

- 2026-10-08: the unit of release is the bundle (Gavin: Studio is the PL and the PS together; the
  coordinator's definition). Flash/SD boot instead of JTAG is a later step and not part of this.
- 2026-10-08: sources are this host's build outputs located from the pz7035 ref (`pl-<name>` ->
  `build/pz_live_<name>` in any worktree of the repo, `core.json` image checked against the ref);
  firmware is checked to be an ancestor of the ref and reported with its commit.

## Progress

- [x] bundle assembly and install.sh changes
- [ ] restore script change (board owner)
- [ ] first bundle restore on the board

## Frame ring (results13, #649)

`PZ_RING_FRAMES` with a matching `mem=` in `PZ_BOOTARGS` (for example 5000 frames and `mem=720M`) puts `MIB_PZ_RING_FRAMES` in the bundle's unit and a
`frame ring:` line in BUILD_INFO; the assembly refuses a `mem=` that reaches the ring base. Nobody edits the standing unit by hand: the ring
size and the boot arg are changed together, in a new bundle.

`PZ_CTI_COMMIT` (with `PZ_CTI` and `PZ_CTI_MD5`) names the commit the producer was built from; BUILD_INFO then names the Studio commit, the PL tag and the producer commit.

`PZ_PZREC_COMMIT` (results14, #667) puts the board owner's `pzrec` CLI (pz7035 `tools/pzrec`) in the bundle as `tools/pzrec` (armv7): built from that commit with `YOFO_SDK`, or taken from `PZ_PZREC` (a prebuilt 32-bit ARM ELF, anything else is refused). BUILD_INFO gets `pzrec: <commit> (tools/pzrec, armv7, md5 ...)` and MD5SUMS lists the binary. The unit gets no `MIB_PZREC` / `MIB_SSD_IMAGE` unless `PZ_STUDIO_SSD=1` (below).
`PZ_STUDIO_SSD=1` (S2, #667) makes Studio drive the SSD: the unit gets `MIB_PZREC=/usr/bin/pzrec` (install.sh installs `tools/pzrec` there), `MIB_SSD_IMAGE=disk` (a placeholder; with `--bounce-phys` the CLI ignores the image) and `MIB_PZREC_ARGS`, written as `Environment=` lines and named in BUILD_INFO (`studio ssd: ...`). The arguments are derived so they cannot drift: `--bounce-phys/--bounce-bytes` from `PZ_SSD_BOUNCE(_BYTES)`, `--ring-base/--ring-bytes` from `PZ_RING_FRAMES` (the ring's real extent, ending at 0x3F000000), `--disk-sectors` from the required `PZ_DISK_SECTORS` (no default). It refuses without pzrec in the bundle, a ring, a bounce buffer or `PZ_DISK_SECTORS`. Without the switch the unit has none of them (the standing bundles stay without an SSD). `pzblk` stays unloaded.

`PZ_SSD_BOUNCE` (with `PZ_SSD_BOUNCE_BYTES`, default 1 MiB, 4 KiB aligned) records the PL's SSD bounce buffer in BUILD_INFO (`ssd bounce: 0x2D000000 (1 MiB)`). The assembly refuses an address inside Linux's RAM (the `mem=` end) or one that overlaps the frame ring (the ring base, or the 0x3F000000 ceiling without a ring).

`PZ_PZBLK_KO` (with `PZ_PZBLK_MD5`, checked) ships the SSD block-device module as `modules/pzblk.ko` (32-bit ARM ELF, listed in MD5SUMS, kernel vermagic in BUILD_INFO). The unit never loads it: the board owner loads it by hand in the bring-up.
