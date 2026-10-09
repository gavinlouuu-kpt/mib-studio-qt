## 2026-10-10 — The bundle can ship pzrec and name the SSD bounce buffer (results14 prep)

`scripts/yofo/bundle_assemble.sh` takes `PZ_PZREC_COMMIT` (pzrec built for armv7 from that pz7035 commit with the YOFO SDK, or a prebuilt ARM binary in `PZ_PZREC`) and installs it as
`tools/pzrec`, with a `pzrec: <commit>` line in BUILD_INFO and its md5 in MD5SUMS; the unit gets no `MIB_PZREC` / `MIB_SSD_IMAGE`. `PZ_SSD_BOUNCE` records the SSD bounce buffer
(`ssd bounce: 0x2D000000 (1 MiB)`) and is refused inside Linux's RAM (`mem=`) or over the frame ring. Tests: `scripts/test_yofo_standing_package.py`.
