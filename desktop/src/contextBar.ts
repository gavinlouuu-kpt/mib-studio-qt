// Persistent active-context & provenance bar (UX-8, issue #312 / epic #304).
//
// The storyboards put a bar across the bottom of every stage answering, at a
// glance: which profile, which camera, is calibration valid, who is operating,
// where is data written, and is the system ready / warning / blocked. This
// derives those segments from the state the shell already has — reusing the
// guided-workflow readiness (UX-1), preflight (UX-3), and quality (UX-4)
// reports for the system/warnings segments.
//
// Segments whose backend surface is not bridged yet (Experiment Profile — UX-2
// #306; operator identity; storage free-space) render as an explicit `pending`
// state rather than being faked, matching the shell's convention.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.
import type { StageStatus, StageTab } from "./workflow";

export type SegStatus = "ok" | "warn" | "blocked" | "pending" | "neutral";

export interface ContextSegment {
  id: string;
  label: string;
  value: string;
  status: SegStatus;
  detail: string;
  /** Stage to navigate to when the segment is activated, if any. */
  tab?: StageTab;
}

export interface ContextBarFacts {
  // Experiment Profile — profile management not bridged yet (UX-2 #306).
  profileName: string;
  // Camera.
  cameraConfigured: boolean;
  cameraRunning: boolean;
  cameraLabel: string;
  // Calibration.
  pixelToMicron: number;
  // System readiness, from the guided workflow (UX-1).
  currentStageTitle: string;
  currentStageStatus: StageStatus;
  allStagesComplete: boolean;
  experimentActive: boolean;
  experimentFailed: boolean;
  // Operator identity — not bridged yet.
  operatorName: string;
  // Storage — output path during a run; free-space check not bridged.
  outputPath: string;
  // Warnings — count of unresolved attention items surfaced elsewhere.
  warningsCount: number;
}

export const SEG_STATUS_LABEL: Readonly<Record<SegStatus, string>> = {
  ok: "OK",
  warn: "Warning",
  blocked: "Blocked",
  pending: "Pending",
  neutral: "—",
};

function stageStatusToSeg(s: StageStatus): SegStatus {
  switch (s) {
    case "complete":
    case "running":
    case "ready":
      return "ok";
    case "needs-attention":
      return "warn";
    case "not-started":
      return "blocked";
  }
}

/** Last path segment of a POSIX/Windows path, for a compact storage label. */
export function baseName(path: string): string {
  const trimmed = path.replace(/[/\\]+$/, "");
  const idx = Math.max(trimmed.lastIndexOf("/"), trimmed.lastIndexOf("\\"));
  return idx >= 0 ? trimmed.slice(idx + 1) : trimmed;
}

function profileSegment(f: ContextBarFacts): ContextSegment {
  if (f.profileName) {
    return {
      id: "profile",
      label: "Profile",
      value: f.profileName,
      status: "ok",
      detail: `Active experiment profile: ${f.profileName}`,
    };
  }
  return {
    id: "profile",
    label: "Profile",
    value: "No profile",
    status: "pending",
    detail: "Experiment profiles arrive with UX-2 (#306).",
  };
}

function cameraSegment(f: ContextBarFacts): ContextSegment {
  let status: SegStatus;
  let value: string;
  let detail: string;
  if (!f.cameraConfigured) {
    status = "blocked";
    value = "No camera";
    detail = "No camera is configured — select one in Hardware Preflight.";
  } else if (f.cameraRunning) {
    status = "ok";
    value = f.cameraLabel || "Camera";
    detail = "Connected and streaming.";
  } else {
    status = "warn";
    value = f.cameraLabel || "Camera";
    detail = "Connected, not streaming.";
  }
  return { id: "camera", label: "Camera", value, status, detail, tab: "connect" };
}

function calibrationSegment(f: ContextBarFacts): ContextSegment {
  return f.pixelToMicron > 0
    ? {
        id: "calibration",
        label: "Calibration",
        value: `${f.pixelToMicron} µm/px`,
        status: "ok",
        detail: "Pixel-to-micron scale is set.",
        tab: "overview",
      }
    : {
        id: "calibration",
        label: "Calibration",
        value: "Not calibrated",
        status: "warn",
        detail: "Set the pixel-to-micron scale in Camera & Alignment.",
        tab: "overview",
      };
}

function systemSegment(f: ContextBarFacts): ContextSegment {
  let status: SegStatus;
  let value: string;
  if (f.experimentFailed) {
    status = "blocked";
    value = "Failed";
  } else if (f.experimentActive) {
    status = "ok";
    value = "Running";
  } else if (f.allStagesComplete) {
    status = "ok";
    value = "Complete";
  } else {
    status = stageStatusToSeg(f.currentStageStatus);
    value = f.currentStageTitle;
  }
  return {
    id: "system",
    label: "Status",
    value,
    status,
    detail: `Workflow: ${f.currentStageTitle} (${f.currentStageStatus}).`,
  };
}

function operatorSegment(f: ContextBarFacts): ContextSegment {
  return f.operatorName
    ? { id: "operator", label: "Operator", value: f.operatorName, status: "neutral", detail: "Current operator." }
    : {
        id: "operator",
        label: "Operator",
        value: "—",
        status: "pending",
        detail: "Operator identity is not captured yet.",
      };
}

function storageSegment(f: ContextBarFacts): ContextSegment {
  if (f.experimentActive && f.outputPath) {
    return {
      id: "storage",
      label: "Storage",
      value: baseName(f.outputPath),
      status: "ok",
      detail: `Writing to ${f.outputPath}.`,
    };
  }
  return {
    id: "storage",
    label: "Storage",
    value: "—",
    status: "pending",
    detail: "Output path shows during a run; free-space check pending a backend contract.",
  };
}

function warningsSegment(f: ContextBarFacts): ContextSegment {
  return f.warningsCount > 0
    ? {
        id: "warnings",
        label: "Warnings",
        value: String(f.warningsCount),
        status: "warn",
        detail: `${f.warningsCount} unresolved attention item(s) across setup.`,
      }
    : { id: "warnings", label: "Warnings", value: "0", status: "ok", detail: "No warnings." };
}

/** Build the persistent context bar segments from shell state. */
export function deriveContextBar(f: ContextBarFacts): { segments: ContextSegment[] } {
  return {
    segments: [
      profileSegment(f),
      cameraSegment(f),
      calibrationSegment(f),
      systemSegment(f),
      operatorSegment(f),
      storageSegment(f),
      warningsSegment(f),
    ],
  };
}
