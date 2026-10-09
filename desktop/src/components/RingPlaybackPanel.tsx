import { useCallback, useEffect, useRef, useState } from "react";
import type { CmdResult } from "../bridge";
import {
  advancePlayback, capacityText, CELL_REASONS, cellMetrics, clampSeq, DEFAULT_DISPLAY_FPS, DISPLAY_FPS, elapsedText,
  metricText, nextOverlay, noMaskNote, OVERLAY_LABEL, playbackAvailability, rangeText, ringFrameRgba, saveClipState,
  stepSeq, type OverlayMode, type RingFrame, type RingStatus,
} from "../ringPlayback";

// PZ7035 Run stopped to review the buffered frames (#649 v1): the newest N frames with their own mask and cell results,
// scrubbed, stepped and played at a display rate. Nothing is recomputed on the host: the PL's own mask and cells are drawn.

type Props = {
  status: RingStatus;
  fetchFrame: (seq: number) => Promise<RingFrame>;
  onResume: () => Promise<CmdResult>;
  append: (line: string) => void;
  /** Saving a clip needs the SSD path (#667); false until it exists. */
  ssdPathAvailable?: boolean;
};

export function RingPlaybackPanel({ status, fetchFrame, onResume, append, ssdPathAvailable = false }: Props) {
  const availability = playbackAvailability(status);
  const range = { first: status.first_seq, last: status.last_seq };
  const [seq, setSeq] = useState(status.last_seq);
  const [playing, setPlaying] = useState(false);
  const [fps, setFps] = useState<number>(DEFAULT_DISPLAY_FPS);
  const [overlay, setOverlay] = useState<OverlayMode>("both");
  const [frame, setFrame] = useState<RingFrame | null>(null);
  const [first, setFirst] = useState<RingFrame | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const carry = useRef(0);

  // A new ring (resume, then stop again) starts at its newest frame.
  const ringKey = `${status.first_seq}:${status.capacity_frames}`;
  useEffect(() => { setSeq(status.last_seq); setPlaying(false); carry.current = 0; setFirst(null); }, [ringKey]); // eslint-disable-line react-hooks/exhaustive-deps

  // One frame in flight; the latest wanted sequence follows it.
  const wanted = useRef(seq);
  const inFlight = useRef(false);
  const pull = useCallback(() => {
    if (inFlight.current) return;
    inFlight.current = true;
    const target = wanted.current;
    void fetchFrame(target)
      .then((f) => { setFrame(f); setError(""); })
      .catch((e) => setError(String(e).replace(/^Error: /, "")))
      .finally(() => { inFlight.current = false; if (wanted.current !== target) pull(); });
  }, [fetchFrame]);
  useEffect(() => { wanted.current = seq; if (availability.ok) pull(); }, [seq, availability.ok, pull]);
  // The first buffered frame anchors the elapsed time.
  useEffect(() => { if (availability.ok && !first) void fetchFrame(range.first).then(setFirst).catch(() => undefined); }, [availability.ok, first, range.first, fetchFrame]); // eslint-disable-line react-hooks/exhaustive-deps

  // Play at the display rate.
  useEffect(() => {
    if (!playing || !availability.ok) return;
    let last = performance.now();
    const id = window.setInterval(() => {
      const now = performance.now();
      const step = advancePlayback(wanted.current, carry.current, now - last, fps, range);
      last = now;
      carry.current = step.carry;
      if (step.seq !== wanted.current) setSeq(step.seq);
      if (step.ended) setPlaying(false);
    }, 33);
    return () => window.clearInterval(id);
  }, [playing, fps, availability.ok, range.first, range.last]); // eslint-disable-line react-hooks/exhaustive-deps

  // Draw: the frame under the chosen overlay, then the cells' boxes in their own colour.
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !frame) return;
    canvas.width = frame.width;
    canvas.height = frame.height;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.putImageData(new ImageData(ringFrameRgba(frame, overlay), frame.width, frame.height), 0, 0);
    if (overlay !== "off") {
      ctx.lineWidth = 1;
      for (const c of frame.cells) {
        ctx.strokeStyle = c.valid ? "#1a7f37" : "#b42318";
        ctx.strokeRect(c.x + 0.5, c.y + 0.5, Math.max(1, c.width - 1), Math.max(1, c.height - 1));
      }
    }
  }, [frame, overlay]);

  const resume = async () => {
    setBusy(true);
    try {
      const r = await onResume();
      append(r.ok ? r.message : `Resume: ${r.message}`);
    } finally { setBusy(false); }
  };
  const save = saveClipState(ssdPathAvailable);

  return (
    <div className="ring-playback" data-testid="ring-playback">
      <p className="mono" role="status">
        Run stopped · {capacityText(status)}
        {availability.ok && <><br />{rangeText(status)}</>}
      </p>
      {!availability.ok ? (
        <p className="pending-note" role="alert" data-testid="ring-unavailable">{availability.reason}</p>
      ) : (
        <>
          {availability.note && <p className="pending-note" role="status" data-testid="ring-stop-incomplete">{availability.note}</p>}
          <div className="canvas-wrap">
            <canvas ref={canvasRef} className="fit" aria-label="Buffered frame" />
          </div>
          {frame && !frame.maskPresent && <p className="pending-note" role="status">{noMaskNote(frame)}</p>}
          {frame?.resultsTruncated && <p className="pending-note" role="status">More cells than the PL can list: the list is cut.</p>}
          {error && <p className="pending-note" role="alert">{error}</p>}
          <div className="ring-controls" role="group" aria-label="Playback">
            <button onClick={() => { setPlaying(false); setSeq(range.first); }} title="First buffered frame" aria-label="First buffered frame">⏮</button>
            <button onClick={() => { setPlaying(false); setSeq(stepSeq(seq, -1, range)); }} title="Previous frame" aria-label="Previous frame">◀</button>
            <button onClick={() => { if (seq >= range.last) setSeq(range.first); setPlaying((p) => !p); }} aria-label={playing ? "Pause" : "Play"}>{playing ? "⏸" : "▶"}</button>
            <button onClick={() => { setPlaying(false); setSeq(stepSeq(seq, 1, range)); }} title="Next frame" aria-label="Next frame">▶|</button>
            <button onClick={() => { setPlaying(false); setSeq(range.last); }} title="Newest buffered frame" aria-label="Newest buffered frame">⏭</button>
            <input
              type="range" aria-label="Frame" min={range.first} max={range.last} step={1} value={clampSeq(seq, range)}
              onChange={(e) => { setPlaying(false); setSeq(clampSeq(Number(e.target.value), range)); }}
            />
            <label>Display <select aria-label="Display rate" value={fps} onChange={(e) => setFps(Number(e.target.value))}>
              {DISPLAY_FPS.map((f) => <option key={f} value={f}>{f} fps</option>)}
            </select></label>
            <button onClick={() => setOverlay(nextOverlay(overlay))} aria-label="Overlay">Overlay: {OVERLAY_LABEL[overlay]}</button>
          </div>
          <p className="mono" role="status">
            Frame {seq.toLocaleString("en-US")}{frame ? ` · id ${frame.frameId.toLocaleString("en-US")} · ${elapsedText(frame, first)}` : ""}
            {" · "}cells {frame?.cells.length ?? "—"}
          </p>
          {frame && frame.cells.length > 0 && (
            <table className="ring-cells" aria-label="Cells of this frame">
              <thead><tr><th>#</th><th>Result</th><th>Box</th><th>Area (px)</th><th>Deformability</th><th>Area ratio</th><th>E (kPa)</th><th>Brightness</th></tr></thead>
              <tbody>
                {frame.cells.map((c) => {
                  const m = cellMetrics(c);
                  return (
                    <tr key={c.index} className={c.valid ? "valid" : "invalid"}>
                      <td>{m.objectId}</td>
                      <td>{c.valid ? "valid" : CELL_REASONS[m.reason] ?? `reason ${m.reason}`}</td>
                      <td>{c.x},{c.y} {c.width}×{c.height}</td>
                      <td>{metricText(m.hullArea, 1)}</td>
                      <td>{metricText(m.deformability, 3)}</td>
                      <td>{metricText(m.areaRatio, 3)}</td>
                      <td>{metricText(m.youngsModulusKpa, 2)}</td>
                      <td>{metricText(m.brightnessMean, 1)}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          )}
          {frame && frame.cells.length === 0 && <p className="mono" role="status">No cell in this frame.</p>}
        </>
      )}
      <p>
        <button onClick={resume} disabled={busy} title="Re-arm the ring: these buffered frames are discarded">Resume Run (discards these frames)</button>
        {" "}
        <button disabled={!save.enabled} title={save.reason} aria-describedby="ring-save-reason">Save clip</button>
        {!save.enabled && <span id="ring-save-reason" className="pending-note"> {save.reason}</span>}
      </p>
    </div>
  );
}
