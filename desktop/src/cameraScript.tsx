import { useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { bridge, type CameraSelection } from "./bridge";
import { CAMERA_SELECTION_MODES } from "./bridgeContract";

export interface CameraScriptContext {
  ready: boolean;
  running: boolean;
  experimentActive: boolean;
  selection: CameraSelection | null;
  append: (message: string) => void;
  refresh: () => Promise<void>;
}

export function cameraScriptBlock(ctx: Pick<CameraScriptContext, "ready" | "running" | "experimentActive" | "selection">): string {
  if (!ctx.ready) return "Backend is not initialized.";
  if (ctx.experimentActive) return "Stop and finalize the experiment before changing camera settings.";
  if (ctx.running || ctx.selection?.running) return "Stop the camera before applying a script or resetting it.";
  if (!ctx.selection?.valid || !ctx.selection.configured || ctx.selection.mode !== CAMERA_SELECTION_MODES.Hardware)
    return "Select an EGrabber hardware camera. MindVision uses the JSON editor below.";
  return "";
}

// Owned by App, not the visible tab: switching views cannot release an in-flight
// native operation or discard the chosen script. Never applies on file selection.
export function useCameraScript(ctx: CameraScriptContext) {
  const [path, setPath] = useState("");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const pending = useRef(false);
  const current = useRef(ctx);
  current.current = ctx;
  const run = async (action: "browse" | "apply" | "reset") => {
    if (pending.current) return;
    const blocked = cameraScriptBlock(current.current);
    if (blocked) { setMessage(blocked); return; }
    if (action === "apply" && !path) return;
    pending.current = true;
    setBusy(true);
    try {
      if (action === "browse") {
        const picked = await open({ multiple: false, directory: false, filters: [{ name: "EGrabber camera script", extensions: ["js"] }] });
        if (typeof picked === "string") { setPath(picked); setMessage("Script selected; not applied."); }
        return;
      }
      // Re-read authoritative selection before a device mutation: UI state may
      // have become stale while an IPC command was queued.
      const selection = await bridge.fetchCameraSelection();
      const reason = cameraScriptBlock({ ...current.current, selection });
      if (reason) { setMessage(reason); return; }
      const original = current.current.selection;
      if (selection.interface_index !== original?.interface_index || selection.device_index !== original?.device_index) {
        setMessage("Camera selection changed. Refresh and retry.");
        return;
      }
      const result = action === "apply" ? await bridge.applyCameraScript(path) : await bridge.resetHardwareCamera();
      const text = `${action === "apply" ? "Camera script" : "Camera reset"}: ${result.message}`;
      setMessage(text);
      current.current.append(text);
      await current.current.refresh();
    } catch (error) {
      const text = `Camera ${action} failed: ${String(error)}`;
      setMessage(text);
      current.current.append(text);
    } finally { pending.current = false; setBusy(false); }
  };
  return { path, busy, message, reason: cameraScriptBlock(ctx), run, clear: () => { if (!pending.current) { setPath(""); setMessage(""); } } };
}

export function CameraScriptControls({ model }: { model: ReturnType<typeof useCameraScript> }) {
  return <section aria-label="Camera script controls">
    <div className="toolbar">
      <button disabled={model.busy || !!model.reason} title={model.reason} onClick={() => void model.run("browse")}>Browse Script…</button>
      <button disabled={model.busy || !!model.reason || !model.path} title={model.reason} onClick={() => void model.run("apply")}>Apply to Camera</button>
      <button disabled={model.busy || !!model.reason} title={model.reason} onClick={() => void model.run("reset")}>Reset Camera</button>
      <button disabled={model.busy || !model.path} onClick={model.clear}>Clear Selection</button>
    </div>
    <p className="mono">{model.path || "No script selected"}</p>
    <p role="status">{model.busy ? "Camera operation in progress…" : model.message || model.reason}</p>
    <p>Choose an existing EGrabber JavaScript file, or use the checked file editor below to edit and save.</p>
  </section>;
}
