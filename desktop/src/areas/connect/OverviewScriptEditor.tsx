// Connect and Overview (epic #863, CONNECT-056..061): the Overview's overviewConfig.js editor: text area, path label and
// "unsaved changes" label, Reset / Save / Apply to Camera / Browse... / Clear (Qt resources/ui/OverviewTab.ui). Hidden for
// a MindVision camera, as Qt hides the whole script area.
import type { OverviewScriptModel } from "./useOverviewScript";
import { MSG_UNSAVED } from "./overviewScript";

export function OverviewScriptEditor({ model: m }: { model: OverviewScriptModel }) {
  if (!m.visible) return null;
  const off = m.busy;
  return (
    <section aria-label="Overview camera script" className="overview-script">
      <h5>Camera script (overviewConfig.js)</h5>
      <div className="toolbar">
        <button disabled={off} onClick={() => void m.reset()} title="Reload the script from its file; the default file is made from the bundled one when it is missing">Reset</button>
        <button disabled={off || !m.loaded} onClick={() => void m.save()} title="Write the script to its file">Save</button>
        <button disabled={off || !m.loaded || !!m.applyBlock} onClick={() => void m.apply()}
          title={m.applyBlock || "Save, then apply the script to the camera (the full sensor). A running camera is stopped and stays stopped."}>
          Apply to Camera
        </button>
        <button disabled={off} onClick={() => void m.browse()} title="Choose another script file; the choice is kept">Browse...</button>
        <button disabled={off} onClick={() => void m.clear()} title="Forget the chosen file and use the default one again">Clear</button>
        <span className="mono overview-script-path" data-testid="overview-script-path" title={m.shownPath}>{m.shownPath || "No script path yet"}</span>
        {m.dirty && <span className="overview-script-unsaved" data-testid="overview-script-unsaved">{MSG_UNSAVED}</span>}
      </div>
      <textarea
        className="script-editor" aria-label="Overview camera script contents" wrap="off" spellCheck={false}
        disabled={off || !m.loaded} value={m.text} onChange={(e) => m.edit(e.target.value)}
      />
      <p role={m.message?.level === "error" ? "alert" : "status"} className={`overview-script-message ${m.message?.level ?? ""}`} data-testid="overview-script-message">
        {m.busy ? "Working…" : m.message?.text ?? ""}
      </p>
    </section>
  );
}
