## 2026-10-21 — Relative output paths resolve under the storage root (#762, F2 of #763)

**Bug (#762, S1):** in the browser UI served by `yofo-studio-server --data-dir <dir>`, a Start
accepted with the prompt's default `experiment.h5` wrote the run, and the HDF5 readiness probe, into
the server's working directory. The snapshot recorded `output_path: "experiment.h5"`, `/files` did
not list the file, and `fetch_instrument_status` kept reporting `storage.path = <dir>`. The browser
prompt only knew the data directory from the PZ7035 status poll, which never runs on a PC server.

**One rule, `app::resolveClientPath(requested, storageRoot)`** (`app/RecordingTarget.h`), reached
through `AppBackend::resolveClientPath`. The storage root is `AppBackend::storageRoot()` =
`app::storageRootOf(dataDir)`: absolute, symlinks resolved; `fetch_instrument_status` now reports it
as `storage.path` (an absolute path even for a relative `--data-dir`). Empty is refused. Absolute is
unchanged (the desktop's native dialogs). With no root configured (an uninitialized backend) a
relative path is unchanged, as before. Otherwise the path is joined to the root and resolved with
`weakly_canonical`; it must stay inside the root, compared by path component (a sibling `data2` is
not inside `data`). `..` that leaves the root, an existing symlink (file or parent) that resolves
outside it and a dangling symlink are refused with a plain reason; the resolved path is what the
writer gets. Strings are UTF-8 in and out (`app::utf8ToPath` / `app::pathToUtf8`; on MSVC
`path::string()` is the ANSI code page): an absolute request comes back byte for byte, and the root,
`storage.path`, a resolved path and the reasons are UTF-8. The readiness probes and
`Hdf5Service::openFile`'s parent-folder creation decode the path as UTF-8 too, as `H5Fcreate` does
on Windows.

**Limit (ruling on #767, round 1).** A local process that swaps a path component for a symlink
between the check and the open is not stopped: the threat model is a path a client sends, not a
local user who can already write to the storage root, and HDF5 opens files by pathname only, so
there is no handle-based walk. Experiment Start checks the path once more right before its open
(a change is `StorageFailed`), which narrows the window after the probes.

**Where it applies.** Experiment Start: `ExperimentCoordinator::acceptOutputPath` (append `.h5`,
then resolve) is the one place the start path is accepted; `evaluateReadiness` and `start` both
call it, so the probes (`.mib_write_probe_*`, `.mib_hdf5_probe_*`) run in the resolved directory and
the candidate, the snapshot `output_path`, the start reply (`experiment started: <path>`), the run
and the status carry the absolute path. An empty path or one that leaves the root is
`InvalidRequest` (checked right after the declared start request, before any state check; nothing is
opened) and readiness fails `storage.output` with the reason without probing. Readiness and Start
now append `.h5` the same way, so a path without the extension no longer reads as stale. Raw
recording (`AppBackend::startFrameRecordingLocked`, an empty path refused too), the review exports
(`BackendFacade::submitReviewExportJson`: `output_root` and `explicit_destination`, so
`review_export_csv` too), Regenerate masks (`submitReviewReanalysisJson` `output_path`) and the
preview buffer save (`savePreviewBufferJson` `output_root`) resolve their client paths the same way.
Not changed: paths a client reads (open recording, sources, config and camera documents, profile
folders). No bridge ABI change: existing fields carry absolute values now.

**Browser prompt (`desktop/src/transport/dialogs.ts`).** `save()` in the browser fetches
`fetch_instrument_status` when the prompt opens (every server answers it, PZ7035 or not; the last
known directory is the fallback), defaults to `<storage.path>/<suggested name>` and says where the
file goes ("The file is saved on the instrument, in <dir>. A relative path is saved there too.").
After a Start the log shows the backend's path. The workspace Plan prompt
(`feat/egrabber-workspace`) is not touched.

**F2 (#763).** With a file open for review, Start stays refused (D1 (a): HDF5 is built without
thread safety, #722) but says how to clear it: `storage.hdf5` remediation `kCloseReviewFile` =
"Close the file in Review (Review → Close File)" (`app/ExperimentReadiness.h`); Start's refusal "an
HDF5 file is already open: Close the file in Review (Review → Close File) before starting an
experiment"; raw recording "… before starting raw recording". While raw recording holds the file,
Start says "raw recording is active: stop it before starting an experiment" and the gate keeps
"finish or close the current file first".

See [[architecture/ExperimentCoordinator]], [[services/ReviewSession]],
[[architecture/AppBackend]], [[architecture/Desktop-Shell]], [[data-model/PZ7035-Records]]. Tests:
`e2e.client_output_path` (the rule, and Start, readiness probes, snapshot, raw recording, export,
Regenerate and preview save through the facade with the working directory moved to an empty folder);
`backend.experiment_readiness`, `backend.facade_review_session` and `backend.facade_boundary` pin the
F2 texts; `dialogs.test.ts` the browser default and prompt text.
