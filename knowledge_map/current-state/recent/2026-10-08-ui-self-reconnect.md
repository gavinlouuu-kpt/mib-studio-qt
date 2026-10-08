## 2026-10-08 — The browser UI reconnects by itself after a link drop or a board reboot

For YOFO Studio as the PZ7035's standing state: `/auth` now carries a per-process `boot_id`, and
`AuthGate` keeps probing it after the app mounted. A lost link shows a banner and is retried; the
same boot id means carry on; a new boot id (backend restarted or board rebooted) re-mounts the app so
it reloads the new backend's state; a rejected token brings the token prompt back over the running
app. No page reload. Verified in Chromium by stopping and restarting a real server under an open
page. See [[architecture/Desktop-Shell]].
