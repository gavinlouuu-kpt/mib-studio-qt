import { describe, expect, it } from "vitest";
import {
  cameraMatchesExpected,
  DEFAULT_REQUIREMENTS,
  derivePreflight,
  expectedCameraModel,
  type CheckStatus,
  type DeviceSnapshot,
  type PreflightInput,
} from "./preflight";

const OFF: DeviceSnapshot = { valid: true, connected: false, identity: "" };
const ON: DeviceSnapshot = { valid: true, connected: true, identity: "COM3" };

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
  it("camera fails when the backend is not initialized", () => {
    const r = derivePreflight({ ...READY, backendReady: false });
    expect(r.checks.find((c) => c.id === "camera")!.status).toBe("failed");
    expect(r.criticalPassed).toBe(false);
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

  it("the profile's camera.window.camera_model is the expected identity (App.tsx:1237)", () => {
    const doc = (camera: unknown) => JSON.stringify({ pixel_to_micron_factor: 0.5, camera });
    expect(expectedCameraModel(doc({ window: { camera_vendor: "SVS-Vistek GmbH", camera_model: " MC2066 " } }))).toBe("MC2066");
    expect(expectedCameraModel(doc({ window: { width: 64 } }))).toBe("");
    expect(expectedCameraModel(doc({ window: { camera_model: 7 } }))).toBe("");
    expect(expectedCameraModel(doc(undefined))).toBe("");
    expect(expectedCameraModel("{broken")).toBe("");
    expect(expectedCameraModel("null")).toBe("");
  });

  it("the connected camera matches by its model or by its label, without case; no expectation matches anything", () => {
    expect(cameraMatchesExpected("", "EGrabber 0", "")).toBe(true);
    expect(cameraMatchesExpected("MC2066", "Coaxlink Quad 0 MC2066 SN1", "MC2066")).toBe(true);
    expect(cameraMatchesExpected("mc2066", "x", "MC2066")).toBe(true);
    expect(cameraMatchesExpected("MC2066", "Coaxlink 0 mc2066 (SVS-Vistek)", "")).toBe(true);
    expect(cameraMatchesExpected("MC2066", "Coaxlink 0 MC1234", "MC1234")).toBe(false);
    expect(cameraMatchesExpected("EG-Cam SN9", "EG-Cam SN9", "")).toBe(true);
  });

  it("the preflight warns when the connected camera's model is not the profile's, and not when it is", () => {
    const wrong = derivePreflight({ ...READY, cameraIdentity: "Coaxlink 0 MC1234", cameraModel: "MC1234", cameraExpected: "MC2066" }).checks.find((c) => c.id === "camera")!;
    expect(wrong.status).toBe("warning");
    expect(wrong.detail).toContain("MC2066");
    expect(statusOf({ ...READY, cameraIdentity: "Coaxlink 0 MC2066", cameraModel: "MC2066", cameraExpected: "MC2066" }, "camera")).toBe("passed");
    expect(statusOf({ ...READY, cameraIdentity: "Mock camera", cameraModel: "", cameraExpected: "" }, "camera")).toBe("passed");
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

  it("storage is an informational Not-required until the data directory's status was read", () => {
    const s = derivePreflight(READY).checks.find((c) => c.id === "storage")!;
    expect(s.status).toBe("not-required");
    expect(s.detail).toContain("has not been read yet");
  });

  it("a read data directory on the desktop is checked: writable, free space, RAM-disk warning (preflight.ts:360-363)", () => {
    const ok = derivePreflight({ ...READY, storageKnown: true, storageWritable: true, storageFreeOk: true, storagePath: "/data" }).checks.find((c) => c.id === "storage")!;
    expect(ok.status).toBe("passed");
    expect(ok.detected).toBe("/data");
    expect(derivePreflight({ ...READY, storageKnown: true, storageWritable: true, storageFreeOk: false }).checks.find((c) => c.id === "storage")!.status).toBe("warning");
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
    expect(r.passed + r.warning + r.failed + r.notRequired).toBe(r.total);
    expect(r.total).toBe(8);
  });
});
