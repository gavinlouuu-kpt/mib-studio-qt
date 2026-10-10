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

describe("deriveWorkflow — required checklist failures (#548)", () => {
  const CONFIRMED: WorkflowFacts = { ...ALIGNED, cameraRunning: false };
  const FAILURES = [
    "PL core (build + weights): PL core unavailable: PL not configured.",
    "Sensor link: Live registers are unavailable.",
    "LED strobe: Live registers are unavailable.",
  ];
  const view = (f: WorkflowFacts) => deriveWorkflow(f).stages.find((s) => s.id === "preflight")!;

  it("blocks confirmation while a required check is not passed, and says which", () => {
    const v = view({ ...CONFIRMED, preflightConfirmedFor: "", requiredFailures: FAILURES });
    expect(v.status).toBe("needs-attention");
    expect(v.blocking).toEqual(FAILURES);
    expect(v.summary).toBe(FAILURES[0]);
    const w = deriveWorkflow({ ...CONFIRMED, preflightConfirmedFor: "", requiredFailures: FAILURES });
    expect(w.recommended?.kind).not.toBe("confirm-preflight");
  });

  it("offers the confirmation when every required check passes", () => {
    const w = deriveWorkflow({ ...CONFIRMED, preflightConfirmedFor: "", requiredFailures: [] });
    expect(view({ ...CONFIRMED, preflightConfirmedFor: "", requiredFailures: [] }).status).toBe("ready");
    expect(w.recommended?.kind).toBe("confirm-preflight");
  });

  it("un-completes a stage that was confirmed before a required check started failing", () => {
    expect(view({ ...CONFIRMED, requiredFailures: [] }).status).toBe("complete");
    expect(view({ ...CONFIRMED, requiredFailures: [FAILURES[2]] }).status).toBe("needs-attention");
  });

  it("tells the operator how to clear an unverified PL build, which blocks like any required check", () => {
    const unverified = "PL core (build + weights): The PL build is not verified: /etc/yofo/expected-core.json is missing. On the instrument run scripts/pz_install_core.sh <build dir> (it writes that file from the build's core.json), then press Retry check.";
    const v = view({ ...CONFIRMED, preflightConfirmedFor: "", requiredFailures: [unverified] });
    expect(v.status).toBe("needs-attention");
    expect(v.summary).toContain("pz_install_core.sh");
  });

  it("replaces the host core pin test with the checklist, so the PL core decides on the instrument", () => {
    // The PZ7035 has no host core; the PL core check in the checklist stands in for it.
    const noHostCore = { ...CONFIRMED, coreValid: false, corePinSatisfied: false };
    expect(view({ ...noHostCore, requiredFailures: [] }).status).toBe("complete");
    expect(view({ ...noHostCore, requiredFailures: undefined }).status).toBe("needs-attention");
  });
});

// #651 G15: on the PZ7035 the camera is the instrument. Run and Align need no Preflight confirmation and the stepper follows the mode.
describe("deriveWorkflow — PZ7035 camera modes (G15)", () => {
  const PZ: WorkflowFacts = { ...BASE, cameraConfigured: true, instrumentMode: "unknown" };

  it("in Run the next step is the experiment, not the Preflight confirmation", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "run" });
    expect(view.currentStageId).toBe("experiment");
    expect(view.recommended).toMatchObject({ stageId: "experiment", kind: "navigate", label: "Start the experiment" });
    expect(view.stages.find((s) => s.id === "preflight")!.status).toBe("ready"); // still unconfirmed, and advisory
    expect(view.stages.find((s) => s.id === "alignment")!.status).toBe("ready");
    expect(view.stages.find((s) => s.id === "experiment")!.status).toBe("ready");
  });

  it("in Align the next step is Camera & Alignment", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "align" });
    expect(view.currentStageId).toBe("alignment");
    expect(view.recommended).toMatchObject({ stageId: "alignment", kind: "confirm-alignment" });
  });

  it("in Align with the alignment confirmed the experiment is next", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "align", alignmentSignature: "pz", alignmentConfirmedFor: "pz" });
    expect(view.currentStageId).toBe("experiment");
  });

  it("with no mode yet the earliest incomplete stage decides, as at start-up", () => {
    const view = deriveWorkflow(PZ);
    expect(view.currentStageId).toBe("preflight");
    expect(view.recommended).toMatchObject({ kind: "confirm-preflight" });
  });

  it("a running experiment is the current stage whatever the mode", () => {
    for (const mode of ["align", "run", "unknown"] as const) {
      const view = deriveWorkflow({ ...PZ, instrumentMode: mode, experimentState: EXPERIMENT_STATES.Active });
      expect(view.currentStageId).toBe("experiment");
      expect(view.stages.find((s) => s.id === "experiment")!.status).toBe("running");
    }
  });

  it("a failed experiment needs attention in Run, with the reason", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "run", experimentState: EXPERIMENT_STATES.Failed });
    const exp = view.stages.find((s) => s.id === "experiment")!;
    expect(exp.status).toBe("needs-attention");
    expect(exp.blocking[0]).toContain("failed");
    expect(view.currentStageId).toBe("experiment");
  });

  it("a failing Preflight is not skipped: required failures keep the banner on Preflight in Run and in Align", () => {
    for (const mode of ["run", "align"] as const) {
      const view = deriveWorkflow({ ...PZ, instrumentMode: mode, requiredFailures: ["Storage missing: no writable data folder"] });
      expect(view.stages.find((s) => s.id === "preflight")!.status).toBe("needs-attention");
      expect(view.currentStageId).toBe("preflight");
      expect(view.recommended).toMatchObject({ stageId: "preflight", kind: "navigate", label: "Resolve hardware preflight" });
    }
  });

  it("an invalid core keeps the banner on Preflight too (no checklist given)", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "run", coreValid: false });
    expect(view.currentStageId).toBe("preflight");
    expect(view.recommended?.label).toBe("Resolve hardware preflight");
  });

  it("a running experiment still wins over a failing Preflight (it cannot be un-run)", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "run", requiredFailures: ["x: y"], experimentState: EXPERIMENT_STATES.Active });
    expect(view.currentStageId).toBe("experiment");
  });

  it("in Align after a failed experiment with the alignment unconfirmed the next step is still Camera & Alignment (decision, not accident)", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "align", experimentState: EXPERIMENT_STATES.Failed });
    expect(view.currentStageId).toBe("alignment");
    expect(view.recommended).toMatchObject({ stageId: "alignment", kind: "confirm-alignment", label: "Confirm alignment & ROI" });
    // the failed experiment is still shown on its own stage
    expect(view.stages.find((s) => s.id === "experiment")!.status).toBe("needs-attention");
  });

  it("a finished experiment moves the next step to Review", () => {
    const view = deriveWorkflow({ ...PZ, instrumentMode: "run", experimentCompleted: true });
    expect(view.currentStageId).toBe("review");
  });

  it("the desktop does not change: without a camera mode the stages still chain on the Preflight confirmation", () => {
    const desktop: WorkflowFacts = { ...BASE, cameraConfigured: true, cameraRunning: true };
    const view = deriveWorkflow(desktop);
    expect(view.currentStageId).toBe("preflight");
    expect(view.stages.find((s) => s.id === "alignment")!.status).toBe("not-started");
    expect(view.stages.find((s) => s.id === "experiment")!.status).toBe("not-started");
    expect(view.recommended).toMatchObject({ kind: "confirm-preflight" });
  });
});
