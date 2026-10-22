## 2026-10-22 — eGrabber fold-in inventory and discovery reply identity (#806)

The E01–E09 ledger and contract proposals are in
`docs/design/egrabber-workspace/E-CAPABILITY-LEDGER.md`; the active execution plan
records the verified workspace base and preserves external classic-view work.
All 37 design views remain in scope. Live Penpot metadata is pinned to 124/0;
this is not a new native interaction or hardware acceptance pass.

The existing React discovery poller now rejects wrong-job and changed-generation
replies before accepting Completed, Cancelled or Failed. IDs remain decimal
strings; a mismatch never cancels a possibly different backend session. Six
regression cases failed before their fixes, including a coalesced waiter timing
out and cancelling another caller's job. Timeout now cancels only a newly
created job. See [[services/DeviceDiscoveryService]].
No backend, ABI, SDK owner, recorder or scientific authority changed. Full fold-in
acceptance still needs the agreed session/feature/evidence contracts and supported
hardware qualification; frontend fixtures cannot prove those requirements.
