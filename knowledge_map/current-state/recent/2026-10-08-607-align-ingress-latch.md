## 2026-10-08 — Whole-frame Align latches the full-field ingress geometry before the bridge is armed (#607)

After the PZ7035 PL was reloaded under Linux (the #515 hardware check), Align's whole-frame preview
never published: all frames were counted lost and no preview was produced. The board owner's
diagnosis: the bridge sizes whole frames from the ingress geometry latched at the last receiver
reset, and the ingress was still sending the Run window. `setInstrumentMode` now calls
`PzInstrumentControl::latchIngressGeometry(816, 624)` (`S[9]`, then one receiver-reset pulse with the
U-Net enable kept, then the 500 ms settle, the order `tools/pzcell roi` uses) after the producer
stops and before the bridge preview is armed. The order is unit tested
(`instrument_modes_test`); the fix is not yet confirmed on hardware. See
[[data-model/PZ7035-Records]], [[architecture/AppBackend]].
