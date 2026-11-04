## 2026-11-05 — Tauri: the permanent acquisition-mode badge (#858, SHELL-055)

The Tauri status bar now shows Qt's acquisition-mode badge on every stage (`MainWindow::updateDeliveryModeBadge`):
"▶ EVERY FRAME · sequence preserved" or "⏩ LATEST FRAME · drops stale frames", " (requested)" until a running camera
confirms the mode, with Qt's tooltip. Before, the mode was visible only by reading config.json (and, with #863 B1, on
the Connect stage).

- `desktop/src/areas/monitoring/deliveryModeBadgeModel.ts` (the text, pure) and `DeliveryModeBadge.tsx` (reads
  `fetch_frame_delivery_mode`, bridge ABI 45 of #863 B1, every 2 s and when the camera starts or stops; hidden on the
  PZ7035, before the backend is ready and when the read fails). One additive line in `areas/shell/StatusBar.tsx`.
- Tests: `deliveryModeBadgeModel.test.ts`, `DeliveryModeBadge.dom.test.tsx`. See [[architecture/Desktop-Shell]].
