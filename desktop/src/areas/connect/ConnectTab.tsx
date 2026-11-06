// Connect and Overview (epic #863): the Connect stage, device sources and the UX-3 hardware preflight
// checklist. Moved from App.tsx without change (#869 wave 0).
import type { FrameDeliveryMode } from "../../bridge";
import { ViewerGate } from "../../components/ViewerGate";
import { CHECK_STATUS_LABEL } from "../../preflight";
import { useStudio } from "../../studio/StudioContext";
import {
  cameraSourceBlockReason, connectBlockReason, connectStatusLine, DELIVERY_MODE_HINT, DELIVERY_MODES, deviceRows, pickedRow, refreshBlockReason,
} from "./connectFlow";

export function ConnectTab() {
  const {
    connectTab, setConnectTab, pz7035, caps, discovery, pickedDevices, setPickedDevices, refreshCameraState, ready,
    onConnectPicked, cameraScript, running, expActive, setShowMockConfig, camSelection, preflight, refreshPreflight,
    scanning, scanNote, actionNote, deliveryModeStatus, onDeliveryModeChange, autoSelect, onAutoSelectChange,
  } = useStudio();
  const rows = deviceRows(discovery, connectTab);
  const blocks = { ready, running, expActive, scriptBusy: cameraScript.busy };
  const connectBlock = connectBlockReason(blocks, connectTab, pickedRow(discovery, connectTab, pickedDevices));
  const sourceBlock = cameraSourceBlockReason(blocks);
  const refreshBlock = refreshBlockReason(ready, scanning);
  const deliveryBlock = !ready ? "Backend is not initialized" : expActive ? "Cannot change the delivery mode while an experiment is active." : !deliveryModeStatus ? "Delivery mode is not available" : undefined;
  const deliveryPending = deliveryModeStatus?.capturing && deliveryModeStatus.confirmed && deliveryModeStatus.active_mode !== deliveryModeStatus.mode
    ? DELIVERY_MODES.find((m) => m.value === deliveryModeStatus.active_mode)?.label : undefined;
  const noDevices = connectTab === "mindvision" ? "No MindVision cameras found (SDK/hardware required)."
    : connectTab === "framegrabbers" ? "No framegrabbers found (EGrabber SDK/hardware required)." : "";
  return (
    <>
      <p style={{ margin: "0 0 6px" }}>Available devices:</p>
      <div className="subtabs" role="tablist" aria-label="Device sources">
        <button className={connectTab === "cameras" ? "active" : ""} onClick={() => setConnectTab("cameras")}>
          Cameras
        </button>
        {!pz7035 && <button className={connectTab === "mindvision" ? "active" : ""} onClick={() => setConnectTab("mindvision")}>
          MindVision
        </button>}
        {caps.egrabber_script && <button className={connectTab === "framegrabbers" ? "active" : ""} onClick={() => setConnectTab("framegrabbers")}>
          Framegrabbers
        </button>}
      </div>
      <div className="subtab-body">
        <div className="devices-list" role="listbox" aria-label="Discovered devices" aria-busy={scanning}>
          {rows.map((row) => (
            <div key={row.key} style={{ padding: "2px 0" }}>
              <label>
                <input
                  type="radio"
                  name={`device-${connectTab}`}
                  checked={pickedDevices[connectTab] === row.key}
                  onChange={() => setPickedDevices((p) => ({ ...p, [connectTab]: row.key }))}
                />{" "}
                {row.label}
              </label>
            </div>
          ))}
          {rows.length === 0 && noDevices && <div>{noDevices}</div>}
        </div>
        <div className="toolbar" style={{ marginTop: 8 }}><ViewerGate>
          <button onClick={refreshCameraState} disabled={!!refreshBlock} title={refreshBlock}>Refresh</button>
          <button
            onClick={onConnectPicked}
            disabled={!!connectBlock}
            title={connectBlock}
          >
            Connect
          </button>
          <label className="delivery-mode" title={deliveryBlock ?? DELIVERY_MODE_HINT}>
            Delivery mode{" "}
            <select
              aria-label="Delivery mode"
              value={deliveryModeStatus?.mode ?? "everyFrame"}
              disabled={!!deliveryBlock}
              onChange={(e) => void onDeliveryModeChange(e.target.value as FrameDeliveryMode)}
            >
              {DELIVERY_MODES.map((m) => <option key={m.value} value={m.value}>{m.label}</option>)}
            </select>
          </label>
          <div className="right">
            <button className="btn" onClick={() => setShowMockConfig(true)} disabled={!!sourceBlock} title={sourceBlock}>
              Configure Mock…
            </button>
          </div>
        </ViewerGate></div>
        {deliveryPending && <p className="path-label">Running with {deliveryPending}; the selected mode applies at the next capture start.</p>}
        {!caps.autofocus && (
          <ViewerGate>
            <label className="auto-select-startup">
              <input type="checkbox" checked={autoSelect} onChange={(e) => onAutoSelectChange(e.target.checked)} />{" "}
              Select the camera automatically at startup
            </label>
          </ViewerGate>
        )}
        <p className="path-label" role="status">
          {connectStatusLine({ scanning, running, actionNote, scanNote, discovery, selection: camSelection })}
        </p>
      </div>

      {/* ---- UX-3 hardware preflight checklist ---- */}
      <div className="preflight-panel">
        <div className="preflight-head">
          <h4>Hardware preflight</h4>
          <span className="preflight-summary">
            {preflight.passed} passed · {preflight.warning} warning · {preflight.failed} failed
            {preflight.notRequired ? ` · ${preflight.notRequired} n/a` : ""}
          </span>
          <button className="btn" onClick={() => void refreshPreflight()} disabled={!ready}>
            Refresh
          </button>
        </div>
        <ul className="preflight-list">
          {preflight.checks.map((c) => (
            <li key={c.id} data-check-id={c.id} className={`preflight-check ${c.status}`}>
              <span className={`check-dot ${c.status}`} aria-hidden="true" />
              <span className="check-main">
                <span className="check-label">
                  {c.label}
                  {c.requirement === "required" && <span className="req-badge">required</span>}
                </span>
                <span className="check-detail">{c.detail}</span>
                {(c.expected || c.detected !== "—") && (
                  <span className="check-identity">
                    {c.expected ? `expected ${c.expected} · ` : ""}
                    detected {c.detected}
                  </span>
                )}
              </span>
              <span
                className="check-status"
                aria-label={`${c.label}: ${CHECK_STATUS_LABEL[c.status]}`}
              >
                {CHECK_STATUS_LABEL[c.status]}
              </span>
              {c.recovery.length > 0 && (
                <span className="check-recovery">
                  {c.recovery.map((a) => (
                    <button
                      key={a.kind}
                      className="btn small"
                      // "No camera selected" rescans the devices as well (App.tsx:1802-1811); the other checks re-poll.
                      onClick={() => void Promise.all([refreshPreflight(), c.id === "camera" ? refreshCameraState() : undefined])}
                      disabled={!ready || (c.id === "camera" && scanning)}
                    >
                      {a.label}
                    </button>
                  ))}
                </span>
              )}
            </li>
          ))}
        </ul>
        <p className={`preflight-foot ${preflight.criticalPassed ? "ok" : "warn"}`}>
          {preflight.criticalPassed
            ? "Required checks pass — confirm readiness in the workflow bar to complete Preflight."
            : "Required checks must pass before Preflight can be confirmed."}
        </p>
      </div>
    </>
  );
}
