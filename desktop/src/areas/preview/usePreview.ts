// Experiment Preview (epic #865): the live frame pipeline (frame pulls, drawing, display rates),
// the preview loop and the PZ7035 Run preview, frame-ring playback, raw recording, the ROI,
// background state and the preview buffer. Moved from App.tsx without change (#869 wave 0);
// studio/useStudioController.ts calls these hooks in App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, isFrameReplyRace, mono8ToImageData, type FrameMeta, type FramePacket } from "../../bridge";
import { displayedWindow, mirrorOf, RUN_WINDOW } from "../../cameraAlignment";
import { FramePullScheduler } from "../../framePullScheduler";
import { holdBest, measureImage, sameBox } from "../../imageQuality";
import { usePreviewBuffer } from "../../previewBuffer";
import { previewIntervalMs } from "../../previewPacing";
import { runPreviewRgba, type RunPreview } from "../../runPreview";
import { save } from "../../transport/dialogs";
import { H5_FILTER } from "../../studio/constants";
import type { ConfigSyncModel } from "../config/useConfig";
import type { ConnectState } from "../connect/useConnect";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { ReviewState } from "../review/useReview";
import type { ShellState } from "../shell/useShell";

export interface RatesRef {
  windowStartMs: number;
  frames: number;
  bytes: number;
  displayFps: number;
  dataRateMBs: number;
  lastFrameIndex: string | null;
}

export function usePreviewState() {
  const [lastMeta, setLastMeta] = useState<FrameMeta | null>(null);
  const [fitWindow, setFitWindow] = useState(true);
  // Recording.
  const [recording, setRecording] = useState(false);
  const [recPath, setRecPath] = useState("");
  const [autoBackgroundEnabled,setAutoBackgroundEnabled] = useState(false);
  const [previewFpsLimit,setPreviewFpsLimit]=useState(30);
  const [backgroundSet, setBackgroundSet] = useState(false);
  const [roiFields, setRoiFields] = useState({ x: "0", y: "0", w: "0", h: "0" });
  const previewCanvasRef = useRef<HTMLCanvasElement>(null);
  const previewLoopRef = useRef<number | null>(null);
  const framePulls = useRef(new FramePullScheduler());
  const lastMetadataRenderMs = useRef(-Infinity);
  const [runPreviewInfo, setRunPreviewInfo] = useState<{frameId: number; listed: number; cells: number; blemishes: number} | null>(null);
  const [showRunMask, setShowRunMask] = useState(true);
  const showRunMaskRef = useRef(true);
  showRunMaskRef.current = showRunMask;

  // Display-side measurements (frames actually drawn / bytes actually pulled
  // over the last 1s window). These are UI measurements, not backend claims.
  const ratesRef = useRef<RatesRef>({
    windowStartMs: performance.now(),
    frames: 0,
    bytes: 0,
    displayFps: 0,
    dataRateMBs: 0,
    lastFrameIndex: null,
  });
  const [, setRatesTick] = useState(0);
  const previewFollowing = useRef(true);
  return {
    lastMeta, setLastMeta, fitWindow, setFitWindow, recording, setRecording, recPath, setRecPath,
    autoBackgroundEnabled, setAutoBackgroundEnabled, previewFpsLimit, setPreviewFpsLimit, backgroundSet, setBackgroundSet,
    roiFields, setRoiFields, previewCanvasRef, previewLoopRef, framePulls, lastMetadataRenderMs,
    runPreviewInfo, setRunPreviewInfo, showRunMask, setShowRunMask, showRunMaskRef, ratesRef, setRatesTick, previewFollowing,
  };
}
export type PreviewState = ReturnType<typeof usePreviewState>;

type LiveFramesDeps = Pick<ShellState, "tab" | "tabRef" | "instrumentModesRef" | "append">
  & Pick<PreviewState, "previewCanvasRef" | "framePulls" | "lastMetadataRenderMs" | "setLastMeta" | "ratesRef" | "setRatesTick"
    | "previewLoopRef">
  & Pick<ConnectState, "liveCanvasRef" | "cameraGeometryRef" | "cameraWindowRef" | "alignBestRef" | "alignImageMs" | "setAlignImage"
    | "camStatus" | "roiOverlayRef">
  & Pick<ReviewState, "reviewCanvasRef" | "reviewPath">;

// Frame pulls: draw each pull response on its canvas, measure the display rates, retire stale replies.
export function useLiveFrames({
  tab, tabRef, instrumentModesRef, append, previewCanvasRef, framePulls, lastMetadataRenderMs, setLastMeta, ratesRef,
  setRatesTick, previewLoopRef, liveCanvasRef, cameraGeometryRef, cameraWindowRef, alignBestRef, alignImageMs, setAlignImage,
  camStatus, reviewCanvasRef, reviewPath, roiOverlayRef,
}: LiveFramesDeps) {
  const activeLiveCanvas = useCallback((): HTMLCanvasElement | null => {
    if (tabRef.current === "experiment") return previewCanvasRef.current;
    return liveCanvasRef.current;
  }, []);

  // Draw only the pixels owned by this exact immutable pull response.
  const draw = useCallback(
    (meta: FramePacket, canvas: HTMLCanvasElement | null) => {
      if (!meta.valid || !canvas) return;
      const bytes = meta.data;
      canvas.width = meta.width;
      canvas.height = meta.height;
      const ctx = canvas.getContext("2d");
      if (!ctx) return;
      ctx.putImageData(mono8ToImageData(bytes, meta.width, meta.height, meta.stride_bytes), 0, 0);
      // Camera & Alignment: the experiment window over a full-sensor image.
      const geometry = cameraGeometryRef.current, experimentWindow = cameraWindowRef.current;
      // An eGrabber Overview frame is whatever the Overview script yields (its window starts at 0, 0), so it
      // is not compared with the sensor size the camera reports (#863).
      // The box is shown only while the Overview's "ROI Overlay" is on, and mirrored for a MindVision camera whose profile flips the
      // image (#863 CONNECT-047/049; the stored window stays in sensor coordinates).
      if (canvas === liveCanvasRef.current && geometry?.overview && experimentWindow && roiOverlayRef.current &&
          (geometry.camera === "egrabber" || (meta.width === geometry.sensor_width && meta.height === geometry.sensor_height))) {
        const box = displayedWindow(experimentWindow, { width: meta.width, height: meta.height }, mirrorOf(geometry));
        ctx.strokeStyle = "#ffd400";
        ctx.lineWidth = 2;
        ctx.strokeRect(box.x + 1, box.y + 1, box.width - 2, box.height - 2);
      }
      // PZ7035 Align: focus number and brightness of the whole frame's Run window.
      if (instrumentModesRef.current && canvas === liveCanvasRef.current && cameraWindowRef.current &&
          meta.width === RUN_WINDOW.sensorWidth && meta.height === RUN_WINDOW.sensorHeight) {
        const box = cameraWindowRef.current;
        const metrics = measureImage(bytes, meta.width, meta.height, meta.stride_bytes, box);
        if (metrics) {
          const held = alignBestRef.current;
          if (!sameBox(held.box, box)) { held.best = null; held.box = { ...box }; }
          held.best = holdBest(held.best, metrics.focus);
          if (performance.now() - alignImageMs.current >= 200) {
            alignImageMs.current = performance.now();
            setAlignImage({ metrics, best: held.best });
          }
        }
      }
      const { data: _pixels, ...metadata } = meta;
      if (canvas !== reviewCanvasRef.current && performance.now() - lastMetadataRenderMs.current >= 200) {
        lastMetadataRenderMs.current = performance.now();
        setLastMeta(metadata);
      }
      const r = ratesRef.current;
      r.frames += 1;
      r.bytes += meta.byte_len;
      r.lastFrameIndex = meta.frame_index;
      const now = performance.now();
      const elapsed = now - r.windowStartMs;
      if (elapsed >= 1000) {
        r.displayFps = (r.frames * 1000) / elapsed;
        r.dataRateMBs = (r.bytes * 1000) / elapsed / (1024 * 1024);
        r.frames = 0;
        r.bytes = 0;
        r.windowStartMs = now;
        setRatesTick((t) => t + 1);
      }
    },
    [],
  );

  useEffect(() => {
    const scheduler = framePulls.current;
    // A pull discarded by this client while the source changes (opening a recording in Review) is
    // not an error; the next pull is unaffected (#763 F20).
    const live = scheduler.mount("live", p => draw(p, activeLiveCanvas()), e => { if (!isFrameReplyRace(e)) append(`frame error: ${e}`); });
    const review = scheduler.mount("review", p => draw(p, reviewCanvasRef.current), e => { if (!isFrameReplyRace(e)) append(`review frame error: ${e}`); });
    return () => { live(); review(); };
  }, [draw, activeLiveCanvas, append]);

  // Navigation/config/source changes retire presentation replies only.
  useEffect(() => { framePulls.current.invalidate(); }, [tab, reviewPath, camStatus]);

  const stopLoop = useCallback(() => {
    framePulls.current.invalidate("live");
    if (previewLoopRef.current !== null) {
      window.clearInterval(previewLoopRef.current);
      previewLoopRef.current = null;
    }

  }, []);

  useEffect(() => () => stopLoop(), [stopLoop]);
  return { activeLiveCanvas, draw, stopLoop };
}
export type LiveFramesModel = ReturnType<typeof useLiveFrames>;

type LivePreviewDeps = Pick<ShellState, "ready" | "tab" | "append" | "runMode" | "runFrozen" | "setInstrument">
  & Pick<ConnectState, "running">
  & Pick<PreviewState, "framePulls" | "previewLoopRef" | "previewFpsLimit" | "previewCanvasRef" | "showRunMaskRef"
    | "setRunPreviewInfo" | "previewFollowing" | "roiFields" | "recording" | "setRecording" | "recPath" | "setRecPath">
  & Pick<LiveFramesModel, "stopLoop"> & Pick<ConfigSyncModel, "refreshConfig">;

// The live preview loop, the PZ7035 Run preview, the ROI, raw recording and frame-ring Stop/Resume.
export function useLivePreview({
  ready, tab, append, runMode, runFrozen, setInstrument, running, framePulls, previewLoopRef, previewFpsLimit, previewCanvasRef,
  showRunMaskRef, setRunPreviewInfo, previewFollowing, roiFields, recording, setRecording, recPath, setRecPath, stopLoop,
  refreshConfig,
}: LivePreviewDeps) {
  const onApplyRoi = useCallback(async () => {
    try {
      const res = await bridge.setProcessingRoi(
        Number(roiFields.x) || 0,
        Number(roiFields.y) || 0,
        Number(roiFields.w) || 0,
        Number(roiFields.h) || 0,
      );
      if (!res.ok) {
        append(`ROI apply failed: ${res.message}`);
        await refreshConfig();
        return;
      }
      append(`ROI set to ${roiFields.w}×${roiFields.h} @ (${roiFields.x}, ${roiFields.y})`);
      await refreshConfig();
    } catch (e) {
      append(`ROI error: ${e}`);
      await refreshConfig();
    }
  }, [roiFields, append, refreshConfig]);

  const seekPreview = useCallback((index: string | null) => {
    previewFollowing.current = index === null;
    framePulls.current.invalidate("live");
    framePulls.current.request("live", index === null ? bridge.fetchFrame : () => bridge.fetchFrameByIndex(index));
  }, []);

  useEffect(() => {
    if (!ready || !running) return;
    previewLoopRef.current = window.setInterval(() => {
      if (previewFollowing.current) framePulls.current.request("live", bridge.fetchFrame, false);
    }, previewIntervalMs(previewFpsLimit));
    return stopLoop;
  }, [ready, running, stopLoop, previewFpsLimit]);

  // PZ7035 Run (#501 P1): the producer is stopped; the preview is the PL cell capture with the
  // U-Net mask and the listed cells of the same frame.
  const drawRunPreview = useCallback((p: RunPreview) => {
    const canvas = previewCanvasRef.current;
    if (!canvas) return;
    canvas.width = p.width;
    canvas.height = p.height;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.putImageData(new ImageData(runPreviewRgba(p, showRunMaskRef.current), p.width, p.height), 0, 0);
    ctx.lineWidth = 1;
    for (const c of p.list) {
      ctx.strokeStyle = c.valid ? "#1a7f37" : "#b42318";
      ctx.strokeRect(c.x + 0.5, c.y + 0.5, Math.max(1, c.width - 1), Math.max(1, c.height - 1));
    }
    setRunPreviewInfo({frameId: p.frameId, listed: p.list.length, cells: p.cells, blemishes: p.blemishes});
  }, []);

  useEffect(() => {
    if (!ready || !runMode || runFrozen || tab !== "experiment") { setRunPreviewInfo(null); return; }
    let busy = false, live = true;
    const id = window.setInterval(() => {
      if (busy) return;
      busy = true;
      void bridge.fetchRunPreview().then((p) => { if (live) drawRunPreview(p); })
        .catch(() => { /* a capture can time out; the next one follows */ })
        .finally(() => { busy = false; });
    }, 100);
    return () => { live = false; window.clearInterval(id); };
  }, [ready, runMode, runFrozen, tab, drawRunPreview]);

  // Stop in Run holds the frame ring for playback; resume re-arms it (a new ring).
  const refreshInstrument = useCallback(() => { void bridge.fetchInstrumentStatus().then(setInstrument).catch(() => undefined); }, []);
  const onStopRun = useCallback(async () => {
    try {
      const r = await bridge.ringFreeze();
      append(r.ok ? r.message : `Stop: ${r.message}`);
    } catch (e) {
      append(`Stop: ${String(e).replace(/^Error: /, "")}`); // a viewer (not the controller) or a lost connection
    }
    refreshInstrument();
  }, [append, refreshInstrument]);
  const onResumeRun = useCallback(async () => {
    try {
      return await bridge.ringResume();
    } finally {
      refreshInstrument();
    }
  }, [refreshInstrument]);

  const resumeFromBanner = useCallback(async () => {
    try {
      const r = await onResumeRun();
      if (r && !r.ok) append(`Resume Run: ${r.message}`);
    } catch (e) {
      append(`Resume Run: ${e}`);
    }
  }, [onResumeRun, append]);

  // ---- Recording (Experiment ▸ Preview toolbar) ----

  const onToggleRecord = useCallback(async () => {
    try {
      if (!recording) {
        const picked = await save({ title: "Recording output", filters: H5_FILTER, defaultPath: recPath || "clip.h5" });
        if (!picked) return;
        setRecPath(picked);
        const res = await bridge.startRecording(picked);
        if (!res.ok) return append(`record failed: ${res.message}`);
        setRecording(true);
        append(`recording → ${picked}`);
      } else {
        const stopped = await bridge.stopRecording();
        if (!stopped.ok) return append(`stop recording failed: ${stopped.message}`);
        setRecording(false);
        append("recording stopped");
      }
    } catch (e) {
      append(`record error: ${e}`);
    }
  }, [recording, recPath, append]);
  return {
    onApplyRoi, seekPreview, drawRunPreview, refreshInstrument, onStopRun, onResumeRun, resumeFromBanner, onToggleRecord,
  };
}
export type LivePreviewModel = ReturnType<typeof useLivePreview>;

type PreviewBufferDeps = Pick<ShellState, "ready"> & Pick<MonitoringState, "expActive">
  & Pick<LivePreviewModel, "seekPreview"> & Pick<ConfigSyncModel, "refreshConfig">;

export function usePreviewBufferModel({ ready, expActive, seekPreview, refreshConfig }: PreviewBufferDeps) {
  const previewBuffer = usePreviewBuffer(ready, expActive, seekPreview, refreshConfig);
  return { previewBuffer };
}
export type PreviewBufferModel = ReturnType<typeof usePreviewBufferModel>;
