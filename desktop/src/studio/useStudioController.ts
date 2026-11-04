import { useViewerOnly } from "../components/ViewerGate";
import { useWallClockSync } from "../transport/wallClockSync";
import { useCameraSetup, useConfigDocuments, useConfigState, useConfigSync, useCores } from "../areas/config/useConfig";
import {
  connectModel, syncScriptSources, useAlignPeakReset, useCameraConnection, useCameraModeSwitch, useCameraWindowing, useConnectState,
} from "../areas/connect/useConnect";
import { useHardwareState, useServiceMode } from "../areas/hardware/useHardware";
import {
  experimentModel, useExperimentLifecycle, useMonitoringPolling, useMonitoringState, useSsdStatus,
} from "../areas/monitoring/useMonitoring";
import { useLiveFrames, useLivePreview, usePreviewBufferModel, usePreviewState } from "../areas/preview/usePreview";
import { useReviewAccounting, useReviewJobs, useReviewSource, useReviewState } from "../areas/review/useReview";
import {
  shellTelemetry, useDocumentTitle, usePlatformStatus, useRemoteDefaultDir, useShellCloseGuard, useShellPreferences,
  useShellStartup, useShellState, useStatePolling,
} from "../areas/shell/useShell";
import { shellWorkflow, useWorkflowLanding } from "../areas/shell/workflowModel";

// The classic view's state and behaviour, composed from the area hooks (#869 wave 0: App.tsx was
// split into desktop/src/areas/<area>/; owners in docs/howto/desktop-area-ownership.md).
//
// ORDER MATTERS. React runs a component's effects in the order they were registered, and these
// calls keep the exact order the effects had in the single App component: for example the
// frame-pull invalidation on a tab change (useLiveFrames) runs before the Review recovery
// requests its first frame (useReviewSource). The use<Area>State() hooks hold only state, refs and
// render-time values (no effects), so they come first and every later hook can use any area's
// state. Add a new effect inside its area's hook; move a call here only on purpose.
//
// Everything returned is provided through StudioContext; area views read it with useStudio().
export function useStudioController() {
  useWallClockSync(); // the PZ7035 has no RTC: the controlling browser sends its clock (G14)
  const states = {
    ...useShellState(),
    ...useConnectState(),
    ...useConfigState(),
    ...usePreviewState(),
    ...useMonitoringState(),
    ...useReviewState(),
    ...useHardwareState(),
  };
  useDocumentTitle(states.brand); // shell
  useAlignPeakReset(states); // connect
  useShellStartup(states); // shell: fault-cleared log line, backend init and native-session recovery
  const liveFrames = useLiveFrames(states); // preview
  const polling = useStatePolling(states); // shell
  const monitoringPolling = useMonitoringPolling(states); // monitoring
  const preferences = useShellPreferences(states); // shell
  const configSync = useConfigSync(states); // config
  const connection = useCameraConnection({ ...states, ...liveFrames, ...polling }); // connect
  const livePreview = useLivePreview({ ...states, ...liveFrames, ...configSync }); // preview
  const ssd = useSsdStatus(states); // monitoring
  const reviewSource = useReviewSource({ ...states, ...liveFrames, ...polling }); // review
  const experiment = useExperimentLifecycle(states); // monitoring
  useReviewAccounting(states); // review
  usePlatformStatus(states); // shell
  const windowing = useCameraWindowing(states); // connect
  useRemoteDefaultDir(states.instrumentDataDir); // shell
  const modeSwitch = useCameraModeSwitch({ ...states, ...windowing }); // connect
  const cameraSetup = useCameraSetup({ ...states, ...connection }); // config
  const previewBuffer = usePreviewBufferModel({ ...states, ...livePreview, ...configSync }); // preview
  const cores = useCores({ ...states, ...configSync }); // config
  // #763 F19: a browser that does not control the instrument gets its control commands refused
  // (VIEWER_ONLY); the controls that send them are disabled (ViewerGate) instead of failing in the log.
  const viewerOnly = useViewerOnly();
  const documents = useConfigDocuments({ ...states, ...configSync, ...cores }); // config
  const reviewJobs = useReviewJobs(states); // review
  syncScriptSources({ ...states, ...cameraSetup, ...documents }); // connect: the camera scripts the stage change and the window save use
  const composed = {
    ...states, ...liveFrames, ...polling, ...monitoringPolling, ...preferences, ...configSync, ...connection,
    ...livePreview, ...ssd, ...reviewSource, ...experiment, ...windowing, ...modeSwitch, ...cameraSetup,
    ...previewBuffer, ...cores, viewerOnly, ...documents, ...reviewJobs,
  };
  const closeGuard = useShellCloseGuard(composed); // shell
  // Render-time values (no hooks).
  const startExperiment = experimentModel(composed); // monitoring
  const connectView = connectModel(composed); // connect: Start Camera gate, preflight, quality gates
  const workflow = shellWorkflow({ ...composed, ...connectView }); // shell: workflow stages, context bar
  const telemetry = shellTelemetry(composed); // shell: sidebar and status-bar rates
  const serviceMode = useServiceMode(composed); // hardware
  const landing = useWorkflowLanding({ ...composed, ...connectView, ...workflow }); // shell
  return {
    ...composed, ...closeGuard, ...startExperiment, ...connectView, ...workflow, ...telemetry, ...serviceMode, ...landing,
  };
}

export type Studio = ReturnType<typeof useStudioController>;
