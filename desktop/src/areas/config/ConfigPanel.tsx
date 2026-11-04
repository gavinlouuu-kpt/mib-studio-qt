// Configuration (epic #864): the configuration sub-tabs under Experiment ▸ Preview: App config
// (processing cores, profiles, the checked config document, the live config.json and the quick
// processing controls) and the camera script / camera documents. Moved from App.tsx without change (#869 wave 0).
import { CameraDocumentEditor } from "../../cameraDocument";
import { CameraScriptControls } from "../../cameraScript";
import { ViewerGate, viewerReason } from "../../components/ViewerGate";
import { ConfigDocumentEditor } from "../../configDocument";
import { CoreManagementPanel } from "../../coreManagement";
import { metricNumber } from "../../metricFormat";
import { PL_IGNORED_SETTINGS_TEXT } from "../../platformCapabilities";
import { ProfilesPanel } from "../../profiles";
import { confirm } from "../../transport/dialogs";
import { useStudio } from "../../studio/StudioContext";

export function ConfigPanel() {
  const {
    configTab, setConfigTab, caps, instrumentModes, configDirty, quickDraft, refreshConfig, ready, onApplyConfigJson,
    liveDraft, viewerOnly, hostProcessing, coreStatus, cores, running, recording, expActive, scriptDocument,
    mindvisionDocument, reviewSourceBusy, experimentRequestBusy, cameraScript, checkedConfig, profiles, reviewExport,
    reanalysis, previewBuffer, configText, procEnabled, pixelToMicron, onApplyProcessing, stats, backgroundSet,
    pzCameraDocument,
  } = useStudio();
  return (
    <>
      <div className="subtabs" style={{ marginTop: 8 }} role="tablist" aria-label="Configuration">
        <button className={configTab === "app" ? "active" : ""} onClick={() => setConfigTab("app")}>
          App config (config.json)
        </button>
        {caps.egrabber_script && <button className={configTab === "script" ? "active" : ""} onClick={() => setConfigTab("script")}>
          Camera script
        </button>}
        {instrumentModes && <button className={configTab === "script" ? "active" : ""} onClick={() => setConfigTab("script")}>
          Camera document
        </button>}
      </div>
      <div className="subtab-body">
        {configTab === "app" && (
          <>
            <div className="toolbar">
              <button onClick={async()=>{if((!configDirty&&!quickDraft.dirty)||await confirm("Discard unsaved live configuration edits and reload?"))void refreshConfig(true);}} disabled={!ready} title="Reload the live config from the backend">
                Reload
              </button>
              <button className="btn" onClick={onApplyConfigJson} disabled={!ready || !configDirty || liveDraft.runtimeChanged || viewerOnly} title={viewerReason(viewerOnly, configDirty ? "Merge-apply the edited document" : "No edits to apply")}>
                Apply
              </button>
              {hostProcessing && <span className="mono right" title="Active processing core identity (backend-owned trust)">
                core {coreStatus?.valid ? `v${coreStatus.active_version} (${coreStatus.source})` : "—"}
                {coreStatus?.valid && !coreStatus.pin_satisfied
                  ? ` · PIN NOT SATISFIED (requires ${coreStatus.required_version})`
                  : ""}
              </span>}
            </div>
            <ViewerGate>
            {caps.core_updates && <CoreManagementPanel model={cores} updatesBlocked={running || recording || expActive || scriptDocument.busy || mindvisionDocument.busy || reviewSourceBusy || experimentRequestBusy || cameraScript.busy || checkedConfig.busy || profiles.busy || profiles.remote.busy || reviewExport.busy || reanalysis.busy || previewBuffer.busy || scriptDocument.dirty || mindvisionDocument.dirty || configDirty || quickDraft.dirty || checkedConfig.dirty || profiles.dirty} />}
            <ProfilesPanel model={profiles} egrabberScript={caps.egrabber_script} autofocus={caps.autofocus} />
            <ConfigDocumentEditor model={checkedConfig} />
            </ViewerGate>
            <div className="config-grid">
              <div className="config-group" style={{ flex: 2 }}>
                <h5>Live config document (merge-applied on Apply)</h5>
                {liveDraft.runtimeChanged&&configDirty&&<p role="status">Runtime configuration changed; your draft is preserved. Reload and reconcile before applying a stale snapshot.</p>}
                {!hostProcessing && <p className="pending-note" data-testid="pl-ignored-settings">
                  The PL runs the science on this instrument. It does not implement {PL_IGNORED_SETTINGS_TEXT}:
                  changing them has no effect (Experiment readiness lists any you changed).
                </p>}
                <textarea
                  className="script-editor"
                  style={{ minHeight: 160 }}
                  value={configText}
                  onChange={(e) => {
                    liveDraft.edit(e.target.value);
                  }}
                  aria-label="Processing configuration JSON"
                />
              </div>
              {/* px→µm is the calibration of every run (Review multiplies area by it), so it stays on the
                  PL instrument; the host's realtime switch, stats and background are host-only (#651 G10). */}
              <div className="config-group">
                <h5>{hostProcessing ? "realtime_processing" : "pixel size"}</h5>
                {hostProcessing && <div className="row">
                  <label>
                    <input
                      type="checkbox"
                      checked={procEnabled}
                      onChange={(e) => quickDraft.edit(JSON.stringify({enabled:e.target.checked,factor:pixelToMicron}))}
                    />{" "}
                    realtime processing
                  </label>
                </div>}
                <div className="row">
                  <label>
                    px→µm{" "}
                    <input
                      type="text"
                      style={{ width: 70 }}
                      value={pixelToMicron}
                      onChange={(e) => quickDraft.edit(JSON.stringify({enabled:procEnabled,factor:e.target.value}))}
                    />
                  </label>
                  <button className="btn" onClick={onApplyProcessing} disabled={!ready || quickDraft.runtimeChanged || viewerOnly} title={viewerReason(viewerOnly, ready ? undefined : "Backend is not initialized")}>
                    Apply
                  </button>
                </div>
                {quickDraft.runtimeChanged && <p role="status">Runtime processing controls changed; your edits are preserved. Use Reload above, then reconcile your changes.</p>}
                {hostProcessing && stats?.valid && (
                  <p className="mono">
                    algo {metricNumber(stats.algo_fps1s)} · valid {metricNumber(stats.valid_fps1s)} · invalid{" "}
                    {metricNumber(stats.invalid_fps1s)} fps · px→µm {metricNumber(stats.pixel_to_micron, 5, true)}
                  </p>
                )}
                {hostProcessing && <p className="mono">background: {backgroundSet ? "set" : "not set"}</p>}
              </div>
            </div>
          </>
        )}
        {configTab === "script" && instrumentModes && <ViewerGate><CameraDocumentEditor model={pzCameraDocument}/></ViewerGate>}
        {configTab === "script" && caps.egrabber_script && <ViewerGate><CameraScriptControls model={cameraScript} /><CameraDocumentEditor model={scriptDocument}/><CameraDocumentEditor model={mindvisionDocument}/></ViewerGate>}
      </div>
    </>
  );
}
