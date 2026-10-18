## 2026-10-19 — Review module on the frontend transport (#512 S9)

Track B (#717), plan [`2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
slice S9. `desktop/src/review/` no longer imports `@tauri-apps/api/core`,
`@tauri-apps/plugin-dialog` or `@tauri-apps/plugin-opener`.
`reviewBridge.ts` and `updates.tsx` use the transport's `invoke`, chart
staging uses the new `upload` (`reviewBridge.stageChart` / `clearCharts`),
and `ReviewPanel`, `RegenerateMasks` and `ChartsView` use `transport/dialogs`,
which gained `ask`. The transport changes are additive:
`Transport.upload(cmd, bytes, headers)` is Tauri's raw IPC body in the desktop
shells; over the WebSocket it rejects with `TransportUnsupportedError`,
because the server reads no binary requests, so the server is unchanged.

In a browser, the job dialog's Show in folder is disabled with its reason, and
a refused chart upload reads "Chart images cannot be sent to the instrument
over this browser connection yet…".

YOFO Review and MIB Studio send the same IPC as before. `reviewIpc.test.tsx`
drives the review flows and compares every call at
`window.__TAURI_INTERNALS__.invoke` with a golden file recorded before the
move. That covers commands, arguments, chart bytes and headers, and dialog
and opener options.

`transportBoundary.test.ts` is the import guard. No contract change. The
WebSocket server does not serve the review commands yet (S11). See
[[YofoReview]] and [[Desktop-Shell]].
