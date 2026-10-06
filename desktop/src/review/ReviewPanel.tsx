// The HDF5 review module (UI-4, #269; plan 2026-10-01-standalone-review-app).
//
// One React tree serves two products: MIB Studio mounts this panel in its
// Review tab, YOFO Review (`review.html` → `ReviewApp.tsx`) mounts it as the
// whole window. The panel owns its review state and the "review" and
// "viewer" slots of the shared FramePullScheduler, and talks only to the
// review bridge (`reviewBridge.ts` → src-tauri/src/review.rs →
// ReviewSession), so the review-only product needs no backend bridge at all.
//
// Layout (the Qt HdfReviewTab): per set (Valid / Invalid, or "Frames" for a
// recording file) a virtualised thumbnail grid on the left, the selected
// frame's preview over the paged metrics table on the right; selection is
// shared by grid, table and preview; double-click / Enter opens the frame
// viewer (series, zoom, prev/next). Charts (experiment files): the
// deformability scatter, the same frame pane and the ring-width histogram
// (`charts/ChartsView.tsx`, PR 3); clicking a point selects that valid cell.
// Export dialogs with progress / cancel / series range arrive with PR 4.

import { forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
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
import "./review.css";

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

export interface ReviewPanelHandle {
  /** The File ▸ Open… action. */
  openFile: () => Promise<void>;
  /** Open a known path (command line, file association, test harness). */
  openPath: (path: string) => Promise<void>;
  /** The File ▸ Close action. */
  closeFile: () => Promise<void>;
}

type ReviewTab = "valid" | "invalid" | "charts";

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
  const [reviewTab, setReviewTab] = useState<ReviewTab>("valid");
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
  const validSet = reviewTab !== "invalid";
  const setTotal = !info ? 0 : Number(validSet ? info.valid_images.count || info.recorded_images.count : info.invalid_images.count);
  const rowTotal = Number(rows?.total ?? 0);
  const selectedRow: ReviewRow | null =
    rows && selected >= rowsOffset && selected < rowsOffset + rows.rows.length ? rows.rows[selected - rowsOffset] : null;
  const shownColumns = visibleColumns(columns, isRecording);

  // Draw only the pixels owned by this exact immutable pull response.
  const draw = useCallback((p: FramePacket) => {
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

  // Preview follows the selection and the overlay / ROI choice.
  useEffect(() => {
    if (!reviewing || selected < 0 || selected >= setTotal) return;
    scheduler.request("review", () =>
      reviewBridge.frame(validSet ? REVIEW_DATASET.ValidImage : REVIEW_DATASET.InvalidImage, selected, overlay, roiOverlay),
    );
  }, [reviewing, reviewTab, selected, setTotal, validSet, overlay, roiOverlay, scheduler]);

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
    const canvas = canvasRef.current;
    canvas?.getContext("2d")?.clearRect(0, 0, canvas.width, canvas.height);
  }, [scheduler]);

  const selectTab = useCallback(
    async (tab: ReviewTab) => {
      resetView();
      setReviewTab(tab);
      if (tab === "charts") return;
      await loadRows(tab !== "invalid", 0);
      setSelected(0);
    },
    [loadRows, resetView],
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
    setReviewTab("valid");
    onInfo?.(null);
    onFileChange?.("");
  }, [log, onFileChange, onInfo, resetView]);

  const openPath = useCallback(
    async (path: string) => {
      beforeLoad?.();
      resetView();
      onFileChange?.(path);
      try {
        const res = await reviewBridge.open(path);
        if (!res.ok) {
          log(`open failed: ${res.message}`);
          return;
        }
        const fresh = await reviewBridge.info();
        setInfo(fresh);
        setFileKey(`${path}#${Date.now()}`);
        onInfo?.(fresh);
        log(`opened ${path}${fresh.accounting_summary}`);
        // Start on the valid set, or the invalid one when there are no valid
        // frames (a regenerated file can have none).
        const startTab = initialTab(fresh);
        setReviewTab(startTab);
        await loadRows(startTab !== "invalid", 0);
        setSelected(setTotalFor(fresh, startTab !== "invalid") > 0 ? 0 : -1);
      } catch (e) {
        log(`open error: ${e}`);
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

  const onRegenerate = useCallback(
    async (r: RegenerateRequest) => {
      setShowRegenerate(false);
      const job = await jobs.start("Regenerate masks", REVIEW_OPERATION_KINDS.RegenerateMasks, () => reviewBridge.regenerateMasks(r));
      // The Qt tab reloads from the regenerated file.
      if (job?.phase === "completed") {
        rememberExportDir(dirName(r.outputPath));
        await openPath(r.outputPath);
      }
    },
    [jobs, openPath],
  );

  const pageTo = (offset: number) => void loadRows(validSet, Math.max(0, offset));

  const previewPane = (
    <div className="canvas-wrap preview" onDoubleClick={() => selected >= 0 && setViewerOpen(true)} title="Double-click to open the viewer">
      <canvas ref={canvasRef} className={fitWindow ? "fit" : ""} />
      <span className="preview-caption mono">
        {selected >= 0
          ? `#${selected + 1} of ${setTotal}${selectedRow ? ` · frame ${selectedRow.frame_index}` : ""}`
          : reviewTab === "charts"
            ? "Click a point to show its cell"
            : "—"}
      </span>
    </div>
  );

  return (
    <>
      <div className="toolbar">
        <button onClick={openFile} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
          Select HDF File…
        </button>
        <button onClick={closeFile} disabled={!reviewing} title={reviewing ? "Close the current file" : "No file loaded"}>
          Close File
        </button>
        <button
          onClick={onExportMetrics}
          disabled={!isExperiment || jobs.busy}
          title={isRecording ? "Recording files carry no metrics" : "Write the metrics CSV (recorded px→µm factor)"}
        >
          Export Metrics to CSV…
        </button>
        <button onClick={onExportAll} disabled={!reviewing || jobs.busy} title="Metrics, images, series and charts into <root>/<file>/">
          Export All…
        </button>
        <div className="more-menu">
          <button aria-haspopup="menu" aria-expanded={showMore} onClick={() => setShowMore((v) => !v)} onBlur={() => window.setTimeout(() => setShowMore(false), 150)}>
            More…
          </button>
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
        <select
          value={overlay}
          disabled={!reviewing || isRecording}
          title={isRecording ? "Recording files carry no masks" : "Overlay drawn by the backend"}
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
        <span className="path-label right" title={info?.file_path}>
          {reviewing && info
            ? `${info.file_path} · ${info.recording_file ? "recording" : "experiment"} · valid ${info.total_valid}, invalid ${info.total_invalid}${info.has_core_identity ? ` · core v${info.core_version}` : ""} · px→µm ${info.pixel_to_micron.toFixed(4)}${info.pixel_to_micron_from_file ? "" : " (fallback)"}${info.accounting_summary}`
            : "No file selected"}
        </span>
      </div>
      <div className="subtabs" role="tablist" aria-label="Review views">
        <button
          className={reviewTab === "valid" ? "active" : ""}
          disabled={!reviewing || setTotalFor(info, true) === 0}
          onClick={() => void selectTab("valid")}
        >
          {isRecording ? "Frames" : "Valid Frames"} {reviewing ? `(${setTotalFor(info, true)})` : ""}
        </button>
        {!isRecording && (
          <button
            className={reviewTab === "invalid" ? "active" : ""}
            disabled={!reviewing || setTotalFor(info, false) === 0}
            onClick={() => void selectTab("invalid")}
          >
            Invalid Frames {reviewing ? `(${setTotalFor(info, false)})` : ""}
          </button>
        )}
        {!isRecording && (
          <button
            className={reviewTab === "charts" ? "active" : ""}
            disabled={!reviewing || setTotalFor(info, true) === 0}
            title="Deformability vs area, ring-width histogram"
            onClick={() => void selectTab("charts")}
          >
            Charts
          </button>
        )}
      </div>
      <div className="subtab-body">
        {!reviewing ? (
          <div className="review-empty">
            <p>No recording loaded.</p>
            <button onClick={openFile} disabled={!ready}>Select HDF File…</button>
          </div>
        ) : reviewTab === "charts" && info ? (
          <ChartsView
            info={info}
            fileKey={fileKey}
            selected={selected}
            validTotal={setTotal}
            onSelect={select}
            framePane={previewPane}
            refreshInfo={refreshInfo}
            onViewChange={onChartViewChange}
            log={log}
          />
        ) : (
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
            <div className="review-side">
              {previewPane}
              <div className="table-toolbar">
                <span className="mono">
                  {rowTotal > 0 ? `${rowsOffset + 1}–${Math.min(rowTotal, rowsOffset + METRICS_PAGE_SIZE)} of ${rowTotal}` : "no rows"}
                </span>
                <button disabled={rowsOffset === 0} onClick={() => pageTo(rowsOffset - METRICS_PAGE_SIZE)}>◀</button>
                <button disabled={rowsOffset + METRICS_PAGE_SIZE >= rowTotal} onClick={() => pageTo(rowsOffset + METRICS_PAGE_SIZE)}>▶</button>
                <span className="right" />
                {!isRecording && (
                  <div className="column-chooser">
                    <button aria-expanded={showColumns} onClick={() => setShowColumns((s) => !s)}>Columns…</button>
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
              <div className="table-panel">
                <table className="metrics-table">
                  <thead>
                    <tr>
                      {shownColumns.map((c) => (
                        <th key={c.id}>{c.label}</th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {(rows?.rows ?? []).map((r, k) => {
                      const index = rowsOffset + k;
                      return (
                        <tr
                          key={index}
                          className={index === selected ? "selected" : r.target_group ? "target" : ""}
                          onClick={() => select(index)}
                          onDoubleClick={() => {
                            select(index);
                            setViewerOpen(true);
                          }}
                        >
                          {shownColumns.map((c) => (
                            <td key={c.id}>{c.format(r)}</td>
                          ))}
                        </tr>
                      );
                    })}
                    {(rows?.rows?.length ?? 0) === 0 && (
                      <tr>
                        <td colSpan={shownColumns.length} style={{ color: "#777" }}>No metric rows in this set.</td>
                      </tr>
                    )}
                  </tbody>
                </table>
              </div>
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
      {showRegenerate && <RegenerateMasksDialog info={info} onRun={(r) => void onRegenerate(r)} onCancel={() => setShowRegenerate(false)} />}
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
