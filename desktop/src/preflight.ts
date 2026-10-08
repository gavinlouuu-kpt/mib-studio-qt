// Profile-aware hardware preflight checklist (UX-3, issue #307 / epic #304).
//
// The Preflight stage introduced by UX-1 (#305) only knew "camera configured +
// core pinned". This turns it into an explicit, per-subsystem checklist so the
// operator can tell "detected" from "ready for this experiment", see the
// expected-vs-detected identity, and get a recovery hint for anything that is
// not ready.
//
// Which devices are required/optional/not-applicable comes from the selected
// Experiment Profile. Profile management is not bridged yet (BE-3 #273
// follow-up / UX-2 #306), so DEFAULT_REQUIREMENTS is applied until a profile
// can declare them — the shape is ready for that override.
//
// On the PZ7035 (#501) the checks follow the instrument's capabilities: the
// PL core (build + weights) replaces the host core pin, the sensor link and
// the LED strobe are required, and MIB-only gear reads "not on this
// instrument" instead of a warning.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

import type { InstrumentStatus, PlatformCapabilities } from "./bridge";
import { DESKTOP_CAPABILITIES, isPz7035 } from "./platformCapabilities";

export type CheckStatus = "passed" | "warning" | "failed" | "not-required";
export type Requirement = "required" | "optional" | "not-applicable";
export type RecoveryKind = "refresh" | "retry";

export interface RecoveryAction {
  kind: RecoveryKind;
  label: string;
}

export interface PreflightCheck {
  id: string;
  label: string;
  requirement: Requirement;
  status: CheckStatus;
  /** Expected identity/target from the profile, or "" when none is declared. */
  expected: string;
  /** Detected value, or "—" when nothing is present. */
  detected: string;
  /** Human-readable cause and/or recommended next step. */
  detail: string;
  recovery: RecoveryAction[];
}

/** A serial/USB device snapshot reduced to the fields preflight cares about. */
export interface DeviceSnapshot {
  valid: boolean;
  connected: boolean;
  identity: string;
}

export interface PreflightInput {
  backendReady: boolean;
  cameraConfigured: boolean;
  cameraRunning: boolean;
  cameraIdentity: string;
  /** Expected camera identity declared by the profile, or "" if none. */
  cameraExpected: string;
  coreValid: boolean;
  corePinSatisfied: boolean;
  coreVersion: string;
  requiredCoreVersion: string;
  autofocus: DeviceSnapshot;
  samplePump: DeviceSnapshot;
  sheathPump: DeviceSnapshot;
  trigger: { valid: boolean; cameraAttached: boolean };
  /** Storage status is not bridged yet; when false the check is informational. */
  storageKnown: boolean;
  storageWritable: boolean;
  storageFreeOk: boolean;
  storagePath: string;
  /** Non-blocking persistence warning for the destination (RAM root, #501); "" when none. */
  storageWarning?: string;
  /** What this instrument has (#501); the MIB desktop when absent. */
  capabilities?: PlatformCapabilities;
  /** PZ7035 identity and health (`fetch_instrument_status`); null until polled. */
  instrument?: InstrumentStatus | null;
}

/** Per-device requirement, normally declared by the selected profile. */
export interface PreflightRequirements {
  autofocus: Requirement;
  samplePump: Requirement;
  sheathPump: Requirement;
  trigger: Requirement;
  storage: Requirement;
}

export const DEFAULT_REQUIREMENTS: PreflightRequirements = {
  autofocus: "optional",
  samplePump: "optional",
  sheathPump: "optional",
  trigger: "optional",
  storage: "optional",
};

/** The requirements an instrument implies before any profile declares them. */
export function requirementsFor(capabilities: PlatformCapabilities): PreflightRequirements {
  return {
    ...DEFAULT_REQUIREMENTS,
    autofocus: capabilities.autofocus ? DEFAULT_REQUIREMENTS.autofocus : "not-applicable",
    trigger: capabilities.trigger ? DEFAULT_REQUIREMENTS.trigger : "not-applicable",
  };
}

export interface PreflightReport {
  checks: PreflightCheck[];
  passed: number;
  warning: number;
  failed: number;
  notRequired: number;
  total: number;
  /** True when every `required` check passed — the gate for readiness. */
  criticalPassed: boolean;
}

const REFRESH: RecoveryAction = { kind: "refresh", label: "Refresh" };
const RETRY: RecoveryAction = { kind: "retry", label: "Retry check" };

function cameraCheck(i: PreflightInput): PreflightCheck {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];
  const detected = i.cameraConfigured ? i.cameraIdentity || "configured camera" : "—";

  if (!i.backendReady) {
    status = "failed";
    detail = "Backend is not initialized.";
    recovery = [RETRY];
  } else if (!i.cameraConfigured) {
    status = "failed";
    detail = "No camera selected — pick a device above and Connect.";
    recovery = [REFRESH];
  } else if (i.cameraExpected !== "" && i.cameraExpected !== i.cameraIdentity) {
    status = "warning";
    detail = `Detected ${detected}, but the profile expects ${i.cameraExpected}.`;
    recovery = [REFRESH];
  } else {
    status = "passed";
    detail = i.cameraRunning ? "Connected and streaming." : "Connected.";
  }

  return {
    id: "camera",
    label: "Camera",
    requirement: "required",
    status,
    expected: i.cameraExpected,
    detected,
    detail,
    recovery,
  };
}

function coreCheck(i: PreflightInput): PreflightCheck {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];

  if (!i.coreValid) {
    status = "failed";
    detail = "Processing-core identity is unavailable.";
    recovery = [RETRY];
  } else if (!i.corePinSatisfied) {
    status = "failed";
    detail = `Core pin not satisfied — requires ${i.requiredCoreVersion || "a pinned version"}.`;
    recovery = [RETRY];
  } else {
    status = "passed";
    detail = "Trusted core is active and pinned.";
  }

  return {
    id: "core",
    label: "Processing core / trust",
    requirement: "required",
    status,
    expected: i.requiredCoreVersion,
    detected: i.coreValid ? i.coreVersion || "unknown" : "—",
    detail,
    recovery,
  };
}

const shortId = (id: string) => (id ? id.slice(0, 8) : "—");

/** PZ7035: the PL build and its weights are the core (ADR 0011 §12). */
function plCoreCheck(i: PreflightInput): PreflightCheck {
  const s = i.instrument;
  const base = { id: "plCore", label: "PL core (build + weights)", requirement: "required" as Requirement };
  if (!s || !s.available || !s.core) {
    return {
      ...base,
      status: "failed",
      expected: "",
      detected: "—",
      detail: s?.error ? `PL core unavailable: ${s.error}.` : "PL core identity not read yet.",
      recovery: [RETRY],
    };
  }
  const c = s.core;
  const detected = `build ${shortId(c.build_id)} · weights ${shortId(c.profile_id)}`;
  const expected = `${c.expected ? `${c.expected.image || "image"} ${shortId(c.expected.build_id)}` : "build ?"} · weights ${shortId(c.pinned_profile_id)}`;
  let status: CheckStatus = "passed";
  let detail = `PL build and weights match${c.expected?.image ? ` ${c.expected.image}` : ""}.`;
  let recovery: RecoveryAction[] = [];
  if (c.profile_match === "mismatch") {
    status = "failed";
    detail = `The PL runs weights ${shortId(c.profile_id)}, not the pinned ${shortId(c.pinned_profile_id)}: load the matching image.`;
    recovery = [RETRY];
  } else if (c.build_match === "mismatch") {
    status = "failed";
    detail = `The PL build ${shortId(c.build_id)} is not the expected image ${shortId(c.expected?.build_id ?? "")}: reload the image or install its core.json.`;
    recovery = [RETRY];
  } else if (c.profile_match === "unknown") {
    status = "warning";
    detail = "No pinned weights in this build: the PL weights are not checked.";
  } else if (c.build_match === "unknown") {
    status = "warning";
    // This blocks Preflight (a required check that is not passed fails closed), so the text is the
    // fix: the file is what lets the shell verify the PL build that is loaded (#548).
    detail = "The PL build is not verified: /etc/yofo/expected-core.json is missing. On the instrument run " +
      "scripts/pz_install_core.sh <build dir> (it writes that file from the build's core.json), then press Retry check.";
    recovery = [RETRY];
  }
  return { ...base, status, expected, detected, detail, recovery };
}

function fmtRate(v: number) {
  return v < 10 ? v.toFixed(1) : v.toFixed(0);
}

/** PZ7035: sensor link health from the PL's cumulative counters. */
function sensorLinkCheck(i: PreflightInput): PreflightCheck {
  const s = i.instrument;
  const base = { id: "sensorLink", label: "Sensor link", requirement: "required" as Requirement, expected: "" };
  if (!s || !s.available || !s.link) {
    return { ...base, status: "failed", detected: "—", detail: s?.error ? `Unavailable: ${s.error}.` : "Not read yet.", recovery: [RETRY] };
  }
  const l = s.link;
  if (!l.rates_valid) {
    return { ...base, status: "warning", detected: "measuring", detail: "Measuring link error rates…", recovery: [] };
  }
  const detected = `${fmtRate(l.ingress_errors_per_s)} err/s · ${fmtRate(l.resyncs_per_s)} resync/s`;
  const issues: string[] = [];
  if (l.ingress_errors_per_s > l.ingress_errors_warn_per_s) issues.push(`ingress errors above ${l.ingress_errors_warn_per_s}/s`);
  if (l.resyncs_per_s > l.resyncs_warn_per_s) issues.push(`resyncs above ${l.resyncs_warn_per_s}/s`);
  if (l.bad_frames_per_s > 0) issues.push("bad frames rising");
  return issues.length
    ? { ...base, status: "warning", detected, detail: `Sensor link: ${issues.join(", ")}. Check the sensor cable and the LED wiring.`, recovery: [RETRY] }
    : { ...base, status: "passed", detected, detail: "Link errors within the known baseline.", recovery: [] };
}

/** PZ7035: LED strobe; the PL guards are authoritative, a trip fails preflight. */
function ledCheck(i: PreflightInput): PreflightCheck {
  const s = i.instrument;
  const base = { id: "led", label: "LED strobe", requirement: "required" as Requirement, expected: "Run 7/60 µs · Align 100/135 µs" };
  if (!s || !s.available || !s.led) {
    return { ...base, status: "failed", detected: "—", detail: s?.error ? `Unavailable: ${s.error}.` : "Not read yet.", recovery: [RETRY] };
  }
  const led = s.led;
  const detected = led.on ? `${led.preset} ${led.delay_us}/${led.width_us} µs` : "off";
  if (led.guard_fault) {
    return { ...base, status: "failed", detected, detail: `The strobe guard tripped (${led.guard_trips} trips): the LED is held off. Switching a mode clears it.`, recovery: [RETRY] };
  }
  if (led.on && led.preset === "custom") {
    return { ...base, status: "warning", detected, detail: "The LED runs a service setting, not a preset.", recovery: [] };
  }
  return { ...base, status: "passed", detected, detail: led.on ? `${led.preset === "run" ? "Run" : "Align"} preset.` : "Off; Align and Run switch it on.", recovery: [] };
}

function captureCheck(i: PreflightInput): PreflightCheck {
  // Capture stability can only be judged once the stream is running; before
  // that it is simply pending rather than a failure.
  const running = i.cameraConfigured && i.cameraRunning;
  return {
    id: "capture",
    label: "Capture stream",
    requirement: "optional",
    status: running ? "passed" : "not-required",
    expected: "",
    detected: running ? "streaming" : "not started",
    detail: running
      ? "Frames are being delivered."
      : "Starts when the camera runs (Camera & Alignment stage).",
    recovery: running ? [] : [],
  };
}

function deviceCheck(
  id: string,
  label: string,
  requirement: Requirement,
  dev: DeviceSnapshot,
  notApplicableDetail = "Not used by this profile.",
): PreflightCheck {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];
  const detected = dev.connected ? dev.identity || "connected" : "—";

  if (requirement === "not-applicable") {
    status = "not-required";
    detail = notApplicableDetail;
  } else if (dev.connected) {
    status = "passed";
    detail = "Connected and responding.";
  } else if (requirement === "required") {
    status = "failed";
    detail = "Required by the profile but not connected.";
    recovery = [RETRY];
  } else {
    status = "not-required";
    detail = "Optional — not connected.";
  }

  return { id, label, requirement, status, expected: "", detected, detail, recovery };
}

function triggerCheck(i: PreflightInput, requirement: Requirement, notApplicableDetail = "Not used by this profile."): PreflightCheck {
  const attached = i.trigger.valid && i.trigger.cameraAttached;
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];

  if (requirement === "not-applicable") {
    status = "not-required";
    detail = notApplicableDetail;
  } else if (attached) {
    status = "passed";
    detail = "Sorter trigger is attached to the camera.";
  } else if (requirement === "required") {
    status = "failed";
    detail = "Required by the profile but the trigger is not attached.";
    recovery = [RETRY];
  } else {
    status = "not-required";
    detail = "Optional — trigger not attached.";
  }

  return {
    id: "trigger",
    label: "Trigger / sorter",
    requirement,
    status,
    expected: "",
    detected: attached ? "attached" : "—",
    detail,
    recovery,
  };
}

function storageCheck(i: PreflightInput, requirement: Requirement): PreflightCheck {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];

  if (!i.storageKnown) {
    // An authoritative storage/free-space contract is not bridged yet.
    status = "not-required";
    detail = "Storage readiness check is pending a backend status contract.";
  } else if (requirement === "not-applicable") {
    status = "not-required";
    detail = "Not used by this profile.";
  } else if (!i.storageWritable) {
    status = "failed";
    detail = "Output location is not writable.";
    recovery = [RETRY];
  } else if (i.storageWarning) {
    // Recordings still work; the operator must copy them off before power-off.
    status = "warning";
    detail = i.storageWarning;
  } else if (!i.storageFreeOk) {
    status = "warning";
    detail = "Low free space at the output location.";
    recovery = [RETRY];
  } else {
    status = "passed";
    detail = "Output location is writable with sufficient free space.";
  }

  return {
    id: "storage",
    label: "Storage destination",
    requirement: i.storageKnown ? requirement : "optional",
    status,
    expected: "",
    detected: i.storageKnown ? i.storagePath || "unknown" : "—",
    detail,
    recovery,
  };
}

/** Build the full preflight report from device snapshots and the profile's
 *  device requirements. */
export function derivePreflight(
  input: PreflightInput,
  requirements?: PreflightRequirements,
): PreflightReport {
  const caps = input.capabilities ?? DESKTOP_CAPABILITIES;
  const req = requirements ?? requirementsFor(caps);
  const pz = isPz7035(caps);
  const notHere = pz ? "Not on this instrument." : "Not used by this profile.";
  const checks: PreflightCheck[] = [
    cameraCheck(input),
    ...(pz ? [plCoreCheck(input), sensorLinkCheck(input), ledCheck(input)] : [coreCheck(input)]),
    captureCheck(input),
    deviceCheck("autofocus", "Autofocus / nanopositioner", req.autofocus, input.autofocus, notHere),
    deviceCheck("samplePump", "Sample pump", req.samplePump, input.samplePump),
    deviceCheck("sheathPump", "Sheath pump", req.sheathPump, input.sheathPump),
    triggerCheck(input, req.trigger, pz ? "The PL times the sort output." : notHere),
    storageCheck(input, req.storage),
  ];

  let passed = 0;
  let warning = 0;
  let failed = 0;
  let notRequired = 0;
  let criticalPassed = true;
  for (const c of checks) {
    if (c.status === "passed") passed++;
    else if (c.status === "warning") warning++;
    else if (c.status === "failed") failed++;
    else notRequired++;
    if (c.requirement === "required" && c.status !== "passed") criticalPassed = false;
  }

  return {
    checks,
    passed,
    warning,
    failed,
    notRequired,
    total: checks.length,
    criticalPassed,
  };
}

export const CHECK_STATUS_LABEL: Readonly<Record<CheckStatus, string>> = {
  passed: "Passed",
  warning: "Warning",
  failed: "Failed",
  "not-required": "Not required",
};
