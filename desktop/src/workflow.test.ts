import { describe, expect, it } from "vitest";
import { deriveWorkflow, type StageId, type StageStatus, type WorkflowFacts } from "./workflow";
import { EXPERIMENT_STATES } from "./bridgeContract";

// A fully-blocked baseline: fresh boot, nothing configured.
const BASE: WorkflowFacts = {
  backendReady: true,
  cameraConfigured: false,
  cameraRunning: false,
  preflightSignature: "",
  preflightConfirmedFor: "",
  alignmentSignature: "",
  alignmentConfirmedFor: "",
  coreValid: true,
  corePinSatisfied: true,
  requiredCoreVersion: "2.1.0",
  experimentState: EXPERIMENT_STATES.Idle,
  experimentCompleted: false,
  reviewFileOpen: false,
  reviewValid: false,
};

function statusOf(f: WorkflowFacts, id: StageId): StageStatus {
  const view = deriveWorkflow(f);
  return view.stages.find((s) => s.id === id)!.status;
}

// Facts for a healthy, confirmed, running-camera setup up through alignment.
const ALIGNED: WorkflowFacts = {
  ...BASE,
  cameraConfigured: true,
  cameraRunning: true,
  preflightSignature: "mock|Demo|core:2.1.0",
  preflightConfirmedFor: "mock|Demo|core:2.1.0",
  alignmentSignature: "mock|Demo",
  alignmentConfirmedFor: "mock|Demo",
};

describe("deriveWorkflow — preflight", () => {
  it("is Not started with no camera configured", () => {
    expect(statusOf(BASE, "preflight")).toBe("not-started");
    const view = deriveWorkflow(BASE);
    expect(view.recommended?.label).toBe("Select a camera");
  });

  it("is Not started when the backend is not initialized", () => {
    expect(statusOf({ ...BASE, backendReady: false }, "preflight")).toBe("not-started");
  });

  it("camera detection alone does NOT complete preflight — only Ready", () => {
    const f = { ...BASE, cameraConfigured: true }; // configured but unconfirmed
    expect(statusOf(f, "preflight")).toBe("ready");
    const view = deriveWorkflow(f);
    expect(view.recommended).toMatchObject({ kind: "confirm-preflight" });
  });

  it("completes only after an explicit confirmation matching the signature", () => {
    const sig = "mock|Demo|core:2.1.0";
    const f = { ...BASE, cameraConfigured: true, preflightSignature: sig, preflightConfirmedFor: sig };
    expect(statusOf(f, "preflight")).toBe("complete");
  });

  it("invalidates the confirmation when the device/core signature changes", () => {
    const f = {
      ...BASE,
      cameraConfigured: true,
      preflightSignature: "mock|OtherCam|core:2.1.0",
      preflightConfirmedFor: "mock|Demo|core:2.1.0",
    };
    expect(statusOf(f, "preflight")).toBe("ready"); // dropped back, must re-confirm
  });

  it("needs attention when the processing-core pin is not satisfied", () => {
    const f = { ...BASE, cameraConfigured: true, corePinSatisfied: false };
    expect(statusOf(f, "preflight")).toBe("needs-attention");
    const stage = deriveWorkflow(f).stages.find((s) => s.id === "preflight")!;
    expect(stage.blocking.join(" ")).toContain("2.1.0");
  });
});

describe("deriveWorkflow — gating", () => {
  it("blocks alignment until preflight is complete", () => {
    const f = { ...BASE, cameraConfigured: true, cameraRunning: true }; // unconfirmed preflight
    expect(statusOf(f, "alignment")).toBe("not-started");
  });

  it("alignment needs the camera running once preflight is complete", () => {
    const f = {
      ...BASE,
      cameraConfigured: true,
      preflightSignature: "s",
      preflightConfirmedFor: "s",
      cameraRunning: false,
    };
    expect(statusOf(f, "alignment")).toBe("needs-attention");
  });

  it("alignment is Ready when running but unconfirmed, then Complete once confirmed", () => {
    const ready = { ...ALIGNED, alignmentConfirmedFor: "" };
    expect(statusOf(ready, "alignment")).toBe("ready");
    expect(deriveWorkflow(ready).recommended).toMatchObject({ kind: "confirm-alignment" });
    expect(statusOf(ALIGNED, "alignment")).toBe("complete");
  });

  it("blocks experiment until alignment is complete", () => {
    const f = { ...ALIGNED, alignmentConfirmedFor: "" };
    expect(statusOf(f, "experiment")).toBe("not-started");
  });
});

describe("deriveWorkflow — experiment & review", () => {
  it("experiment is Ready when aligned and camera running", () => {
    expect(statusOf(ALIGNED, "experiment")).toBe("ready");
    expect(deriveWorkflow(ALIGNED).currentStageId).toBe("experiment");
  });

  it("experiment is Running while active, and that is the current stage", () => {
    const f = { ...ALIGNED, experimentState: EXPERIMENT_STATES.Active };
    expect(statusOf(f, "experiment")).toBe("running");
    expect(deriveWorkflow(f).currentStageId).toBe("experiment");
  });

  it("a failed experiment needs attention", () => {
    const f = { ...ALIGNED, experimentState: EXPERIMENT_STATES.Failed };
    expect(statusOf(f, "experiment")).toBe("needs-attention");
  });

  it("completed experiment makes Review Ready", () => {
    const f = { ...ALIGNED, experimentCompleted: true };
    expect(statusOf(f, "experiment")).toBe("complete");
    expect(statusOf(f, "review")).toBe("ready");
  });

  it("review completes when a valid file is open; whole workflow then has no next action", () => {
    const f = { ...ALIGNED, experimentCompleted: true, reviewFileOpen: true, reviewValid: true };
    expect(statusOf(f, "review")).toBe("complete");
    expect(deriveWorkflow(f).recommended).toBeNull();
    expect(deriveWorkflow(f).currentStageId).toBe("review");
  });

  it("an unreadable opened file surfaces as needs-attention", () => {
    const f = { ...ALIGNED, reviewFileOpen: true, reviewValid: false };
    expect(statusOf(f, "review")).toBe("needs-attention");
  });
});

describe("deriveWorkflow — invariants", () => {
  it("is a pure function of the facts (no hidden navigation/tab input)", () => {
    // Two identical fact sets must produce identical stage statuses: there is
    // no way for 'visiting a tab' to change completion.
    const a = deriveWorkflow(ALIGNED);
    const b = deriveWorkflow({ ...ALIGNED });
    expect(a.stages.map((s) => s.status)).toEqual(b.stages.map((s) => s.status));
  });

  it("always exposes the four stages in workflow order", () => {
    const ids = deriveWorkflow(BASE).stages.map((s) => s.id);
    expect(ids).toEqual(["preflight", "alignment", "experiment", "review"]);
  });
});
