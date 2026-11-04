## 2026-11-04 — Tauri Connect stage: discovery outcomes, selection, startup auto-select, delivery mode (#863 B1, #852)

The React/Tauri Connect stage follows Qt's `ConnectTab` / `DeviceInitManager`. A cancelled, incomplete or
refused scan reads "Camera scan cancelled." / "(incomplete: <provider> <kind>)" / "Camera discovery
incomplete: <reason>" on the status line, never "0 cameras found"; row 0 is preselected after a scan,
framegrabber rows are selectable and Connect acts on the shown sub-tab; Refresh joins a running scan.
Connect and Configure Mock say why they are blocked (camera running, experiment active) and the backend
refuses them too (`select_hardware_camera` in Rust, `ConfigureMockCamera` in the facade). After Connect or
Configure Mock the status says "Connected to <label> (not capturing)" and the stage moves to Overview.
The startup camera selection is on by default (#852) with an opt-out, visible on every instrument; its
outcome (camera highlighted, sub-tab open, Overview; "No cameras found." with the check-power dialog;
several cameras; incomplete) is reported once per camera job. New bridge ABI 45: `fetch_frame_delivery_mode`
and `set_frame_delivery_mode` back the Connect delivery-mode select; `CaptureService::config()` and a
lock-guarded `setConfig`. Decisions and the Qt-label-goes-stale note are in the PR. See
[[architecture/Desktop-Shell]], [[architecture/Rust-Bridge]], [[services/CaptureService]].
