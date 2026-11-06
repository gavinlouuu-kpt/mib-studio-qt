// @vitest-environment jsdom
// The run lock on the Preview and Review controls (epic #866: MON-013, MON-014).
import { act, createRef } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../../bridge", () => ({ bridge: {} }));
vi.mock("../../components/BackgroundCalibrationControls", () => ({ BackgroundCalibrationControls: () => null }));
vi.mock("../../components/ProcessedPreview", () => ({ ProcessedPreview: () => null }));
vi.mock("../../components/ProcessingSetupControls", () => ({ ProcessingSetupControls: () => null }));
vi.mock("../../components/RingPlaybackPanel", () => ({ RingPlaybackPanel: () => null }));
vi.mock("../../previewBuffer", () => ({ PreviewBufferControls: () => null }));
vi.mock("../config/ConfigPanel", () => ({ ConfigPanel: () => null }));
vi.mock("../hardware/InstrumentLedPanel", () => ({ InstrumentLedPanel: () => null }));
vi.mock("../../components/ResultMetricCells", () => ({ ResultMetricCells: () => null }));
vi.mock("../../components/ReviewCharts", () => ({ ReviewCharts: () => null }));
vi.mock("../../components/SavedReviewImage", () => ({ SavedReviewImage: () => null }));
vi.mock("../../exportControls", () => ({ ReviewExportOptions: () => null }));
vi.mock("../../reanalysisControls", () => ({ ReanalysisControls: () => null }));

import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { PreviewPanel } from "../preview/PreviewPanel";
import { ReviewTab } from "../review/ReviewTab";
import { STAGE_LOCK_REASON } from "./runStatus";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
const render = async (s: unknown, view: JSX.Element) => act(async () => root.render(<StudioContext.Provider value={s as Studio}>{view}</StudioContext.Provider>));
const button = (label: string) => Array.from(host.querySelectorAll<HTMLButtonElement>("button")).find((b) => b.textContent?.trim() === label)!;

describe("Raw Record (MON-014)", () => {
  const preview = (over: Record<string, unknown> = {}) => ({
    runFrozen: false, ring: null, append: vi.fn(), lastMeta: null, runPreviewInfo: null, instrumentModes: false, runMode: false, cameraWin: {}, modeError: null,
    previewCanvasRef: createRef(), fitWindow: false, instrument: null, resultsNow: null, showRunMask: false, setShowRunMask: vi.fn(), faultNote: "", expActive: false,
    viewerOnly: false, hostProcessing: true, ready: true, running: true, caps: { frame_buffer: true }, backgroundSet: false, autoBackgroundEnabled: false,
    refreshConfig: vi.fn(), setConfigTab: vi.fn(), onToggleRecord: vi.fn(), recording: false, setFitWindow: vi.fn(), previewBuffer: {}, tab: "experiment",
    expTab: "preview", recordReason: undefined, experimentRequestBusy: false, onResumeRun: vi.fn(), onStopRun: vi.fn(), ...over,
  });

  it("is enabled for a running camera with nothing in the way", async () => {
    await render(preview(), <PreviewPanel />);
    expect(button("Record")).toMatchObject({ disabled: false, title: "Record raw frames to an HDF5 file" });
  });
  it("is disabled with the reason while an experiment is not idle or an HDF5 file is open", async () => {
    const reason = "Stop or reset the experiment and close the existing HDF5 file before recording.";
    await render(preview({ recordReason: reason }), <PreviewPanel />);
    expect(button("Record")).toMatchObject({ disabled: true, title: reason });
  });
  it("keeps the old reason for a camera that is not running", async () => {
    await render(preview({ running: false }), <PreviewPanel />);
    expect(button("Record")).toMatchObject({ disabled: true, title: "Camera is not running" });
  });
});

describe("Select HDF File (MON-013)", () => {
  const review = (over: Record<string, unknown> = {}) => ({
    onSelectHdf: vi.fn(), ready: true, reviewSourceBusy: false, reviewMeta: null, expActive: false, recording: false, reviewSourcePending: { current: false },
    setReviewSourceBusy: vi.fn(), reviewSourceGeneration: { current: 0 }, append: vi.fn(), clearReviewSource: vi.fn(), reviewExport: { busy: false }, stats: null,
    reviewing: false, reviewPath: "", reviewTab: "raw", metricsGeneration: { current: 0 }, setMetricsPage: vi.fn(), setReviewTab: vi.fn(), setReviewImgIndex: vi.fn(),
    loadMetricsPage: vi.fn(), reviewOutcome: null, caps: {}, reanalysis: {}, reviewImgIndex: 0, fitWindow: false, reviewCanvasRef: createRef(), reviewIndex: 0,
    onScrub: vi.fn(), metricsPage: null, metricsOffset: 0, METRICS_PAGE_SIZE: 100, runLocked: false, ...over,
  });
  it("is open without a run and shut, with the reason, while a run is locked", async () => {
    await render(review(), <ReviewTab />);
    expect(button("Select HDF File…")).toMatchObject({ disabled: false });
    await render(review({ runLocked: true }), <ReviewTab />);
    expect(button("Select HDF File…")).toMatchObject({ disabled: true, title: STAGE_LOCK_REASON });
  });
});
