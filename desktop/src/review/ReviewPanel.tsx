import { noImagesNotice } from "./noImages";
// The HDF5 review module (UI-4, #269; plan 2026-10-01-standalone-review-app).
//
// YOFO Review (`review.html` → `ReviewApp.tsx`) mounts this panel as its
// Inspect stage. MIB Studio does not mount it yet: its Review tab is built
// inline in `App.tsx` (develop #450, ADR 0014 amendment); mounting it there
// is #512 step B5. The panel owns its review state and the "review" and
// "viewer" slots of the shared FramePullScheduler, and talks only to the
// review bridge (`reviewBridge.ts` → src-tauri/src/review.rs →
// ReviewSession), so the review-only product needs no backend bridge at all.
// Backend calls, dialogs and the opener go through `../transport` (#512 S9),
// so the module is the same over Tauri and the WebSocket.
//
// Layout: R3 Inspect on the @mib/ui kit (#717 B3a; Penpot R3 board
// dcdbaa6b-9790-4e69-bea4-936747f588b1). A toolbar with the sub-views
// Scatter / Records / Frames and the set (Valid / Invalid, or "Frames" for a
// recording file), then the file actions and exports; a second row with the
// overlay and ROI the backend draws. The left column is the sub-view: the
// deformability scatter over the ring-width histogram (`charts/ChartsView`,
// experiment files; clicking a point selects that valid record), the
// virtualised thumbnail grid, and under both the paged records table; the
// Records sub-view shows the table alone. The right column is the selected
// record (`inspect/InspectCards.tsx`: frame and object, Raw / Contour / Mask
// crops or the full frame, stored values, what is recorded) over what the
// stored results came from. Selection is shared by scatter, grid, table and
// record card; Record details, a double-click on the full frame or a
// thumbnail, or Enter opens the frame viewer (series, zoom, prev/next).
// Export dialogs with progress / cancel / series range are in `exports/`.
import { forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState } from "react";
import { open, save } from "../transport/dialogs";
import type { FramePacket } from "../framePacket";
import type { FramePullScheduler } from "../framePullScheduler";
import { REVIEW_OPERATION_KINDS } from "./reviewContract";
import { ChartsView } from "./charts/ChartsView";
import { gatherChartInputs, renderChartPngs, stageCharts, type ChartViewState } from "./charts/chartExport";
import { JobDialog, SeriesPrompt } from "./exports/ExportDialogs";
import { defaultMetricsName, dirName, exportDir, joinPath, rememberExportDir, seriesRangeFor, type SeriesChoice } from "./exports/exportHelpers";
import { useReviewJobs, type TrackedJob } from "./exports/useReviewJobs";
import { RegenerateMasksDialog, type RegenerateRequest } from "./RegenerateMasks";
import { FrameViewer } from "./FrameViewer";
import { DEFAULT_COLUMNS, METRIC_COLUMNS, pageOffsetFor, toggleColumn, visibleColumns } from "./metricsColumns";
import {
  OVERLAY,
  REVIEW_DATASET,
  packetToImageData,
  reviewBridge,
  type OverlayMode,
  type ReviewInfo,
  type ReviewRow,
  type ReviewRows,
} from "./reviewBridge";
import { ThumbnailGrid } from "./ThumbnailGrid";
import { Button, Card, DataTable, Notice, Toolbar, type DataTableColumn } from "@mib/ui";
import { RecordCard, StoredCard, type CropState } from "./inspect/InspectCards";
import { cropOverlay, cropSquare, type CellView } from "./inspect/inspectModel";
import { cropPacketToDataUrl } from "./inspect/packetCrop";
import "./review.css";
import "./inspect/inspect.css";

export const H5_FILTER = [{ name: "HDF5", extensions: ["h5", "hdf5"] }];
export const METRICS_PAGE_SIZE = 100;
const COLUMNS_KEY = "yofo.review.columns";

const H5_MULTI = [{ name: "HDF5", extensions: ["h5", "hdf5"] }];

const OVERLAY_LABELS: { mode: OverlayMode; label: string }[] = [
  { mode: OVERLAY.None, label: "Overlay: None" },
  { mode: OVERLAY.AllContour, label: "Overlay: Contours" },
  { mode: OVERLAY.OuterInnerColorCoded, label: "Overlay: Outer/Inner" },
  { mode: OVERLAY.AllMask, label: "Overlay: Mask" },
  { mode: OVERLAY.FilteredMask, label: "Overlay: Filtered mask" },
];

export interface ReviewPanelProps {
  /** Backend initialized (open is refused otherwise). */
  ready: boolean;
  /** Shared scheduler; the panel mounts its "review" and "viewer" slots. */
  scheduler: FramePullScheduler;
  /** Image scaling preference shared with the host's live canvases. */
  fitWindow: boolean;
  /** Log drawer sink. */
  log: (line: string) => void;
  /** Called before an open replaces the current source (MIB Studio stops the
   * live preview loop here). */
  beforeLoad?: () => void;
  /** The loaded file path changed (host invalidates its scheduler views). */
  onFileChange?: (path: string) => void;
  /** Fresh info after an open or close (workflow facts, status bar). */
  onInfo?: (info: ReviewInfo | null) => void;
  /** px→µm used when a file records none (TD-17): YOFO Review's
   * preference, MIB Studio's processing factor. Applied on change. */
  fallbackPixelToMicron?: number;
}

/** How an open ended: `message` is the backend's reason when `ok` is false. */
export interface OpenOutcome {
  ok: boolean;
  message: string;
}

export interface ReviewPanelHandle {
  /** The File ▸ Open… action. */
  openFile: () => Promise<void>;
  /** Open a known path (command line, file association, R1 Open's library,
   * test harness). */
  openPath: (path: string) => Promise<OpenOutcome>;
  /** The File ▸ Close action. */
  closeFile: () => Promise<void>;
}

/** Inspect's sub-views (R3: Scatter, Records, Frames). */
export type InspectView = "scatter" | "records" | "frames";

function loadColumns(): string[] {
  try {
    const raw = localStorage.getItem(COLUMNS_KEY);
    const ids = raw ? (JSON.parse(raw) as unknown) : null;
    if (Array.isArray(ids) && ids.every((x) => typeof x === "string") && ids.length > 0) {
      const known = new Set(METRIC_COLUMNS.map((c) => c.id));
      const kept = ids.filter((x) => known.has(x));
      if (kept.length > 0) return kept;
    }
  } catch {
    // Private window or blocked storage: defaults.
  }
  return DEFAULT_COLUMNS;
}

export const ReviewPanel = forwardRef<ReviewPanelHandle, ReviewPanelProps>(function ReviewPanel(props, ref) {
  const { ready, scheduler, fitWindow, log, beforeLoad, onFileChange, onInfo, fallbackPixelToMicron } = props;
  const jobs = useReviewJobs(ready, log);
  const [seriesAsk, setSeriesAsk] = useState<{ count: number; resolve: (c: SeriesChoice | null) => void } | null>(null);
  const [showRegenerate, setShowRegenerate] = useState(false);
  const [showMore, setShowMore] = useState(false);
  const chartView = useRef<ChartViewState | null>(null);
  const onChartViewChange = useCallback((v: ChartViewState) => {
    chartView.current = v;
  }, []);

  const [info, setInfo] = useState<ReviewInfo | null>(null);
  const [fileKey, setFileKey] = useState("");
  const [subView, setSubView] = useState<InspectView>("frames");
  const [validSet, setValidSet] = useState(true);
  const [cellView, setCellView] = useState<CellView>("full");
  const [crop, setCrop] = useState<CropState>({ status: "idle" });
  const [rows, setRows] = useState<ReviewRows | null>(null);
  const [rowsOffset, setRowsOffset] = useState(0);
  const [selected, setSelected] = useState(-1);
  const [viewerOpen, setViewerOpen] = useState(false);
  const [overlay, setOverlay] = useState<OverlayMode>(OVERLAY.None);
  const [roiOverlay, setRoiOverlay] = useState(false);
  const [columns, setColumns] = useState<string[]>(loadColumns);
  const [showColumns, setShowColumns] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const rowsRef = useRef<ReviewRows | null>(null);
  rowsRef.current = rows;

  const reviewing = !!info?.file_open;
  const isRecording = !!info?.recording_file;
  const setTotal = !info ? 0 : Number(validSet ? info.valid_images.count || info.recorded_images.count : info.invalid_images.count);
  const rowTotal = Number(rows?.total ?? 0);
  const selectedRow: ReviewRow | null =
    rows && selected >= rowsOffset && selected < rowsOffset + rows.rows.length ? rows.rows[selected - rowsOffset] : null;
  const shownColumns = visibleColumns(columns, isRecording);
  // The crop views cut the selected record's box out of the frame packet.
  const cropView = cellView !== "full";
  const cellViewRef = useRef(cellView);
  cellViewRef.current = cellView;
  const selectedRowRef = useRef(selectedRow);
  selectedRowRef.current = selectedRow;
  const cropKey = cropView && selectedRow ? `${selectedRow.frame_index}:${selectedRow.object_id}:${selectedRow.bbox_x},${selectedRow.bbox_y},${selectedRow.bbox_width},${selectedRow.bbox_height}` : "";

  // Draw only the pixels owned by this exact immutable pull response: the
  // full frame into the canvas, or the record's crop as the CellImage's URL.
  const draw = useCallback((p: FramePacket) => {
    if (cellViewRef.current !== "full") {
      const row = selectedRowRef.current;
      const rect = p.valid && row ? cropSquare(row, p.width, p.height) : null;
      const src = rect ? cropPacketToDataUrl(p, rect) : null;
      setCrop(src ? { status: "ready", src } : { status: "failed" });
      return;
    }
    const canvas = canvasRef.current;
    if (!p.valid || !canvas) return;
    canvas.width = p.width;
    canvas.height = p.height;
    canvas.getContext("2d")?.putImageData(packetToImageData(p), 0, 0);
  }, []);

  useEffect(() => scheduler.mount("review", draw, (e) => log(`review frame error: ${e}`)), [scheduler, draw, log]);

  useEffect(() => {
    try {
      localStorage.setItem(COLUMNS_KEY, JSON.stringify(columns));
    } catch {
      // Storage unavailable: the choice lasts for this session only.
    }
  }, [columns]);

  const loadRows = useCallback(
    async (valid: boolean, offset: number) => {
      try {
        const page = await reviewBridge.rows(valid, offset, METRICS_PAGE_SIZE);
        if (page.valid) {
          setRows(page);
          setRowsOffset(offset);
        }
      } catch (e) {
        log(`metrics page error: ${e}`);
      }
    },
    [log],
  );

  // The record card's image follows the selection, its view and (full
  // frame) the overlay / ROI choice. A crop waits for the record's row (its
  // bounding box); the backend draws the overlay either way.
  useEffect(() => {
    if (!reviewing || selected < 0 || selected >= setTotal) return;
    const dataset = validSet ? REVIEW_DATASET.ValidImage : REVIEW_DATASET.InvalidImage;
    if (cellView === "full") {
      scheduler.request("review", () => reviewBridge.frame(dataset, selected, overlay, roiOverlay));
      return;
    }
    setCrop({ status: "loading" });
    if (!cropKey) {
      scheduler.invalidate("review");
      return;
    }
    const mode = cropOverlay(cellView);
    scheduler.request("review", () => reviewBridge.frame(dataset, selected, mode, false));
  }, [reviewing, selected, setTotal, validSet, overlay, roiOverlay, cellView, cropKey, scheduler]);

  // A view a record cannot show (a raw recording has no objects to crop)
  // falls back to the full frame.
  useEffect(() => {
    if (isRecording && cellView !== "full") setCellView("full");
  }, [isRecording, cellView]);

  // The table page follows the selection.
  const select = useCallback(
    (index: number) => {
      setSelected(index);
      const offset = pageOffsetFor(index, METRICS_PAGE_SIZE);
      const current = rowsRef.current;
      if (!current || offset !== Number(current.offset)) void loadRows(validSet, offset);
    },
    [loadRows, validSet],
  );

  const resetView = useCallback(() => {
    scheduler.invalidate("review");
    scheduler.invalidate("viewer");
    setRows(null);
    setRowsOffset(0);
    setSelected(-1);
    setViewerOpen(false);
    setCrop({ status: "idle" });
    const canvas = canvasRef.current;
    canvas?.getContext("2d")?.clearRect(0, 0, canvas.width, canvas.height);
  }, [scheduler]);

  // The set (Valid / Invalid): its first page and record. The scatter shows
  // valid records only, so the invalid set shows its frames.
  const selectSet = useCallback(
    async (valid: boolean) => {
      resetView();
      setValidSet(valid);
      if (!valid) setSubView((v) => (v === "scatter" ? "frames" : v));
      await loadRows(valid, 0);
      setSelected(0);
    },
    [loadRows, resetView],
  );

  // A sub-view only changes what is shown; the selection stays. The scatter
  // (valid records) from the invalid set starts on the valid set with no
  // record selected: a click on a point selects one.
  const selectView = useCallback(
    async (view: InspectView) => {
      setSubView(view);
      if (view === "scatter" && !validSet) {
        resetView();
        setValidSet(true);
        await loadRows(true, 0);
      }
    },
    [loadRows, resetView, validSet],
  );

  const closeFile = useCallback(async () => {
    try {
      await reviewBridge.close();
    } catch (e) {
      log(`close error: ${e}`);
    }
    resetView();
    setInfo(null);
    setFileKey("");
    setSubView("frames");
    setValidSet(true);
    onInfo?.(null);
    onFileChange?.("");
  }, [log, onFileChange, onInfo, resetView]);

  const openPath = useCallback(
    async (path: string): Promise<OpenOutcome> => {
      beforeLoad?.();
      resetView();
      onFileChange?.(path);
      try {
        const res = await reviewBridge.open(path);
        if (!res.ok) {
          log(`open failed: ${res.message}`);
          // The backend closed the previous file before it tried this one
          // (ReviewSession::open resets first): nothing is open now.
          setInfo(null);
          setFileKey("");
          onInfo?.(null);
          onFileChange?.("");
          return { ok: false, message: res.message };
        }
        const fresh = await reviewBridge.info();
        setInfo(fresh);
        setFileKey(`${path}#${Date.now()}`);
        onInfo?.(fresh);
        log(`opened ${path}${fresh.accounting_summary}`);
        // Start on the valid set, or the invalid one when there are no valid
        // frames (a regenerated file can have none).
        const startTab = initialTab(fresh);
        setSubView("frames");
        setValidSet(startTab !== "invalid");
        await loadRows(startTab !== "invalid", 0);
        setSelected(setTotalFor(fresh, startTab !== "invalid") > 0 ? 0 : -1);
        return { ok: true, message: "" };
      } catch (e) {
        log(`open error: ${e}`);
        return { ok: false, message: String(e) };
      }
    },
    [beforeLoad, loadRows, log, onFileChange, onInfo, resetView],
  );

  const refreshInfo = useCallback(async () => {
    try {
      const fresh = await reviewBridge.info();
      if (!fresh.file_open) return;
      setInfo(fresh);
      onInfo?.(fresh);
    } catch (e) {
      log(`info error: ${e}`);
    }
  }, [log, onInfo]);

  const openFile = useCallback(async () => {
    const picked = await open({ title: "Open recording", filters: H5_FILTER, multiple: false });
    if (typeof picked === "string") await openPath(picked);
  }, [openPath]);

  useImperativeHandle(ref, () => ({ openFile, openPath, closeFile }), [openFile, openPath, closeFile]);

  // A new file starts from an unopened Charts view (data extent, prefs).
  useEffect(() => {
    chartView.current = null;
  }, [fileKey]);

  // TD-17 fallback factor: files without a recorded factor use it; the info
  // (and scatter µm²) follow a change.
  useEffect(() => {
    if (!ready || !(fallbackPixelToMicron !== undefined && Number.isFinite(fallbackPixelToMicron) && fallbackPixelToMicron > 0)) return;
    void reviewBridge
      .setPixelToMicron(fallbackPixelToMicron)
      .then(() => refreshInfo())
      .catch((e) => log(`px→µm fallback error: ${e}`));
  }, [ready, fallbackPixelToMicron, refreshInfo, log]);

  // ---- jobs -----------------------------------------------------------------
  // One job at a time (the backend refuses a second); each runs behind the
  // progress dialog with Cancel; successful output directories are
  // remembered (Qt QSettings → localStorage).
  const isExperiment = reviewing && !isRecording;
  const imagesPerRecord = info?.has_series ? Number(info.series_count) : info?.multi_image_enabled ? Number(info.multi_image_count) : 0;
  const hasSeries = !!info?.has_series || imagesPerRecord > 1;

  const askSeries = useCallback(
    (count: number) => new Promise<SeriesChoice | null>((resolve) => setSeriesAsk({ count, resolve })),
    [],
  );

  const remember = (job: TrackedJob | null, dir: string) => {
    if (job?.phase === "completed") rememberExportDir(dir);
  };

  // Chart snapshots over the whole run with the Charts view's toggles and
  // contours (defaults when never opened); staged for the next Export All /
  // Export Charts.
  const stageChartSnapshots = useCallback(async () => {
    if (!info) return;
    await stageCharts(await renderChartPngs(await gatherChartInputs(info, chartView.current)));
  }, [info]);

  const onExportMetrics = useCallback(async () => {
    if (!info) return;
    try {
      const dir = exportDir(info.file_path);
      const name = defaultMetricsName(info.file_path, dir ? await reviewBridge.listDir(dir) : []);
      const picked = await save({ title: "Export Metrics to CSV", filters: [{ name: "CSV", extensions: ["csv"] }], defaultPath: joinPath(dir, name) });
      if (!picked) return;
      remember(await jobs.start("Export Metrics", REVIEW_OPERATION_KINDS.ExportMetrics, () => reviewBridge.exportMetrics(picked)), dirName(picked));
    } catch (e) {
      log(`export error: ${e}`);
    }
  }, [info, jobs, log]);

  const onExportAll = useCallback(async () => {
    if (!info) return;
    try {
      const choice = hasSeries ? await askSeries(imagesPerRecord) : ({ kind: "all" } as SeriesChoice);
      if (!choice) return;
      const root = await open({ title: "Export All — choose the output root", directory: true, multiple: false, defaultPath: exportDir(info.file_path) || undefined });
      if (typeof root !== "string") return;
      // Recording files: images only, no charts.
      const prepare = { label: "Rendering charts…", work: isExperiment ? stageChartSnapshots : () => stageCharts([]) };
      remember(await jobs.start("Export All", REVIEW_OPERATION_KINDS.ExportAll, () => reviewBridge.exportAll(root, seriesRangeFor(choice)), prepare), root);
    } catch (e) {
      log(`export error: ${e}`);
    }
  }, [askSeries, hasSeries, imagesPerRecord, info, isExperiment, jobs, log, stageChartSnapshots]);

  const onExportCharts = useCallback(async () => {
    if (!info) return;
    try {
      const dir = await open({ title: "Select Directory to Export Charts", directory: true, multiple: false, defaultPath: exportDir(info.file_path) || undefined });
      if (typeof dir !== "string") return;
      const prepare = { label: "Rendering charts…", work: stageChartSnapshots };
      remember(await jobs.start("Export Charts", REVIEW_OPERATION_KINDS.ExportCharts, () => reviewBridge.exportCharts(dir), prepare), dir);
    } catch (e) {
      log(`chart export error: ${e}`);
    }
  }, [info, jobs, log, stageChartSnapshots]);

  const onBatch = useCallback(
    async (metricsOnly: boolean) => {
      try {
        const picked = await open({ title: metricsOnly ? "Batch Metrics — choose files" : "Batch Export All — choose files", filters: H5_MULTI, multiple: true });
        const sources = Array.isArray(picked) ? picked : typeof picked === "string" ? [picked] : [];
        if (sources.length === 0) return;
        // One series choice for every multi-image record of every file.
        const choice = metricsOnly ? ({ kind: "all" } as SeriesChoice) : await askSeries(0);
        if (!choice) return;
        const root = await open({ title: "Choose the output root", directory: true, multiple: false, defaultPath: exportDir(info?.file_path ?? sources[0]) || undefined });
        if (typeof root !== "string") return;
        const title = metricsOnly ? "Batch Metrics" : "Batch Export All";
        remember(await jobs.start(title, REVIEW_OPERATION_KINDS.BatchExport, () => reviewBridge.batchExport(sources, root, metricsOnly, seriesRangeFor(choice))), root);
      } catch (e) {
        log(`batch error: ${e}`);
      }
    },
    [askSeries, info, jobs, log],
  );

  // Regenerate never replaces a file (#512 S8, D6): a refusal (an existing
  // output) goes back to the dialog, which stays open so the operator can
  // choose another name; the dialog closes once the job has started.
  const onRegenerate = useCallback(
    (r: RegenerateRequest) =>
      new Promise<string | null>((settle) => {
        jobs
          .start("Regenerate masks", REVIEW_OPERATION_KINDS.RegenerateMasks, () => reviewBridge.regenerateMasks(r), undefined, {
            onRefused: (message) => settle(message),
            onStarted: () => {
              settle(null);
              setShowRegenerate(false);
            },
          })
          .then(async (job) => {
            // The Qt tab reloads from the regenerated file.
            if (job?.phase === "completed") {
              rememberExportDir(dirName(r.outputPath));
              await openPath(r.outputPath);
            }
          })
          .catch((e) => {
            log(`regenerate error: ${e}`);
            settle(String(e));
          });
      }),
    [jobs, log, openPath],
  );

  const pageTo = (offset: number) => void loadRows(validSet, Math.max(0, offset));
  const openViewer = () => selected >= 0 && setViewerOpen(true);
  const step = (delta: number) => {
    const next = selected + delta;
    if (selected >= 0 && next >= 0 && next < setTotal) select(next);
  };

  // The records table: today's metric columns on the kit's DataTable, one
  // page at a time (the kit has no paging, #715); a double-click on a row
  // opens the viewer as before.
  type PageRow = { row: ReviewRow; index: number };
  const pageRows: PageRow[] = (rows?.rows ?? []).map((row, k) => ({ row, index: rowsOffset + k }));
  const tableColumns: DataTableColumn<PageRow>[] = shownColumns.map((c) => ({
    key: c.id,
    header: c.label,
    cell: (r) => c.format(r.row),
    emphasis: c.id === "frame_index",
  }));
  const setName = isRecording ? "Frames" : validSet ? "Valid records" : "Invalid records";
  const recordsCard = (
    <Card
      className="rv-records-card"
      title="Records"
      subtitle={`${setName} · one row per stored record · ${rowTotal > 0 ? `${rowsOffset + 1}–${Math.min(rowTotal, rowsOffset + METRICS_PAGE_SIZE)} of ${rowTotal}` : "no rows"}`}
    >
      <div className="table-toolbar">
        <Button size="small" aria-label="Previous page" disabled={rowsOffset === 0} onClick={() => pageTo(rowsOffset - METRICS_PAGE_SIZE)}>◀</Button>
        <Button size="small" aria-label="Next page" disabled={rowsOffset + METRICS_PAGE_SIZE >= rowTotal} onClick={() => pageTo(rowsOffset + METRICS_PAGE_SIZE)}>▶</Button>
        <span className="right" />
        {!isRecording && (
          <div className="column-chooser">
            <Button size="small" aria-expanded={showColumns} onClick={() => setShowColumns((s) => !s)}>Columns…</Button>
            {showColumns && (
              <div className="menu-popup column-popup" role="menu">
                {METRIC_COLUMNS.filter((c) => !c.recording || c.id === "frame_index").map((c) => (
                  <label key={c.id}>
                    <input type="checkbox" checked={columns.includes(c.id)} onChange={() => setColumns((ids) => toggleColumn(ids, c.id))} /> {c.label}
                  </label>
                ))}
                <button onClick={() => setColumns(DEFAULT_COLUMNS)}>Reset</button>
              </div>
            )}
          </div>
        )}
      </div>
      <div
        className="table-panel"
        onDoubleClick={(e) => {
          if ((e.target as Element).closest?.("tbody tr.mib-table__row")) openViewer();
        }}
      >
        <DataTable
          className="metrics-table"
          density="compact"
          columns={tableColumns}
          rows={pageRows}
          rowKey={(r) => String(r.index)}
          selectedKey={selected >= 0 ? String(selected) : undefined}
          onSelect={(_, r) => select(r.index)}
          empty="No metric rows in this set."
        />
      </div>
    </Card>
  );

  const counts = (valid: boolean) => (reviewing ? ` (${setTotalFor(info, valid)})` : "");
  const imagesMissing = !!info && !(validSet ? (isRecording ? info.recorded_images : info.valid_images) : info.invalid_images).present;

  return (
    <>
      <Toolbar className="rv-inspect-toolbar" aria-label="Inspect" title={reviewing && info ? `Inspect · ${setName.toLowerCase()}` : "Inspect"}>
        {!isRecording && (
          <Button
            variant={subView === "scatter" ? "selected" : "secondary"}
            aria-pressed={subView === "scatter"}
            disabled={!reviewing || setTotalFor(info, true) === 0}
            title="Area–deformability scatter and the ring-width histogram (valid records)"
            onClick={() => void selectView("scatter")}
          >
            Scatter
          </Button>
        )}
        <Button variant={subView === "records" ? "selected" : "secondary"} aria-pressed={subView === "records"} disabled={!reviewing} onClick={() => void selectView("records")}>
          Records
        </Button>
        <Button variant={subView === "frames" ? "selected" : "secondary"} aria-pressed={subView === "frames"} disabled={!reviewing} onClick={() => void selectView("frames")}>
          Frames
        </Button>
        <span className="rv-toolbar-gap" aria-hidden="true" />
        <Button
          variant={reviewing && validSet ? "selected" : "secondary"}
          aria-pressed={reviewing && validSet}
          disabled={!reviewing || setTotalFor(info, true) === 0}
          onClick={() => void selectSet(true)}
        >
          {isRecording ? "Frames" : "Valid Frames"}{counts(true)}
        </Button>
        {!isRecording && (
          <Button
            variant={reviewing && !validSet ? "selected" : "secondary"}
            aria-pressed={reviewing && !validSet}
            disabled={!reviewing || setTotalFor(info, false) === 0}
            onClick={() => void selectSet(false)}
          >
            Invalid Frames{counts(false)}
          </Button>
        )}
      </Toolbar>
      <div className="toolbar rv-inspect-actions">
        <Button size="small" onClick={openFile} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
          Select HDF File…
        </Button>
        <Button size="small" onClick={closeFile} disabled={!reviewing} title={reviewing ? "Close the current file" : "No file loaded"}>
          Close File
        </Button>
        <Button
          size="small"
          onClick={onExportMetrics}
          disabled={!isExperiment || jobs.busy}
          title={isRecording ? "Recording files carry no metrics" : "Write the metrics CSV (recorded px→µm factor)"}
        >
          Export Metrics to CSV…
        </Button>
        <Button size="small" onClick={onExportAll} disabled={!reviewing || jobs.busy} title="Metrics, images, series and charts into <root>/<file>/">
          Export All…
        </Button>
        <div className="more-menu">
          <Button size="small" aria-haspopup="menu" aria-expanded={showMore} onClick={() => setShowMore((v) => !v)} onBlur={() => window.setTimeout(() => setShowMore(false), 150)}>
            More…
          </Button>
          {showMore && (
            <div className="menu-popup" role="menu">
              <button role="menuitem" disabled={!ready || jobs.busy} onClick={() => void onBatch(true)}>
                Batch Metrics…
              </button>
              <button role="menuitem" disabled={!ready || jobs.busy} onClick={() => void onBatch(false)}>
                Batch Export All…
              </button>
              <button role="menuitem" disabled={!isExperiment || jobs.busy} title={isRecording ? "Recording files carry no metrics to chart" : undefined} onClick={() => void onExportCharts()}>
                Export Charts…
              </button>
              <button role="menuitem" disabled={!ready || jobs.busy} onClick={() => setShowRegenerate(true)}>
                Regenerate masks…
              </button>
            </div>
          )}
        </div>
        <span className="rv-toolbar-gap" aria-hidden="true" />
        <select
          value={overlay}
          disabled={!reviewing || isRecording}
          title={isRecording ? "Recording files carry no masks" : "Overlay drawn by the backend on thumbnails, the full frame and the viewer"}
          onChange={(e) => setOverlay(Number(e.target.value) as OverlayMode)}
          aria-label="Overlay mode"
        >
          {OVERLAY_LABELS.map((o) => (
            <option key={o.mode} value={o.mode}>{o.label}</option>
          ))}
        </select>
        <label title={isRecording ? "Recording files carry no ROI" : "Draw the recorded ROI"}>
          <input type="checkbox" checked={roiOverlay} disabled={!reviewing || isRecording} onChange={(e) => setRoiOverlay(e.target.checked)} /> ROI
        </label>
        <span className="legend">
          <span className="chip"><span className="swatch" style={{ background: "#2b6cb0" }} /> Target</span>
          <span className="chip"><span className="swatch" style={{ background: "#1a7f37" }} /> Valid</span>
          <span className="chip"><span className="swatch" style={{ background: "#b42318" }} /> Invalid</span>
        </span>
      </div>
      {noImagesNotice(info) && (
        <Notice tone="unknown" title="Images not recorded" data-testid="review-no-images">
          {noImagesNotice(info)}
        </Notice>
      )}
      <div className="subtab-body rv-inspect-body">
        {!reviewing || !info ? (
          <div className="review-empty">
            <Notice
              tone="info"
              title="No run open"
              actions={
                <Button variant="primary" onClick={openFile} disabled={!ready}>
                  Select HDF File…
                </Button>
              }
            >
              Open a run from the Open stage, or choose a file.
            </Notice>
          </div>
        ) : (
          <div className="rv-inspect-grid">
            <div className="rv-inspect-main">
              {subView === "scatter" && !isRecording ? (
                <Card className="rv-scatter-card" title="Area–Deformability" subtitle="Valid records · click a point to inspect its record · double-click empty space to reset the zoom">
                  <ChartsView
                    info={info}
                    fileKey={fileKey}
                    selected={validSet ? selected : -1}
                    validTotal={setTotalFor(info, true)}
                    onSelect={select}
                    refreshInfo={refreshInfo}
                    onViewChange={onChartViewChange}
                    log={log}
                  />
                </Card>
              ) : subView === "frames" ? (
                <Card className="rv-frames-card" title="Frames" subtitle={`${setName} · ${setTotal} stored · double-click a frame to open the viewer`}>
                  <div className="review-frames">
                    <ThumbnailGrid
                      valid={validSet}
                      total={setTotal}
                      overlay={isRecording ? OVERLAY.None : overlay}
                      roiOverlay={!isRecording && roiOverlay}
                      selected={selected}
                      fileKey={fileKey}
                      imageWidth={imageDims(info, validSet).w}
                      imageHeight={imageDims(info, validSet).h}
                      onSelect={select}
                      onOpen={(i) => {
                        select(i);
                        setViewerOpen(true);
                      }}
                      log={log}
                    />
                  </div>
                </Card>
              ) : null}
              {recordsCard}
            </div>
            <div className="rv-inspect-side">
              <RecordCard
                info={info}
                valid={validSet}
                position={selected}
                total={setTotal}
                row={selectedRow}
                view={cellView}
                onView={setCellView}
                canvasRef={canvasRef}
                fitWindow={fitWindow}
                crop={crop}
                imagesMissing={imagesMissing}
                onPrevious={() => step(-1)}
                onNext={() => step(1)}
                onDetails={openViewer}
                emptyHint={subView === "scatter" ? "Click a point to inspect its record" : "Select a record"}
              />
              <StoredCard info={info} onReprocess={() => setShowRegenerate(true)} reprocessDisabled={!ready || jobs.busy} />
            </div>
          </div>
        )}
      </div>
      {seriesAsk && (
        <SeriesPrompt
          imagesPerRecord={seriesAsk.count || undefined}
          onChoose={(c) => {
            seriesAsk.resolve(c);
            setSeriesAsk(null);
          }}
          onCancel={() => {
            seriesAsk.resolve(null);
            setSeriesAsk(null);
          }}
        />
      )}
      {showRegenerate && <RegenerateMasksDialog info={info} onRun={onRegenerate} onCancel={() => setShowRegenerate(false)} />}
      {jobs.job && <JobDialog job={jobs.job} onCancel={jobs.cancel} onClose={jobs.dismiss} />}
      {viewerOpen && reviewing && selected >= 0 && (
        <FrameViewer
          scheduler={scheduler}
          valid={validSet}
          index={selected}
          total={setTotal}
          overlay={isRecording ? OVERLAY.None : overlay}
          roiOverlay={!isRecording && roiOverlay}
          row={selectedRow}
          onIndexChange={select}
          onClose={() => setViewerOpen(false)}
          log={log}
        />
      )}
    </>
  );
});

function imageDims(info: ReviewInfo | null, valid: boolean): { w: number; h: number } {
  if (!info) return { w: 0, h: 0 };
  const d = valid ? (info.recording_file ? info.recorded_images : info.valid_images) : info.invalid_images;
  return { w: d.width, h: d.height };
}

/** The set a freshly opened file shows first. */
export function initialTab(info: ReviewInfo): "valid" | "invalid" {
  return setTotalFor(info, true) === 0 && !info.recording_file && setTotalFor(info, false) > 0 ? "invalid" : "valid";
}

export function setTotalFor(info: ReviewInfo | null, valid: boolean): number {
  if (!info) return 0;
  if (valid) return Number(info.recording_file ? info.recorded_images.count : info.valid_images.count);
  return Number(info.invalid_images.count);
}
