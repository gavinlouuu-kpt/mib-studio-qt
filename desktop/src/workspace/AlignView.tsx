// O2 Align (P3, track A step A3): the persistent live image with fit / 1:1 / zoom / pan, a
// pixel probe, a histogram and a line profile, beside typed quick controls (requested →
// readback). The image is the classic preview's own stream: the shell hands every frame it
// already pulled to one sink here, so there is no second capture path. Inspection is bounded and
// runs off the delivery path (FrameInspector). Opening this view sends no command: the camera
// mode switch the classic tab does on entry is a button here, and writes go out only on Write.
import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent, type PointerEvent as ReactPointerEvent, type ReactNode } from "react";
import { Button, Card, DataTable, Notice, StateChip, Toolbar, type DataTableColumn } from "@mib/ui";
import { mono8ToImageData } from "../bridge";
import type { FramePacket } from "../framePacket";
import {
  alignMode,
  readbackRows,
  roiOverlay,
  roiRow,
  type AlignFacts,
  type ReadbackRow,
  type RoiKind,
  type RoiRow,
} from "./alignControls";
import {
  FrameInspector,
  histogramPath,
  histogramSummary,
  panBy,
  probeAt,
  profilePoints,
  screenToImage,
  transformFor,
  ZOOM_STEP,
  zoomAt,
  type InspectFrame,
  type Inspection,
  type Point,
  type ProfileAxis,
  type Size,
  type ViewMode,
  type ViewTransform,
} from "./alignImageModel";
import {
  checkRect,
  controlStatus,
  RECT_KEYS,
  rectDraft,
  rectEqual,
  rectText,
  type Codec,
  type Rect,
  type RectDraft,
  type WriteResult,
} from "./quickControlModel";
import { useQuickControl } from "./useQuickControl";

/** Takes a delivered packet; true when the view shows it (false while frozen or invalid). */
export type FrameSink = (packet: FramePacket) => boolean;

export interface AlignViewProps {
  facts: AlignFacts;
  /** Register the one sink for the frames the classic preview pulls (null to stop). */
  subscribeFrames: (sink: FrameSink | null) => void;
  /** The classic tab's switch to the full sensor / Align, run only on the operator's press. */
  onSwitchMode: () => Promise<unknown>;
  /** The classic tab's ROI write for this kind, with the readback read after it. */
  writeRoi: (kind: RoiKind, rect: Rect) => Promise<WriteResult<Rect>>;
  /** Why writes are locked now (a run is active); "" when they are not. */
  lockedReason: string;
  /** Back to the classic view (All features, XML, scripts). */
  onClassic: () => void;
}

const RECT_CODEC: Codec<Rect, RectDraft> = { format: rectDraft, equal: rectEqual };
const EMPTY_DRAFT: RectDraft = { x: "", y: "", width: "", height: "" };
const DEFAULT_VIEW: Size = { width: 640, height: 300 };
const VIEWPORT_MIN_HEIGHT = 180, VIEWPORT_MAX_HEIGHT = 480;
const MODE_TEXT: Record<ViewMode, string> = { fit: "fit", "1:1": "1:1", zoom: "zoom" };
const FIELD_LABEL: Record<keyof Rect, string> = { x: "X", y: "Y", width: "W", height: "H" };
const NOT_WRITABLE = <span className="ws-detail" title="Not writable here">—</span>;

export function AlignView(p: AlignViewProps) {
  const [inspection, setInspection] = useState<Inspection | null>(null);
  const roi = roiRow(p.facts, inspection?.size ?? null);
  // A different kind of ROI (the platform report arrived, the camera changed) is a new control.
  return <AlignBody key={roi.kind ?? "none"} {...p} roi={roi} inspection={inspection} onInspection={setInspection} />;
}

interface BodyProps extends AlignViewProps {
  roi: RoiRow;
  inspection: Inspection | null;
  onInspection: (i: Inspection) => void;
}

function AlignBody(p: BodyProps) {
  const { roi } = p;
  const mode = alignMode(p.facts);
  const rows = readbackRows(p.facts);
  const [switching, setSwitching] = useState(false);
  const writeRoi = p.writeRoi;
  const control = useQuickControl<Rect, RectDraft>({
    codec: RECT_CODEC, empty: EMPTY_DRAFT, readback: roi.readback, initial: roi.initial,
    check: useCallback((d: RectDraft) => (roi.bounds ? checkRect(d, roi.bounds) : { ok: false as const, reason: roi.why }), [roi.bounds, roi.why]),
    write: useCallback(async (r: Rect): Promise<WriteResult<Rect>> => {
      if (!roi.kind) return { ok: false, reason: roi.why };
      const result = await writeRoi(roi.kind, r);
      return result.ok && roi.deferred && !rectEqual(result.readback, r) ? { ...result, deferred: roi.deferred } : result;
    }, [roi.kind, roi.why, roi.deferred, writeRoi]),
  });
  const draftRect = roi.bounds && control.state.edited ? checkRect(control.state.draft, roi.bounds) : null;
  const shownRoi = draftRect?.ok ? draftRect.value : control.state.readback ?? roi.initial;
  const overlay = roiOverlay(roi, shownRoi, p.inspection?.size ?? null);

  const switchMode = async () => {
    if (switching) return;
    setSwitching(true);
    try { await p.onSwitchMode(); } finally { setSwitching(false); }
  };

  return (
    <div className="ws-view" data-testid="workspace-align">
      <AlignImage {...p} overlay={overlay}
        modeNotice={mode.switchNeeded ? (
          <Notice
            tone={mode.tone === "blocked" ? "blocked" : "info"}
            title={`Camera · ${mode.label}`}
            data-testid="align-mode"
            actions={<Button variant="primary" disabled={!!mode.blocked || switching} title={mode.blocked || undefined} onClick={() => void switchMode()}>
              {switching ? "Switching…" : mode.action}
            </Button>}
          >
            {mode.blocked ? `${mode.action} is unavailable: ${mode.blocked}.` : mode.detail}
          </Notice>
        ) : null}
        modeChip={<StateChip tone={mode.tone} data-testid="align-mode-chip">{mode.label}</StateChip>}
        controls={<QuickControls roi={roi} rows={rows} control={control} lockedReason={p.lockedReason} pz7035={p.facts.pz7035} />}
      />
    </div>
  );
}

interface ImageProps extends BodyProps {
  overlay: Rect | null;
  modeNotice: ReactNode;
  modeChip: ReactNode;
  controls: ReactNode;
}

function AlignImage(p: ImageProps) {
  const { subscribeFrames, onInspection } = p;
  const viewport = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const offscreen = useRef<HTMLCanvasElement | null>(null);
  const inspector = useRef<FrameInspector | null>(null);
  const paintPending = useRef(false);
  const [view, setView] = useState<Size>(DEFAULT_VIEW);
  const [imageSize, setImageSize] = useState<Size | null>(null);
  const imageSizeRef = useRef<Size | null>(null);
  const [viewMode, setViewMode] = useState<ViewMode>("fit");
  const [transform, setTransform] = useState<ViewTransform>({ scale: 1, offsetX: 0, offsetY: 0 });
  const transformRef = useRef(transform);
  transformRef.current = transform;
  const [frozen, setFrozen] = useState(false);
  const [showRoi, setShowRoi] = useState(true);
  const [axis, setAxis] = useState<ProfileAxis>("row");
  const [pinned, setPinned] = useState<Point | null>(null);
  const [hover, setHover] = useState<Point | null>(null);
  const drag = useRef<{ start: Point; last: Point; moved: boolean } | null>(null);
  const overlayRef = useRef<{ roi: Rect | null; axis: ProfileAxis; pinned: Point | null }>({ roi: null, axis, pinned });
  overlayRef.current = { roi: showRoi ? p.overlay : null, axis, pinned };

  // Paint the newest frame at the display's pace (one paint per animation frame at most).
  const paint = useCallback(() => {
    paintPending.current = false;
    const el = canvas.current, frame = inspector.current?.latest();
    if (!el || !frame) return;
    const ctx = el.getContext("2d");
    if (!ctx) return;
    let off = offscreen.current;
    if (!off || off.width !== frame.width || off.height !== frame.height) {
      off = document.createElement("canvas");
      off.width = frame.width;
      off.height = frame.height;
      offscreen.current = off;
    }
    off.getContext("2d")?.putImageData(mono8ToImageData(frame.data, frame.width, frame.height, frame.stride_bytes), 0, 0);
    const t = transformRef.current, o = overlayRef.current;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, el.width, el.height);
    ctx.imageSmoothingEnabled = false;
    ctx.setTransform(t.scale, 0, 0, t.scale, t.offsetX, t.offsetY);
    ctx.drawImage(off, 0, 0);
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    const sx = (x: number) => t.offsetX + x * t.scale, sy = (y: number) => t.offsetY + y * t.scale;
    ctx.lineWidth = 1.5;
    if (o.roi) {
      ctx.strokeStyle = "#ffd400";
      ctx.strokeRect(sx(o.roi.x), sy(o.roi.y), o.roi.width * t.scale, o.roi.height * t.scale);
    }
    // The profile line, through the pinned point or the middle of the frame.
    ctx.strokeStyle = "#3a6ea5";
    ctx.beginPath();
    if (o.axis === "row") {
      const y = sy((o.pinned?.y ?? Math.floor(frame.height / 2)) + 0.5);
      ctx.moveTo(sx(0), y); ctx.lineTo(sx(frame.width), y);
    } else {
      const x = sx((o.pinned?.x ?? Math.floor(frame.width / 2)) + 0.5);
      ctx.moveTo(x, sy(0)); ctx.lineTo(x, sy(frame.height));
    }
    ctx.stroke();
  }, []);
  const requestPaint = useCallback(() => {
    if (paintPending.current) return;
    paintPending.current = true;
    const raf = typeof window.requestAnimationFrame === "function" ? window.requestAnimationFrame.bind(window) : (cb: () => void) => window.setTimeout(cb, 16);
    raf(() => paint());
  }, [paint]);

  // The sink: keep a reference to the delivered frame and return. No pixel work happens here.
  useEffect(() => {
    const ins = new FrameInspector(onInspection, { set: (fn, ms) => window.setTimeout(fn, ms), clear: (h) => window.clearTimeout(h as number) },
      () => performance.now());
    inspector.current = ins;
    subscribeFrames((packet) => {
      if (!packet.valid || packet.width <= 0 || packet.height <= 0) return false;
      // A frozen display keeps its frame and with it its size: the view transform and the pointer
      // mapping must keep matching the pixels shown, whatever the camera delivers meanwhile.
      if (!ins.deliver(packet as InspectFrame)) return false;
      const size = imageSizeRef.current;
      if (!size || size.width !== packet.width || size.height !== packet.height) {
        imageSizeRef.current = { width: packet.width, height: packet.height };
        setImageSize(imageSizeRef.current);
      }
      requestPaint();
      return true;
    });
    return () => { subscribeFrames(null); ins.dispose(); inspector.current = null; };
  }, [subscribeFrames, onInspection, requestPaint]);

  useEffect(() => { inspector.current?.setFrozen(frozen); }, [frozen]);
  useEffect(() => { inspector.current?.setProfile(axis, pinned ? (axis === "row" ? pinned.y : pinned.x) : null); }, [axis, pinned]);

  // The viewport follows the card's width.
  useEffect(() => {
    const el = viewport.current;
    if (!el || typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(([entry]) => {
      const w = Math.floor(entry.contentRect.width), h = Math.floor(entry.contentRect.height);
      if (w > 0 && h > 0) setView((v) => (v.width === w && v.height === h ? v : { width: w, height: h }));
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // Fit and 1:1 follow the image and viewport size; zoom keeps the operator's view.
  useEffect(() => {
    if (imageSize) setTransform((t) => transformFor(viewMode, t, imageSize, view));
  }, [viewMode, imageSize, view]);
  useEffect(() => { requestPaint(); }, [transform, view, p.overlay, showRoi, axis, pinned, requestPaint]);

  const zoom = (factor: number, anchor?: Point) => {
    if (!imageSize) return;
    setViewMode("zoom");
    setTransform((t) => zoomAt(t, factor, anchor ?? { x: view.width / 2, y: view.height / 2 }, imageSize, view));
  };
  // Wheel zoom needs a non-passive listener to keep the page from scrolling.
  const zoomRef = useRef(zoom);
  zoomRef.current = zoom;
  useEffect(() => {
    const el = canvas.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const box = el.getBoundingClientRect();
      zoomRef.current(e.deltaY < 0 ? ZOOM_STEP : 1 / ZOOM_STEP, { x: e.clientX - box.left, y: e.clientY - box.top });
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, []);

  const local = (e: ReactPointerEvent<HTMLCanvasElement>): Point => {
    const box = e.currentTarget.getBoundingClientRect();
    return { x: e.clientX - box.left, y: e.clientY - box.top };
  };
  const onPointerDown = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const at = local(e);
    drag.current = { start: at, last: at, moved: false };
    e.currentTarget.setPointerCapture?.(e.pointerId);
  };
  const onPointerMove = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const at = local(e);
    if (imageSize) setHover(screenToImage(transformRef.current, at, imageSize));
    const d = drag.current;
    if (!d || !imageSize) return;
    if (!d.moved && Math.hypot(at.x - d.start.x, at.y - d.start.y) < 3) return;
    d.moved = true;
    const dx = at.x - d.last.x, dy = at.y - d.last.y;
    d.last = at;
    setViewMode("zoom");
    setTransform((t) => panBy(t, dx, dy, imageSize, view));
  };
  const onPointerUp = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const d = drag.current;
    drag.current = null;
    if (!d || d.moved || !imageSize) return;
    // A click pins the point: the profile runs through it.
    const at = screenToImage(transformRef.current, local(e), imageSize);
    if (at) setPinned(at);
  };
  const onKeyDown = (e: KeyboardEvent<HTMLCanvasElement>) => {
    if (!imageSize) return;
    const step = 32, moves: Record<string, [number, number]> = { ArrowLeft: [step, 0], ArrowRight: [-step, 0], ArrowUp: [0, step], ArrowDown: [0, -step] };
    const m = moves[e.key];
    if (m) { e.preventDefault(); setViewMode("zoom"); setTransform((t) => panBy(t, m[0], m[1], imageSize, view)); }
    else if (e.key === "+" || e.key === "=") { e.preventDefault(); zoom(ZOOM_STEP); }
    else if (e.key === "-") { e.preventDefault(); zoom(1 / ZOOM_STEP); }
  };

  // The viewport takes the image's aspect at the card's width, within bounds.
  const viewportHeight = imageSize ? Math.min(VIEWPORT_MAX_HEIGHT, Math.max(VIEWPORT_MIN_HEIGHT, Math.round(view.width * imageSize.height / imageSize.width))) : 0;
  const frame = inspector.current?.latest() ?? null;
  const probe = hover && frame ? probeAt(frame, hover.x, hover.y) : null;
  const pinnedValue = pinned && frame ? probeAt(frame, pinned.x, pinned.y) : null;
  const ins = p.inspection;
  const size = ins?.size ?? imageSize;
  const subtitle = size
    ? `Frame ${ins?.frameIndex ?? "…"} · ${size.width} × ${size.height} · Mono8 · ${MODE_TEXT[viewMode]} ${Math.round(transform.scale * 100)} %${frozen ? " · display frozen" : ""}`
    : "No frame yet";
  const noFrameHint = !frame
    ? p.facts.running ? "Waiting for the first frame…"
      : p.facts.instrumentModes && p.facts.instrumentMode !== "align" ? "No live camera image outside Align. Switch to Align above."
        : "No live frame: the camera is not running. Start it from the classic view."
    : "";

  return (
    <>
      <Toolbar title="Align">
        <Button variant="selected" aria-current="page">Quick controls</Button>
        <Button onClick={p.onClassic} title="All features, the GenApi XML and scripts stay in the classic view">All features · classic view</Button>
        <Button variant={frozen ? "selected" : "secondary"} aria-pressed={frozen} onClick={() => setFrozen((f) => !f)}
          title="Holds the shown frame for inspection; capture and polling go on">
          {frozen ? "Resume display" : "Freeze display"}
        </Button>
      </Toolbar>
      <div className="ws-body">
        {p.modeNotice}
        <div className="ws-align">
          <Card
            className="ws-align__image"
            title="Live image"
            subtitle={subtitle}
            status={<>
              {p.facts.running ? <StateChip tone="confirmed">Camera running</StateChip> : <StateChip tone="neutral">Camera stopped</StateChip>}
              {p.modeChip}
            </>}
          >
            <div className="ws-align__tools" role="group" aria-label="Image view">
              <Button size="small" variant={viewMode === "fit" ? "selected" : "secondary"} onClick={() => setViewMode("fit")}>Fit</Button>
              <Button size="small" variant={viewMode === "1:1" ? "selected" : "secondary"} onClick={() => setViewMode("1:1")}>1:1</Button>
              <Button size="small" aria-label="Zoom in" disabled={!imageSize} onClick={() => zoom(ZOOM_STEP)}>+</Button>
              <Button size="small" aria-label="Zoom out" disabled={!imageSize} onClick={() => zoom(1 / ZOOM_STEP)}>−</Button>
              <Button size="small" variant={showRoi ? "selected" : "secondary"} aria-pressed={showRoi} onClick={() => setShowRoi((s) => !s)}>ROI</Button>
              <Button size="small" variant={axis === "row" ? "selected" : "secondary"} onClick={() => setAxis("row")}>Profile: row</Button>
              <Button size="small" variant={axis === "column" ? "selected" : "secondary"} onClick={() => setAxis("column")}>Profile: column</Button>
            </div>
            <div className="ws-align__viewport" ref={viewport} style={viewportHeight ? { height: viewportHeight } : undefined}>
              <canvas
                ref={canvas}
                width={view.width}
                height={view.height}
                tabIndex={0}
                aria-label="Live image: drag to pan, wheel or + and − to zoom, click to pin the profile point"
                data-testid="align-canvas"
                onPointerDown={onPointerDown}
                onPointerMove={onPointerMove}
                onPointerUp={onPointerUp}
                onPointerCancel={() => { drag.current = null; }}
                onPointerLeave={() => setHover(null)}
                onKeyDown={onKeyDown}
              />
              {noFrameHint ? <span className="ws-align__hint" data-testid="align-no-frame">{noFrameHint}</span> : null}
            </div>
            <p className="ws-note" data-testid="align-probe">
              {probe ? `Pixel probe · x ${probe.x} · y ${probe.y} · value ${probe.value}` : "Pixel probe · move over the image"}
              {pinnedValue ? ` · pinned x ${pinnedValue.x} · y ${pinnedValue.y} · value ${pinnedValue.value}` : ""}
            </p>
            <div className="ws-align__plots">
              <HistogramPlot inspection={ins} />
              <ProfilePlot inspection={ins} />
            </div>
          </Card>
          {p.controls}
        </div>
      </div>
    </>
  );
}

function HistogramPlot({ inspection }: { inspection: Inspection | null }) {
  const h = inspection?.histogram;
  const d = useMemo(() => (h ? histogramPath(h, 256, 64) : ""), [h]);
  return (
    <figure className="ws-plot" data-testid="align-histogram">
      <figcaption>Histogram</figcaption>
      <svg viewBox="0 0 256 64" preserveAspectRatio="none" role="img" aria-label="Histogram of the shown frame">
        {d ? <path d={d} /> : null}
      </svg>
      <p className="ws-note">{h ? histogramSummary(h) : "No frame yet"}</p>
    </figure>
  );
}

function ProfilePlot({ inspection }: { inspection: Inspection | null }) {
  const pr = inspection?.profile;
  const length = pr ? (pr.axis === "row" ? inspection!.size.width : inspection!.size.height) : 0;
  const points = useMemo(() => (pr ? profilePoints(pr, length, 256, 64) : ""), [pr, length]);
  return (
    <figure className="ws-plot" data-testid="align-profile">
      <figcaption>Line profile</figcaption>
      <svg viewBox="0 0 256 64" preserveAspectRatio="none" role="img" aria-label="Line profile of the shown frame">
        {points ? <polyline points={points} /> : null}
      </svg>
      <p className="ws-note">
        {pr ? `${pr.axis === "row" ? "Row y" : "Column x"} ${pr.index} · ${pr.values.length} points · min ${pr.min} · max ${pr.max}` : "No frame yet"}
      </p>
    </figure>
  );
}

type ControlApi = ReturnType<typeof useQuickControl<Rect, RectDraft>>;
type TableRow = { id: string; label: string; roi?: RoiRow; readback?: ReadbackRow };

function QuickControls({ roi, rows, control, lockedReason, pz7035 }: { roi: RoiRow; rows: ReadbackRow[]; control: ControlApi; lockedReason: string; pz7035: boolean }) {
  const s = control.state;
  const roiStatus = controlStatus(s, roi.access, RECT_CODEC, rectText, roi.why);
  const pending = s.phase === "pending";
  const locked = !!lockedReason;
  const fixed = roi.bounds?.fixedSize;
  const columns: DataTableColumn<TableRow>[] = [
    { key: "label", header: "Feature", emphasis: true, width: "20%" },
    {
      key: "requested", header: "Requested", width: "30%", cell: (r) => r.roi
        ? (r.roi.access === "rw" ? (
          <span className="ws-roi" data-testid="align-roi-draft">
            {RECT_KEYS.map((k) => (
              <label key={k} className="ws-field">
                <span>{FIELD_LABEL[k]}</span>
                <input
                  type="number"
                  inputMode="numeric"
                  aria-label={`${r.label} ${FIELD_LABEL[k]}`}
                  value={s.draft[k]}
                  disabled={pending || locked || (!!fixed && (k === "width" || k === "height"))}
                  onChange={(e) => control.edit({ ...s.draft, [k]: e.target.value })}
                  onKeyDown={(e) => { if (e.key === "Enter") control.submit(); if (e.key === "Escape") control.revert(); }}
                />
              </label>
            ))}
          </span>
        ) : NOT_WRITABLE)
        : NOT_WRITABLE,
    },
    {
      key: "readback", header: "Readback", width: "24%", cell: (r) => r.roi
        ? (s.readback ? rectText(s.readback) : r.roi.access === "rw" ? `No readback · ${r.roi.why}` : "—")
        : <>
          {r.readback!.value ?? "Not reported"}
          {r.readback!.detail ? <span className="ws-detail">{r.readback!.detail}</span> : null}
        </>,
    },
    {
      key: "state", header: "State", width: "26%", cell: (r) => {
        const rb = r.readback;
        const st = r.roi ? { ...roiStatus, title: roiStatus.label } : rb!.access === "unavailable"
          ? { tone: "unsupported" as const, label: `Unavailable · ${rb!.chip}`, title: `Unavailable: ${rb!.why}` }
          : { tone: "neutral" as const, label: `Read-only · ${rb!.chip}`, title: `Read-only: ${rb!.why}` };
        return <StateChip size="small" tone={st.tone} title={st.title} data-testid={`align-state-${r.id}`}>{st.label}</StateChip>;
      },
    },
  ];
  const table: TableRow[] = [...rows.map((r) => ({ id: r.id, label: r.label, readback: r })), { id: "roi", label: roi.label, roi }];
  const canWrite = roi.access === "rw" && !pending && !locked;
  return (
    <Card
      className="ws-align__controls"
      title="Quick controls"
      subtitle="Requested → readback · a value is current only after readback"
      data-testid="align-quick-controls"
      actions={roi.access === "rw" ? <>
        <Button disabled={!canWrite || (!s.edited && s.phase === "idle")} onClick={control.revert}>Revert to readback</Button>
        <Button disabled={!canWrite} onClick={control.submit}>{pending ? "Writing…" : `Write ${roi.kind === "run" ? "Run window" : "ROI"}`}</Button>
      </> : undefined}
    >
      <DataTable columns={columns} rows={table} rowKey={(r) => r.id} density="compact" />
      {locked ? <p className="ws-note" data-testid="align-locked">Writes are locked: {lockedReason}.</p> : null}
      {s.phase === "idle" && s.written && s.reason ? <p className="ws-note" data-testid="align-snap-note">Written after snapping: {s.reason}.</p> : null}
      <p className="ws-note">
        Exposure, gain and frame rate are {pz7035 ? "set in Config → Camera document" : "set by the camera script or configuration"} in the classic view, as are all features, the XML and scripts.
        Nothing here is written until you press Write.
      </p>
    </Card>
  );
}
