## 2026-10-09 — Files view and the post-run Download link (#651 G3, UI)

In the browser the Experiment tab has a **Files** view over the read-only `/files` routes of the server: breadcrumbs from
the data dir, size and date, a Download link per file (a plain link, so the browser's download manager and resume apply),
"being written" for the output file of a run in progress, and the "Recording to RAM" warning while the data dir is on the
RAM root. After a finished run the outcome area shows a Download link for its file. Model and URL helpers are in
`filesView.ts`, the view in `components/FilesPanel.tsx`; hidden in the desktop shell (native dialogs there). See
[[Desktop-Shell]].
