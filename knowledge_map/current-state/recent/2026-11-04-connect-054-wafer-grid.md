## 2026-11-04 — Wafer Grid on the Tauri Overview (CONNECT-054, #863)

The React Overview stage has Qt's **Wafer Grid** toggle: dot-grid localization decodes the live frame only while the
Overview is on screen, and the live canvas shows the detected dots, the image-centre cross and the pose text (or the
failure reason). Bridge ABI next (#878, set at merge) adds `set_dot_grid_enabled`, `set_dot_grid_paused` and `fetch_dot_grid_pose`
([[architecture/Rust-Bridge]]); the Qt-free half is `DotGridService::bundledRegistry()` (the registry compiled into
`mib_backend`), `AppBackend::setDotGridEnabled` and the `BackendFacade` pose JSON ([[services/DotGridService]]). The
facade starts the service paused and the Overview resumes it. Also fixed: `AppBackend::installFrameStore` now hands the
new buffer to the dot-grid service (a MindVision/Aravis Overview switch used to leave it on the dead store), and
switching the service on forgets the earlier pose. The `dot_grid` section of a profile's `config.json` is still not
applied in this app. The React side is `desktop/src/areas/connect/{waferGrid.ts,useWaferGrid.ts,WaferGridControls.tsx}`
with a one-line overlay call in `usePreview.ts` `draw()` ([[frontend/OverviewTab]], [[architecture/Desktop-Shell]]).
