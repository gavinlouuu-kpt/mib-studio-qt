// @vitest-environment jsdom
// Review (epic #867): what MIB Studio's open path tells the operator when the backend refuses or cannot
// read a file (REVIEW-003/004), and that a failed read of the saved run accounting is logged, not swallowed.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { CORRUPT_FILE_DETAIL, MISSING_FILE_DETAIL } from "../../review/reviewOpen";

const mocks = vi.hoisted(() => ({
  bridge: { loadRecording: vi.fn(), fetchReviewMetadata: vi.fn(), fetchRunAccounting: vi.fn(), pollEvents: vi.fn(), fetchReviewMetricsPage: vi.fn(), fetchReviewImage: vi.fn() },
  open: vi.fn(),
}));
vi.mock("../../bridge", async (importOriginal) => ({ ...(await importOriginal<typeof import("../../bridge")>()), bridge: mocks.bridge }));
vi.mock("../../transport/dialogs", () => ({ open: mocks.open }));
import { useReviewAccounting, useReviewSource } from "./useReview";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  vi.resetAllMocks();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

function fakeDeps() {
  return {
    ready: true, tab: "review", setTab: vi.fn(), append: vi.fn(), framePulls: { current: { invalidate: vi.fn(), request: vi.fn() } }, stopLoop: vi.fn(),
    applyEvents: vi.fn(), setReviewIndex: vi.fn(), metricsGeneration: { current: 0 }, reviewMeta: null, setMetricsPage: vi.fn(), setMetricsOffset: vi.fn(),
    setReviewMeta: vi.fn(), setReviewPath: vi.fn(), setReviewing: vi.fn(), setReviewImgIndex: vi.fn(), reviewCanvasRef: { current: null },
    reviewSourcePending: { current: false }, setReviewSourceBusy: vi.fn(), reviewSourceGeneration: { current: 0 }, setReviewTab: vi.fn(),
    range: { earliest: "0", latest: "0", count: "0" }, recoveredReview: { current: false }, METRICS_PAGE_SIZE: 50,
    setReviewOpenPhase: vi.fn(), setReviewOpenFailure: vi.fn(),
  };
}

async function selectHdf(deps: ReturnType<typeof fakeDeps>) {
  let select!: () => Promise<void>;
  function Probe() { select = useReviewSource(deps as never).onSelectHdf; return null; }
  await act(async () => root.render(<Probe />));
  await act(async () => { await select(); });
}

describe("opening a file in MIB Studio", () => {
  it("shows Qt's missing-file failure when the backend says file not found", async () => {
    const deps = fakeDeps();
    mocks.open.mockResolvedValue("/data/gone.h5");
    mocks.bridge.loadRecording.mockResolvedValue({ ok: false, message: "Recording load failed: file not found" });
    mocks.bridge.fetchReviewMetadata.mockResolvedValue({ file_open: false });
    await selectHdf(deps);
    expect(deps.setReviewOpenPhase.mock.calls.map((c) => c[0])).toEqual(["loading", "error"]);
    expect(deps.setReviewOpenFailure).toHaveBeenCalledWith({ path: "/data/gone.h5", detail: MISSING_FILE_DETAIL });
    expect(deps.append).toHaveBeenCalledWith("load failed: Recording load failed: file not found");
    expect(deps.setReviewMeta).toHaveBeenCalledWith(null); // the failed open left no file (clearReviewSource)
  });

  it("shows the corrupt-file text with the h5recover hint for any other open failure", async () => {
    const deps = fakeDeps();
    mocks.open.mockResolvedValue("/data/bad.h5");
    mocks.bridge.loadRecording.mockResolvedValue({ ok: false, message: "Recording load failed" });
    mocks.bridge.fetchReviewMetadata.mockResolvedValue({ file_open: false });
    await selectHdf(deps);
    expect(deps.setReviewOpenFailure).toHaveBeenCalledWith({ path: "/data/bad.h5", detail: CORRUPT_FILE_DETAIL });
  });

  it("keeps a refusal about the instrument in the log: no dialog, no error status", async () => {
    const deps = fakeDeps();
    mocks.open.mockResolvedValue("/data/run.h5");
    mocks.bridge.loadRecording.mockResolvedValue({ ok: false, message: "Experiment must be idle before loading review data" });
    mocks.bridge.fetchReviewMetadata.mockResolvedValue({ file_open: false });
    await selectHdf(deps);
    expect(deps.setReviewOpenFailure).not.toHaveBeenCalled();
    expect(deps.setReviewOpenPhase.mock.calls.map((c) => c[0])).toEqual(["loading", "idle"]);
    expect(deps.append).toHaveBeenCalledWith("load failed: Experiment must be idle before loading review data");
  });

  it("does not touch the status when the operator cancels the file picker", async () => {
    const deps = fakeDeps();
    mocks.open.mockResolvedValue(null);
    await selectHdf(deps);
    expect(deps.setReviewOpenPhase).not.toHaveBeenCalled();
    expect(mocks.bridge.loadRecording).not.toHaveBeenCalled();
  });
});

describe("saved run accounting", () => {
  it("logs a failed read instead of dropping the outcome notice silently", async () => {
    const append = vi.fn(), setReviewAccounting = vi.fn();
    mocks.bridge.fetchRunAccounting.mockRejectedValue(new Error("backend gone"));
    function Probe() { useReviewAccounting({ reviewFilePath: "/data/run.h5", setReviewAccounting, append }); return null; }
    await act(async () => root.render(<Probe />));
    expect(append).toHaveBeenCalledWith("review run accounting unavailable: Error: backend gone");
    expect(setReviewAccounting).toHaveBeenCalledWith(null);
  });
  it("stays quiet when the read works", async () => {
    const append = vi.fn(), setReviewAccounting = vi.fn();
    mocks.bridge.fetchRunAccounting.mockResolvedValue({ available: true });
    function Probe() { useReviewAccounting({ reviewFilePath: "/data/run.h5", setReviewAccounting, append }); return null; }
    await act(async () => root.render(<Probe />));
    expect(append).not.toHaveBeenCalled();
    expect(setReviewAccounting).toHaveBeenCalledWith({ available: true });
  });
});
