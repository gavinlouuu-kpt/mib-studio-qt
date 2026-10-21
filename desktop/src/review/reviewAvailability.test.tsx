// @vitest-environment jsdom
// #763 F7: YOFO Review over the WebSocket against a yofo-studio-server that
// does not serve the review commands yet (#512 S11). The transport index is a
// real wsTransport over a fake socket whose server answers every review
// command "UNKNOWN_COMMAND: <name>", the string mib-app-commands' dispatcher
// returns, and serves the backend's own boot commands. The shell must say the
// review commands are unavailable (not offer Open), and nothing may keep
// asking the server: no event poll, no repeated errors. The answer holds for
// one connection: after a restart or upgrade (a new connection) or Retry the
// shell asks again, and the panel and the poll come back only if the server
// now serves them.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  REVIEW_UNAVAILABLE_TEXT,
  ReviewUnavailableError,
  gateReviewCommand,
  isReviewUnavailable,
  isUnknownCommand,
  reviewAvailability,
  reviewPollAllowed,
  watchReviewConnection,
} from "./reviewAvailability";
import type { SessionSource, SessionState } from "../transport/Transport";
import { useReviewJobs } from "./exports/useReviewJobs";
import { reviewBridge } from "./reviewBridge";
import ReviewApp from "./ReviewApp";

// The fake yofo-studio-server. It always serves the backend's boot commands;
// `review` holds the review commands it serves (none before #512 S11). Each
// connection gets a new client id (the session message the real server sends
// on connect); `down` refuses new connections (a restart in progress).
const BOOT: Record<string, unknown> = { is_initialized: true, init: true, shell_log: null };
const S11_REVIEW: Record<string, unknown> = {
  review_abi_version: 2,
  poll_review_events: { events: [] },
  review_set_pixel_to_micron: null,
  fetch_review_info: { file_open: false },
};
const server = vi.hoisted(() => ({
  requests: [] as string[],
  review: {} as Record<string, unknown>,
  sockets: [] as { close(): void }[],
  clients: 0,
  down: false,
}));
const tauriListen = vi.hoisted(() => vi.fn(async () => () => undefined));

vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "0.0.0") }));
vi.mock("@tauri-apps/api/event", () => ({ listen: tauriListen }));
vi.mock("../transport", async (importOriginal) => {
  const real = await importOriginal<typeof import("../transport")>();
  const { createWsTransport } = await import("../transport/wsTransport");
  class FakeSocket {
    readyState = 0;
    binaryType = "blob";
    onopen?: () => void;
    onclose?: () => void;
    onerror?: () => void;
    onmessage?: (message: { data: unknown }) => void;
    send(data: unknown) {
      const request = JSON.parse(String(data)) as { request_id: number; cmd: string };
      server.requests.push(request.cmd);
      const known = { ...BOOT, ...server.review };
      const reply =
        request.cmd in known
          ? { request_id: request.request_id, ok: known[request.cmd] }
          : { request_id: request.request_id, error: `UNKNOWN_COMMAND: ${request.cmd}` };
      setTimeout(() => this.onmessage?.({ data: JSON.stringify(reply) }), 0);
    }
    close() {
      this.readyState = 3;
      this.onclose?.();
    }
  }
  (globalThis as { WebSocket?: unknown }).WebSocket ??= { OPEN: 1 };
  const ws = createWsTransport("ws://instrument/ws", {
    reconnectMs: 0,
    createSocket: () => {
      const s = new FakeSocket();
      setTimeout(() => {
        if (server.down) return s.close();
        s.readyState = 1;
        s.onopen?.();
        const id = ++server.clients;
        s.onmessage?.({ data: JSON.stringify({ session: { client_id: id, controller_id: id } }) });
        server.sockets.push(s);
      }, 0);
      return s as unknown as WebSocket;
    },
  });
  return { ...real, transport: ws, isRemote: true, invoke: ws.invoke, upload: ws.upload };
});

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

let host: HTMLDivElement;
let root: Root;

beforeEach(() => {
  vi.useFakeTimers();
  reviewAvailability.reset();
  server.requests.length = 0;
  server.review = {};
  server.down = false;
  tauriListen.mockClear();
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});

afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  reviewAvailability.reset();
});

const settle = (ms: number) => act(async () => vi.advanceTimersByTimeAsync(ms));
const isBoot = (c: string) => c in BOOT || c === "review_check_update";
const sent = (cmd: string) => server.requests.filter((c) => c === cmd).length;
const buttons = (text: string) => Array.from(host.querySelectorAll("button")).filter((b) => b.textContent?.includes(text));
const logLines = async () => {
  if (!host.querySelector('[role="log"]'))
    await act(async () => (Array.from(host.querySelectorAll("button")).find((b) => b.textContent?.startsWith("Log")) as HTMLButtonElement).click());
  return Array.from(host.querySelectorAll('[role="log"] div')).map((d) => d.textContent ?? "");
};
/** The server goes away and comes back (a restart), optionally upgraded. */
const restartServer = async (review: Record<string, unknown>, downMs = 0) => {
  server.down = downMs > 0;
  await act(async () => server.sockets[server.sockets.length - 1]?.close());
  server.review = review;
  if (downMs > 0) await settle(downMs);
  server.down = false;
};

describe("the server's answer", () => {
  it("recognises UNKNOWN_COMMAND in the shapes a transport rejects with", () => {
    expect(isUnknownCommand("UNKNOWN_COMMAND: review_list_dir")).toBe(true);
    expect(isUnknownCommand(new Error("UNKNOWN_COMMAND: poll_review_events"))).toBe(true);
    expect(isUnknownCommand({ code: "UNKNOWN_COMMAND" })).toBe(true);
    for (const other of ["VIEWER_ONLY: another client controls the instrument", "TRANSPORT_LOST", new Error("boom"), null, undefined, 42])
      expect(isUnknownCommand(other)).toBe(false);
    expect(isReviewUnavailable(new ReviewUnavailableError("review_open"))).toBe(true);
    expect(isReviewUnavailable("TRANSPORT_LOST")).toBe(false);
  });

  it("marks the review commands unavailable on UNKNOWN_COMMAND and then refuses locally", async () => {
    const call = vi.fn(async () => Promise.reject("UNKNOWN_COMMAND: review_list_dir"));
    await expect(gateReviewCommand("review_list_dir", call)).rejects.toBe("UNKNOWN_COMMAND: review_list_dir");
    expect(reviewAvailability.get()).toEqual({ state: "unavailable", command: "review_list_dir" });
    const next = vi.fn(async () => ["a.h5"]);
    await expect(gateReviewCommand("poll_review_events", next)).rejects.toBeInstanceOf(ReviewUnavailableError);
    expect(next).not.toHaveBeenCalled();
  });

  it("marks them available on a reply and passes other errors through (the Tauri path)", async () => {
    await expect(gateReviewCommand("review_abi_version", async () => 3)).resolves.toBe(3);
    expect(reviewAvailability.get().state).toBe("available");
    await expect(gateReviewCommand("review_open", async () => Promise.reject("TRANSPORT_LOST"))).rejects.toBe("TRANSPORT_LOST");
    expect(reviewAvailability.get().state).toBe("available");
    const call = vi.fn(async () => ["run.h5"]);
    await expect(gateReviewCommand("review_list_dir", call)).resolves.toEqual(["run.h5"]);
    expect(call).toHaveBeenCalledOnce();
  });
});

describe("the job-event poll", () => {
  let log: ReturnType<typeof vi.fn>;
  function Jobs({ ready }: { ready: boolean }) {
    useReviewJobs(ready, log);
    return null;
  }

  it("stops after the server answers UNKNOWN_COMMAND, with one log line", async () => {
    log = vi.fn();
    const poll = vi.spyOn(reviewBridge, "pollEvents");
    try {
      await act(async () => root.render(<Jobs ready={true} />));
      await settle(3000);
      expect(server.requests.filter((c) => c === "poll_review_events")).toHaveLength(1);
      expect(poll).toHaveBeenCalledOnce();
      expect(log.mock.calls.map(([line]) => line)).toEqual(["review job events are not served by this server; polling stopped"]);
    } finally {
      poll.mockRestore();
    }
  });

  it("logs any other error once while it repeats", async () => {
    log = vi.fn();
    const poll = vi.spyOn(reviewBridge, "pollEvents").mockRejectedValue("TRANSPORT_LOST");
    try {
      await act(async () => root.render(<Jobs ready={true} />));
      await settle(2000);
      expect(poll.mock.calls.length).toBeGreaterThan(5);
      expect(log.mock.calls.map(([line]) => line)).toEqual(["review events error: TRANSPORT_LOST"]);
    } finally {
      poll.mockRestore();
    }
  });
});

describe("YOFO Review on a server without the review commands", () => {
  it("says the review commands are unavailable, offers no Open and stops asking", async () => {
    await act(async () => root.render(<ReviewApp />));
    await settle(3000);
    const notice = host.querySelector('[data-testid="review-unavailable-text"]');
    expect(notice?.textContent).toBe(REVIEW_UNAVAILABLE_TEXT);
    const openButtons = buttons("Select HDF File");
    expect(openButtons.length).toBeGreaterThan(0);
    for (const b of openButtons) expect(b.disabled).toBe(true);
    // Only the boot probe reached the server; no event poll, nothing repeated.
    const review = server.requests.filter((c) => !isBoot(c));
    expect(review).toEqual(["review_abi_version"]);
    expect(tauriListen).not.toHaveBeenCalled();
    const lines = await logLines();
    expect(lines.filter((l) => l.includes("review commands are not served by this server"))).toHaveLength(1);
    expect(lines.filter((l) => l.includes("UNKNOWN_COMMAND"))).toEqual([]);
  });

  it("asks again after the server restarts with the review commands, then enables review and the poll", async () => {
    await act(async () => root.render(<ReviewApp />));
    await settle(3000);
    expect(reviewAvailability.get().state).toBe("unavailable");
    // A restart that takes 2.5 s: the recheck cannot reach it at first and tries again.
    await restartServer(S11_REVIEW, 2500);
    expect(reviewAvailability.get().state).toBe("checking");
    expect(buttons("Checking")[0]?.disabled).toBe(true);
    expect(sent("poll_review_events")).toBe(0);
    await settle(5000);
    expect(reviewAvailability.get().state).toBe("available");
    expect(host.querySelector('[data-testid="review-unavailable"]')).toBeNull();
    for (const b of buttons("Select HDF File")) expect(b.disabled).toBe(false);
    expect(sent("review_abi_version")).toBe(2);
    await settle(2000); // the poll starts with the render that follows the answer
    expect(sent("poll_review_events")).toBeGreaterThan(5);
    const lines = await logLines();
    expect(lines.some((l) => l.includes("review check: TRANSPORT_UNAVAILABLE; trying again"))).toBe(true);
    expect(lines.filter((l) => l.includes("review enabled"))).toHaveLength(1);
  });

  it("stays off when the new connection still lacks them, without polling", async () => {
    await act(async () => root.render(<ReviewApp />));
    await settle(3000);
    await restartServer({});
    await settle(3000);
    expect(reviewAvailability.get().state).toBe("unavailable");
    expect(host.querySelector('[data-testid="review-unavailable-text"]')?.textContent).toBe(REVIEW_UNAVAILABLE_TEXT);
    expect(sent("review_abi_version")).toBe(2);
    expect(sent("poll_review_events")).toBe(0);
    // Same connection again: nothing more is asked.
    await settle(5000);
    expect(server.requests.filter((c) => !isBoot(c))).toEqual(["review_abi_version", "review_abi_version"]);
  });

  it("Retry asks again on the same connection and enables review once they are served", async () => {
    await act(async () => root.render(<ReviewApp />));
    await settle(3000);
    const clients = server.clients;
    // Retry against the same server: still unavailable, the notice stays.
    await act(async () => buttons("Retry")[0].click());
    await settle(1000);
    expect(reviewAvailability.get().state).toBe("unavailable");
    expect(buttons("Retry")).toHaveLength(1);
    expect(sent("review_abi_version")).toBe(2);
    expect(sent("poll_review_events")).toBe(0);
    // The server now serves them (no reconnect needed): Retry enables review.
    server.review = S11_REVIEW;
    await act(async () => buttons("Retry")[0].click());
    await settle(1000);
    expect(reviewAvailability.get().state).toBe("available");
    expect(host.querySelector('[data-testid="review-unavailable"]')).toBeNull();
    for (const b of buttons("Select HDF File")) expect(b.disabled).toBe(false);
    expect(sent("review_abi_version")).toBe(3);
    await settle(1000);
    expect(sent("poll_review_events")).toBeGreaterThan(2);
    expect(server.clients).toBe(clients);
  });
});

describe("the connection the answer came on", () => {
  function fakeSession(initial: SessionState) {
    let state = initial;
    const listeners = new Set<(s: SessionState) => void>();
    const source: SessionSource = {
      get: () => state,
      subscribe: (l) => {
        listeners.add(l);
        return () => listeners.delete(l);
      },
    };
    return { source, emit: (next: SessionState) => { state = next; listeners.forEach((l) => l(state)); } };
  }
  const markUnavailable = () => gateReviewCommand("review_list_dir", async () => Promise.reject("UNKNOWN_COMMAND: review_list_dir")).catch(() => undefined);

  it("asks again on a lost or new connection, not on a handover", async () => {
    const session = fakeSession({ clientId: 1, controllerId: 1 });
    const stop = watchReviewConnection(session.source);
    try {
      await markUnavailable();
      session.emit({ clientId: 1, controllerId: 4 }); // another client took control
      expect(reviewAvailability.get().state).toBe("unavailable");
      session.emit({}); // the connection was lost
      expect(reviewAvailability.get().state).toBe("checking");
      session.emit({ clientId: 2, controllerId: 2 }); // the probe's new connection (ignored while checking)
      expect(reviewAvailability.get().state).toBe("checking");
      await markUnavailable(); // the probe there: still unknown to that server
      expect(reviewAvailability.get().state).toBe("unavailable");
      session.emit({ clientId: 2, controllerId: 5 });
      expect(reviewAvailability.get().state).toBe("unavailable");
      session.emit({ clientId: 9, controllerId: 9 }); // a new session
      expect(reviewAvailability.get().state).toBe("checking");
      await gateReviewCommand("review_abi_version", async () => 2);
      expect(reviewAvailability.get().state).toBe("available");
      session.emit({}); // available stays available; a review call finds out
      expect(reviewAvailability.get().state).toBe("available");
    } finally {
      stop();
    }
  });

  it("Retry is the same recheck, and the poll waits for its answer", () => {
    reviewAvailability.recheck();
    expect(reviewAvailability.get().state).toBe("unknown");
    expect(watchReviewConnection(undefined)).toBeTypeOf("function"); // the Tauri transport has no session
    expect(["unknown", "available"].map((s) => reviewPollAllowed(s as never))).toEqual([true, true]);
    expect(["unavailable", "checking"].map((s) => reviewPollAllowed(s as never))).toEqual([false, false]);
  });
});
