## 2026-10-10 — The bundle can ship pzblk.ko (results14 bring-up)

`scripts/yofo/bundle_assemble.sh` takes `PZ_PZBLK_KO` (+ `PZ_PZBLK_MD5`) and installs the SSD block-device module as `modules/pzblk.ko`: checked to be a 32-bit ARM ELF, its md5 verified when
given, listed in MD5SUMS, with a BUILD_INFO line naming it and the kernel vermagic. The unit does not load it (no `insmod`): the board owner loads it by hand during the slot. Tests: `scripts/test_yofo_standing_package.py`.
