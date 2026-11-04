// What the operator is told when a run ends with loss (epic #866: MON-023).
//
// Qt (MainWindow::onExperimentStatus) raises a warning alert and, once the file is closed, a modal that
// lists the run's counts whenever the run is neither Complete nor IntentionallyPartial. Tauri keeps its
// richer headline (runOutcome.ts, RunOutcomeNotice); this adds the Qt breakdown and the modal.
// Pure: no React or Tauri imports.
import { RUN_COMPLETION_STATES } from "../../bridgeContract";
import type { RunAccounting } from "../../bridge";
import type { ExperimentStatus } from "../../eventAdapter";
import type { StartDialog } from "./startReadiness";

/** Qt's remediation for the "run.accounting" alert. */
export const ACCOUNTING_REMEDIATION = "Review the frame accounting in the Review tab before using this run.";

/** The contract name of a completion value ("IncompleteLoss"), as Qt prints it. */
export function completionName(completion: number): string {
  return Object.entries(RUN_COMPLETION_STATES).find(([, v]) => v === completion)?.[0] ?? "Unknown";
}

/** True when a finished run needs the loss alert and modal: a terminal, not cancelled run whose completion is
 *  neither Complete nor IntentionallyPartial, and whose failure is not the fatal save error (that one has its
 *  own recovery panel, ExperimentRecovery). */
export function runLostData(s: ExperimentStatus | null): boolean {
  return !!s && s.valid && s.terminal && !s.cancelled && s.fault_code !== "experiment.saveFailed" &&
    s.completion !== RUN_COMPLETION_STATES.Complete && s.completion !== RUN_COMPLETION_STATES.IntentionallyPartial;
}

const count = (n: number | undefined) => (n === undefined || n === null ? "n/a" : String(n));

/** The counts line of the modal: "Empty 0 · processed 120 · rejected 3 · processing failed 0 · store loss 2 ·
 *  persisted 118/120 · persistence failed 0". Counts the run's accounting does not carry read "n/a". */
export function accountingCounts(s: ExperimentStatus, acc: RunAccounting | null | undefined): string {
  const have = !!acc && acc.available && (acc.start_generation === undefined || String(acc.start_generation) === s.start_generation);
  const a = have ? acc : undefined;
  const storeLoss = a ? (a.store_overwritten ?? 0) + (a.store_not_committed ?? 0) + (a.store_malformed ?? 0) : undefined;
  return `Empty ${count(a?.empty)} · processed ${count(a?.processed)} · rejected ${count(a?.scientifically_rejected)} · ` +
    `processing failed ${count(a?.processing_failed)} · store loss ${count(storeLoss)} · ` +
    `persisted ${s.persistence_committed}/${s.persistence_admitted} · persistence failed ${s.persistence_failed}`;
}

/** The "Experiment Accounting" modal, or null when the run did not lose data. */
export function accountingDialog(s: ExperimentStatus | null, acc: RunAccounting | null | undefined): StartDialog | null {
  if (!s || !runLostData(s)) return null;
  return {
    kind: "warning",
    title: "Experiment Accounting",
    text: `The run is recorded as '${completionName(s.completion)}': ${s.completion_reason}\n\n${accountingCounts(s, acc)}\n\n${ACCOUNTING_REMEDIATION}`,
  };
}
