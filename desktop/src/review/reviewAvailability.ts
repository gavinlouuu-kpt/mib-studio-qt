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
// commands unavailable for the page's lifetime: the review bridge refuses
// every later review call locally with ReviewUnavailableError (nothing is
// sent, nothing is logged by the server), the shell says so in plain words,
// and the job-event poll stops. A reply marks them available. The Tauri
// shells serve every review command, so there the state only ever becomes
// "available".
import { useSyncExternalStore } from "react";

export type ReviewAvailabilityState = "unknown" | "available" | "unavailable";

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
  /** Back to "unknown" (tests; a shell that wants to probe again). */
  reset: () => set({ state: "unknown", command: "" }),
};

/** Run one review command through the availability gate: refused locally once
 * the commands are known unavailable; an UNKNOWN_COMMAND answer marks them
 * unavailable (and still rejects with the server's answer); a reply marks
 * them available. Other errors pass through unchanged. */
export async function gateReviewCommand<T>(command: string, call: () => Promise<T>): Promise<T> {
  if (current.state === "unavailable") throw new ReviewUnavailableError(command);
  try {
    const value = await call();
    if (current.state === "unknown") set({ state: "available", command: "" });
    return value;
  } catch (e) {
    if (isUnknownCommand(e)) set({ state: "unavailable", command });
    throw e;
  }
}

/** The availability, re-rendering when it changes. */
export function useReviewAvailability(): ReviewAvailability {
  return useSyncExternalStore(reviewAvailability.subscribe, reviewAvailability.get, reviewAvailability.get);
}
