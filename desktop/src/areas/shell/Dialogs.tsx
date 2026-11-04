// Shell (epic #862): Central Methods, offline help, Diagnostics and About.
// Moved from App.tsx without change (#869 wave 0).
import { CentralMethodsPanel } from "../../CentralMethodsPanel";
import { DiagnosticsPanel } from "../../components/DiagnosticsPanel";
import { volatileDataNotice } from "../../diagnosticsView";
import { OfflineHelp } from "../../OfflineHelp";
import { stripView } from "../../ssdView";
import { isRemote } from "../../transport";
import { useStudio } from "../../studio/StudioContext";

export function ShellDialogs() {
  const {
    showCentralMethods, setShowCentralMethods, append, offlineHelp, helpVersion, setOfflineHelp, showDiagnostics,
    setShowDiagnostics, instrument, faultNote, ssd, showAbout, setShowAbout, brand, abi, ready,
  } = useStudio();
  return (
    <>
      {/* ---- Central Methods (registry, #398) ---- */}
      {showCentralMethods && (
        <CentralMethodsPanel onClose={() => setShowCentralMethods(false)} onError={append} />
      )}

      {offlineHelp && <OfflineHelp kind={offlineHelp} version={helpVersion} close={() => setOfflineHelp(null)} />}
      {/* ---- About modal ---- */}
      {showDiagnostics && isRemote && (
        <div className="modal-backdrop" onClick={() => setShowDiagnostics(false)}>
          <div className="modal" style={{ maxWidth: 900, width: "90vw" }} onClick={(e) => e.stopPropagation()} role="dialog" aria-label="Diagnostics">
            <h3>Diagnostics</h3>
            <DiagnosticsPanel volatileNotice={volatileDataNotice(instrument?.storage)} instrumentLines={[
              ...(instrument?.core ? [`PL build ${instrument.core.build_id.slice(0, 8)}, weights ${instrument.core.profile_id.slice(0, 8)}, ABI ${instrument.core.abi_version}, expected ${instrument.core.expected?.image ?? "?"} (${instrument.core.build_match})`] : []),
              ...(instrument?.sensor ? [`sensor ${instrument.sensor.width}x${instrument.sensor.height} at ${instrument.sensor.fps.toFixed(1)} fps`] : []),
              ...(instrument?.storage ? [`storage ${instrument.storage.path}${instrument.storage.ram ? " (RAM)" : ""}, ${Math.round((instrument.storage.free_bytes ?? 0) / 1e6)} MB free`] : []),
              ...(faultNote ? [faultNote] : []),
              ...(ssd ? [`SSD: ${stripView(ssd).text}`] : []),
            ]} />
            <div className="actions">
              <button className="btn" onClick={() => setShowDiagnostics(false)}>Close</button>
            </div>
          </div>
        </div>
      )}
      {showAbout && (
        <div className="modal-backdrop" onClick={() => setShowAbout(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()} role="dialog" aria-label="About">
            <h3>{brand} (React + Tauri)</h3>
            <p>
              Desktop shell for the Qt-free MIB backend (epic #246).
              <br />
              Bridge ABI version: {abi ?? "unknown"} · backend {ready ? "initialized" : "not initialized"}.
            </p>
            <div className="actions">
              <button className="btn" onClick={() => setShowAbout(false)}>Close</button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}
