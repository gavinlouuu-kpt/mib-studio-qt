# HelpDialog

**Source:** `src/frontend/dialogs/HelpDialog.cpp`,
`include/frontend/dialogs/HelpDialog.h`; Tauri counterpart:
`desktop/src/OfflineHelp.tsx`.

## Responsibility

Render What's New and the operator manual offline. Qt reads packaged Markdown
from `<exe>/resources/release-notes/` and `resources/manual/`; development falls
back to the compiled source tree's `docs/` directory. Tauri uses eager Vite raw
imports for Markdown and URL imports for images, without bridge ABI additions.

## Entry points

`HelpDialog(false)` displays running-version notes first and earlier releases
newest first. `HelpDialog(true)` opens `index.md`; relative Markdown links and
images resolve locally, with an Index button and an online-documentation button.
[[MainWindow]] stores `Help/LastSeenVersion` in QSettings and uses
`shouldShowWhatsNew` to prompt once on upgrades, staying quiet on fresh installs
and downgrades. Tauri's Help menu uses `OfflineHelp` for the same content.

## Gotchas and validation

The stable release gate requires curated `docs/release-notes/vX.Y.Z.md`; this
feature does not provide release content. Only local manual pages inside the
manual directory are navigable. Qt's `frontend.offline_help` test covers fixture
notes ordering, local links/images, upgrade policy, and the updater's inline
notes prompt. Vitest's `OfflineHelp.test.tsx` covers menu actions and rendering.
Packaging copies the manual with images and versioned notes beside the Qt exe.
See [[../build-and-run/Build]] and [[System-Utilities]].
