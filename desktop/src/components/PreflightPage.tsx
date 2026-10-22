// The classic Hardware Preflight stage's first page (#796; board "Classic · Hardware Preflight ·
// summary-first"): the approved O1b summary (workspace/PrepareSummary.tsx) in the classic shell,
// with a `mib-root` wrapper so the kit's tokens apply. No hardware configuration is rendered here:
// each hardware row has at most one "Set up …" button, which asks App for the Hardware setup page
// (`onSetup(section)`). The page reads the backend gates without a path every 2 s and the
// calibration records while `active` (the classic stage shown, no workspace open, no run), so the
// classic tab never polls beside the workspace. Confirm is the "Next:" band's action.
import { derivePreflight, type PreflightInput } from "../preflight";
import type { StageStatus } from "../bridge";
import { PrepareSummary, type SetupSection } from "../workspace/PrepareSummary";
import { confirmationState, prepareVerdict } from "../workspace/prepareModel";
import { calibrationGate } from "../workspace/calibrationModel";
import { usePrepareReadiness } from "../workspace/usePrepareReadiness";
import { useCalibration } from "../workspace/useCalibration";
import { loadStartupPreference } from "../startupPreference";
import "./PreflightPage.css";

export interface PreflightPageProps {
  /** Whether the page reads (the stage is shown, no workspace is open and no run is active). */
  active: boolean;
  preflightInput: PreflightInput;
  zStage: StageStatus | null;
  signature: string;
  confirmedFor: string;
  onConfirm: () => void;
  confirmLockedReason: string;
  experimentActive: boolean;
  onRefreshChecks: () => void;
  onSetup: (section: SetupSection) => void;
  controlHeld: boolean;
}

/** The remembered nanopositioner endpoint of the startup preference; "" for none. */
function rememberedEndpoint(): string {
  try {
    return loadStartupPreference(localStorage).preference.endpoint;
  } catch {
    return "";
  }
}

export function PreflightPage(p: PreflightPageProps) {
  const readiness = usePrepareReadiness(p.active);
  const calibration = useCalibration(p.active, { controlHeld: p.controlHeld, experimentActive: p.experimentActive, afterApply: readiness.refresh });
  const report = derivePreflight(p.preflightInput);
  const confirmation = confirmationState(p.signature, p.confirmedFor, report.criticalPassed);
  const verdict = prepareVerdict(report, readiness.snapshot, readiness.error, p.experimentActive, confirmation === "confirmed");
  return (
    <div className="mib-root classic-preflight" data-testid="classic-preflight">
      <PrepareSummary
        preflightInput={p.preflightInput} report={report} readiness={readiness} verdict={verdict} zStage={p.zStage}
        signature={p.signature} confirmedFor={p.confirmedFor} onConfirm={p.onConfirm} confirmLockedReason={p.confirmLockedReason}
        experimentActive={p.experimentActive} onRefreshChecks={p.onRefreshChecks} onSetup={p.onSetup} controlHeld={p.controlHeld}
        calibration={calibration} calibrationGate={calibrationGate(readiness.snapshot)} now={Date.now()}
        rememberedEndpoint={rememberedEndpoint()} />
    </div>
  );
}
