## 2026-10-08 — The PZ7035 LED is switched off when the backend shuts down

On the board, stopping the YOFO Studio server after an Align session left the LED strobe running
(`S[0]` = 1, 428,065 pulses counted before the board owner turned it off). `AppBackend::shutdown()`
now calls `PzInstrumentControl::ledOff()` after the capture is stopped; a blank PL or another image
is refused as for every instrument write, so nothing is touched then. The server's
`bridge.shutdown()` destroys the backend, which runs it. Tested in `instrument_modes_test`. See
[[architecture/AppBackend]].
