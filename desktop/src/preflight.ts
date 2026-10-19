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
// States follow the backend's readiness vocabulary (#713, `ExperimentReadiness.h`): a value that
// has not been read yet is `unknown` (never `failed`, never `not-required`, never a pass), and a
// capability the instrument does not have is `unsupported` (never blocks). `checkGateStatus` maps
// each state to the backend's Pass / Warn / Fail / Unavailable / NotRequired. Blocking is #548's
// rule, failing closed: a required check that is not passed (failed, unknown or warning) blocks
// the Preflight confirmation; unsupported and not-required rows never block. Relaxing a specific
// warning needs an explicit, audited per-warning acknowledgement (#310), not a backend gate: the
// backend's `science.pl` gate, for one, never checks the PL build identity.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

import type { InstrumentStatus, PlatformCapabilities } from "./bridge";
import { READINESS_GATE_STATUSES } from "./bridgeContract";
import { linkReadout, sensorReadout } from "./linkHealth";
import { DESKTOP_CAPABILITIES, isPz7035 } from "./platformCapabilities";

export type CheckStatus = "passed" | "warning" | "failed" | "unknown" | "unsupported" | "not-required";
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
  /** True when this check blocks the Preflight confirmation (see the module comment). */
  blocks: boolean;
}

/** A serial/USB device snapshot reduced to the fields preflight cares about. */
export interface DeviceSnapshot {
  /** False until the backend reported the device (not read yet, or not reported). */
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
  /** False while the processing-core status has not been read yet (undefined = read). */
  coreKnown?: boolean;
  coreValid: boolean;
  corePinSatisfied: boolean;
  coreVersion: string;
  requiredCoreVersion: string;
  autofocus: DeviceSnapshot;
  samplePump: DeviceSnapshot;
  sheathPump: DeviceSnapshot;
  trigger: { valid: boolean; cameraAttached: boolean };
  /** False when nothing reported the recording destination (the desktop before Start): the check
   *  is then `unknown`, and Start checks the chosen file. */
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

/** The checklist poll's period while it is shown, and how long one stuck read may hold the poll
 *  before the next tick starts another (App, `SerialReads`; #736 review). */
export const PREFLIGHT_POLL_MS = 1500;
export const PREFLIGHT_READ_STALE_MS = 10_000;

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
  unknown: number;
  unsupported: number;
  notRequired: number;
  total: number;
  /** True when no check blocks (`blocks`): the gate for the Preflight confirmation. */
  criticalPassed: boolean;
}

/** The backend readiness status a checklist state corresponds to. Unknown is never Pass. */
export function checkGateStatus(status: CheckStatus): number {
  switch (status) {
    case "passed": return READINESS_GATE_STATUSES.Pass;
    case "warning": return READINESS_GATE_STATUSES.Warn;
    case "failed": return READINESS_GATE_STATUSES.Fail;
    case "unknown": return READINESS_GATE_STATUSES.Unavailable;
    case "unsupported":
    case "not-required": return READINESS_GATE_STATUSES.NotRequired;
  }
}

type Draft = Omit<PreflightCheck, "blocks">;

const REFRESH: RecoveryAction = { kind: "refresh", label: "Refresh" };
const RETRY: RecoveryAction = { kind: "retry", label: "Retry check" };

function cameraCheck(i: PreflightInput): Draft {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];
  const detected = i.cameraConfigured ? i.cameraIdentity || "configured camera" : "—";

  if (!i.backendReady) {
    // Nothing has been read yet: unknown, not a failure (#713).
    status = "unknown";
    detail = "Not read yet: the backend is not initialized.";
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

function coreCheck(i: PreflightInput): Draft {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];

  if (i.coreKnown === false) {
    status = "unknown";
    detail = "Processing-core status not read yet.";
    recovery = [RETRY];
  } else if (!i.coreValid) {
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
function plCoreCheck(i: PreflightInput): Draft {
  const s = i.instrument;
  const base = { id: "plCore", label: "PL core (build + weights)", requirement: "required" as Requirement };
  if (!s || (s.available && !s.core)) {
    // Not polled yet, or polled without a core block: unknown, not a failure (#713).
    return { ...base, status: "unknown", expected: "", detected: "—", detail: s ? "PL core identity not reported." : "PL core identity not read yet.", recovery: [RETRY] };
  }
  if (!s.available || !s.core) {
    return {
      ...base,
      status: "failed",
      expected: "",
      detected: "—",
      detail: s.error ? `PL core unavailable: ${s.error}.` : "PL core unavailable.",
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
    // The weights cannot be verified: unknown, never a pass, and it blocks (#548, #713).
    status = "unknown";
    detail = "No pinned weights in this build: the PL weights are not checked.";
  } else if (c.build_match === "unknown") {
    // The build cannot be verified: unknown, never a pass. This blocks Preflight (a required
    // check that is not passed fails closed), so the text is the fix: the file is what lets the
    // shell verify the PL build that is loaded (#548).
    status = "unknown";
    detail = "The PL build is not verified: /etc/yofo/expected-core.json is missing. On the instrument run " +
      "scripts/pz_install_core.sh <build dir> (it writes that file from the build's core.json), then press Retry check.";
    recovery = [RETRY];
  }
  return { ...base, status, expected, detected, detail, recovery };
}

/** PZ7035: sensor link health from the PL's cumulative counters. */
function sensorLinkCheck(i: PreflightInput): Draft {
  const s = i.instrument;
  const base = { id: "sensorLink", label: "Sensor link", requirement: "required" as Requirement, expected: "" };
  if (!s || (s.available && !s.link)) {
    return { ...base, status: "unknown", detected: "—", detail: s ? "Not reported." : "Not read yet.", recovery: [RETRY] };
  }
  if (!s.available || !s.link) {
    return { ...base, status: "failed", detected: "—", detail: s.error ? `Unavailable: ${s.error}.` : "Unavailable.", recovery: [RETRY] };
  }
  const l = s.link;
  if (!l.rates_valid) {
    // No rate measured yet: unknown until the first window completes (#713).
    return { ...base, status: "unknown", detected: "measuring", detail: "Measuring link error rates…", recovery: [] };
  }
  const detected = linkReadout(s).text;
  const sensor = sensorReadout(s);
  const issues: string[] = [];
  if (l.ingress_errors_warn) issues.push(`ingress errors above ${l.ingress_errors_warn_per_s}/s (5 s average)`);
  if (l.resyncs_per_s > l.resyncs_warn_per_s) issues.push(`resyncs above ${l.resyncs_warn_per_s}/s`);
  if (l.bad_frames_warn) issues.push("bad frames above 1% of the frame rate for 5 s");
  if (l.dropped_warn) issues.push("dropped frames above 0.1% of the frame rate for 5 s (data loss in Run)");
  return issues.length
    ? { ...base, status: "warning", detected, detail: `Sensor link: ${issues.join(", ")}. Check the sensor cable and the LED wiring.`, recovery: [RETRY] }
    : { ...base, status: "passed", detected, detail: `Link errors within the known baseline. Sensor ${sensor.text}.`, recovery: [] };
}

/** PZ7035: LED strobe; the PL guards are authoritative, a trip fails preflight. */
function ledCheck(i: PreflightInput): Draft {
  const s = i.instrument;
  const base = { id: "led", label: "LED strobe", requirement: "required" as Requirement, expected: "Run 7/60 µs · Align 100/135 µs" };
  if (!s || (s.available && !s.led)) {
    return { ...base, status: "unknown", detected: "—", detail: s ? "Not reported." : "Not read yet.", recovery: [RETRY] };
  }
  if (!s.available || !s.led) {
    return { ...base, status: "failed", detected: "—", detail: s.error ? `Unavailable: ${s.error}.` : "Unavailable.", recovery: [RETRY] };
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

function captureCheck(i: PreflightInput): Draft {
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

/** How a row reads when the profile or the instrument leaves it out. */
interface NotApplicable {
  /** False when the instrument does not have the capability: the row is `unsupported`. */
  supported: boolean;
  detail: string;
}
const PROFILE_NA: NotApplicable = { supported: true, detail: "Not used by this profile." };

function deviceCheck(
  id: string,
  label: string,
  requirement: Requirement,
  dev: DeviceSnapshot,
  na: NotApplicable = PROFILE_NA,
): Draft {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];
  const detected = dev.connected ? dev.identity || "connected" : "—";

  if (requirement === "not-applicable") {
    status = na.supported ? "not-required" : "unsupported";
    detail = na.detail;
  } else if (!dev.valid) {
    // Not read yet (or not reported): unknown, never "not connected" (#713).
    status = "unknown";
    detail = "Not reported yet.";
    recovery = [RETRY];
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

function triggerCheck(i: PreflightInput, requirement: Requirement, na: NotApplicable = PROFILE_NA): Draft {
  const attached = i.trigger.valid && i.trigger.cameraAttached;
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];

  if (requirement === "not-applicable") {
    status = na.supported ? "not-required" : "unsupported";
    detail = na.detail;
  } else if (!i.trigger.valid) {
    status = "unknown";
    detail = "Not reported yet.";
    recovery = [RETRY];
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

function storageCheck(i: PreflightInput, requirement: Requirement): Draft {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];

  if (requirement === "not-applicable") {
    status = "not-required";
    detail = "Not used by this profile.";
  } else if (!i.storageKnown) {
    // Nothing reported the destination (the desktop picks it at Start): unknown, never
    // "not required" (#713). Start evaluates the backend storage gates with the chosen file.
    status = "unknown";
    detail = "Not reported before Start: the backend checks the chosen file when Start is pressed.";
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

/** Whether a check blocks the Preflight confirmation (#548, failing closed): a required check
 *  that failed, is unknown or warns. Unsupported and not-required rows never block. */
function blocks(c: Draft): boolean {
  return c.requirement === "required" && (c.status === "failed" || c.status === "unknown" || c.status === "warning");
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
  // A capability the instrument does not have is unsupported, not "not used by this profile".
  const naFor = (supported: boolean, detail: string): NotApplicable => (supported ? PROFILE_NA : { supported: false, detail });
  const drafts: Draft[] = [
    cameraCheck(input),
    ...(pz ? [plCoreCheck(input), sensorLinkCheck(input), ledCheck(input)] : [coreCheck(input)]),
    captureCheck(input),
    // No nanopositioner on the PZ7035: the row would only say "Not on this instrument."
    ...(pz ? [] : [deviceCheck("autofocus", "Autofocus / nanopositioner", req.autofocus, input.autofocus, naFor(caps.autofocus, "Not on this instrument."))]),
    deviceCheck("samplePump", "Sample pump", req.samplePump, input.samplePump),
    deviceCheck("sheathPump", "Sheath pump", req.sheathPump, input.sheathPump),
    triggerCheck(input, req.trigger, naFor(caps.trigger, pz ? "The PL times the sort output." : "Not on this instrument.")),
    storageCheck(input, req.storage),
  ];
  const checks: PreflightCheck[] = drafts.map((c) => ({ ...c, blocks: blocks(c) }));

  const count = (status: CheckStatus) => checks.filter((c) => c.status === status).length;
  return {
    checks,
    passed: count("passed"),
    warning: count("warning"),
    failed: count("failed"),
    unknown: count("unknown"),
    unsupported: count("unsupported"),
    notRequired: count("not-required"),
    total: checks.length,
    criticalPassed: checks.every((c) => !c.blocks),
  };
}

export const CHECK_STATUS_LABEL: Readonly<Record<CheckStatus, string>> = {
  passed: "Passed",
  warning: "Warning",
  failed: "Failed",
  unknown: "Unknown",
  unsupported: "Unsupported",
  "not-required": "Not required",
};
