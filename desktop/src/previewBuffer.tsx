import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { decimalU64 } from "./framePacket";
export interface PreviewRange { available: boolean; first: string; last: string; count: string; capture_running: boolean }
export interface PreviewSave { ok: boolean; output_path: string; error: string }
export function usePreviewBuffer(ready: boolean, active: boolean, seek: (index: string | null) => void) {
  const [range, setRange] = useState<PreviewRange | null>(null);
  const [index, setIndex] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const pending = useRef(false);
  const [message, setMessage] = useState("");
  const [format, setFormat] = useState("tiff");
  const [fps, setFps] = useState("30");
  useEffect(() => {
    if (!ready) return;
    let disposed = false, polling = false;
    const poll = async () => {
      if (polling) return; polling = true;
      try { const value = await invoke<PreviewRange>("fetch_preview_buffer"); if (!disposed) setRange(value); }
      catch (e) { if (!disposed) setMessage(String(e)); }
      finally { polling = false; }
    };
    void poll(); const timer = window.setInterval(() => void poll(), 500);
    return () => { disposed = true; window.clearInterval(timer); };
  }, [ready]);
  const select = (value: string | null) => {
    try { const exact = value === null ? null : decimalU64(value); setIndex(exact); seek(exact); }
    catch (e) { setMessage(String(e)); }
  };
  const save = async () => {
    if (pending.current || !ready || active || !range?.available || range.capture_running) return;
    pending.current = true; setBusy(true); setMessage("");
    try {
      const root = await open({ directory: true, multiple: false, title: "Save preview buffer into a new folder" });
      if (typeof root !== "string") return;
      const current = await invoke<PreviewRange>("fetch_preview_buffer");
      if (!current.available || current.capture_running) throw new Error("Stop capture and refresh before saving");
      const result = await invoke<PreviewSave>("save_preview_buffer", {request: JSON.stringify({
        output_root: root, first: current.first, last: current.last, format, fps: Number(fps),
      })});
      setMessage(result.ok ? `Saved: ${result.output_path}` : `${result.error}${result.output_path ? ` — partial output: ${result.output_path}` : ""}`);
    } catch (e) { setMessage(`Buffer save failed: ${e}`); }
    finally { pending.current = false; setBusy(false); }
  };
  return { range, index, busy, message, format, setFormat, fps, setFps, select, save };
}
export function PreviewBufferControls({model}: {model: ReturnType<typeof usePreviewBuffer>}) {
  const {range, index, busy} = model;
  const max = range?.available ? BigInt(range.last) - BigInt(range.first) : 0n;
  const offset = range?.available && index !== null ? BigInt(index) - BigInt(range.first) : max;
  const evicted = index !== null && range?.available && (offset < 0n || offset > max);
  return <section aria-label="Preview buffer">
    <div className="toolbar">
      <button onClick={() => model.select(index === null && range?.available ? range.last : null)} disabled={!range?.available}>{index === null ? "Pause preview" : "Follow live"}</button>
      <span>{range?.available ? `${range.count} buffered frames · ${index === null ? "live" : `frame ${index}`}` : "Buffer empty"}</span>
      {evicted && <span role="status">Selected frame was evicted; select a retained frame.</span>}
      <select aria-label="Buffer format" value={model.format} disabled={busy} onChange={e => model.setFormat(e.target.value)}><option value="tiff">TIFF sequence</option><option value="avi">AVI (no per-frame timestamps)</option></select>
      {model.format === "avi" && <label>Playback FPS <input aria-label="AVI playback FPS" type="number" min="0.01" max="1000" value={model.fps} onChange={e => model.setFps(e.target.value)} /></label>}
      <button disabled={busy || !range?.available || range.capture_running} onClick={() => void model.save()} title="Stop capture first; saves the retained range into a new folder without overwriting files">{busy ? "Saving buffer…" : "Save Buffer…"}</button>
    </div>
    <input type="range" className="scrub" aria-label="Preview buffer scrub" min="0" max={max.toString()} value={(offset < 0n ? 0n : offset > max ? max : offset).toString()} disabled={!range?.available || max > BigInt(Number.MAX_SAFE_INTEGER)} onChange={e => model.select((BigInt(range!.first) + BigInt(e.target.value)).toString())} />
    {model.message && <p role="status">{model.message}</p>}
  </section>;
}
