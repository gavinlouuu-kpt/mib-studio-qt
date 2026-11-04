// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { CapacityGate, RecordingCapacity } from "../../bridge";
import { READINESS_GATE_STATUSES } from "../../bridgeContract";
import { capacityGateLine, describeRecordingCapacity } from "./recordingCapacity";
import { RecordingCapacityPanel } from "./RecordingCapacityPanel";

const gate = (id: string, status: number, over: Partial<CapacityGate> = {}): CapacityGate =>
  ({ id, status, status_name: "", reason: "", remediation: "", detail: "", ...over });

const known = (over: Partial<RecordingCapacity> = {}): RecordingCapacity => ({
  busy: false, known: true, planned: true, status: "ok",
  gates: [
    gate("camera.window", READINESS_GATE_STATUSES.Pass, { detail: "planned window 1216x208 at (368, 616): the camera applies it with the Experiment script" }),
    gate("camera.rate", READINESS_GATE_STATUSES.Pass, { detail: "5000 fps within the camera's 9000 fps" }),
    gate("storage.buffer", READINESS_GATE_STATUSES.Pass, { detail: "4000 frames, 1.9 GiB" }),
    gate("storage.throughput", READINESS_GATE_STATUSES.Pass, { detail: "2529 MB/s sustained" }),
  ],
  window: { width: 1216, height: 208 }, images_per_frame: 2, bytes_per_frame: 505856, bytes_per_frame_text: "494 KiB",
  requested_fps: 5000, requested_fps_source: "profile target_fps", max_fps: 0, invalid_sampling_rate: 10,
  save_stream: { every_frame_bytes_per_second: 2.5e9, every_frame_text: "2529 MB/s", floor_fps: 500, floor_bytes_per_second: 2.5e8, floor_text: "253 MB/s" },
  buffer: { frames: 4000, bytes: 2.0e9, bytes_text: "1.9 GiB", seconds: 0.8, source: "RAM fraction", summary: "4000 frames, 1.9 GiB, 0.80 s at 5000 fps", clamped_by_ram: false },
  destination: { bytes_per_second: 2.6e9, text: "2600 MB/s", note: "measured on D:\\" },
  ...over,
});

describe("Recording capacity wording (Qt RecordingCapacityPanel)", () => {
  it("states the window, rate, save stream, buffer and destination", () => {
    const text = describeRecordingCapacity(known());
    expect(text.status).toBe("OK");
    expect(text.facts).toEqual([
      "Planned window 1216x208, 2 images per saved frame (494 KiB)",
      "Rate 5000 fps requested (profile target_fps), camera max read when the Experiment script applies the window",
      "Save stream: every frame 2529 MB/s, no cells 253 MB/s (every 10th invalid frame)",
      "Buffer 4000 frames, 1.9 GiB, 0.80 s at 5000 fps (RAM fraction)",
      "Destination 2600 MB/s (measured on D:\\)",
    ]);
    expect(text.summary).toBe("Buffer 1.9 GiB, 0.80 s - OK");
    // the camera's own maximum when it is known, "unknown" outside the Overview plan
    expect(describeRecordingCapacity(known({ max_fps: 9000 })).facts[1]).toBe("Rate 5000 fps requested (profile target_fps), camera max 9000 fps");
    expect(describeRecordingCapacity(known({ planned: false })).facts[0]).toBe("Window 1216x208, 2 images per saved frame (494 KiB)");
    expect(describeRecordingCapacity(known({ planned: false })).facts[1]).toContain("camera max unknown");
  });

  it("words an unknown rate and an unknown destination", () => {
    const text = describeRecordingCapacity(known({ requested_fps: 0, save_stream: undefined, destination: { bytes_per_second: 0, text: "", note: "not measured yet" } }));
    expect(text.facts).toContain("Rate unknown (set target_fps in the profile)");
    expect(text.facts.some((f) => f.startsWith("Save stream"))).toBe(false);
    expect(text.facts).toContain("Destination rate unknown (not measured yet)");
    expect(text.summary).toBe("Buffer 1.9 GiB - OK");
  });

  it("orders the gates worst first and names the blocking ones; blocked is text, not only colour", () => {
    const view = known({
      gates: [
        gate("camera.window", READINESS_GATE_STATUSES.Warn, { reason: "the script's window was overridden", remediation: "save the window to the profile" }),
        gate("camera.rate", READINESS_GATE_STATUSES.Fail, { reason: "5000 fps exceeds the camera's 3000 fps", remediation: "lower target_fps or the window height" }),
        gate("storage.buffer", READINESS_GATE_STATUSES.Pass, { detail: "4000 frames, 1.9 GiB" }),
        gate("storage.throughput", READINESS_GATE_STATUSES.Unavailable, { reason: "the destination rate is not measured", remediation: "choose an output folder" }),
      ],
    });
    const text = describeRecordingCapacity(view);
    expect(text.status).toBe("Blocked");
    expect(text.gates.map((g) => g.text)).toEqual([
      "Blocked - camera.rate: 5000 fps exceeds the camera's 3000 fps -> lower target_fps or the window height",
      "Blocked - storage.throughput: the destination rate is not measured -> choose an output folder",
      "Warning - camera.window: the script's window was overridden -> save the window to the profile",
      "OK - storage.buffer: 4000 frames, 1.9 GiB",
    ]);
    expect(text.summary).toBe("Buffer 1.9 GiB, 0.80 s - Blocked: camera.rate, storage.throughput");
    expect(capacityGateLine(gate("x", READINESS_GATE_STATUSES.NotRequired, { detail: "n/a" })).text).toBe("OK - x: n/a");
  });

  it("says the window is not known yet before the camera runs", () => {
    const text = describeRecordingCapacity({ busy: false, known: false, planned: false, status: "unknown", gates: [] });
    expect(text.status).toBe("Unknown");
    expect(text.facts).toEqual(["The camera window is not known yet: start Live View."]);
    expect(text.summary).toBe("Buffer: start Live View to size it");
  });
});

describe("Recording capacity panel", () => {
  let host: HTMLDivElement, root: Root;
  beforeEach(() => { Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
  afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

  it("renders the status word, the facts and every gate as text", async () => {
    await act(async () => root.render(<RecordingCapacityPanel view={known({ gates: [gate("camera.rate", READINESS_GATE_STATUSES.Fail, { reason: "too fast", remediation: "slow down" })] })} />));
    const panel = host.querySelector('[data-testid="recording-capacity"]')!;
    expect(panel.textContent).toContain("Recording capacity: Blocked");
    expect(panel.textContent).toContain("Planned window 1216x208");
    expect(panel.textContent).toContain("Blocked - camera.rate: too fast -> slow down");
    expect(panel.querySelector(".capacity-status")!.classList.contains("blocked")).toBe(true);
  });

  it("says it is waiting before the first reply", async () => {
    await act(async () => root.render(<RecordingCapacityPanel view={null} />));
    expect(host.textContent).toContain("Recording capacity: Unknown");
  });
});
