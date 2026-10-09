## 2026-10-09 — The #667 Studio note is aligned with the board owner's SSD design

`docs/exec-plans/active/2026-10-09-ssd-record-experiments.md` gained a section that maps its requirements (R1 to R9) onto the board
owner's `docs/SSD_RECORDING_DESIGN.md` (pz7035 15603ac9): `libpzrec` and the `pzrec` CLI over the 0x40102000 window, the 256 x 128 B run
table with the deleted flag and a time source, the `drops_before` FRAME byte and the trailer totals, the `pzblk` block device
(`/dev/yofoblk0p1` for the raw area), the drain-wins arbitration, and the accounting mapping of the new totals. S1 runs against
`libpzrec` on a file-backed fake block device. Docs only.
