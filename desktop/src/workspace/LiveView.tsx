// O4 Experiment · Live (#716 A6b; exec plan P4 "Keep the live inspection features…", S5): the live
// frame, the backend's detected-cell strip and pins (bridge ABI 41, monitoring_cells), the
// area–deformability scatter with click-to-pin, the pinned cell with previous / next and a larger
// viewer, the run's status and counts, the sample queue and the run's lifecycle, and separate stop
// actions. The pure rules are in liveModel.ts and the cells' reads in useLiveCells.ts.
//
// Nothing acts on navigation. Showing the view reads (the cells, the monitoring snapshot App polls
// while Live is shown, the frames the live preview already pulls) and, from the controlling client,
// turns monitoring on, as the classic Monitoring tab does. Leaving it turns monitoring off only when
// no run is in progress (App's monitoring switch). A pin, unpin or clear is sent on a click only,
// and only by the controlling client. "Stop & finalize run"
// stops one named run: the sample queue's own row (through the queue, as Next sample does), or the
// run the status shows active, by its start generation (ABI 40). "Stop acquisition" stops the
// camera and never runs under a run. "Freeze display" holds what is shown; acquisition, processing
// and recording go on. The queue's rules are kept: a control loss pauses it, nothing resumes on
// its own, and resuming sends nothing.
import { useCallback, useEffect, useMemo, useRef, useState, type MouseEvent, type ReactNode } from "react";
import {
  AreaDeformChart,
  Button,
  Card,
  CellImage,
  CellStrip,
  ConditionSwatch,
  FactList,
  Notice,
  StatTile,
  StateChip,
  Stepper,
  Toolbar,
  type CellStripItem,
  type StateTone,
} from "@mib/ui";
import { EXPERIMENT_STATES, EXPERIMENT_STOP_OUTCOMES } from "../bridgeContract";
import { mono8ToImageData, type MonitoringSnapshot } from "../bridge";
import type { ExperimentStatus } from "../eventAdapter";
import type { FramePacket } from "../framePacket";
import { metricNumber } from "../metricFormat";
import { cellKeyString, type MonitoringCell, type MonitoringCellsListing } from "../monitoringCells";
import type { FrameSink } from "./AlignView";
import type { ExperimentSubView } from "./ExperimentView";
import { packetToUrl } from "./liveImages";
import {
  cellName,
  cellOnChart,
  cellWhere,
  chartDomain,
  endedPins,
  isEndedPin,
  liveScatter,
  nearestPoint,
  pinCap,
  runCounts,
  runInProgress,
  statusLifecycle,
  stopAcquisitionReason,
  stopTarget,
  type LiveNotice,
} from "./liveModel";
import { conditionSlots, type SampleRow } from "./planModel";
import { currentIndex, lifecycle, queueBegun, recordedPath, rowChip, rowState, type LifecycleStep } from "./queueModel";
import { useLiveCells, VIEWER_PIN_REASON, type CellPicture, type PacketToUrl } from "./useLiveCells";
import type { PlanController, StopReply } from "./usePlan";

export interface LiveViewProps {
  /** The Experiment sub-view buttons (Plan · Live · Summary), first in the toolbar. */
  tabs: ReactNode;
  plan: PlanController;
  /** The monitoring snapshot App reads while Live is shown; null before the first read. */
  snapshot: MonitoringSnapshot | null;
  /** The camera streams. */
  running: boolean;
  experimentActive: boolean;
  /** Where the science runs: false on the PZ7035, whose PL rows carry measurements, not images. */
  hostProcessing: boolean;
  /** Register the one sink for the frames the live preview pulls (null to stop). */
  subscribeFrames: (sink: FrameSink | null) => void;
  /** The classic Stop Camera (it stops a recording clip first). */
  onStopAcquisition: () => Promise<void>;
  /** `experiment_stop` naming this run (expectedStartGeneration, ABI 40). */
  onStopRun: (expectedStartGeneration: string) => Promise<StopReply>;
  /** Show another Experiment sub-view (navigation only). */
  onView: (view: ExperimentSubView) => void;
  /** Draws a packet as an image URL (jsdom has no canvas: tests pass their own). */
  toUrl?: PacketToUrl;
}

const CHART_W = 560, CHART_H = 300;
const basename = (path: string) => path.split(/[\\/]/).pop() || path;
const describe = (r: SampleRow) => {
  const what = r.sample.trim() || r.condition.trim();
  return what ? `${r.ref} · ${what}` : r.ref;
};
const plain = (e: unknown) => String(e).replace(/^Error: /, "");
/** A fully transparent pixel: the crop's place while its pixels load (the kit says "Image not
 *  recorded" for a missing src, which would be untrue; a loading state is requested on #715). */
const LOADING_SRC = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

type Layer = "raw" | "mask";

/** The props the kit's CellImage takes for one cell and the crop the view holds for it. */
function cellImageProps(cell: MonitoringCell, picture: CellPicture | undefined, layer: Layer) {
  const img = cell.image;
  const base = { recordId: cellName(cell), imageWidth: img?.width ?? 1, imageHeight: img?.height ?? 1 };
  if (!img) return { ...base, overlay: "none" as const, title: cell.no_image_reason ?? undefined };
  if (picture?.src === undefined) return { ...base, src: picture?.reason ? undefined : LOADING_SRC, overlay: "none" as const, title: picture?.reason };
  // The mask overlay only once its pixels are here; a cell recorded without a mask says so.
  const overlay = layer === "mask" && (picture.maskSrc !== undefined || !img.has_mask) ? ("mask" as const) : ("none" as const);
  return { ...base, src: picture.src, maskSrc: picture.maskSrc, overlay, title: picture.maskReason };
}

export function LiveView({ tabs, plan, snapshot, running, experimentActive, hostProcessing, subscribeFrames, onStopAcquisition, onStopRun, onView, toUrl = packetToUrl }: LiveViewProps) {
  const queue = plan.queue;
  const status = queue.status;
  const controlHeld = queue.controlHeld;
  const [frozen, setFrozen] = useState(false);
  const [layer, setLayer] = useState<Layer>("raw");
  const cells = useLiveCells({ active: true, controlHeld, frozen, masks: layer === "mask", toUrl });

  // The scatter: held while frozen.
  const [heldSnapshot, setHeldSnapshot] = useState<MonitoringSnapshot | null>(null);
  const snapshotRef = useRef(snapshot);
  snapshotRef.current = snapshot;
  useEffect(() => { setHeldSnapshot(frozen ? snapshotRef.current : null); }, [frozen]);
  const scatter = useMemo(() => liveScatter(frozen ? heldSnapshot : snapshot), [frozen, heldSnapshot, snapshot]);

  // ---- the pinned cell: selected by the operator, else the newest pin ----
  const listing = cells.listing;
  const pins = listing?.pins ?? [];
  const [selected, setSelected] = useState<string | null>(null);
  useEffect(() => { if (cells.lastPinned) setSelected(cells.lastPinned); }, [cells.lastPinned]);
  const selIndex = (() => {
    const i = pins.findIndex((p) => cellKeyString(p) === selected);
    return i >= 0 ? i : pins.length - 1;
  })();
  const pinned = selIndex >= 0 ? pins[selIndex] : null;
  const step = (by: -1 | 1) => { if (pins.length) setSelected(cellKeyString(pins[(selIndex + by + pins.length) % pins.length])); };
  const [viewer, setViewer] = useState(false);
  useEffect(() => { if (!pinned) setViewer(false); }, [pinned]);

  // ---- the run the view follows, and the stops ----
  const order = plan.draft.rows.map((r) => r.ref);
  const queueMode = plan.draft.format === "queue";
  const q = queue.state;
  const begun = queueMode && queueBegun(q);
  const cur = queueMode ? currentIndex(order, q) : -1;
  const curRow = cur >= 0 ? plan.draft.rows[cur] : null;
  const curState = curRow ? rowState(q, curRow.ref) : null;
  const paused = q.paused !== null;
  const [stopping, setStopping] = useState(false);
  const [stopNotice, setStopNotice] = useState<LiveNotice | null>(null);
  const target = stopTarget({ controlHeld, pending: stopping || queue.pending !== null, status, queue: begun ? { row: curState, paused } : null });
  const acquisitionReason = stopAcquisitionReason(controlHeld, running, experimentActive);
  const [acquisitionPending, setAcquisitionPending] = useState(false);

  const stopRun = async () => {
    if (target.kind === "queue") { setStopNotice(null); await queue.stopCurrent(); return; }
    if (target.kind !== "run") return;
    setStopping(true);
    setStopNotice(null);
    try {
      const reply = await onStopRun(target.generation);
      setStopNotice(reply.ok
        ? { tone: "info", title: `Stop requested · run generation ${target.generation}`, text: "The backend drains and finalizes it; the lifecycle below follows the status." }
        : reply.stop_outcome === EXPERIMENT_STOP_OUTCOMES.RunMismatch
          ? { tone: "unknown", title: "Not stopped · that run is no longer the active one", text: `${reply.message}. Nothing was stopped.` }
          : { tone: "warn", title: "Not stopped", text: reply.message });
    } catch (e) {
      setStopNotice({ tone: "blocked", title: "Not stopped · no reply to the stop", text: plain(e) });
    } finally {
      setStopping(false);
    }
  };
  const stopAcquisition = async () => {
    if (acquisitionReason || acquisitionPending) return;
    setAcquisitionPending(true);
    try { await onStopAcquisition(); } finally { setAcquisitionPending(false); }
  };

  const stripItems: CellStripItem[] = cells.strip.map((c) => cellImageProps(c, cells.pictures.get(cellKeyString(c)), layer));
  const pinFromStrip = (name: string) => { const c = cells.strip.find((s) => cellName(s) === name); if (c) void cells.pin(c); };
  const pinnedName = pinned ? cellName(pinned) : undefined;
  const ended = endedPins(listing);

  return (
    <div className="ws-view" data-testid="workspace-live">
      <Toolbar title="Live experiment">
        {tabs}
        <Button disabled={!!acquisitionReason || acquisitionPending} title={acquisitionReason || "Stops the camera (and a recording clip first)"} onClick={() => void stopAcquisition()} data-testid="live-stop-acquisition">
          {acquisitionPending ? "Stopping acquisition…" : "Stop acquisition"}
        </Button>
        <Button variant={frozen ? "selected" : "secondary"} aria-pressed={frozen} onClick={() => setFrozen((f) => !f)} data-testid="live-freeze"
          title="Holds the live frame, the cell strip and the scatter shown; acquisition, processing and recording go on">
          {frozen ? "Resume display" : "Freeze display"}
        </Button>
        <Button variant="destructive" disabled={target.kind === "none"} title={target.kind === "none" ? target.reason : target.kind === "queue" ? `Stops ${curRow?.ref}'s run (stop → drain → finalize), as Next sample does` : `Stops run generation ${target.generation} (stop → drain → finalize)`}
          onClick={() => void stopRun()} data-testid="live-stop-run">
          {stopping || queue.pending === "stop" ? "Stopping…" : "Stop & finalize run"}
        </Button>
      </Toolbar>
      <div className="ws-body" data-testid="live-body">
        <LiveNotices controlHeld={controlHeld} paused={q.paused} snapshot={snapshot} hostProcessing={hostProcessing} frozen={frozen} frozenChange={cells.frozenChange}
          readError={cells.readError} stopNotice={stopNotice} onResume={queue.resume} />
        {cells.notice ? (
          <Notice tone={cells.notice.tone} title={cells.notice.title} data-testid="live-pin-notice" actions={<Button size="small" onClick={cells.dismiss}>Dismiss</Button>}>
            {cells.notice.text}
          </Notice>
        ) : null}

        <div className="ws-live__row">
          <Card title="Live cells" className="ws-live__cells" data-testid="live-cells"
            subtitle={<LiveFrameCaption listing={listing} />}
            status={<span className="ws-live__layers">
              <Button size="small" variant={layer === "raw" ? "selected" : "secondary"} aria-pressed={layer === "raw"} onClick={() => setLayer("raw")}>Raw</Button>
              <Button size="small" variant={layer === "mask" ? "selected" : "secondary"} aria-pressed={layer === "mask"} onClick={() => setLayer("mask")}>Mask</Button>
            </span>}>
            <LiveFrame subscribeFrames={subscribeFrames} frozen={frozen} running={running} />
            <span className="ws-plan-label">{controlHeld ? "Detected cells · newest first · click to pin" : "Detected cells · newest first"}</span>
            <CellStrip cells={stripItems} pinnedId={pinned && listing && !isEndedPin(pinned, listing) ? pinnedName : undefined}
              onPin={controlHeld ? pinFromStrip : undefined} size={64} data-testid="live-strip"
              empty={cells.frozenChange ? "The frozen cells were of an earlier capture: dropped." : listing ? "No cells detected in this capture yet." : "The live cells have not been read yet."} />
            <p className="ws-note">{`The backend keeps the ${listing?.caps.strip_max_cells ?? 24} newest cells of this capture as copies (at most ${mib(listing?.caps.strip_max_bytes)} MiB); the oldest is evicted first, and the strip is cleared when the capture changes. A pin keeps its own copy.`}</p>
          </Card>

          <Card title="Area–Deformability" className="ws-live__scatter" data-testid="live-scatter"
            subtitle={controlHeld ? "Click a point to pin its cell · acquisition is unaffected" : "Valid, calibrated rows of the monitoring buffer"}>
            <LiveScatter points={scatter.points} pinned={pinned && listing && !isEndedPin(pinned, listing) ? pinned : null}
              onPick={controlHeld ? (i) => {
                const p = scatter.points[i];
                if (p.key) void cells.pin(p.key);
                else cells.inform({ tone: "warn", title: `Not pinned · ${cellName(p.row)}`, text: "This row carries no run or capture identity (an older backend reports none), so it cannot be pinned. Nothing was sent." });
              } : undefined} />
            <p className="ws-note" data-testid="live-scatter-note">
              {`${scatter.points.length} valid rows, area in µm² with each row's own analysis-time px→µm${scatter.uncalibrated ? `; ${scatter.uncalibrated} without one are left out` : ""}. Rows are never recalibrated with the current settings.`}
            </p>
          </Card>
        </div>

        <div className="ws-live__row">
          <PinnedCard pinned={pinned} index={selIndex} pins={pins} listing={listing} picture={pinned ? cells.pictures.get(cellKeyString(pinned)) : undefined} layer={layer}
            controlHeld={controlHeld} busy={cells.busy} ended={ended.length}
            onStep={step} onOpen={() => setViewer(true)} onUnpin={() => { if (pinned) void cells.unpin(pinned); }}
            onClearEnded={() => void cells.clearEnded()} onClearAll={() => void cells.clearAll()} />
          <RunStatusCard status={status} />
        </div>

        <div className="ws-live__row ws-live__row--wide">
          <QueueCard plan={plan} cur={cur} begun={begun} />
          <LifecycleCard plan={plan} curRow={curRow} status={status} onView={onView} />
        </div>
      </div>
      {viewer && pinned ? (
        <CellViewer pins={pins} index={selIndex} listing={listing} picture={cells.pictures.get(cellKeyString(pinned))} layer={layer} onLayer={setLayer}
          controlHeld={controlHeld} busy={cells.busy} onStep={step} onUnpin={() => void cells.unpin(pinned)} onClose={() => setViewer(false)} />
      ) : null}
    </div>
  );
}

const mib = (bytes: number | undefined) => metricNumber((bytes ?? 4194304) / 1048576, 0);

function LiveFrameCaption({ listing }: { listing: MonitoringCellsListing | null }) {
  if (!listing) return <>Live frame · newest detected cells below</>;
  const run = listing.ring_start_generation === "0" ? "outside a run" : `run generation ${listing.ring_start_generation}`;
  return <>{`Capture ${listing.ring_capture_epoch} · ${run} · newest detected cells below`}</>;
}

function LiveNotices({ controlHeld, paused, snapshot, hostProcessing, frozen, frozenChange, readError, stopNotice, onResume }: {
  controlHeld: boolean; paused: string | null; snapshot: MonitoringSnapshot | null; hostProcessing: boolean; frozen: boolean; frozenChange: string; readError: string;
  stopNotice: LiveNotice | null; onResume: () => void;
}) {
  return <>
    {paused ? (
      <Notice tone="warn" title="Queue paused · control lost" data-testid="live-paused"
        actions={<Button disabled={!controlHeld} title={controlHeld ? undefined : "Take control of the instrument first"} onClick={onResume} data-testid="live-resume">Resume queue</Button>}>
        {`Paused because ${paused}. It never resumes on its own and sends nothing while paused. Resuming sends nothing: you press each step again.`}
      </Notice>
    ) : null}
    {!controlHeld ? (
      <Notice tone="blocked" title="Watching only · this client does not control the instrument" data-testid="live-viewer">
        {`${VIEWER_PIN_REASON} Stopping a run or the camera also needs control; take it from the banner at the top.`}
      </Notice>
    ) : null}
    {snapshot?.valid && !snapshot.monitoring_active ? (
      <Notice tone="info" title={controlHeld ? "Monitoring is being turned on" : "Monitoring is off on the instrument"} data-testid="live-monitoring-off">
        {controlHeld
          ? "Monitoring runs while a monitoring view (this one, or the classic Monitoring tab) is shown; the next read shows its rows."
          : "Only the controlling client turns monitoring on: it runs while that client shows Live or the classic Monitoring tab, and once on it stays on until the run in progress ends."}
      </Notice>
    ) : null}
    {!hostProcessing ? (
      <Notice tone="info" title="Measurements without images on this instrument" data-testid="live-pl">
        The PL measures every frame and reports results only, so cells and pins carry their measurements and the reason, never an image.
      </Notice>
    ) : null}
    {frozen && frozenChange ? (
      <Notice tone="warn" title="Display frozen · the capture changed" data-testid="live-frozen" data-changed="true">
        {`${frozenChange} The live frame and the scatter stay held; a point of the earlier capture is not pinned. Acquisition, processing and recording go on; pins and the run status keep updating.`}
      </Notice>
    ) : frozen ? (
      <Notice tone="info" title="Display frozen" data-testid="live-frozen">
        The live frame, the cell strip and the scatter are held as they were. Acquisition, processing and recording go on; pins and the run status keep updating.
      </Notice>
    ) : null}
    {readError ? <Notice tone="unknown" title="Live cells not read" data-testid="live-read-error">{readError}</Notice> : null}
    {stopNotice ? <Notice tone={stopNotice.tone} title={stopNotice.title} data-testid="live-stop-notice">{stopNotice.text}</Notice> : null}
  </>;
}

/** The newest frame the live preview pulls, painted at most once per animation frame. */
function LiveFrame({ subscribeFrames, frozen, running }: { subscribeFrames: (sink: FrameSink | null) => void; frozen: boolean; running: boolean }) {
  const canvas = useRef<HTMLCanvasElement | null>(null);
  const latest = useRef<FramePacket | null>(null);
  const frozenRef = useRef(frozen);
  frozenRef.current = frozen;
  const paintPending = useRef(false);
  const [frame, setFrame] = useState<{ index: string; width: number; height: number } | null>(null);
  const labelMs = useRef(-Infinity);
  const paint = useCallback(() => {
    paintPending.current = false;
    const el = canvas.current, p = latest.current;
    if (!el || !p) return;
    if (el.width !== p.width || el.height !== p.height) { el.width = p.width; el.height = p.height; }
    const ctx = el.getContext("2d");
    if (ctx) ctx.putImageData(mono8ToImageData(p.data, p.width, p.height, p.stride_bytes), 0, 0);
    // The label at most four times a second (the pixels every animation frame).
    const now = performance.now();
    if (now - labelMs.current >= 250) { labelMs.current = now; setFrame({ index: p.frame_index, width: p.width, height: p.height }); }
  }, []);
  useEffect(() => {
    subscribeFrames((p) => {
      if (frozenRef.current || !p.valid) return false;
      latest.current = p;
      if (!paintPending.current) {
        paintPending.current = true;
        const raf = typeof window.requestAnimationFrame === "function" ? window.requestAnimationFrame.bind(window) : (cb: () => void) => window.setTimeout(cb, 16);
        raf(() => paint());
      }
      return true;
    });
    return () => subscribeFrames(null);
  }, [subscribeFrames, paint]);
  return (
    <div className="ws-live__frame" data-testid="live-frame">
      <canvas ref={canvas} className="ws-live__canvas" aria-label="Live frame" />
      <span className="ws-live__frame-label">{frame ? `LIVE FRAME · ${frame.index}` : running ? "LIVE FRAME · waiting for a frame" : "LIVE FRAME · the camera is not running"}</span>
    </div>
  );
}

function LiveScatter({ points, pinned, onPick }: {
  points: ReturnType<typeof liveScatter>["points"]; pinned: MonitoringCell | null; onPick?: (index: number) => void;
}) {
  const box = useRef<HTMLDivElement | null>(null);
  const domain = chartDomain(points);
  const at = pinned ? cellOnChart(pinned) : null;
  const click = (e: MouseEvent) => {
    if (!onPick) return;
    const svg = box.current?.querySelector("svg");
    if (!svg) return;
    const r = svg.getBoundingClientRect();
    // Client pixels to chart pixels (the chart is drawn at CHART_W × CHART_H, maybe scaled by CSS).
    const sx = r.width > 0 ? CHART_W / r.width : 1, sy = r.height > 0 ? CHART_H / r.height : 1;
    const i = nearestPoint(points, { x: (e.clientX - r.left) * sx, y: (e.clientY - r.top) * sy }, domain, CHART_W, CHART_H);
    if (i >= 0) onPick(i);
  };
  return (
    <div ref={box} className={`ws-live__chart${onPick ? " ws-live__chart--pick" : ""}`} onClick={click} data-testid="live-chart">
      <AreaDeformChart points={points.map((p) => ({ area: p.area, deformability: p.deformability }))} width={CHART_W} height={CHART_H}
        xDomain={domain.x} yDomain={domain.y} xTicks={domain.xTicks} yTicks={domain.yTicks}
        inspected={at && pinned ? { ...at, label: `${cellName(pinned)} · pinned` } : undefined} />
    </div>
  );
}

function pinFacts(cell: MonitoringCell, listing: MonitoringCellsListing | null) {
  const at = cellOnChart(cell);
  const ended = listing ? isEndedPin(cell, listing) : false;
  const image = cell.image
    ? `owned copy · ${cell.image.width} × ${cell.image.height} px${cell.image.has_mask ? " with mask" : ""}`
    : cell.no_image_reason ?? "none";
  return [
    { label: "Area", value: at ? `${metricNumber(at.area, 1)} µm²` : cell.area === null ? null : `${metricNumber(cell.area, 1)} px² (no calibration)` },
    { label: "Deformability", value: cell.deformability === null ? null : metricNumber(cell.deformability, 3) },
    { label: "E", value: cell.youngs_modulus !== null && cell.youngs_modulus > 0 ? `${metricNumber(cell.youngs_modulus, 2)} kPa` : null },
    { label: "Image", value: image, tone: cell.image ? ("confirmed" as const) : ended ? ("unknown" as const) : ("warn" as const) },
    { label: "Run", value: cell.start_generation === "0" ? "outside a run" : `generation ${cell.start_generation}` },
    { label: "Capture", value: ended ? `${cell.capture_epoch} · ended` : cell.capture_epoch, tone: ended ? ("unknown" as const) : undefined },
  ];
}

/** An app-local box for a pin without an image: the reason, never a picture (the kit's CellImage
 *  says "Image not recorded"; a custom missing text is requested on #715). */
function NoImage({ reason, size }: { reason: string; size: number }) {
  return <div className="ws-live__noimage" style={{ width: size, minHeight: size }} data-testid="live-noimage"><strong>No image</strong><span>{reason}</span></div>;
}

function PinnedCard({ pinned, index, pins, listing, picture, layer, controlHeld, busy, ended, onStep, onOpen, onUnpin, onClearEnded, onClearAll }: {
  pinned: MonitoringCell | null; index: number; pins: MonitoringCell[]; listing: MonitoringCellsListing | null; picture: CellPicture | undefined; layer: Layer;
  controlHeld: boolean; busy: boolean; ended: number;
  onStep: (by: -1 | 1) => void; onOpen: () => void; onUnpin: () => void; onClearEnded: () => void; onClearAll: () => void;
}) {
  const cap = pinCap(listing);
  const why = controlHeld ? undefined : VIEWER_PIN_REASON;
  const endedHere = pinned && listing ? isEndedPin(pinned, listing) : false;
  return (
    <Card title="Pinned cell · live" className="ws-live__pinned" data-testid="live-pinned"
      subtitle={pinned ? `${cellWhere(pinned)} · pin ${index + 1} of ${pins.length}` : "Click a detected cell or a scatter point to pin it"}
      status={<>
        <StateChip tone={pins.length >= cap ? "warn" : "neutral"} data-testid="live-pin-count">{`${pins.length} of ${cap} pins`}</StateChip>
        {ended ? <StateChip tone="unknown" data-testid="live-ended-count">{`${ended} ended`}</StateChip> : null}
      </>}
      actions={<>
        <Button size="small" aria-label="Previous pin" disabled={pins.length < 2} onClick={() => onStep(-1)}>◀</Button>
        <Button size="small" aria-label="Next pin" disabled={pins.length < 2} onClick={() => onStep(1)}>▶</Button>
        <Button size="small" disabled={!pinned} onClick={onOpen} data-testid="live-open-viewer">Open in window</Button>
        <Button size="small" aria-label="Unpin" disabled={!pinned || !controlHeld || busy} title={why} onClick={onUnpin} data-testid="live-unpin">Unpin</Button>
        <Button size="small" disabled={!ended || !controlHeld || busy} title={why ?? "Unpins the pins of captures that have ended; they still count toward the limit"} onClick={onClearEnded} data-testid="live-clear-ended">
          {`Clear ended pins${ended ? ` (${ended})` : ""}`}
        </Button>
        <Button size="small" disabled={!pins.length || !controlHeld || busy} title={why} onClick={onClearAll} data-testid="live-clear-pins">Clear all pins</Button>
      </>}>
      {pinned ? (
        <div className="ws-live__pin">
          {pinned.image === null || endedHere
            ? <NoImage reason={pinned.no_image_reason ?? "no image"} size={120} />
            : <CellImage {...cellImageProps(pinned, picture, layer)} size={120} selected data-testid="live-pinned-image" />}
          <FactList facts={pinFacts(pinned, listing)} data-testid="live-pinned-facts" />
        </div>
      ) : null}
      <p className="ws-note">{`A pin's copy survives ring eviction. With no image (the row had left the buffer, the instrument records none, or its capture ended) the pin keeps its measurements and the reason. Ended pins count toward the ${cap}-pin limit until unpinned.`}</p>
    </Card>
  );
}

function CellViewer({ pins, index, listing, picture, layer, onLayer, controlHeld, busy, onStep, onUnpin, onClose }: {
  pins: MonitoringCell[]; index: number; listing: MonitoringCellsListing | null; picture: CellPicture | undefined; layer: Layer; onLayer: (l: Layer) => void;
  controlHeld: boolean; busy: boolean; onStep: (by: -1 | 1) => void; onUnpin: () => void; onClose: () => void;
}) {
  const cell = pins[index];
  const dialog = useRef<HTMLDivElement | null>(null);
  useEffect(() => { dialog.current?.focus(); }, []);
  const ended = listing ? isEndedPin(cell, listing) : false;
  return (
    <div className="ws-live__backdrop" onClick={onClose}>
      <div ref={dialog} className="ws-live__viewer" role="dialog" aria-modal="true" aria-label={`Pinned cell ${cellName(cell)}`} tabIndex={-1} data-testid="live-viewer-dialog"
        onClick={(e) => e.stopPropagation()}
        onKeyDown={(e) => {
          if (e.key === "Escape") onClose();
          else if (e.key === "ArrowLeft") onStep(-1);
          else if (e.key === "ArrowRight") onStep(1);
        }}>
        <Card title={`Pinned cell · ${cellName(cell)}`} subtitle={`${cellWhere(cell)} · pin ${index + 1} of ${pins.length}`}
          status={<span className="ws-live__layers">
            <Button size="small" variant={layer === "raw" ? "selected" : "secondary"} aria-pressed={layer === "raw"} onClick={() => onLayer("raw")}>Raw</Button>
            <Button size="small" variant={layer === "mask" ? "selected" : "secondary"} aria-pressed={layer === "mask"} onClick={() => onLayer("mask")}>Mask</Button>
          </span>}
          actions={<>
            <Button size="small" aria-label="Previous pin" disabled={pins.length < 2} onClick={() => onStep(-1)}>◀ Previous</Button>
            <Button size="small" aria-label="Next pin" disabled={pins.length < 2} onClick={() => onStep(1)}>Next ▶</Button>
            <Button size="small" disabled={!controlHeld || busy} title={controlHeld ? undefined : VIEWER_PIN_REASON} onClick={onUnpin}>Unpin</Button>
            <Button size="small" variant="primary" onClick={onClose} data-testid="live-viewer-close">Close</Button>
          </>}>
          <div className="ws-live__pin ws-live__pin--large">
            {cell.image === null || ended
              ? <NoImage reason={cell.no_image_reason ?? "no image"} size={256} />
              : <CellImage {...cellImageProps(cell, picture, layer)} size={256} selected />}
            <FactList facts={pinFacts(cell, listing)} />
          </div>
        </Card>
      </div>
    </div>
  );
}

function runChip(s: ExperimentStatus | null): { tone: StateTone; text: string } {
  if (!s?.valid) return { tone: "unknown", text: "Status not read" };
  if (s.state === EXPERIMENT_STATES.Failed || (s.terminal && !s.finalization_ok)) return { tone: "blocked", text: "Failed" };
  if (s.terminal) return { tone: "confirmed", text: "Finalized · file closed" };
  if (s.state === EXPERIMENT_STATES.Stopping || s.flushing) return { tone: "info", text: "Stopping · draining" };
  if (s.state === EXPERIMENT_STATES.Active) return { tone: "info", text: "Recording" };
  if (s.state === EXPERIMENT_STATES.Starting) return { tone: "info", text: "Starting" };
  return { tone: "neutral", text: "No run" };
}

function RunStatusCard({ status }: { status: ExperimentStatus | null }) {
  const counts = runCounts(status);
  const chip = runChip(status);
  const file = status?.valid && status.output_path ? status.output_path : "";
  return (
    <Card title="Recording and run status" className="ws-live__status" data-testid="live-run-status"
      subtitle="Scientific run · HDF5 · one file per run"
      status={<StateChip tone={chip.tone} data-testid="live-run-chip">{chip.text}</StateChip>}>
      <span className="ws-live__file" title={file || undefined} data-testid="live-run-file">{file ? basename(file) : "No run file"}</span>
      {counts ? (
        <div className="ws-live__stats">
          <StatTile label="Valid saved" value={counts.saved} tone="confirmed" />
          <StatTile label="Invalid saved" value={counts.invalidSaved} />
          <StatTile label="Pending" value={counts.pending} />
          <StatTile label="Dropped" value={counts.dropped} tone={counts.dropped === "0" ? "default" : "blocked"} />
          <StatTile label="Failed to save" value={counts.failed} tone={counts.failed === "0" ? "default" : "blocked"} />
        </div>
      ) : null}
      {status?.valid && (status.fault_message || status.completion_reason) ? <p className="ws-note">{status.fault_message || status.completion_reason}</p> : null}
    </Card>
  );
}

function QueueCard({ plan, cur, begun }: { plan: PlanController; cur: number; begun: boolean }) {
  const rows = plan.draft.rows;
  const q = plan.queue.state;
  const slots = conditionSlots(rows);
  const queueMode = plan.draft.format === "queue";
  return (
    <Card title="Sample queue" className="ws-live__queue" data-testid="live-queue"
      subtitle={queueMode
        ? `${cur >= 0 ? `${cur + 1} of ${rows.length}` : begun ? `${rows.length} of ${rows.length} · finished` : `${rows.length} planned`} · each entry is its own run and file`
        : "Single sample · one run and one file"}>
      <div className="ws-live__samples">
        {rows.map((r, i) => {
          const st = queueMode ? rowState(q, r.ref) : null;
          const chip = st ? rowChip(st, q.ended) : plan.started ? { tone: "info" as const, text: plan.started.outcome === "started" ? "Started" : "Start outcome not known" } : { tone: "neutral" as const, text: "Planned" };
          const slot = slots.get(r.condition.trim());
          const what = r.sample.trim() || r.condition.trim();
          return (
            <div key={r.ref} className={`ws-live__sample${i === cur ? " ws-live__sample--current" : ""}`} data-testid={`live-sample-${r.ref}`} aria-current={i === cur ? "step" : undefined}>
              <span className="ws-live__sample-name">{slot ? <ConditionSwatch condition={slot}>{`${r.ref}${what ? ` ${what}` : ""}`}</ConditionSwatch> : `${r.ref}${what ? ` ${what}` : ""}`}</span>
              <StateChip size="small" tone={chip.tone}>{chip.text}</StateChip>
            </div>
          );
        })}
      </div>
    </Card>
  );
}

function LifecycleCard({ plan, curRow, status, onView }: { plan: PlanController; curRow: SampleRow | null; status: ExperimentStatus | null; onView: (v: ExperimentSubView) => void }) {
  const queue = plan.queue;
  const q = queue.state;
  const queueMode = plan.draft.format === "queue";
  let title: string, steps: LifecycleStep[] | null, note: ReactNode = null;
  if (queueMode && queueBegun(q) && curRow) {
    const st = rowState(q, curRow.ref);
    const last = st.attempts[st.attempts.length - 1];
    title = `${describe(curRow)}${last ? ` · run ${last.runIndex}` : ""}`;
    steps = lifecycle(st);
    if (st.phase === "finalized") note = (
      <Button variant="primary" disabled={q.paused !== null || !queue.controlHeld || queue.pending !== null} onClick={queue.confirmClosed} data-testid="live-confirm-closed"
        title="Confirms the finalized run's file is closed (sends nothing)">Confirm closed</Button>
    );
    else if (["planned", "refused", "unknown", "failed", "interrupted"].includes(st.phase)) note = (
      <span className="ws-live__lifecycle-next">
        <span className="ws-note">{st.failure ? `${st.failure.title}. ` : ""}{`${curRow.ref} starts from the Plan: it picks the file and checks readiness with it.`}</span>
        <Button size="small" onClick={() => onView("Plan")} data-testid="live-to-plan">Go to Plan</Button>
      </span>
    );
  } else {
    steps = statusLifecycle(status);
    const ours = !queueMode && plan.started && status?.valid && status.output_path === recordedPath(plan.started.path);
    title = steps === null ? "No run" : ours ? `Run 1 · ${basename(status!.output_path)}` : `A run started outside this plan${status?.output_path ? ` · ${basename(status.output_path)}` : ""}`;
    if (steps === null) note = <span className="ws-live__lifecycle-next"><span className="ws-note">Start a run from the Plan.</span><Button size="small" onClick={() => onView("Plan")} data-testid="live-to-plan">Go to Plan</Button></span>;
    else if (!ours && runInProgress(status)) note = <span className="ws-note">The status shows this run; the classic view or another client started it.</span>;
  }
  return (
    <Card title="Run lifecycle" className="ws-live__lifecycle" data-testid="live-lifecycle"
      subtitle={`${title} · distinct states; a step is done only when the backend confirmed it`}>
      {steps ? <Stepper steps={steps.map((s) => ({
        state: s.state,
        label: <span className="ws-queue-step">{s.label}{s.flag ? <StateChip size="small" tone={s.flag.tone}>{s.flag.text}</StateChip> : null}</span>,
      }))} data-testid="live-stepper" /> : null}
      {note}
    </Card>
  );
}
