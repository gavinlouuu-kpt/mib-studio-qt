import { describe, expect, it } from "vitest";
import { baseName, deriveContextBar, type ContextBarFacts, type SegStatus } from "./contextBar";

const BASE: ContextBarFacts = {
  profileName: "",
  cameraConfigured: false,
  cameraRunning: false,
  cameraLabel: "",
  pixelToMicron: 0,
  currentStageTitle: "Hardware Preflight",
  currentStageStatus: "not-started",
  allStagesComplete: false,
  experimentActive: false,
  experimentFailed: false,
  operatorName: "",
  outputPath: "",
  warningsCount: 0,
};

function seg(f: ContextBarFacts, id: string) {
  return deriveContextBar(f).segments.find((s) => s.id === id)!;
}
function statusOf(f: ContextBarFacts, id: string): SegStatus {
  return seg(f, id).status;
}

describe("deriveContextBar — segments present", () => {
  it("always exposes the seven context segments in order", () => {
    const ids = deriveContextBar(BASE).segments.map((s) => s.id);
    expect(ids).toEqual(["profile", "camera", "calibration", "system", "operator", "storage", "warnings"]);
  });
});

describe("deriveContextBar — unbridged segments are Pending, not faked", () => {
  it("profile and operator are pending until bridged", () => {
    expect(statusOf(BASE, "profile")).toBe("pending");
    expect(statusOf(BASE, "operator")).toBe("pending");
  });

  it("profile shows the name once available", () => {
    const s = seg({ ...BASE, profileName: "CellTypeA v2" }, "profile");
    expect(s.status).toBe("ok");
    expect(s.value).toBe("CellTypeA v2");
  });
});

describe("deriveContextBar — camera", () => {
  it("is blocked with no camera, warns when connected but idle, ok when streaming", () => {
    expect(statusOf(BASE, "camera")).toBe("blocked");
    expect(statusOf({ ...BASE, cameraConfigured: true, cameraLabel: "MV-2048" }, "camera")).toBe("warn");
    const ok = seg({ ...BASE, cameraConfigured: true, cameraRunning: true, cameraLabel: "MV-2048" }, "camera");
    expect(ok.status).toBe("ok");
    expect(ok.value).toBe("MV-2048");
  });
});

describe("deriveContextBar — calibration & warnings", () => {
  it("calibration warns at zero, ok with a scale", () => {
    expect(statusOf(BASE, "calibration")).toBe("warn");
    expect(seg({ ...BASE, pixelToMicron: 0.4886 }, "calibration").value).toContain("0.4886");
  });

  it("warnings ok at zero, warn when there are attention items", () => {
    expect(statusOf(BASE, "warnings")).toBe("ok");
    expect(statusOf({ ...BASE, warningsCount: 3 }, "warnings")).toBe("warn");
  });
});

describe("deriveContextBar — system status", () => {
  it("mirrors the current stage when idle", () => {
    expect(statusOf(BASE, "system")).toBe("blocked"); // not-started → blocked
    expect(seg({ ...BASE, currentStageStatus: "ready", currentStageTitle: "Experiment" }, "system").value).toBe(
      "Experiment",
    );
  });

  it("shows Running while an experiment is active and Failed on failure", () => {
    expect(seg({ ...BASE, experimentActive: true }, "system").value).toBe("Running");
    expect(statusOf({ ...BASE, experimentFailed: true }, "system")).toBe("blocked");
  });

  it("shows Complete when every stage is complete", () => {
    const s = seg({ ...BASE, allStagesComplete: true, currentStageStatus: "complete" }, "system");
    expect(s.value).toBe("Complete");
    expect(s.status).toBe("ok");
  });
});

describe("deriveContextBar — storage", () => {
  it("is pending until a run writes output, then shows the file name", () => {
    expect(statusOf(BASE, "storage")).toBe("pending");
    const s = seg({ ...BASE, experimentActive: true, outputPath: "/data/2026-05-10_001.h5" }, "storage");
    expect(s.status).toBe("ok");
    expect(s.value).toBe("2026-05-10_001.h5");
  });
});

describe("baseName", () => {
  it("handles POSIX and Windows paths and trailing slashes", () => {
    expect(baseName("/data/run/out.h5")).toBe("out.h5");
    expect(baseName("C:\\data\\run\\out.h5")).toBe("out.h5");
    expect(baseName("/data/run/")).toBe("run");
    expect(baseName("out.h5")).toBe("out.h5");
  });
});
