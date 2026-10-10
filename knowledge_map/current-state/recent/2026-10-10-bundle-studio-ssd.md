## 2026-10-10 — The bundle assembler can wire Studio to the SSD (`PZ_STUDIO_SSD=1`, #667 S2)

`scripts/yofo/bundle_assemble.sh` with `PZ_STUDIO_SSD=1` writes `MIB_PZREC=/usr/bin/pzrec`, `MIB_SSD_IMAGE=disk` and `MIB_PZREC_ARGS` into the unit and names them in BUILD_INFO
(`studio ssd: ...`); `deploy/yofo-studio/install.sh` installs `tools/pzrec` as `/usr/bin/pzrec` (the RAM root forgets it at every boot). The arguments come from the bundle's own numbers
(bounce from `PZ_SSD_BOUNCE(_BYTES)`, ring from `PZ_RING_FRAMES`, `--disk-sectors` from the required `PZ_DISK_SECTORS`) so they cannot drift, and the assembler refuses without pzrec,
a ring, a bounce buffer or the disk size. Without the switch the unit has none of them. The CLI's default `--ring-bytes 0x12B34000` overshoots the ring (it ends at 0x40000000); the derived
value is the ring's true extent (0x11B34000 for 5000 frames), so the bounce-versus-ring overlap check is exact.
