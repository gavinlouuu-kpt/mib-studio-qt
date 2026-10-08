## 2026-10-08 — The PZ7035 Run window survives a restart of the instrument

`#611` restored the Run window after a page reload from `fetch_instrument_status.mode.run_set`, but
the backend kept the window in memory only, so restarting the server (a power cycle, a redeploy)
put the operator back at "not placed". Every successful Run switch now writes
`{x, y, set}` to `<data dir>/instrument_run_window.json` (atomic rename), and
`AppBackend::initialize` reads it back into `instrumentRunOffset()`/`run_set`. A file that is not a
window the Run switch could have applied (off the x%8 / y%4 grid, off the sensor, `set` false,
garbage) is ignored; nothing clears the window except placing another. Tested in
`instrument_modes_test` (restart restores, bad files ignored). See [[architecture/Desktop-Shell]],
[[architecture/AppBackend]].
