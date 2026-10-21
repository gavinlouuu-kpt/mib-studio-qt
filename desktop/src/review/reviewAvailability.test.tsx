// @vitest-environment jsdom
// #763 F7: YOFO Review over the WebSocket against a yofo-studio-server that
// does not serve the review commands yet (#512 S11). The transport index is a
// real wsTransport over a fake socket whose server answers every review
// command "UNKNOWN_COMMAND: <name>", the string mib-app-commands' dispatcher
// returns, and serves the backend's own boot commands. The shell must say the
// review commands are unavailable (not offer Open), and nothing may keep
// asking the server: no event poll, no repeated errors.
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
} from "./reviewAvailability";
import { useReviewJobs } from "./exports/useReviewJobs";
import { reviewBridge } from "./reviewBridge";
import ReviewApp from "./ReviewApp";

const server = vi.hoisted(() => ({ requests: [] as string[], served: new Set(["is_initialized", "init", "shell_log"]) }));
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
      const reply = server.served.has(request.cmd)
        ? { request_id: request.request_id, ok: request.cmd === "shell_log" ? null : true }
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
        s.readyState = 1;
        s.onopen?.();
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
    const notice = host.querySelector('[data-testid="review-unavailable"]');
    expect(notice?.textContent).toBe(REVIEW_UNAVAILABLE_TEXT);
    const openButtons = Array.from(host.querySelectorAll("button")).filter((b) => b.textContent?.includes("Select HDF File"));
    expect(openButtons.length).toBeGreaterThan(0);
    for (const b of openButtons) expect(b.disabled).toBe(true);
    // Only the boot probe reached the server; no event poll, nothing repeated.
    const review = server.requests.filter((c) => !server.served.has(c) && c !== "review_check_update");
    expect(review).toEqual(["review_abi_version"]);
    expect(tauriListen).not.toHaveBeenCalled();
    await act(async () => (Array.from(host.querySelectorAll("button")).find((b) => b.textContent?.startsWith("Log")) as HTMLButtonElement).click());
    const lines = Array.from(host.querySelectorAll('[role="log"] div')).map((d) => d.textContent ?? "");
    expect(lines.filter((l) => l.includes("review commands are not served by this server"))).toHaveLength(1);
    expect(lines.filter((l) => l.includes("UNKNOWN_COMMAND"))).toEqual([]);
  });
});
