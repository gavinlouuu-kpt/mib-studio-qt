// Review job tracking (plan 2026-10-01-standalone-review-app, PR 4). The
// panel owns the review event drain in both products (MIB Studio never
// drained it before, so its job outcomes were lost): `poll_review_events`
// at 5 Hz, terminal outcomes to the log, and the one job the panel started
// (the backend runs one at a time) tracked for the progress dialog.
//
// Events are buffered per operation id so a job that finishes before its
// start call returns (Export Charts on two images) still reaches its dialog.
//
// A server without the review commands (yofo-studio-server before #512 S11)
// answers the poll with UNKNOWN_COMMAND: the poll stops at once and says so
// once, instead of failing five times a second (#763 F7). Any other error is
// logged when it changes, not on every tick.
import { useCallback, useEffect, useRef, useState } from "react";
import { OPERATION_STATES } from "../../bridgeContract";
import { REVIEW_OPERATION_KINDS } from "../reviewContract";
import { isTransportUnsupported } from "../../transport";
import { reviewBridge, unsupportedText, type ReviewCmdResult, type ReviewEvent } from "../reviewBridge";
import { isReviewUnavailable, useReviewAvailability } from "../reviewAvailability";

const POLL_MS = 200;
const BUFFERED_IDS = 64;

export type JobPhase = "running" | "completed" | "failed" | "cancelled";

export interface TrackedJob {
  id: string;
  kind: number;
  title: string;
  phase: JobPhase;
  progress: number;
  total: number;
  /** Latest progress text, or the terminal message (output path, summary, error). */
  message: string;
  cancelling: boolean;
}

export function kindName(kind: number): string {
  return Object.entries(REVIEW_OPERATION_KINDS).find(([, v]) => v === kind)?.[0] ?? `kind ${kind}`;
}

export function isTerminal(state: number): boolean {
  return state === OPERATION_STATES.Completed || state === OPERATION_STATES.Failed || state === OPERATION_STATES.Cancelled || state === OPERATION_STATES.TimedOut;
}

/** Fold one event into a tracked job (pure; exported for tests). */
export function applyJobEvent(job: TrackedJob, e: ReviewEvent): TrackedJob {
  if (e.operation_id !== job.id || job.phase !== "running") return job;
  switch (e.state) {
    case OPERATION_STATES.Progress:
      return { ...job, progress: Number(e.progress), total: Number(e.total), message: e.message || job.message };
    case OPERATION_STATES.Completed:
      return { ...job, phase: "completed", message: e.message, progress: job.total || 1, total: job.total || 1 };
    case OPERATION_STATES.Failed:
    case OPERATION_STATES.TimedOut:
      return { ...job, phase: "failed", message: e.message };
    case OPERATION_STATES.Cancelled:
      return { ...job, phase: "cancelled", message: e.message };
    default:
      return job;
  }
}

/** The dialog text for a preparation that failed: plain words when the
 * transport cannot carry it (chart snapshots over the WebSocket), else the
 * label and the error. */
export function prepareFailureText(label: string, e: unknown): string {
  return isTransportUnsupported(e) ? unsupportedText(e) : `${label} failed: ${e}`;
}

/** Optional start hooks. With `onRefused` a refusal goes back to the caller
 * (still logged) instead of the job dialog, so the caller's own dialog can
 * show it and let the operator choose again (Regenerate masks, #512 S8);
 * `onStarted` runs once the backend accepted the job. */
export interface StartHooks {
  onRefused?: (message: string) => void;
  onStarted?: () => void;
}

export interface ReviewJobs {
  /** The tracked job (running, or finished and not yet dismissed). */
  job: TrackedJob | null;
  /** Start a job: `prepare` (optional; e.g. rendering chart snapshots)
   * runs first behind a "Preparing…" dialog, then `run` issues the bridge
   * command. Resolves with the finished job (null when refused or the
   * preparation failed — logged and shown, or handed to `hooks.onRefused`). */
  start: (
    title: string,
    kind: number,
    run: () => Promise<ReviewCmdResult>,
    prepare?: { label: string; work: () => Promise<void> },
    hooks?: StartHooks,
  ) => Promise<TrackedJob | null>;
  cancel: () => void;
  dismiss: () => void;
  /** True while a tracked job runs. */
  busy: boolean;
}

export function useReviewJobs(ready: boolean, log: (line: string) => void): ReviewJobs {
  const [job, setJob] = useState<TrackedJob | null>(null);
  const jobRef = useRef<TrackedJob | null>(null);
  const buffer = useRef(new Map<string, ReviewEvent[]>());
  const waiters = useRef(new Map<string, (j: TrackedJob) => void>());
  const tickBusy = useRef(false);
  const availability = useReviewAvailability().state;

  const update = useCallback((next: TrackedJob | null) => {
    jobRef.current = next;
    setJob(next);
    if (next && next.phase !== "running") {
      const w = waiters.current.get(next.id);
      if (w) {
        waiters.current.delete(next.id);
        w(next);
      }
    }
  }, []);

  const ingest = useCallback(
    (events: ReviewEvent[]) => {
      let current = jobRef.current;
      for (const e of events) {
        if (isTerminal(e.state)) {
          const what = `${kindName(e.kind)} ${e.operation_id}`;
          if (e.state === OPERATION_STATES.Completed) log(`${what} completed: ${e.message}`);
          else if (e.state === OPERATION_STATES.Cancelled) log(`${what} cancelled${e.message ? `: ${e.message}` : ""}`);
          else log(`${what} failed: ${e.message}`);
        }
        if (current && e.operation_id === current.id) {
          current = applyJobEvent(current, e);
        } else {
          const list = buffer.current.get(e.operation_id) ?? [];
          list.push(e);
          buffer.current.set(e.operation_id, list);
          while (buffer.current.size > BUFFERED_IDS) buffer.current.delete(buffer.current.keys().next().value as string);
        }
      }
      if (current !== jobRef.current) update(current);
    },
    [log, update],
  );

  useEffect(() => {
    if (!ready || availability === "unavailable") return;
    let stopped = false;
    let lastError = "";
    const id = window.setInterval(async () => {
      if (tickBusy.current || stopped) return;
      tickBusy.current = true;
      try {
        ingest(await reviewBridge.pollEvents());
        lastError = "";
      } catch (e) {
        if (isReviewUnavailable(e)) {
          stopped = true;
          window.clearInterval(id);
          log("review job events are not served by this server; polling stopped");
          return;
        }
        const text = String(e);
        if (text !== lastError) log(`review events error: ${text}`);
        lastError = text;
      } finally {
        tickBusy.current = false;
      }
    }, POLL_MS);
    return () => {
      stopped = true;
      window.clearInterval(id);
    };
  }, [ready, availability, ingest, log]);

  const start = useCallback(
    async (
      title: string,
      kind: number,
      run: () => Promise<ReviewCmdResult>,
      prepare?: { label: string; work: () => Promise<void> },
      hooks?: StartHooks,
    ): Promise<TrackedJob | null> => {
      if (prepare) {
        // Not an operation yet: no id, nothing to cancel.
        update({ id: "", kind, title, phase: "running", progress: 0, total: 0, message: prepare.label, cancelling: true });
        try {
          await prepare.work();
        } catch (e) {
          const message = prepareFailureText(prepare.label, e);
          log(`${title}: ${message}`);
          update({ id: "0", kind, title, phase: "failed", progress: 0, total: 0, message, cancelling: false });
          return null;
        }
      }
      let r: ReviewCmdResult;
      try {
        r = await run();
      } catch (e) {
        r = { ok: false, command: 0, message: String(e), operation_id: "0" };
      }
      if (!r.ok || r.operation_id === "0") {
        log(`${title} refused: ${r.message}`);
        if (hooks?.onRefused) {
          if (prepare) update(null);
          hooks.onRefused(r.message || "refused");
        } else {
          update({ id: "0", kind, title, phase: "failed", progress: 0, total: 0, message: r.message || "refused", cancelling: false });
        }
        return null;
      }
      log(`${title} started (operation ${r.operation_id})`);
      hooks?.onStarted?.();
      let tracked: TrackedJob = { id: r.operation_id, kind, title, phase: "running", progress: 0, total: 0, message: "", cancelling: false };
      for (const e of buffer.current.get(r.operation_id) ?? []) tracked = applyJobEvent(tracked, e);
      buffer.current.delete(r.operation_id);
      const done = new Promise<TrackedJob>((resolve) => waiters.current.set(tracked.id, resolve));
      update(tracked);
      return done;
    },
    [log, update],
  );

  const cancel = useCallback(() => {
    const j = jobRef.current;
    if (!j || j.phase !== "running" || j.cancelling) return;
    update({ ...j, cancelling: true });
    void reviewBridge.cancel(j.id).then((r) => {
      if (!r.ok) log(`cancel refused: ${r.message}`);
    });
  }, [log, update]);

  const dismiss = useCallback(() => {
    const j = jobRef.current;
    if (j && j.phase === "running") return;
    update(null);
  }, [update]);

  return { job, start, cancel, dismiss, busy: job?.phase === "running" };
}
