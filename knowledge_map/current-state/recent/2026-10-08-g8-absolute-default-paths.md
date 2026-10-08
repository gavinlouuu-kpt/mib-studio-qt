## 2026-10-08 — Browser save paths start under the instrument's data directory (#651 G8)

In the browser, "Save Experiment Data" and Record prompt for a path on the instrument with a relative default
(`experiment.h5`, `clip.h5`); the server's working directory was not set, so a default accepted as typed landed
wherever systemd started it. The unit now sets `WorkingDirectory=/var/lib/yofo-studio` (the data dir
`install.sh` creates), and the remote dialog shim makes a relative default absolute under the instrument's data
directory (`fetch_instrument_status` `storage.path`, via `setRemoteDefaultDir` and `resolveRemoteDefault`). Absolute
defaults, and the desktop's native dialogs, are untouched. A file browser comes with the listing route of G3. See
[[Desktop-Shell]].
