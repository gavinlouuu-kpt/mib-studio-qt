// Camera & Alignment image-quality gates (UX-4, issue #308 / epic #304).
//
// The Camera & Alignment stage (UX-1 #305) lets the operator confirm the image
// is ready, but "ready" was a judgement call. This derives concrete quality
// gates from already-bridged signals — focus metric + staleness, background
// availability, ROI validity, calibration — so focus/background/ROI/calibration
// each expose pass/warn/fail with an explanation, mirroring the preflight
// checklist (UX-3). It is the detailed content of the Align stage.
//
// Honest scope: illumination stability/saturation and channel-wall ROI insets
// (#295) need signals that are not bridged yet, and save-to-profile needs
// profile management (UX-2 #306); those are follow-ups. Gates only report what
// the backend actually exposes — no image quality is simulated.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

export type GateStatus = "pass" | "warn" | "fail" | "unknown";

export interface QualityGate {
  id: string;
  label: string;
  status: GateStatus;
  /** Short measured value or state, e.g. "0.412", "512×96", "Set". */
  value: string;
  /** Human-readable explanation / next step. */
  detail: string;
}

export interface QualityInput {
  cameraRunning: boolean;
  // Focus: autofocus ring-ratio metric and its freshness (schema v11).
  focusConnected: boolean;
  focusMetric: number;
  /** Microsecond timestamp of the last focus-metric update; 0 = never. */
  focusUpdatedUs: number;
  focusAgeMs: number;
  focusStaleMs: number;
  // Background policy (processing config).
  backgroundSet: boolean;
  // ROI (processing config) vs the current frame size.
  roiW: number;
  roiH: number;
  frameW: number;
  frameH: number;
  // Calibration (px→µm).
  pixelToMicron: number;
}

export interface QualityReport {
  gates: QualityGate[];
  pass: number;
  warn: number;
  fail: number;
  unknown: number;
}

export const GATE_STATUS_LABEL: Readonly<Record<GateStatus, string>> = {
  pass: "Pass",
  warn: "Warn",
  fail: "Fail",
  unknown: "Unknown",
};

function focusGate(i: QualityInput): QualityGate {
  let status: GateStatus;
  let value = "—";
  let detail: string;

  if (!i.cameraRunning) {
    status = "unknown";
    detail = "Start the camera to measure focus.";
  } else if (!i.focusConnected) {
    status = "unknown";
    detail = "Autofocus controller is not connected.";
  } else if (i.focusUpdatedUs === 0) {
    status = "warn";
    detail = "No focus metric has been reported yet.";
  } else if (i.focusAgeMs > i.focusStaleMs) {
    status = "warn";
    value = i.focusMetric.toFixed(3);
    detail = `Focus metric is stale (${Math.round(i.focusAgeMs)} ms old).`;
  } else {
    status = "pass";
    value = i.focusMetric.toFixed(3);
    detail = "Live focus metric.";
  }

  return { id: "focus", label: "Focus", status, value, detail };
}

function backgroundGate(i: QualityInput): QualityGate {
  return i.backgroundSet
    ? { id: "background", label: "Background", status: "pass", value: "Set", detail: "Background frame captured." }
    : {
        id: "background",
        label: "Background",
        status: "warn",
        value: "Not set",
        detail: "Capture a background frame for clean processing.",
      };
}

function roiGate(i: QualityInput): QualityGate {
  if (i.roiW <= 0 || i.roiH <= 0) {
    return { id: "roi", label: "ROI", status: "warn", value: "Not set", detail: "Define a processing ROI." };
  }
  if (i.frameW > 0 && i.frameH > 0 && (i.roiW > i.frameW || i.roiH > i.frameH)) {
    return {
      id: "roi",
      label: "ROI",
      status: "fail",
      value: `${i.roiW}×${i.roiH}`,
      detail: "ROI is larger than the current frame.",
    };
  }
  return {
    id: "roi",
    label: "ROI",
    status: "pass",
    value: `${i.roiW}×${i.roiH}`,
    detail: "ROI is set and within the frame.",
  };
}

function calibrationGate(i: QualityInput): QualityGate {
  return i.pixelToMicron > 0
    ? {
        id: "calibration",
        label: "Calibration",
        status: "pass",
        value: `${i.pixelToMicron} µm/px`,
        detail: "Pixel-to-micron scale is set.",
      }
    : {
        id: "calibration",
        label: "Calibration",
        status: "warn",
        value: "Not calibrated",
        detail: "Set the pixel-to-micron scale.",
      };
}

/** Derive the Camera & Alignment quality gates from bridged signals. */
export function deriveQualityGates(input: QualityInput): QualityReport {
  const gates: QualityGate[] = [
    focusGate(input),
    backgroundGate(input),
    roiGate(input),
    calibrationGate(input),
  ];

  let pass = 0;
  let warn = 0;
  let fail = 0;
  let unknown = 0;
  for (const g of gates) {
    if (g.status === "pass") pass++;
    else if (g.status === "warn") warn++;
    else if (g.status === "fail") fail++;
    else unknown++;
  }

  return { gates, pass, warn, fail, unknown };
}
