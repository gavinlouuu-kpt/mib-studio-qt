# Desktop classic view: area ownership

The MIB Studio desktop app's classic view (React + Tauri, `desktop/src/`) used to live in one
2685-line `App.tsx` and one 1266-line `App.css`. For the Tauri parity program (#869, wave 0) it is
split into one module per area, with no change in behaviour, so the seven area streams (#862–#868)
can work in parallel without editing the same file. This page says which stream owns which files,
how the pieces fit together, and the few rules that keep the split safe.

Program plan: [`../exec-plans/active/2026-11-05-tauri-parity-program.md`](../exec-plans/active/2026-11-05-tauri-parity-program.md).
Architecture note: [`knowledge_map/architecture/Desktop-Shell.md`](../../knowledge_map/architecture/Desktop-Shell.md).

## How it fits together

```
desktop/src/
  App.tsx                     composition only: provider, layout, stage-tab routing
  App.css                     @import manifest: the area style files in cascade order
  studio/
    useStudioController.ts    calls every area hook, in the original effect order
    StudioContext.tsx         StudioContext + useStudio(): what every area view reads
    constants.ts              constants used by several areas (H5_FILTER)
  styles/                     shared CSS: base.css, widgets.css, modal.css
  areas/<area>/
    use<Area>.ts              use<Area>State() (state only) + the area's callback/effect hooks
    *.tsx                     the area's views (each reads useStudio())
    *.css                     the area's rules (imported by App.css)
```

- **State.** Each area declares its state in `use<Area>State()` (`useState`/`useRef` and values
  derived from them, never an effect). `useStudioController()` calls all of these first, so any area's
  hook can read or set any area's state, the way the single `App` component could.
- **Behaviour.** Callbacks and effects live in named hooks in the area's `use<Area>.ts` (for example
  `useReviewSource`, `useCameraModeSwitch`). Each hook declares what it reads as a typed `Pick` of
  other areas' state; `tsc` (`noUnusedLocals`) rejects a dependency it does not use.
- **Views.** Every view is a component under `areas/<area>/` that takes what it needs from
  `useStudio()`. No props are drilled; `App.tsx` only places the components.

## Owner map

Waves are from #869: wave 1 starts after this split merges, wave 2 follows.

| Area | Epic | Wave | Files under `desktop/src/areas/<area>/` | Existing modules it mainly uses (paths unchanged) |
|---|---|---|---|---|
| `shell`: menus, sidebar, stage tabs and workflow bar, recovery banners, context bar, status bar and log, dialogs (About, Diagnostics, offline help), startup, platform/instrument status, preferences, close guard | [#862](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/862) | 2 | `useShell.ts`, `workflowModel.ts`, `MenuBar.tsx`, `Sidebar.tsx`, `Workflow.tsx`, `RecoveryBanners.tsx`, `StatusBar.tsx`, `Dialogs.tsx`, `menubar.css`, `layout.css`, `workflow.css`, `context-bar.css`, `statusbar.css`, `narrow-windows.css` | `OfflineHelp.tsx`, `brand.ts`, `workflow.ts`, `contextBar.ts`, `persistedState.ts`, `runtimeRecovery.ts`, `diagnosticsView.ts`, `linkHealth.ts`, `experimentCounters.ts`, `components/{CaptureRecovery,ExperimentRecovery,DiagnosticsPanel,PanelErrorBoundary,AuthGate,ControlBanner}.tsx` |
| `connect`: Connect stage (device sources, mock camera, preflight checklist), Overview stage (camera window, live canvas, quality gates), Start/Stop Camera, Align/Run mode switch | [#863](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/863) | 1 | `useConnect.ts`, `ConnectTab.tsx`, `OverviewTab.tsx`, `CameraActions.tsx`, `MockCameraDialog.tsx`, `preflight.css`, `quality-gates.css`, `devices-list.css`, `camera-window.css`, `mode-notice.css` | `preflight.ts`, `quality.ts`, `imageQuality.ts`, `cameraAlignment.ts`, `cameraWindow.ts`, `runFps.ts`, `components/RunWindowNotice.tsx` (shared with monitoring) |
| `config`: App config sub-tab (cores, profiles, checked config document, live `config.json`, quick processing controls), camera script and camera documents, Central Methods | [#864](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/864) | 2 | `useConfig.ts`, `ConfigPanel.tsx`, `central-methods.css` | `liveConfigDraft.ts`, `configDocument.tsx`, `profiles.tsx`, `profileCatalog.tsx`, `coreManagement.tsx`, `applicationUpdate.tsx`, `cameraScript.tsx`, `cameraDocument.tsx`, `CentralMethodsPanel.tsx`, `registry.ts` |
| `preview`: frame pulls and drawing, display rates, live and PZ7035 Run preview, frame-ring playback, Record, ROI, background, preview buffer | [#865](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/865) | 2 | `usePreview.ts`, `PreviewPanel.tsx`, `ring-playback.css` | `framePullScheduler.ts`, `framePacket.ts`, `previewBuffer.tsx`, `previewPacing.ts`, `runPreview.ts`, `ringPlayback.ts`, `processedPreview.ts`, `components/{RingPlaybackPanel,ProcessedPreview,ProcessingSetupControls,BackgroundCalibrationControls}.tsx` |
| `monitoring`: Experiment stage frame (sub-tabs, Start/Stop Experiment, readiness and outcome, SSD, Files, Recordings), Monitoring sub-tab | [#866](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/866) | 1 | `useMonitoring.ts`, `ExperimentTab.tsx`, `MonitoringPanel.tsx`, `run-status.css`, `ssd.css` | `experimentStart.ts`, `runOutcome.ts`, `resultsRates.ts`, `monitoringCells.ts`, `ssdView.ts`, `filesView.ts`, `components/{MonitoringCharts,RunRateControl,RunOutcomeNotice,SsdStrip,SsdRunsPanel,FilesPanel,ResultMetricCells}.tsx` |
| `review`: MIB Studio's Review stage (open/close, scrubbers, metrics pages, exports, re-analysis, charts) | [#867](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/867) | 1 | `useReview.ts`, `ReviewTab.tsx` | `exportControls.tsx`, `reviewExport.ts`, `reanalysisControls.tsx`, `components/{ReviewCharts,SavedReviewImage}.tsx`; YOFO Review (`desktop/src/review/**`, [YofoReview](../../knowledge_map/frontend/YofoReview.md)) |
| `hardware`: Operator/Service mode and the actuation arm, device panel (pumps, nanopositioner/autofocus, pulse generator, Z stage), trigger tests, PZ7035 LED controls | [#868](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/868) | 1 | `useHardware.ts`, `ServiceMode.tsx`, `HardwarePanel.tsx`, `TriggerTestControls.tsx`, `InstrumentLedPanel.tsx`, `service-mode.css` | `commissioning.ts`, `pumpRate.ts`, `components/{HardwareControls,PumpControls,AutofocusControls,autofocusControlModel,autofocusMessages,HardwareConnectionFields,StageControls,PulseGeneratorControls,InstrumentLedControls,EndpointDiscovery,StartupDiscoveryControls}.tsx` (+ their `.css` and `*Model.ts`), `discovery.ts`, `startupPreference.ts` |

**Shared, change only with a note in the PR (the orchestrator merges them):** `App.tsx`, `App.css`,
`studio/**`, `styles/**`, `bridge.ts`, `bridgeContract.ts`, `eventAdapter.ts`, `transport/**`,
`platformCapabilities.ts`, `metricFormat.ts`, `components/ViewerGate.tsx`, and modules two areas use
(`components/ResultMetricCells.tsx`, `components/RunOutcomeNotice.tsx`, `runOutcome.ts`,
`components/RunWindowNotice.tsx`). Keep shared edits small and additive.

**Platform stream (`fix/tauri-platform-bugs`, #845/#850/#851) owns** `desktop/src-tauri/**`,
`crates/**`, `desktop/src/closeGuard.ts` and `desktop/scripts/windows-runtime.cmake`.

### Seams: one area's view hosts another's part

| Host view | Hosted part (owner) |
|---|---|
| `shell/MenuBar.tsx` (menu row) | `hardware/ServiceMode.tsx` `ModeToggle` |
| `shell/Workflow.tsx` `StageTabs` (tab header) | `connect/CameraActions.tsx` |
| `connect/OverviewTab.tsx` toolbar | the host ROI fields: state and `onApplyRoi` are preview's (`usePreview.ts`); the JSX stays in the Overview toolbar |
| `monitoring/ExperimentTab.tsx` | `preview/PreviewPanel.tsx` (Preview sub-tab), `monitoring/MonitoringPanel.tsx` |
| `preview/PreviewPanel.tsx` | `hardware/InstrumentLedPanel.tsx`, `config/ConfigPanel.tsx` (configuration sub-tabs) |
| `monitoring/MonitoringPanel.tsx` toolbar | `hardware/TriggerTestControls.tsx` |
| `shell/Dialogs.tsx` | `CentralMethodsPanel.tsx` (config) |

## Rules that keep the split safe

1. **Do not reorder the hook calls in `studio/useStudioController.ts`.** React runs a component's
   effects in the order they were registered, and the calls keep the order the effects had in the
   single `App` (for example, the frame-pull invalidation on a tab change runs before Review's
   recovered file requests its first frame). Add a new effect inside your area's hook. A new hook
   call is fine anywhere; moving an existing one is a behaviour change.
2. **New state goes in your area's `use<Area>State()`**, never an effect there. Returned names share
   one namespace (the context); pick names that do not clash with another area's.
3. **Keep each `<ViewerGate>` in the same file as the control it gates.**
   `components/ViewerGate.test.tsx` scans `App.tsx` and `areas/**/*.tsx` and checks every control
   command sits inside a `<ViewerGate>` of its own file (or carries `viewerOnly` in its `disabled`).
4. **CSS order is the cascade.** `App.css` imports each style file once, in the old file's order
   (Vite inlines `@import` in place, so the built CSS was byte-identical after the split). Add rules
   to your area's file; put a new file's `@import` where its rules belong (usually last). Do not
   reorder the imports. The #853 selector (`.subtab-body > .config-group`) is in
   `styles/widgets.css`; the Review epic fixes it.
5. **Line references.** The parity matrix (PR #844) cites `desktop/src/App.tsx:<line>` at
   `5f906ac2`. [`desktop-area-ownership-linemap.json`](desktop-area-ownership-linemap.json) maps every
   moved line to its new file and line at the split commit (`old`, `file`, `new` ranges). Look one up:
   `jq '.[] | select(.old[0] <= 1853 and .old[1] >= 1853)' docs/howto/desktop-area-ownership-linemap.json`.
   After later edits, follow the file's history (`git log -L`).

## Checks every area PR runs

`cd desktop && npx tsc --noEmit && npx vitest run` (the suite and the review IPC golden snapshot),
the Rust tests and `desktop/scripts/native-workflow.py` as in `.github/workflows/desktop-ci.yml`,
and `python3 scripts/check_docs.py`. Camera, capture and eGrabber changes are also tested on the
win11-agent rig, scheduled by the orchestrator (#869).
