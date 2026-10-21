// Whether this backend serves the review commands (#763 F7).
//
// YOFO Review in a browser talks to yofo-studio-server, which does not serve
// the review commands yet (#512 S11, which registers them as one set). The
// server answers each with the error string "UNKNOWN_COMMAND: <name>"
// (`mib-app-commands` dispatch; the WebSocket transport rejects with that
// string). Before this the page still said the backend was ready, listed an
// empty folder and polled the job events five times a second, logging the
// same error on every tick.
//
// Now the first UNKNOWN_COMMAND answer to a review command marks the review
// commands unavailable on that connection: the review bridge refuses every
// later review call locally with ReviewUnavailableError (nothing is sent,
// nothing is logged by the server), the shell says so in plain words, and
// the job-event poll stops. A reply marks them available. The Tauri shells
// serve every review command, so there the state only ever becomes
// "available".
//
// The answer holds for one connection only: the server may be restarted or
// upgraded while the page is open. A lost connection, a new session (another
// client id) or the notice's Retry moves "unavailable" to "checking"; the
// shell probes again (`review_abi_version`), and only a reply makes them
// available and lets the poll run again.
import { useSyncExternalStore } from "react";
import type { SessionSource, SessionState } from "../transport/Transport";

export type ReviewAvailabilityState = "unknown" | "available" | "unavailable" | "checking";

export interface ReviewAvailability {
  state: ReviewAvailabilityState;
  /** The command the server did not know ("" unless unavailable). */
  command: string;
}

/** What the shell shows instead of "ready" when the server lacks the review commands. */
export const REVIEW_UNAVAILABLE_TEXT =
  "This instrument's server does not provide the review commands yet, so YOFO Review cannot list or open files over this browser connection. Use the Review tab of YOFO Studio, or YOFO Review on a computer.";

/** The server did not know the command: the transport rejected with the
 * dispatcher's "UNKNOWN_COMMAND: <name>" (a string over the WebSocket; an
 * Error or an object with a `code` is accepted too). */
export function isUnknownCommand(error: unknown): boolean {
  if (typeof error === "string") return error.startsWith("UNKNOWN_COMMAND");
  if (error instanceof Error) return error.message.startsWith("UNKNOWN_COMMAND");
  if (error && typeof error === "object" && "code" in error) return (error as { code: unknown }).code === "UNKNOWN_COMMAND";
  return false;
}

/** A review call refused locally: the server is known not to serve the review commands. */
export class ReviewUnavailableError extends Error {
  readonly code = "REVIEW_UNAVAILABLE";
  readonly command: string;
  constructor(command: string) {
    super(`REVIEW_UNAVAILABLE: ${command} is not served by this server`);
    this.name = "ReviewUnavailableError";
    this.command = command;
  }
}

/** True for the local refusal and for the server's own UNKNOWN_COMMAND answer. */
export function isReviewUnavailable(error: unknown): boolean {
  return error instanceof ReviewUnavailableError || isUnknownCommand(error);
}

let current: ReviewAvailability = { state: "unknown", command: "" };
const listeners = new Set<() => void>();
// The connection the server's UNKNOWN_COMMAND came on (its client id, when
// the transport has sessions) and the session source being watched.
let watched: SessionSource | undefined;
let unavailableOn: number | undefined;

function set(next: ReviewAvailability) {
  if (next.state === current.state && next.command === current.command) return;
  current = next;
  for (const listener of listeners) listener();
}

export const reviewAvailability = {
  get: (): ReviewAvailability => current,
  subscribe(listener: () => void): () => void {
    listeners.add(listener);
    return () => {
      listeners.delete(listener);
    };
  },
  /** Back to "unknown" (tests). */
  reset: () => set({ state: "unknown", command: "" }),
  /** Ask again: "unavailable" becomes "checking" until the shell's probe
   * answers (the notice's Retry, a new connection). No-op otherwise. */
  recheck: () => {
    if (current.state === "unavailable") set({ state: "checking", command: current.command });
  },
};

/** Whether the job-event poll may run: not while the commands are unavailable
 * or being checked again ("unknown" keeps the boot order of the shells). */
export function reviewPollAllowed(state: ReviewAvailabilityState): boolean {
  return state === "unknown" || state === "available";
}

/** A connection change that may have changed the server: the connection was
 * lost (empty session) or a new one has another client id. A handover (only
 * the controller changes) is the same connection. */
function connectionChanged(next: SessionState): boolean {
  if (next.clientId === undefined) return next.controllerId === undefined;
  return next.clientId !== unavailableOn;
}

/** Watch the transport's connection (the WebSocket session; the Tauri
 * transport has none): while the commands are unavailable, a lost or new
 * connection asks again (`recheck`). Returns the unsubscribe. */
export function watchReviewConnection(session: SessionSource | undefined): () => void {
  if (!session) return () => {};
  watched = session;
  const stop = session.subscribe((next) => {
    if (current.state === "unavailable" && connectionChanged(next)) reviewAvailability.recheck();
  });
  return () => {
    stop();
    if (watched === session) watched = undefined;
  };
}

/** Run one review command through the availability gate: refused locally once
 * the commands are known unavailable; an UNKNOWN_COMMAND answer marks them
 * unavailable (and still rejects with the server's answer); a reply marks
 * them available. Other errors pass through unchanged. */
export async function gateReviewCommand<T>(command: string, call: () => Promise<T>): Promise<T> {
  if (current.state === "unavailable") throw new ReviewUnavailableError(command);
  try {
    const value = await call();
    if (current.state === "unknown" || current.state === "checking") set({ state: "available", command: "" });
    return value;
  } catch (e) {
    if (isUnknownCommand(e)) {
      unavailableOn = watched?.get().clientId;
      set({ state: "unavailable", command });
    }
    throw e;
  }
}

/** The availability, re-rendering when it changes. */
export function useReviewAvailability(): ReviewAvailability {
  return useSyncExternalStore(reviewAvailability.subscribe, reviewAvailability.get, reviewAvailability.get);
}
