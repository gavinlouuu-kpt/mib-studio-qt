// The background preview at the top of the side panel (epic #866: MON-102). Qt's BackgroundPreviewWidget shows
// "Background: WxH" over a scaled image, or "No background set". The image is pulled from the backend
// (fetch_background_packet) when a background appears and again when the processing configuration was re-read
// with one (`revision`, bumped by useConfigSync after Set Background): never on a timer and never per frame.
import { useEffect, useRef, useState } from "react";
import { bridge, isFrameReplyRace, mono8ToImageData } from "../../bridge";

export function BackgroundThumbnail({ backgroundSet, revision, ready }: { backgroundSet: boolean; revision: number; ready: boolean }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState<{ w: number; h: number } | null>(null);
  const [problem, setProblem] = useState("");

  useEffect(() => {
    if (!ready || !backgroundSet) { setSize(null); setProblem(""); return; }
    let live = true;
    void bridge.fetchBackground().then((packet) => {
      if (!live) return;
      const el = canvas.current;
      const ctx = el?.getContext("2d");
      if (!packet.valid || !el || !ctx) { setSize(null); setProblem(packet.valid ? "" : "The backend holds no background image."); return; }
      // The pixels are drawn once at full size; CSS scales the canvas to the panel.
      el.width = packet.width; el.height = packet.height;
      ctx.putImageData(mono8ToImageData(packet.data, packet.width, packet.height, packet.stride_bytes), 0, 0);
      setSize({ w: packet.width, h: packet.height }); setProblem("");
    }).catch((e) => {
      // A pull this client discarded itself (a source change was in flight) is not a failure: the next revision fetches again.
      if (live && !isFrameReplyRace(e)) { setSize(null); setProblem(`The background preview could not be read: ${e}`); }
    });
    return () => { live = false; };
  }, [ready, backgroundSet, revision]);

  const label = !backgroundSet ? "No background set" : size ? `Background: ${size.w}x${size.h}` : "Background set";
  return (
    <div className="bg-preview bg-thumbnail" data-tour="monitoring-background-thumbnail" title={problem || "Processing background (set/clear in Experiment ▸ Preview)"}>
      <span className="bg-thumbnail-label">{label}</span>
      <canvas ref={canvas} className="bg-thumbnail-canvas" aria-label="Background image preview" hidden={!backgroundSet || !size} />
      {problem && <span className="bg-thumbnail-problem" role="status">{problem}</span>}
    </div>
  );
}
