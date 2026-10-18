## 2026-10-18 — Pumps stop when the last remote client leaves (#711)

`stop_and_save` (`yofo-studio-server`, run when the last client has been gone for the grace time and on
SIGTERM) stopped the experiment, the Z stage and the recording and put a PZ7035 into idle, but never
stopped a pump: a sample or sheath pump started by a remote operator kept pumping unattended. In the
same pass that stops the stage it now sends `pump_stop` and then `pump_stop_purge` to every connected
pump slot (DLSP syringe and Tushui peristaltic alike), whatever the status snapshot reports (it is only
as fresh as the last poll, and the DLSP purges through another register than it runs). Every stop is a
line in the returned actions; a stop that fails (`ok: false` or an error) is reported as
`<cmd> <slot> failed: ...` and the other pump is still stopped. No bridge ABI or contract change.
See [[architecture/Rust-Bridge]] and [[architecture/Desktop-Shell]]. Guards:
`client_loss_stops_every_connected_pump_and_its_purge`, `client_loss_reports_a_pump_that_fails_to_stop`.
