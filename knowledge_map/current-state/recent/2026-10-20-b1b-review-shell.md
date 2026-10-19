## 2026-10-20 — YOFO Review shell on the kit, R1 Open as a folder list (#717 B1b)

Track B (#717) step B1, the shell half of R1 Open (#269), on
`feat/egrabber-workspace`. `ReviewApp` is now an `@mib/ui` `AppShell` with the
Review app's stages Open, Overview, Inspect and Export. Header buttons (Fit,
Settings, Updates…, About) replace the menu row. Selecting a stage never calls
the backend.

- **Open (R1)** lists a chosen folder's `.h5` / `.hdf5` files (names only,
  `review_list_dir`; the folder is remembered) in a `DataTable`. It opens a run
  with `review_open` and shows an **Open run** `FactList`/`Card` of what the
  file records, with the run accounting as a `StateChip`. Missing values read
  "Not reported", and a legacy file's accounting reads "Not recorded · legacy
  file". A refused open is a blocked `Notice`. The panel's `openPath` now
  returns `{ ok, message }` and leaves nothing open after a failure, as the
  backend does.
- **Overview** says it is not available yet: statistics come from the #523
  worker.
- **Inspect** is today's `ReviewPanel`, unchanged and always mounted.
- **Export** points to Inspect's toolbar, where every export still runs.
- Launch-path and macOS open requests open straight into Inspect.

Grouping by experiment needs B1a's `review_probe_runs` (review ABI 2, #746)
on this branch and follows the next develop sync. Missing kit pieces for the R1
board (search field, filter chips, row overflow menu, grouped rows) are listed
on #715. Tests: `ReviewApp.test.tsx`, `open/openModel.test.ts`; the existing
review tests are unchanged. See [[YofoReview]].
