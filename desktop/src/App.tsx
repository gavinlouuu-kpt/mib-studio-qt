import { PanelErrorBoundary } from "./components/PanelErrorBoundary";
import { CameraDialogs } from "./areas/connect/CameraDialogs";
import { ConnectTab } from "./areas/connect/ConnectTab";
import { MockCameraDialog } from "./areas/connect/MockCameraDialog";
import { OverviewTab } from "./areas/connect/OverviewTab";
import { HardwarePanel } from "./areas/hardware/HardwarePanel";
import { ServiceBanner } from "./areas/hardware/ServiceMode";
import { ExperimentTab } from "./areas/monitoring/ExperimentTab";
import { ReviewTab } from "./areas/review/ReviewTab";
import { ShellDialogs } from "./areas/shell/Dialogs";
import { MenuBar } from "./areas/shell/MenuBar";
import { RecoveryBanners } from "./areas/shell/RecoveryBanners";
import { TelemetrySidebar } from "./areas/shell/Sidebar";
import { ContextBar, StatusBar } from "./areas/shell/StatusBar";
import { StageTabs, WorkflowNext } from "./areas/shell/Workflow";
import { StudioContext } from "./studio/StudioContext";
import { useStudioController } from "./studio/useStudioController";
import "./App.css";

// Operator shell aligned with the Qt application on main (issue #266):
// menu row, collapsible telemetry sidebar, Connect / Overview / Experiment /
// Review tabs with camera actions in the header, and a metrics status bar.
// All implemented bridge schema-v3 actions stay wired; everything else is
// visible but disabled with an explanatory tooltip.
export default function App() {
  // Composition only (#869 wave 0): the state and behaviour are the area hooks composed by
  // studio/useStudioController.ts; each region below is a component under areas/<area>/
  // (owners: docs/howto/desktop-area-ownership.md).
  const studio = useStudioController();
  const { tab } = studio;
  return (
    <StudioContext.Provider value={studio}>
      <div className="app">
        {/* ---- Menu row ---- */}
        <MenuBar />
        <ServiceBanner />

        <div className="body">
          {/* ---- Telemetry sidebar ---- */}
          <TelemetrySidebar />

          {/* ---- Main tabbed area ---- */}
          <main className="main">
            <StageTabs />
            <WorkflowNext />
            <RecoveryBanners />
            <div className="tab-body">
              <PanelErrorBoundary>{() => <>
                <HardwarePanel />
                {/* ---- Connect ---- */}
                {tab === "connect" && <ConnectTab />}
                {/* ---- Overview ---- */}
                {tab === "overview" && <OverviewTab />}
                {/* ---- Experiment ---- */}
                {tab === "experiment" && <ExperimentTab />}
                {/* ---- Review ---- */}
                {tab === "review" && <ReviewTab />}
              </>}</PanelErrorBoundary>
            </div>
          </main>
        </div>

        {/* ---- Status bar ---- */}
        {/* ---- UX-8 persistent active-context bar (all stages) ---- */}
        <ContextBar />
        <StatusBar />

        {/* ---- Configure Mock modal ---- */}
        <MockCameraDialog />
        {/* ---- No camera found, refused camera commands ---- */}
        <CameraDialogs />
        {/* ---- Central Methods, offline help, Diagnostics, About ---- */}
        <ShellDialogs />
      </div>
    </StudioContext.Provider>
  );
}
