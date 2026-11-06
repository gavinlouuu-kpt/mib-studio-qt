// The Start Experiment flow (extracted from App.tsx so it can be tested). The order is the Qt app's
// (MainWindow::onStartExperiment, epic #866):
//   1. readiness with no destination: every blocking gate but storage.output is explained BEFORE any
//      file dialog opens (MON-003),
//   2. ask before a stopped Run's buffered frames are discarded (Tauri-only, PZ7035),
//   3. pick the file,
//   4. tell the operator when a multi-image series forces inline realtime mode (MON-008),
//   5. readiness for that destination, explained gate by gate (MON-004),
//   6. start, and word each refusal by its typed outcome (MON-010).
// Every dialog goes through `inform`, so the flow stays testable without a window.
import { startDiscardNotice, type RingStatus } from "./ringPlayback";
import {
  DESTINATION_GATE, INLINE_SERIES_NOTICE, blockersLine, blockingGates, explainReadiness, startOutcomeWording,
  warningGates, warningNotice, type StartDialog, type StartGate,
} from "./areas/monitoring/startReadiness";

export type { StartDialog, StartGate } from "./areas/monitoring/startReadiness";
export interface StartReadiness { valid: boolean; ready: boolean; gates: StartGate[] }
/** `outcome` is the reply's typed `start_outcome` (bridge ABI 40); absent on an older backend. */
export interface StartResult { ok: boolean; message: string; outcome?: number }

export interface StartDeps {
  /** A fresh ring status (not the polled snapshot, which can be seconds old or missing for another client). */
  fetchRingStatus: () => Promise<RingStatus | null | undefined>;
  confirm: (message: string) => Promise<boolean>;
  pickFile: () => Promise<string | null | undefined>;
  /** The gates for a destination; "" is the preflight, before a file is chosen. */
  fetchReadiness: (path: string) => Promise<StartReadiness>;
  start: (path: string, acknowledgeDiscardRing: boolean) => Promise<StartResult>;
  /** A modal message (the dialog transport's `message`). Absent: the outcome still carries the wording. */
  inform?: (dialog: StartDialog) => Promise<void>;
  /** True when the processing configuration asks for a multi-image series. Absent: no note. */
  needsInlineForSeries?: () => Promise<boolean>;
}

export type StartOutcome =
  | { kind: "cancelled" }
  | { kind: "not-ready"; message: string; gates: StartGate[] }
  | { kind: "refused"; path: string; message: string; summary: string; outcome?: number }
  | { kind: "started"; path: string; notice: string; warnings: StartGate[] };

const UNAVAILABLE = "Backend readiness unavailable; experiment was not started.";

export async function startExperiment(deps: StartDeps): Promise<StartOutcome> {
  const inform = async (dialog: StartDialog | null) => { if (dialog && deps.inform) await deps.inform(dialog).catch(() => undefined); };

  // 1. The preflight: only the destination is still unknown.
  const preflight = await deps.fetchReadiness("");
  if (!preflight.valid) return { kind: "not-ready", message: UNAVAILABLE, gates: [] };
  const early = blockingGates(preflight.gates, [DESTINATION_GATE]);
  if (early.length) {
    const explained = explainReadiness(early, warningGates(preflight.gates, false));
    if (explained) await inform({ kind: "warning", ...explained });
    return { kind: "not-ready", message: blockersLine(null, early), gates: early };
  }

  // 2. A stopped Run (after Stop, or after an SSD run) holds its buffered frames and the start discards them: ask first, and tell the backend that the operator agreed.
  const discard = startDiscardNotice(await deps.fetchRingStatus().catch(() => null));
  if (discard && !(await deps.confirm(discard))) return { kind: "cancelled" };

  // 3. The file.
  const picked = await deps.pickFile();
  if (!picked) return { kind: "cancelled" };

  // 4. The series note, once the operator has committed to a file.
  if (deps.needsInlineForSeries && (await deps.needsInlineForSeries().catch(() => false))) {
    await inform({ kind: "info", title: "Start Experiment", text: INLINE_SERIES_NOTICE });
  }

  // 5. Readiness with the destination.
  const readiness = await deps.fetchReadiness(picked);
  const blockers = blockingGates(readiness.gates);
  if (!readiness.valid || !readiness.ready || blockers.length) {
    const explained = explainReadiness(blockers, warningGates(readiness.gates));
    if (explained) await inform({ kind: "warning", ...explained });
    return { kind: "not-ready", message: blockersLine(picked, readiness.valid ? blockers : []), gates: blockers };
  }

  // 6. Start.
  const res = await deps.start(picked, discard !== "");
  if (!res.ok) {
    const wording = startOutcomeWording(res.outcome, res.message, picked);
    if (wording.explainGates) {
      // NotReady: the reply carries no gates; read them again for the explanation Qt shows from the refused Start.
      const again = await deps.fetchReadiness(picked).catch(() => null);
      const refused = again ? blockingGates(again.gates) : [];
      const explained = explainReadiness(refused, again ? warningGates(again.gates) : []);
      if (explained) {
        await inform({ kind: "warning", ...explained });
        return { kind: "not-ready", message: blockersLine(picked, refused), gates: refused };
      }
    }
    await inform(wording.dialog);
    return { kind: "refused", path: picked, message: res.message, summary: wording.summary, outcome: res.outcome };
  }
  const warnings = warningGates(readiness.gates);
  return { kind: "started", path: picked, notice: warningNotice(readiness.gates), warnings };
}
