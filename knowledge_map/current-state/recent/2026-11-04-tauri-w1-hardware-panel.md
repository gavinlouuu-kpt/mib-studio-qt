## 2026-11-04 — Hardware panel beside every stage tab (HW-018, HW-042; epic #868, PR5)

The desktop classic view's device panel (pumps, autofocus, pulse generator, Z stage) moved out of the Connect tab into a
collapsible, resizable right-hand panel (`desktop/src/areas/hardware/HardwareDock.tsx`, placed by `App.tsx` beside `<main>`)
shown with Connect, Overview, Experiment and Review, as Qt's hardware sidebar. Its collapsed flag and width persist
(localStorage and the shell preferences file, restored defensively); when the window cannot give the main area its minimum
width the panel narrows, then hides with Qt's status-bar notice, and returns by itself (never stored as the operator's choice).
`toggleHardwarePanel` is in the studio context for the shell's menu item and Ctrl+Shift+H (SHELL-012, #862). Frontend only; the
panel contents (`HardwareControls`, `StageControls`, PR1/PR2 files) are unchanged. See [[architecture/Desktop-Shell]].
Review (2026-11-06): `hardwarePanel.app.test.tsx` now pins that the panel stays mounted while collapsed and while hidden for
space (the same Stop pump and serial-port nodes, a typed port draft kept); both tests fail if `HardwareDock` renders
`HardwarePanel` only while the panel is visible.
