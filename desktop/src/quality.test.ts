import { describe, expect, it } from "vitest";
import { deriveQualityGates, type GateStatus, type QualityInput } from "./quality";
import type { ImageMetrics } from "./imageQuality";

// A healthy aligned setup: streaming, focus live, background set, ROI valid,
// calibrated.
const GOOD: QualityInput = {
  cameraRunning: true,
  focusConnected: true,
  focusMetric: 0.412,
  focusUpdatedUs: 1_000_000,
  focusAgeMs: 50,
  focusStaleMs: 500,
  backgroundSet: true,
  roiW: 400,
  roiH: 300,
  frameW: 512,
  frameH: 384,
  pixelToMicron: 0.4886,
};

function gate(input: QualityInput, id: string) {
  return deriveQualityGates(input).gates.find((g) => g.id === id)!;
}
function statusOf(input: QualityInput, id: string): GateStatus {
  return gate(input, id).status;
}

describe("deriveQualityGates — focus", () => {
  it("is Unknown until the camera is running", () => {
    expect(statusOf({ ...GOOD, cameraRunning: false }, "focus")).toBe("unknown");
  });

  it("is Unknown when the autofocus controller is not connected", () => {
    expect(statusOf({ ...GOOD, focusConnected: false }, "focus")).toBe("unknown");
  });

  it("warns when no focus metric has been reported yet", () => {
    expect(statusOf({ ...GOOD, focusUpdatedUs: 0 }, "focus")).toBe("warn");
  });

  it("warns and reports age when the metric is stale", () => {
    const g = gate({ ...GOOD, focusAgeMs: 900 }, "focus");
    expect(g.status).toBe("warn");
    expect(g.detail).toContain("900 ms");
  });

  it("passes with the live metric value when fresh", () => {
    const g = gate(GOOD, "focus");
    expect(g.status).toBe("pass");
    expect(g.value).toBe("0.412");
  });
});

describe("deriveQualityGates — background / ROI / calibration", () => {
  it("background warns when not set, passes when set", () => {
    expect(statusOf({ ...GOOD, backgroundSet: false }, "background")).toBe("warn");
    expect(statusOf(GOOD, "background")).toBe("pass");
  });

  it("ROI warns when unset", () => {
    expect(statusOf({ ...GOOD, roiW: 0, roiH: 0 }, "roi")).toBe("warn");
  });

  it("ROI fails when larger than the current frame", () => {
    const g = gate({ ...GOOD, roiW: 999, roiH: 300 }, "roi");
    expect(g.status).toBe("fail");
    expect(g.detail).toContain("larger than");
  });

  it("ROI passes and reports dimensions when valid", () => {
    const g = gate(GOOD, "roi");
    expect(g.status).toBe("pass");
    expect(g.value).toBe("400×300");
  });

  it("calibration warns at zero scale, passes with a positive scale", () => {
    expect(statusOf({ ...GOOD, pixelToMicron: 0 }, "calibration")).toBe("warn");
    expect(gate(GOOD, "calibration").value).toContain("0.4886");
  });
});

describe("deriveQualityGates — accounting", () => {
  it("counts sum to the number of gates", () => {
    const r = deriveQualityGates(GOOD);
    expect(r.pass + r.warn + r.fail + r.unknown).toBe(r.gates.length);
    expect(r.gates.length).toBe(4);
  });

  it("a fully healthy setup passes every gate", () => {
    expect(deriveQualityGates(GOOD).pass).toBe(4);
  });
});

describe("PZ7035 Align image gates (#501)", () => {
  const lit: ImageMetrics = { focus: 80, mean: 143, p99: 190, saturated: 0, pixels: 49152 };
  const gate = (image: Parameters<typeof deriveQualityGates>[0]["image"], id: string) =>
    deriveQualityGates({ ...GOOD, pz7035: true, image }).gates.find((g) => g.id === id)!;

  it("adds focus and brightness only when the Align view supplies an image", () => {
    expect(deriveQualityGates({ ...GOOD, pz7035: true }).gates.map((g) => g.id)).toEqual(["calibration"]);
    expect(deriveQualityGates({ ...GOOD, pz7035: true, image: { metrics: lit, best: 80 } }).gates.map((g) => g.id))
      .toEqual(["focus", "brightness", "calibration"]);
  });

  it("is unknown, not a warning, before the first full-sensor frame", () => {
    const r = deriveQualityGates({ ...GOOD, pz7035: true, image: { metrics: null, best: null } });
    expect(r.gates.find((g) => g.id === "focus")?.status).toBe("unknown");
    expect(r.gates.find((g) => g.id === "brightness")?.status).toBe("unknown");
    expect(r.warn + r.fail).toBe(0);
  });

  it("shows the focus number and how it compares with the best seen", () => {
    expect(gate({ metrics: lit, best: 80 }, "focus")).toMatchObject({ status: "pass", value: "80.0" });
    expect(gate({ metrics: lit, best: 80 }, "focus").detail).toContain("At the best seen");
    const past = gate({ metrics: { ...lit, focus: 40 }, best: 80 }, "focus");
    expect(past.value).toBe("40.0");
    expect(past.detail).toContain("50 % of the best seen (80.0)");
  });

  it("warns on a dark or clipping window and passes a lit one", () => {
    expect(gate({ metrics: lit, best: 80 }, "brightness").status).toBe("pass");
    const dark = gate({ metrics: { ...lit, mean: 18 }, best: 80 }, "brightness");
    expect(dark.status).toBe("warn");
    expect(dark.detail).toContain("LED");
    const clipped = gate({ metrics: { ...lit, mean: 200, saturated: 0.05 }, best: 80 }, "brightness");
    expect(clipped.status).toBe("warn");
    expect(clipped.detail).toContain("5.0 % of the window is saturated");
  });
});
