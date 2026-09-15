import { describe, expect, it } from "vitest";
import {
  canActuate,
  canStartPeriodic,
  canStopPeriodic,
  DEFAULT_MODE,
  type ActuationInput,
} from "./commissioning";

// A fully-permissive service-mode setup: armed, trigger attached, no run.
const SERVICE_READY: ActuationInput = {
  mode: "service",
  experimentActive: false,
  triggerAttached: true,
  armed: true,
};

describe("commissioning — default mode", () => {
  it("every new session starts in operator mode", () => {
    expect(DEFAULT_MODE).toBe("operator");
  });
});

describe("canActuate — gate order", () => {
  it("blocks in operator mode (no accidental one-click pulse)", () => {
    const r = canActuate({ ...SERVICE_READY, mode: "operator" });
    expect(r.allowed).toBe(false);
    expect(r.reason).toContain("Service");
  });

  it("blocks while an experiment is active", () => {
    expect(canActuate({ ...SERVICE_READY, experimentActive: true }).allowed).toBe(false);
  });

  it("blocks when no trigger output is attached", () => {
    expect(canActuate({ ...SERVICE_READY, triggerAttached: false }).allowed).toBe(false);
  });

  it("blocks until the control is armed", () => {
    const r = canActuate({ ...SERVICE_READY, armed: false });
    expect(r.allowed).toBe(false);
    expect(r.reason).toContain("Arm");
  });

  it("allows only when in service mode, armed, attached, and idle", () => {
    const r = canActuate(SERVICE_READY);
    expect(r.allowed).toBe(true);
    expect(r.reason).toBe("");
  });
});

describe("periodic test gating", () => {
  it("starting a periodic test uses the same gate as actuation", () => {
    expect(canStartPeriodic(SERVICE_READY).allowed).toBe(true);
    expect(canStartPeriodic({ ...SERVICE_READY, mode: "operator" }).allowed).toBe(false);
  });

  it("stopping is always allowed while a periodic test is running, even in operator mode", () => {
    expect(canStopPeriodic({ periodicActive: true }).allowed).toBe(true);
  });

  it("stopping is a no-op when nothing is running", () => {
    expect(canStopPeriodic({ periodicActive: false }).allowed).toBe(false);
  });
});
