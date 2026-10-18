import { describe, expect, it } from "vitest";
import { READINESS_GATE_STATUSES as G } from "./bridgeContract";
import {
  CHECK_STATUS_LABEL,
  DEFAULT_REQUIREMENTS,
  checkGateStatus,
  derivePreflight,
  type CheckStatus,
  type DeviceSnapshot,
  type PreflightInput,
} from "./preflight";
import { DESKTOP_CAPABILITIES } from "./platformCapabilities";

const OFF: DeviceSnapshot = { valid: true, connected: false, identity: "" };
const ON: DeviceSnapshot = { valid: true, connected: true, identity: "COM3" };
const UNREAD: DeviceSnapshot = { valid: false, connected: false, identity: "" };

// A healthy baseline: backend up, camera configured, trusted core, no optional
// hardware connected.
const READY: PreflightInput = {
  backendReady: true,
  cameraConfigured: true,
  cameraRunning: false,
  cameraIdentity: "MV-2048 SN1234",
  cameraExpected: "",
  coreValid: true,
  corePinSatisfied: true,
  coreVersion: "2.1.0",
  requiredCoreVersion: "2.1.0",
  autofocus: OFF,
  samplePump: OFF,
  sheathPump: OFF,
  trigger: { valid: true, cameraAttached: false },
  storageKnown: false,
  storageWritable: false,
  storageFreeOk: false,
  storagePath: "",
};

function statusOf(input: PreflightInput, id: string, reqs = DEFAULT_REQUIREMENTS): CheckStatus {
  return derivePreflight(input, reqs).checks.find((c) => c.id === id)!.status;
}

describe("derivePreflight — required checks", () => {
  it("camera is unknown, not failed, while the backend is not initialized, and still blocks (#713)", () => {
    const r = derivePreflight({ ...READY, backendReady: false });
    const camera = r.checks.find((c) => c.id === "camera")!;
    expect(camera.status).toBe("unknown");
    expect(camera.blocks).toBe(true);
    expect(r.criticalPassed).toBe(false);
  });

  it("the core is unknown until its status is read, and blocks (#713)", () => {
    const r = derivePreflight({ ...READY, coreKnown: false, coreValid: false });
    expect(r.checks.find((c) => c.id === "core")).toMatchObject({ status: "unknown", blocks: true });
    expect(statusOf({ ...READY, coreKnown: true, coreValid: false }, "core")).toBe("failed");
  });

  it("camera fails and criticalPassed is false with no camera configured", () => {
    expect(statusOf({ ...READY, cameraConfigured: false }, "camera")).toBe("failed");
    expect(derivePreflight({ ...READY, cameraConfigured: false }).criticalPassed).toBe(false);
  });

  it("a healthy camera + core passes and, with all-optional devices, criticalPassed is true", () => {
    const r = derivePreflight(READY);
    expect(statusOf(READY, "camera")).toBe("passed");
    expect(statusOf(READY, "core")).toBe("passed");
    expect(r.criticalPassed).toBe(true);
  });

  it("warns when the detected camera differs from the profile's expected identity", () => {
    const r = derivePreflight({ ...READY, cameraExpected: "EG-Cam SN9" });
    const cam = r.checks.find((c) => c.id === "camera")!;
    expect(cam.status).toBe("warning");
    expect(cam.detail).toContain("EG-Cam SN9");
  });

  it("a required warning blocks, failing closed (#548)", () => {
    const r = derivePreflight({ ...READY, cameraExpected: "EG-Cam SN9" });
    expect(r.checks.find((c) => c.id === "camera")).toMatchObject({ status: "warning", blocks: true });
    expect(r.criticalPassed).toBe(false);
  });

  it("an optional warning does not block", () => {
    const r = derivePreflight({ ...READY, storageKnown: true, storageWritable: true, storageFreeOk: false });
    expect(r.checks.find((c) => c.id === "storage")).toMatchObject({ status: "warning", requirement: "optional", blocks: false });
    expect(r.criticalPassed).toBe(true);
  });

  it("core fails and blocks criticality when the pin is not satisfied", () => {
    const r = derivePreflight({ ...READY, corePinSatisfied: false });
    const core = r.checks.find((c) => c.id === "core")!;
    expect(core.status).toBe("failed");
    expect(core.detail).toContain("2.1.0");
    expect(r.criticalPassed).toBe(false);
  });
});

describe("derivePreflight — optional vs required devices", () => {
  it("an optional disconnected device is Not required and does not block", () => {
    expect(statusOf(READY, "autofocus")).toBe("not-required");
    expect(derivePreflight(READY).criticalPassed).toBe(true);
  });

  it("an optional device not read yet is Unknown, never Not required, and does not block (#713)", () => {
    const r = derivePreflight({ ...READY, autofocus: UNREAD, samplePump: UNREAD, trigger: { valid: false, cameraAttached: false } });
    for (const id of ["autofocus", "samplePump", "trigger"]) expect(r.checks.find((c) => c.id === id)).toMatchObject({ status: "unknown", blocks: false });
    expect(r.criticalPassed).toBe(true);
  });

  it("a required device not read yet is Unknown and blocks: Unknown is never Pass (#713)", () => {
    const reqs = { ...DEFAULT_REQUIREMENTS, sheathPump: "required" as const };
    const r = derivePreflight({ ...READY, sheathPump: UNREAD }, reqs);
    expect(r.checks.find((c) => c.id === "sheathPump")).toMatchObject({ status: "unknown", blocks: true });
    expect(r.criticalPassed).toBe(false);
  });

  it("a capability the instrument lacks is Unsupported and never blocks", () => {
    const caps = { ...DESKTOP_CAPABILITIES, autofocus: false, trigger: false };
    const r = derivePreflight({ ...READY, capabilities: caps });
    expect(r.checks.find((c) => c.id === "autofocus")).toMatchObject({ status: "unsupported", blocks: false });
    expect(r.checks.find((c) => c.id === "trigger")).toMatchObject({ status: "unsupported", blocks: false });
    expect(r.unsupported).toBe(2);
    expect(r.criticalPassed).toBe(true);
  });

  it("a connected optional device shows Passed", () => {
    expect(statusOf({ ...READY, samplePump: ON }, "samplePump")).toBe("passed");
  });

  it("a profile-required device that is disconnected Fails and blocks criticality", () => {
    const reqs = { ...DEFAULT_REQUIREMENTS, samplePump: "required" as const };
    const r = derivePreflight(READY, reqs);
    expect(r.checks.find((c) => c.id === "samplePump")!.status).toBe("failed");
    expect(r.criticalPassed).toBe(false);
  });

  it("a not-applicable device is Not required regardless of connection", () => {
    const reqs = { ...DEFAULT_REQUIREMENTS, trigger: "not-applicable" as const };
    expect(statusOf({ ...READY, trigger: { valid: true, cameraAttached: true } }, "trigger", reqs)).toBe(
      "not-required",
    );
  });
});

describe("derivePreflight — trigger, storage, capture", () => {
  it("trigger passes when attached to the camera", () => {
    expect(statusOf({ ...READY, trigger: { valid: true, cameraAttached: true } }, "trigger")).toBe("passed");
  });

  it("unreported storage is Unknown, never Not required, and does not block (#713)", () => {
    const s = derivePreflight(READY).checks.find((c) => c.id === "storage")!;
    expect(s).toMatchObject({ status: "unknown", requirement: "optional", blocks: false });
    expect(s.detail).toContain("Start");
    expect(statusOf(READY, "storage", { ...DEFAULT_REQUIREMENTS, storage: "not-applicable" })).toBe("not-required");
  });

  it("a known, unwritable storage location Fails", () => {
    const r = derivePreflight({ ...READY, storageKnown: true, storageWritable: false, storageFreeOk: true });
    expect(r.checks.find((c) => c.id === "storage")!.status).toBe("failed");
    expect(r.criticalPassed).toBe(true); // storage is optional by default
  });

  it("capture is pending before running and passes once streaming", () => {
    expect(statusOf(READY, "capture")).toBe("not-required");
    expect(statusOf({ ...READY, cameraRunning: true }, "capture")).toBe("passed");
  });
});

describe("derivePreflight — accounting", () => {
  it("status counts sum to the total number of checks", () => {
    const r = derivePreflight(READY);
    expect(r.passed + r.warning + r.failed + r.unknown + r.unsupported + r.notRequired).toBe(r.total);
    expect(r.total).toBe(8);
    expect(r.unknown).toBe(1); // storage, not reported before Start
  });
});

describe("checklist states in the backend's readiness vocabulary (#713)", () => {
  it("maps every state to Pass / Warn / Fail / Unavailable / NotRequired, and Unknown is never Pass", () => {
    const states: CheckStatus[] = ["passed", "warning", "failed", "unknown", "unsupported", "not-required"];
    expect(states.map(checkGateStatus)).toEqual([G.Pass, G.Warn, G.Fail, G.Unavailable, G.NotRequired, G.NotRequired]);
    expect(checkGateStatus("unknown")).not.toBe(G.Pass);
    expect(states.map((s) => CHECK_STATUS_LABEL[s])).toEqual(["Passed", "Warning", "Failed", "Unknown", "Unsupported", "Not required"]);
  });

  it("blocks exactly the required rows that are not passed; unsupported and not-required never block", () => {
    const reqs = { autofocus: "required", samplePump: "required", sheathPump: "required", trigger: "not-applicable", storage: "required" } as const;
    const r = derivePreflight({ ...READY, autofocus: UNREAD, samplePump: ON, sheathPump: OFF, storageKnown: true, storageWritable: true, storageFreeOk: false }, reqs);
    const blocking = Object.fromEntries(r.checks.map((c) => [c.id, [c.status, c.blocks]]));
    expect(blocking).toMatchObject({
      autofocus: ["unknown", true], samplePump: ["passed", false], sheathPump: ["failed", true],
      trigger: ["not-required", false], storage: ["warning", true], capture: ["not-required", false],
    });
  });
});
