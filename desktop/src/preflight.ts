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
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

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
): PreflightCheck {
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];
  const detected = dev.connected ? dev.identity || "connected" : "—";

  if (requirement === "not-applicable") {
    status = "not-required";
    detail = "Not used by this profile.";
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

function triggerCheck(i: PreflightInput, requirement: Requirement): PreflightCheck {
  const attached = i.trigger.valid && i.trigger.cameraAttached;
  let status: CheckStatus;
  let detail: string;
  let recovery: RecoveryAction[] = [];

  if (requirement === "not-applicable") {
    status = "not-required";
    detail = "Not used by this profile.";
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
  requirements: PreflightRequirements = DEFAULT_REQUIREMENTS,
): PreflightReport {
  const checks: PreflightCheck[] = [
    cameraCheck(input),
    coreCheck(input),
    captureCheck(input),
    deviceCheck("autofocus", "Autofocus / nanopositioner", requirements.autofocus, input.autofocus),
    deviceCheck("samplePump", "Sample pump", requirements.samplePump, input.samplePump),
    deviceCheck("sheathPump", "Sheath pump", requirements.sheathPump, input.sheathPump),
    triggerCheck(input, requirements.trigger),
    storageCheck(input, requirements.storage),
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
