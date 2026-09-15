# Troubleshooting

When something looks wrong, compare your screen against this manual's
screenshots for your version (Help ▸ About shows the version; the
screenshots are regenerated for every release). A difference between your
screen and the manual is worth mentioning in a bug report.

## Where to look first

| Artifact | Location |
|---|---|
| Application log | **File ▸ Open Logs Folder** → `%LOCALAPPDATA%\MIB_Studio_Qt\logs\app.log` (portable/dev installs write `data\logs\app.log` next to the executable instead) |
| Crash dumps + state snapshots | `%LOCALAPPDATA%\MIB_Studio_Qt\crashes\` — a `.dmp` minidump plus a `.json` snapshot of service state at crash time |
| Data folder | **File ▸ Open Data Folder** → `Documents\MIB_Studio_Qt` |
| Live diagnostics | **Diagnostics…** in the status bar (or Help ▸ Diagnostics…) — capture session, delivery mode, transport counters, timestamp source, active core, process memory |

Logs rotate at 10 MB (5 files kept) and flush every few seconds, so the
tail of `app.log` reflects the state moments before a problem.

## Common symptoms

**The app doesn't start.** Usually missing runtime files (e.g. Qt
`platforms\qwindows.dll`) after a broken install — reinstall with the full
setup installer. Early startup errors are appended to
`%LOCALAPPDATA%\MIB_Studio_Qt\crash_log.txt`.

**"No camera found" on startup.** Check the camera cable, frame-grabber
seating, and that the eGrabber driver is installed; then click **Refresh**
on the Connect tab. To verify the rest of the app independently of
hardware, use the [mock camera](connect.md#mock-camera-no-hardware).

**Live view frozen, or frame rate / MB/s stuck at a stale value.** Stop the
camera, then start it again. If it recurs, save the log tail — stale
transport statistics point to a capture start/stop ordering problem.

**Start Experiment refuses.** The readiness dialog names the blocked
check and its remedy: `camera.session` (start Live View and wait for
Running), `camera.source` (the hardware camera you selected is unavailable
and the app would have used the simulated camera — select the mock camera
explicitly or connect the hardware), `processing.core` (activate the pinned
core), `storage.output` (choose a writable destination with free space),
`lifecycle.fault` (the previous run's data could not be saved completely —
acknowledge it in the dialog). *"The configuration changed while the
experiment was being prepared"* means something (ROI, background,
configuration, camera) changed after the check: start again.

**Recording stops with a save error.** The app stops the experiment, the
run state reads **Failed – recovery required** and a red alert banner
shows the reason and remedy (it stays until you click **Acknowledge**;
the next Start remains blocked until the fault is acknowledged in the
readiness dialog). Typical causes: disk full, missing permissions on the
target folder, or antivirus blocking writes. The already-written part of
the HDF5 file remains readable in the Review tab.

**An alert keeps coming back after Acknowledge.** Acknowledge only hides
the banner; the same problem happening again re-shows it with an updated
count. Fix the cause (open **Details** on the banner for every open alert)
or check **Diagnostics…** for the underlying transport/core values.

**Review tab is slow on a huge file.** Keep the file on a local SSD rather
than a network share; thumbnails stream in as you scroll. Close other
files first (**Close File**).

**Charts empty on the Monitoring page.** Charts only accumulate while the
Monitoring tab is visible and the camera is running; they reset with each
experiment.

**No update prompt appears.** Check **Help ▸ Software Updates…** manually.
If the entry is greyed out, auto-update was disabled under **Settings ▸
Boot Service Toggles…**. Very old installations may predate the current
update server and need one manual reinstall with the full installer.

## Reporting a problem

**Help ▸ Report a Problem** opens the issue tracker. Include:

1. The app version (Help ▸ About) and what you were doing, step by step.
2. The last ~100 lines of `app.log` (File ▸ Open Logs Folder).
3. If the app crashed: the newest `.dmp` and `.json` pair from
   `%LOCALAPPDATA%\MIB_Studio_Qt\crashes\`.
4. A screenshot of your screen — ideally next to the corresponding manual
   image, if the UI looks different from this manual.
