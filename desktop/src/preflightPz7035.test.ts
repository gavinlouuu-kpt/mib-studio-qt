// #501: preflight on the PZ7035 follows the capability report and the PL's
// identity and health, and a healthy instrument shows 0 warnings at idle.
import { describe, expect, it } from "vitest";
import type { InstrumentStatus, PlatformCapabilities } from "./bridge";
import { derivePreflight, type DeviceSnapshot, type PreflightInput } from "./preflight";
import { deriveQualityGates } from "./quality";
import { capabilitiesOf, DESKTOP_CAPABILITIES } from "./platformCapabilities";

const OFF: DeviceSnapshot = { valid: true, connected: false, identity: "" };

const PZ7035: PlatformCapabilities = {
  ...DESKTOP_CAPABILITIES,
  instrument: "pz7035",
  autofocus: false,
  trigger: false,
  host_background: false,
  frame_buffer: false,
  reanalysis: false,
  core_updates: false,
  egrabber_script: false,
  pl_identity: true,
  led_strobe: true,
  pump: { model: "tushui_peristaltic", port: "/dev/ttyPS1", sample_address: 3, sheath_address: 4, microliters_per_rev: 25 },
};

// results6 with the release weights, LED off at idle, quiet link.
const HEALTHY: InstrumentStatus = {
  available: true,
  core: {
    build_id: "76aea7655189e35b1b629289f363df5e",
    profile_id: "eea09a3f9cbf552c749fa66e205ef961",
    abi_version: 0x00010002,
    science_profile: 2,
    profile_version: 2,
    expected: { build_id: "76aea7655189e35b1b629289f363df5e", profile_id: "eea09a3f9cbf552c749fa66e205ef961", commit: "76aea76", image: "pz_live_results6", abi_major: 1, abi_minor: 2 },
    pinned_profile_id: "eea09a3f9cbf552c749fa66e205ef961",
    build_match: "match",
    profile_match: "match",
  },
  led: { on: false, preset: "off", delay_us: 7, width_us: 60, guard_fault: false, guard_trips: 0 },
  link: { rates_valid: true, ingress_errors_per_s: 0.2, resyncs_per_s: 0.1, bad_frames_per_s: 0, dropped_per_s: 0, ingress_errors_warn_per_s: 10, resyncs_warn_per_s: 1 },
  latency: { last_us: 97, max_us: 110.9, over_budget: 0, frames: 300118 },
};

// The board at idle: camera (GenTL producer) connected, host core absent.
const IDLE: PreflightInput = {
  backendReady: true,
  cameraConfigured: true,
  cameraRunning: false,
  cameraIdentity: "PZ7035 IMX426",
  cameraExpected: "",
  coreValid: false,
  corePinSatisfied: false,
  coreVersion: "",
  requiredCoreVersion: "",
  autofocus: OFF,
  samplePump: OFF,
  sheathPump: OFF,
  trigger: { valid: false, cameraAttached: false },
  storageKnown: false,
  storageWritable: false,
  storageFreeOk: false,
  storagePath: "",
  capabilities: PZ7035,
  instrument: HEALTHY,
};

const check = (input: PreflightInput, id: string) => derivePreflight(input).checks.find((c) => c.id === id);

describe("PZ7035 preflight (#501)", () => {
  it("a healthy instrument at idle has 0 warnings and passes", () => {
    const r = derivePreflight(IDLE);
    expect(r.warning + r.failed).toBe(0);
    expect(r.criticalPassed).toBe(true);
    const quality = deriveQualityGates({ cameraRunning: false, focusConnected: false, focusMetric: 0, focusUpdatedUs: 0, focusAgeMs: 0, focusStaleMs: 1000, backgroundSet: false, roiW: 0, roiH: 0, frameW: 512, frameH: 96, pixelToMicron: 0.4886, pz7035: true });
    expect(quality.warn + quality.fail).toBe(0);
    expect(quality.gates.map((g) => g.id)).toEqual(["calibration"]);
  });

  it("checks the PL core instead of the host core pin", () => {
    const ids = derivePreflight(IDLE).checks.map((c) => c.id);
    expect(ids).toContain("plCore");
    expect(ids).toContain("sensorLink");
    expect(ids).toContain("led");
    expect(ids).not.toContain("core");
  });

  it("MIB-only gear reads as not on this instrument", () => {
    expect(check(IDLE, "autofocus")).toMatchObject({ requirement: "not-applicable", status: "not-required", detail: "Not on this instrument." });
    expect(check(IDLE, "trigger")).toMatchObject({ requirement: "not-applicable", status: "not-required" });
  });

  it("other weights or another build fail; a missing expected core warns", () => {
    const core = HEALTHY.core!;
    expect(check({ ...IDLE, instrument: { ...HEALTHY, core: { ...core, profile_id: "00000000", profile_match: "mismatch" } } }, "plCore")!.status).toBe("failed");
    expect(check({ ...IDLE, instrument: { ...HEALTHY, core: { ...core, build_match: "mismatch" } } }, "plCore")!.status).toBe("failed");
    const unknown = check({ ...IDLE, instrument: { ...HEALTHY, core: { ...core, expected: null, build_match: "unknown" } } }, "plCore")!;
    expect(unknown.status).toBe("warning");
    expect(unknown.detail).toContain("expected-core.json");
    expect(unknown.detail).toContain("scripts/pz_install_core.sh");
    expect(unknown.detail).toContain("Retry check");
  });

  it("an unloaded PL or unread status fails preflight", () => {
    const r = derivePreflight({ ...IDLE, instrument: { available: false, error: "no PZ-MIB platform bridge loaded (identity)" } });
    expect(r.criticalPassed).toBe(false);
    expect(check({ ...IDLE, instrument: null }, "plCore")!.status).toBe("failed");
  });

  it("a strobe guard trip fails; a service LED setting warns; presets pass", () => {
    const led = HEALTHY.led!;
    expect(check({ ...IDLE, instrument: { ...HEALTHY, led: { ...led, on: true, preset: "run", guard_fault: true, guard_trips: 2 } } }, "led")!.status).toBe("failed");
    expect(check({ ...IDLE, instrument: { ...HEALTHY, led: { ...led, on: true, preset: "custom", delay_us: 200, width_us: 90 } } }, "led")!.status).toBe("warning");
    expect(check({ ...IDLE, instrument: { ...HEALTHY, led: { ...led, on: true, preset: "run" } } }, "led")!.status).toBe("passed");
  });

  it("link rates above the thresholds warn; the LED baseline passes", () => {
    const link = HEALTHY.link!;
    expect(check({ ...IDLE, instrument: { ...HEALTHY, link: { ...link, ingress_errors_per_s: 2 } } }, "sensorLink")!.status).toBe("passed");
    expect(check({ ...IDLE, instrument: { ...HEALTHY, link: { ...link, ingress_errors_per_s: 25 } } }, "sensorLink")!.status).toBe("warning");
    expect(check({ ...IDLE, instrument: { ...HEALTHY, link: { ...link, resyncs_per_s: 3 } } }, "sensorLink")!.status).toBe("warning");
    expect(check({ ...IDLE, instrument: { ...HEALTHY, link: { ...link, rates_valid: false } } }, "sensorLink")!.status).toBe("warning");
  });
});

describe("capabilities", () => {
  it("an older server without a report is the MIB desktop", () => {
    expect(capabilitiesOf(null)).toEqual(DESKTOP_CAPABILITIES);
    expect(capabilitiesOf({ science: "host", host_processing: true, aravis: false })).toEqual(DESKTOP_CAPABILITIES);
  });

  it("desktop preflight keeps the host core check", () => {
    const desktop = derivePreflight({ ...IDLE, capabilities: undefined, instrument: undefined, coreValid: true, corePinSatisfied: true });
    expect(desktop.checks.map((c) => c.id)).toContain("core");
    expect(desktop.checks.map((c) => c.id)).not.toContain("plCore");
  });
});

describe("PZ7035 recording target (#501)", () => {
  const ON_DISK = { ...IDLE, storageKnown: true, storageWritable: true, storageFreeOk: true, storagePath: "/mnt/sata/yofo", storageWarning: "" };
  const warning = "Recording to RAM: lost on power-off. Copy data off before shutdown. 0.6 GB free.";
  it("warns, without blocking, while recordings go to RAM", () => {
    const r = derivePreflight({ ...ON_DISK, storagePath: "/var/lib/yofo-studio", storageWarning: warning });
    expect(check({ ...ON_DISK, storageWarning: warning }, "storage")).toMatchObject({ status: "warning", detail: warning });
    expect(r.warning).toBe(1);
    expect(r.criticalPassed).toBe(true);
  });
  it("clears once the target is a disk", () => {
    const r = derivePreflight(ON_DISK);
    expect(check(ON_DISK, "storage")?.status).toBe("passed");
    expect(r.warning + r.failed).toBe(0);
  });
});
