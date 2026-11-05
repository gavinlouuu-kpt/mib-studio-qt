// Review (epic #867): the recording/experiment file open in MIB Studio's Review stage, its frame
// scrubber and metrics pages, its saved run accounting, and the export and re-analysis jobs.
// (YOFO Review, the standalone app, is desktop/src/review/.) Moved from App.tsx without change
// (#869 wave 0); studio/useStudioController.ts calls these hooks in App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type ReviewMetadata, type ReviewMetricsPage, type RunAccounting } from "../../bridge";
import { useReviewExport } from "../../exportControls";
import { decimalU64 } from "../../framePacket";
import { useReanalysis } from "../../reanalysisControls";
import { describeReviewOutcome } from "../../runOutcome";
import { isOpenFailureMessage, openFailureFor, type OpenFailure, type OpenPhase } from "../../review/reviewOpen";
import { open } from "../../transport/dialogs";

// The Qt tab's open dialog (HdfReviewTab: "Open HDF File", *.h5 *.hdf5 + All Files). MIB raw
// recordings from tools/yzr-extract (zu15eg-linux) open the same way (/recording_info).
export const REVIEW_OPEN_FILTERS = [{ name: "HDF5 Files", extensions: ["h5", "hdf5"] }, { name: "All Files", extensions: ["*"] }];
import type { LiveFramesModel, PreviewState } from "../preview/usePreview";
import type { ShellState, StatePollingModel } from "../shell/useShell";

export function useReviewState() {
  const recoveredReview=useRef(false);

  // Review.
  const [reviewPath, setReviewPath] = useState("");
  const [reviewing, setReviewing] = useState(false);
  const [reviewTab, setReviewTab] = useState<"raw" | "valid" | "invalid" | "charts">("raw");
  const [range, setRange] = useState({ earliest: "0", latest: "0", count: "0" });
  const [reviewIndex, setReviewIndex] = useState("0");
  // Paged review (bridge schema v9, BE-6).
  const [reviewMeta, setReviewMeta] = useState<ReviewMetadata | null>(null);
  const [metricsPage, setMetricsPage] = useState<ReviewMetricsPage | null>(null);
  const [metricsOffset, setMetricsOffset] = useState(0);
  const [reviewImgIndex, setReviewImgIndex] = useState(0);
  const METRICS_PAGE_SIZE = 50;
  const reviewCanvasRef = useRef<HTMLCanvasElement>(null);
  const metricsGeneration = useRef(0);
  const reviewSourcePending=useRef(false);
  const reviewSourceGeneration=useRef(0);
  const [reviewSourceBusy,setReviewSourceBusy]=useState(false);
  // Qt's status label ("Loading...", "Error loading file") and the critical dialog of a failed open.
  const [reviewOpenPhase,setReviewOpenPhase]=useState<OpenPhase>("idle");
  const [reviewOpenFailure,setReviewOpenFailure]=useState<OpenFailure|null>(null);
  // The accounting saved in the file loaded for review.
  const [reviewAccounting, setReviewAccounting] = useState<RunAccounting | null>(null);
  const reviewFilePath = reviewMeta?.file_open ? reviewMeta.file_path : "";
  const reviewOutcome = describeReviewOutcome(reviewAccounting);
  return {
    recoveredReview, reviewPath, setReviewPath, reviewing, setReviewing, reviewTab, setReviewTab, range, setRange,
    reviewIndex, setReviewIndex, reviewMeta, setReviewMeta, metricsPage, setMetricsPage, metricsOffset, setMetricsOffset,
    reviewImgIndex, setReviewImgIndex, METRICS_PAGE_SIZE, reviewCanvasRef, metricsGeneration,
    reviewSourcePending, reviewSourceGeneration, reviewSourceBusy, setReviewSourceBusy,
    reviewAccounting, setReviewAccounting, reviewFilePath, reviewOutcome,
    reviewOpenPhase, setReviewOpenPhase, reviewOpenFailure, setReviewOpenFailure,
  };
}
export type ReviewState = ReturnType<typeof useReviewState>;

type ReviewSourceDeps = Pick<ShellState, "ready" | "tab" | "setTab" | "append"> & Pick<PreviewState, "framePulls">
  & Pick<LiveFramesModel, "stopLoop"> & Pick<StatePollingModel, "applyEvents">
  & Pick<ReviewState, "setReviewIndex" | "metricsGeneration" | "reviewMeta" | "setMetricsPage" | "setMetricsOffset"
    | "setReviewMeta" | "setReviewPath" | "setReviewing" | "setReviewImgIndex" | "reviewCanvasRef" | "reviewSourcePending"
    | "setReviewSourceBusy" | "reviewSourceGeneration" | "setReviewTab" | "range" | "recoveredReview" | "METRICS_PAGE_SIZE"
    | "setReviewOpenPhase" | "setReviewOpenFailure">;

// Open and close the file, scrub its frames, page its metrics; reopen a file a native session recovered.
export function useReviewSource({
  ready, tab, setTab, append, framePulls, stopLoop, applyEvents, setReviewIndex, metricsGeneration, reviewMeta,
  setMetricsPage, setMetricsOffset, setReviewMeta, setReviewPath, setReviewing, setReviewImgIndex, reviewCanvasRef,
  reviewSourcePending, setReviewSourceBusy, reviewSourceGeneration, setReviewTab, range, recoveredReview, METRICS_PAGE_SIZE,
  setReviewOpenPhase, setReviewOpenFailure,
}: ReviewSourceDeps) {
  // ---- Review ----

  const onScrub = useCallback((input: number | string) => {
    const idx = decimalU64(input);
    setReviewIndex(idx);
    framePulls.current.request("review", async () => {
      return bridge.fetchReviewImage(2, idx);
    });
  }, []);

  const loadMetricsPage = useCallback(
    async (valid: boolean, offset: number, source = reviewMeta?.file_path) => {
      const generation = ++metricsGeneration.current;
      setMetricsPage(null);
      try {
        const before = await bridge.fetchReviewMetadata();
        if (!before.file_open || before.file_path !== source || generation !== metricsGeneration.current) return;
        const page = await bridge.fetchReviewMetricsPage(valid, offset, METRICS_PAGE_SIZE);
        const after = await bridge.fetchReviewMetadata();
        if (page.valid && after.file_open && after.file_path === source && generation === metricsGeneration.current) {
          setMetricsPage(page);
          setMetricsOffset(offset);
        }
      } catch (e) {
        if (generation === metricsGeneration.current) append(`metrics page error: ${e}`);
      }
    },
    [append, reviewMeta?.file_path],
  );

  const drawReviewImage = useCallback((dataset: number, index: number) => {
    framePulls.current.request("review", () => bridge.fetchReviewImage(dataset, index));
  }, []);

  const clearReviewSource=useCallback(()=>{
    framePulls.current.invalidate("review"); ++metricsGeneration.current;
    setReviewMeta(null);setMetricsPage(null);setReviewPath("");setReviewing(false);setReviewIndex("0");setReviewImgIndex(0);
    const canvas=reviewCanvasRef.current;if(canvas)canvas.getContext("2d")?.clearRect(0,0,canvas.width,canvas.height);
  },[]);
  // The load, after a path is chosen. The caller holds the source lock (reviewSourcePending) and
  // passes the generation it took; a failure throws to the caller (recoverFromLoadError).
  const loadReviewFile = useCallback(async (picked: string, sourceGeneration: number): Promise<boolean> => {
    ++metricsGeneration.current;
    framePulls.current.invalidate("review");
    setMetricsPage(null);
    setReviewOpenPhase("loading");
    const res = await bridge.loadRecording(picked);
    if(sourceGeneration!==reviewSourceGeneration.current)return false;
    if (!res.ok) { const metadata=await bridge.fetchReviewMetadata();if(!metadata.file_open)clearReviewSource();else setReviewMeta(metadata);
      // Qt's critical "Cannot Open HDF5 File" for a file that cannot be opened; a refusal about the instrument stays in the log.
      if (isOpenFailureMessage(res.message)) { setReviewOpenPhase("error"); setReviewOpenFailure(openFailureFor(picked, res.message)); } else setReviewOpenPhase("idle");
      append(`load failed: ${res.message}`);return false; }
    setReviewPath(picked);
    setReviewing(true);
    append(`loaded ${picked}`);
    applyEvents(await bridge.pollEvents());
    const meta = await bridge.fetchReviewMetadata();
    setReviewMeta(meta);
    setReviewOpenPhase("idle");
    setReviewTab(meta.recording_file ? "raw" : "valid");
    setReviewImgIndex(0);
    await loadMetricsPage(true, 0, picked);
    if (meta.recording_file) {
      await onScrub("0");
    } else if (meta.valid_images.present && meta.valid_images.count > 0) {
      await drawReviewImage(0, 0);
    }
    return true;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [append, applyEvents, range.earliest, stopLoop, onScrub, loadMetricsPage, drawReviewImage,clearReviewSource]);
  const recoverFromLoadError = useCallback(async (e: unknown) => {
    setReviewOpenPhase("error");
    append(`load error: ${e}`);
    try { const metadata=await bridge.fetchReviewMetadata();if(!metadata.file_open)clearReviewSource();else setReviewMeta(metadata); } catch { clearReviewSource(); }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [append, clearReviewSource]);
  const onSelectHdf = useCallback(async () => {
    if(reviewSourcePending.current)return;reviewSourcePending.current=true;setReviewSourceBusy(true);
    const sourceGeneration=++reviewSourceGeneration.current;
    try {
      const picked = await open({ title: "Open HDF File", filters: REVIEW_OPEN_FILTERS, multiple: false });
      if (typeof picked !== "string") return;
      await loadReviewFile(picked, sourceGeneration);
    } catch (e) {
      await recoverFromLoadError(e);
    } finally {reviewSourcePending.current=false;setReviewSourceBusy(false);}
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [loadReviewFile, recoverFromLoadError]);
  // Open a known path in the Review stage without the picker (a regenerated file, #867 REVIEW-067).
  // True once the file is loaded; false when another open is in flight, the load was refused or failed.
  const openReviewPath = useCallback(async (path: string): Promise<boolean> => {
    if(reviewSourcePending.current)return false;reviewSourcePending.current=true;setReviewSourceBusy(true);
    const sourceGeneration=++reviewSourceGeneration.current;
    try {
      return await loadReviewFile(path, sourceGeneration);
    } catch (e) {
      await recoverFromLoadError(e);
      return false;
    } finally {reviewSourcePending.current=false;setReviewSourceBusy(false);}
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [loadReviewFile, recoverFromLoadError]);

  useEffect(()=>{
    if(!ready||tab!=="review"||!recoveredReview.current||!reviewMeta?.file_open)return;
    recoveredReview.current=false;
    void loadMetricsPage(true,0,reviewMeta.file_path);
    if(reviewMeta.recording_file)onScrub("0");
    else if(reviewMeta.valid_images.present&&reviewMeta.valid_images.count>0)drawReviewImage(0,0);
  },[ready,tab,reviewMeta,loadMetricsPage,onScrub,drawReviewImage]);

  const openReviewFromMenu = useCallback(() => {
    setTab("review");
    void onSelectHdf();
  }, [onSelectHdf]);
  return { onScrub, loadMetricsPage, drawReviewImage, clearReviewSource, onSelectHdf, openReviewPath, openReviewFromMenu };
}

type ReviewAccountingDeps = Pick<ReviewState, "reviewFilePath" | "setReviewAccounting"> & Pick<ShellState, "append">;

// The run accounting saved in the file loaded for review.
export function useReviewAccounting({ reviewFilePath, setReviewAccounting, append }: ReviewAccountingDeps) {
  useEffect(() => {
    if (!reviewFilePath) { setReviewAccounting(null); return; }
    let live = true;
    // A failed read must not look like a file without accounting: the outcome notice would just vanish.
    void bridge.fetchRunAccounting("review").catch((e) => { if (live) append(`review run accounting unavailable: ${e}`); return null; }).then((acc) => { if (live) setReviewAccounting(acc); });
    return () => { live = false; };
  }, [reviewFilePath]);
}

// `openReviewPath` (useReviewSource) loads the file a finished regenerate job wrote (REVIEW-067).
export function useReviewJobs({ ready, append, openReviewPath }: Pick<ShellState, "ready" | "append"> & { openReviewPath?: (path: string) => Promise<boolean> }) {
  const reviewExport = useReviewExport(ready, append);
  const reanalysis = useReanalysis(ready, openReviewPath);
  return { reviewExport, reanalysis };
}
export type ReviewJobsModel = ReturnType<typeof useReviewJobs>;
