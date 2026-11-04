// Connect and Overview (epic #863): the Connect stage, device sources and the UX-3 hardware preflight
// checklist. Moved from App.tsx without change (#869 wave 0).
import { ViewerGate } from "../../components/ViewerGate";
import { CHECK_STATUS_LABEL } from "../../preflight";
import { useStudio } from "../../studio/StudioContext";

export function ConnectTab() {
  const {
    connectTab, setConnectTab, pz7035, caps, discovery, pickedDevice, setPickedDevice, refreshCameraState, ready,
    onConnectPicked, cameraScript, running, expActive, setShowMockConfig, camSelection, preflight, refreshPreflight,
  } = useStudio();
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
        <div className="devices-list" role="listbox" aria-label="Discovered devices">
          {connectTab !== "framegrabbers" &&
            (discovery?.cameras ?? [])
              .filter((c) => (connectTab === "mindvision" ? c.camera_type === 1 : c.camera_type !== 1))
              .map((c) => {
                const key = `${c.camera_type}:${c.interface_index}:${c.device_index}:${c.camera_index}`;
                return (
                  <div key={key} style={{ padding: "2px 0" }}>
                    <label>
                      <input
                        type="radio"
                        name="device"
                        checked={pickedDevice === key}
                        onChange={() => setPickedDevice(key)}
                      />{" "}
                      {c.label}
                    </label>
                  </div>
                );
              })}
          {connectTab === "framegrabbers" &&
            (discovery?.framegrabbers ?? []).map((g) => (
              <div key={`${g.interface_index}:${g.device_index}:${g.stream_index}`} style={{ padding: "2px 0" }}>
                {g.label}
              </div>
            ))}
          {connectTab === "mindvision" && (discovery?.cameras ?? []).every((c) => c.camera_type !== 1) && (
            <div>No MindVision cameras found (SDK/hardware required).</div>
          )}
          {connectTab === "framegrabbers" && (discovery?.framegrabbers ?? []).length === 0 && (
            <div>No framegrabbers found (EGrabber SDK/hardware required).</div>
          )}
        </div>
        <div className="toolbar" style={{ marginTop: 8 }}><ViewerGate>
          <button onClick={refreshCameraState} disabled={!ready}>Refresh</button>
          <button
            onClick={onConnectPicked}
            disabled={!ready || !pickedDevice || cameraScript.busy || running || expActive}
            title={pickedDevice ? undefined : "Pick a device first"}
          >
            Connect
          </button>
          <div className="right">
            <button className="btn" onClick={() => setShowMockConfig(true)} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
              Configure Mock…
            </button>
          </div>
        </ViewerGate></div>
        <p className="path-label">
          Found {(discovery?.framegrabbers ?? []).length} framegrabber(s),{" "}
          {(discovery?.cameras ?? []).filter((c) => c.camera_type === 0).length} eGrabber camera(s),{" "}
          {(discovery?.cameras ?? []).filter((c) => c.camera_type === 1).length} MindVision camera(s)
          {camSelection?.configured
            ? ` · selected: ${
                camSelection.mode === 1
                  ? `mock (${camSelection.mock_frame_dir || "no folder"})`
                  : camSelection.label || "camera"
              }`
            : " · no camera selected"}
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
                      onClick={() => void refreshPreflight()}
                      disabled={!ready}
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
