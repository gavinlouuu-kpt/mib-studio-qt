// The Start Experiment flow (extracted from App.tsx so it can be tested). The order is the Qt app's
// (MainWindow::onStartExperiment, epic #866):
//   1. readiness with no destination: every blocking gate but storage.output is explained BEFORE any
//      file dialog opens (MON-003),
//   2. a capture that runs Latest Frame: Qt's Switch to Every Frame / Continue with Latest Frame / Cancel, and
//      the operator's acknowledgement travels with the start (#858, SHELL-054),
//   3. ask before a stopped Run's buffered frames are discarded (Tauri-only, PZ7035),
//   4. pick the file,
//   5. tell the operator when a multi-image series forces inline realtime mode (MON-008),
//   6. readiness for that destination, explained gate by gate (MON-004),
//   7. start, and word each refusal by its typed outcome (MON-010).
// Every dialog goes through `inform`, so the flow stays testable without a window.
import { startDiscardNotice, type RingStatus } from "./ringPlayback";
import {
  DESTINATION_GATE, INLINE_SERIES_NOTICE, SWITCHED_NOTICE, blockersLine, blockingGates, explainReadiness, runsLatestFrame,
  startOutcomeWording, warningGates, warningNotice, type LatestFrameChoice, type StartDialog, type StartGate,
} from "./areas/monitoring/startReadiness";

export type { LatestFrameChoice, StartDialog, StartGate } from "./areas/monitoring/startReadiness";
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
  start: (path: string, acknowledgeDiscardRing: boolean, acknowledgeLatestFrameDrops: boolean) => Promise<StartResult>;
  /** A modal message (the dialog transport's `message`). Absent: the outcome still carries the wording. */
  inform?: (dialog: StartDialog) => Promise<void>;
  /** True when the processing configuration asks for a multi-image series. Absent: no note. */
  needsInlineForSeries?: () => Promise<boolean>;
  /** Qt's Latest Frame question; `offerSwitch` false leaves out "Switch to Every Frame". Absent: no question, and a
   *  Latest Frame start goes to the backend unacknowledged (it refuses it). */
  chooseLatestFrame?: (offerSwitch: boolean) => Promise<LatestFrameChoice>;
  /** Sets the delivery mode the next capture start uses to Every Frame (Qt's Switch). Absent: no Switch button. */
  switchToEveryFrame?: () => Promise<{ ok: boolean; message: string }>;
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

  // 2. Latest Frame discards frames on purpose: ask, as Qt does, before the file dialog. Switch and Continue both
  //    acknowledge, because the running capture keeps Latest Frame until it restarts (Qt: "this session still runs
  //    LatestFrame"); Switch also sets Every Frame for the next camera start.
  let acknowledgeLatestFrameDrops = false;
  let switchedNotice = "";
  if (deps.chooseLatestFrame && runsLatestFrame(preflight.gates)) {
    const choice = await deps.chooseLatestFrame(!!deps.switchToEveryFrame);
    if (choice === "cancel") return { kind: "cancelled" };
    if (choice === "switch" && deps.switchToEveryFrame) {
      const switched = await deps.switchToEveryFrame().catch((e: unknown) => ({ ok: false, message: String(e) }));
      if (!switched.ok) {
        // Qt's switch cannot fail; here the backend can refuse it. Stop rather than record in a mode the operator turned down.
        await inform({ kind: "warning", title: "Start Experiment", text: `The delivery mode could not be set to Every Frame: ${switched.message}\n\nThe experiment was not started.` });
        return { kind: "cancelled" };
      }
      switchedNotice = SWITCHED_NOTICE;
    }
    acknowledgeLatestFrameDrops = true;
  }

  // 3. A stopped Run (after Stop, or after an SSD run) holds its buffered frames and the start discards them: ask first, and tell the backend that the operator agreed.
  const discard = startDiscardNotice(await deps.fetchRingStatus().catch(() => null));
  if (discard && !(await deps.confirm(discard))) return { kind: "cancelled" };

  // 4. The file.
  const picked = await deps.pickFile();
  if (!picked) return { kind: "cancelled" };

  // 5. The series note, once the operator has committed to a file.
  if (deps.needsInlineForSeries && (await deps.needsInlineForSeries().catch(() => false))) {
    await inform({ kind: "info", title: "Start Experiment", text: INLINE_SERIES_NOTICE });
  }

  // 6. Readiness with the destination.
  const readiness = await deps.fetchReadiness(picked);
  const blockers = blockingGates(readiness.gates);
  if (!readiness.valid || !readiness.ready || blockers.length) {
    const explained = explainReadiness(blockers, warningGates(readiness.gates));
    if (explained) await inform({ kind: "warning", ...explained });
    return { kind: "not-ready", message: blockersLine(picked, readiness.valid ? blockers : []), gates: blockers };
  }

  // 7. Start.
  const res = await deps.start(picked, discard !== "", acknowledgeLatestFrameDrops);
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
  const notice = [switchedNotice, warningNotice(readiness.gates)].filter(Boolean).join("\n");
  return { kind: "started", path: picked, notice, warnings };
}
