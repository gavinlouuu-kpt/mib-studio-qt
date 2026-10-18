// @vitest-environment jsdom
// #512 S9: the review module over the WebSocket transport (YOFO Studio in a
// browser against the instrument). The transport index is replaced by a real
// wsTransport over a fake socket: chart staging is refused by the transport
// (no binary request path on the server) and the job dialog says so in plain
// words; "Show in folder" is disabled with its reason instead of calling the
// opener.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { revealItemInDir } from "@tauri-apps/plugin-opener";
import { REVIEW_OPERATION_KINDS } from "./reviewContract";
import { stageCharts } from "./charts/chartExport";
import { JobDialog, REMOTE_REVEAL_REASON } from "./exports/ExportDialogs";
import { prepareFailureText, useReviewJobs, type ReviewJobs, type TrackedJob } from "./exports/useReviewJobs";
import { TransportUnsupportedError } from "../transport";

const socket = vi.hoisted(() => ({ text: [] as { cmd: string; args: unknown }[], binary: 0 }));

vi.mock("@tauri-apps/plugin-opener", () => ({ revealItemInDir: vi.fn(), openUrl: vi.fn() }));
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
      if (typeof data !== "string") {
        socket.binary++;
        return;
      }
      const request = JSON.parse(data) as { request_id: number; cmd: string; args: unknown };
      socket.text.push({ cmd: request.cmd, args: request.args });
      setTimeout(() => this.onmessage?.({ data: JSON.stringify({ request_id: request.request_id, ok: null }) }), 0);
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

let root: Root;
let host: HTMLDivElement;
beforeEach(() => {
  socket.text.length = 0;
  socket.binary = 0;
  vi.mocked(revealItemInDir).mockClear();
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
});

const CHARTS_TEXT = "Chart images cannot be sent to the instrument over this browser connection yet, so exports that include charts cannot run here.";

it("disables Show in folder with its reason and never calls the opener", async () => {
  const job: TrackedJob = { id: "5", kind: REVIEW_OPERATION_KINDS.ExportMetrics, title: "Export Metrics", phase: "completed", progress: 1, total: 1, message: "/data/run_metrics.csv", cancelling: false };
  await act(async () => root.render(<JobDialog job={job} onCancel={() => {}} onClose={() => {}} />));
  const reveal = Array.from(host.querySelectorAll("button")).find((b) => b.textContent === "Show in folder")!;
  expect(reveal.disabled).toBe(true);
  expect(reveal.title).toBe(REMOTE_REVEAL_REASON);
  await act(async () => void reveal.dispatchEvent(new MouseEvent("click", { bubbles: true })));
  expect(revealItemInDir).not.toHaveBeenCalled();
  expect(host.textContent).toContain("Written to /data/run_metrics.csv");
});

it("refuses chart staging over the WebSocket and says so in plain words", async () => {
  const jobs: { current: ReviewJobs | null } = { current: null };
  const log: string[] = [];
  function Harness() {
    const j = useReviewJobs(false, (l) => void log.push(l));
    jobs.current = j;
    return j.job ? <JobDialog job={j.job} onCancel={j.cancel} onClose={j.dismiss} /> : null;
  }
  await act(async () => root.render(<Harness />));
  const run = vi.fn(async () => ({ ok: true, command: 0, message: "", operation_id: "9" }));
  let finished: TrackedJob | null = { id: "?" } as TrackedJob;
  await act(async () => {
    finished = await jobs.current!.start("Export Charts", REVIEW_OPERATION_KINDS.ExportCharts, run, {
      label: "Rendering charts…",
      work: () => stageCharts([{ name: "scatter_plot.tiff", bytes: new Uint8Array([0x89, 0x50, 0x4e, 0x47]) }]),
    });
  });
  expect(finished).toBeNull();
  expect(run).not.toHaveBeenCalled();
  expect(host.textContent).toContain(`Not started: ${CHARTS_TEXT}`);
  expect(log).toEqual([`Export Charts: ${CHARTS_TEXT}`]);
  // The clear went out as a normal request; the chart bytes never left.
  expect(socket.text).toEqual([{ cmd: "review_clear_charts", args: {} }]);
  expect(socket.binary).toBe(0);
});

it("keeps the existing wording for every other preparation failure", () => {
  expect(prepareFailureText("Rendering charts…", new Error("no 2D canvas context"))).toBe("Rendering charts… failed: Error: no 2D canvas context");
  expect(prepareFailureText("Rendering charts…", "TRANSPORT_LOST")).toBe("Rendering charts… failed: TRANSPORT_LOST");
  expect(prepareFailureText("Rendering charts…", new TransportUnsupportedError("ws", "review_stage_chart"))).toBe(CHARTS_TEXT);
});
