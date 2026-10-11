// The Start Experiment flow (extracted from App.tsx so it can be tested): pick the file, ask before a stopped Run's buffered frames are discarded, evaluate readiness, start.
import { READINESS_GATE_STATUSES } from "./bridgeContract";
import { startDiscardNotice, type RingStatus } from "./ringPlayback";

export interface StartGate { id: string; status: number; reason: string; remediation?: string }
export interface StartReadiness { valid: boolean; ready: boolean; gates: StartGate[] }
export interface StartResult { ok: boolean; message: string }

export interface StartDeps {
  /** A fresh ring status (not the polled snapshot, which can be seconds old or missing for another client). */
  fetchRingStatus: () => Promise<RingStatus | null | undefined>;
  confirm: (message: string) => Promise<boolean>;
  pickFile: () => Promise<string | null | undefined>;
  fetchReadiness: (path: string) => Promise<StartReadiness>;
  start: (path: string, acknowledgeDiscardRing: boolean) => Promise<StartResult>;
}

export type StartOutcome =
  | { kind: "cancelled" }
  | { kind: "not-ready"; message: string }
  | { kind: "refused"; path: string; message: string }
  | { kind: "started"; path: string; notice: string };

export async function startExperiment(deps: StartDeps): Promise<StartOutcome> {
  // A stopped Run (after Stop, or after an SSD run) holds its buffered frames and the start discards them: ask first, and tell the backend that the operator agreed.
  const discard = startDiscardNotice(await deps.fetchRingStatus().catch(() => null));
  if (discard && !(await deps.confirm(discard))) return { kind: "cancelled" };
  const picked = await deps.pickFile();
  if (!picked) return { kind: "cancelled" };
  const readiness = await deps.fetchReadiness(picked);
  if (!readiness.valid || !readiness.ready) {
    const reason = readiness.gates.filter(g => g.status === READINESS_GATE_STATUSES.Fail || g.status === READINESS_GATE_STATUSES.Unavailable)
      .map(g => `${g.id}: ${g.reason}${g.remediation ? ` — ${g.remediation}` : ""}`).join("; ");
    return { kind: "not-ready", message: `${picked}: ${reason || "Backend readiness unavailable; experiment was not started."}` };
  }
  const notice = readiness.gates.filter(g => g.id === "storage.persistent" && g.status === READINESS_GATE_STATUSES.Warn).map(g => g.reason).join(" ");
  const res = await deps.start(picked, discard !== "");
  if (!res.ok) return { kind: "refused", path: picked, message: res.message };
  return { kind: "started", path: picked, notice };
}
