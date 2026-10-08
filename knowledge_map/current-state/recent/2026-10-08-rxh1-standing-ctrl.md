## 2026-10-08 — Studio sets the RXH1 v1 persist time at service start

The v1 receiver self-heal (results9/10) gives up on about a third of the Align starts at its default
CTRL (persist 20 ms); with persist 250 ms (CTRL word 1 = 0x64FA0801) it was 2/14 on the board
(2026-10-08). The value does not survive a PL reload, so Studio, the /dev/mem owner, writes it at
service start: `PzInstrumentControl::applyRxHealCtrl` writes P[257] once when the RXH1 block is
present and the v2 word P[280] is zero, reads it back and logs it (`AppBackend::applyRxHealStandingCtrl`).
It does nothing when CTRL already has the value, on an image without the block, on a v2 build (heal v2
backs off on its own), or while the PL is blank or not the cell image. This is the one PL write at
start; the LED and the cell path are still written only when the operator picks a camera mode. See
[[Desktop-Shell]].
