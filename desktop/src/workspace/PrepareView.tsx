// O1 Prepare (#713) in the workspace: the toolbar, then the shared summary-first preflight
// (PrepareSummary.tsx, O1b). The "Set up …" buttons leave the workspace for the classic Hardware
// setup page; nothing here acts on hardware.
import { Button, Toolbar } from "@mib/ui";
import { PrepareSummary, type PrepareSummaryProps, type SetupSection } from "./PrepareSummary";

export interface PrepareViewProps extends Omit<PrepareSummaryProps, "onSetup"> {
  /** Leave the workspace for the classic Hardware setup page, at a section. */
  onOpenSetup: (part: SetupSection) => void;
}

export function PrepareView({ onOpenSetup, ...p }: PrepareViewProps) {
  return (
    <div className="ws-view" data-testid="workspace-prepare">
      <Toolbar title="Hardware preflight">
        <Button variant="selected" aria-current="page">Overview</Button>
        <Button onClick={() => { p.onRefreshChecks(); p.readiness.refresh(); }}>Refresh</Button>
      </Toolbar>
      <div className="ws-body">
        <PrepareSummary {...p} onSetup={onOpenSetup} />
      </div>
    </div>
  );
}
