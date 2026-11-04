## 2026-11-04 — Hardware panel split into pump and autofocus components (#868, #855)

`desktop/src/components/HardwareControls.tsx` (211 lines) is split with no operator-visible change:
`PumpControls.tsx` (Sample and Sheath pumps), `AutofocusControls.tsx` (nanopositioner and autofocus) and
`HardwareConnectionFields.tsx` (the shared port/baud/address fields) now carry the device panels, and
`HardwareControls.tsx` only composes them with `StartupDiscoveryControls` and `PulseGeneratorControls` in the
old order. Each panel polls its own status, so a failed pump read no longer blanks the autofocus panel (and
the reverse). The Tauri parity wave 1 hardware stream builds on this seam: pumps and autofocus gate, poll
and purge in their own files. See [[architecture/Desktop-Shell]] and `docs/howto/desktop-area-ownership.md`.
