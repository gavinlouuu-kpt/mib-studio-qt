## 2026-10-22 — The workspace branch opens in the workspace; the classic view one click away

Gavin, testing `feat/egrabber-workspace`: the branch should open in the workspace, with the
classic view one click away. The app now opens on Prepare unless the operator last chose the
classic view in this browser. `desktop/src/workspace/landing.ts` remembers the explicit choice in
`localStorage` (`yofo.workspace.landing`, default `workspace`): "Workspace preview" records the
workspace, the workspace's "Classic view" (its `onExit`) records the classic view, and task jumps
(Prepare's setup buttons, Open in Review) record nothing. The Tauri desktop and the PZ7035 browser
UI share `App`, so both get the same default; the Review app is unaffected. Opening in the
workspace acts on nothing: the classic tab stays on Connect and the classic startup landing never
runs in that session, even when the operator leaves before the first camera snapshot (Codex
review: it would otherwise land on Experiment after a failed run and switch the desktop camera). The band no longer says the classic view is the default; it keeps "Navigation never
acts…". `develop` is unchanged until promotion. Tests: `landing.test.ts` and
`navigation.test.tsx` ("the workspace branch opens in the workspace"); the earlier navigation gates
start from a remembered classic choice. See [[architecture/Desktop-Shell]].

The native webview gate (`desktop/scripts/native-workflow.py`) drives the classic view through
its buttons and reads visible text, so it failed once the app opened in the workspace. It now asserts
the workspace landing, takes "Classic view" and restarts the webview before the classic workflow.
