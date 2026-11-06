// Connect and Overview (epic #863, CONNECT-053): the Recording capacity of the next run as the Qt
// RecordingCapacityPanel states it. The numbers, the four gates and which cap bound come from the
// backend (fetch_recording_capacity, #833/#842); this only words them. Status is text first, colour
// second.
import type { CapacityGate, RecordingCapacity } from "../../bridge";
import { READINESS_GATE_STATUSES } from "../../bridgeContract";

export type CapacityLevel = "ok" | "warn" | "blocked";
export type CapacityStatus = "Blocked" | "Warning" | "OK" | "Unknown";

export interface CapacityGateLine { id: string; level: CapacityLevel; status: "Blocked" | "Warning" | "OK"; text: string }
export interface CapacityText {
  status: CapacityStatus;
  facts: string[];
  gates: CapacityGateLine[];
  /** "Buffer 4.7 GiB, 2.00 s - OK" (Qt's one-line summary). */
  summary: string;
}

/** Fail and Unavailable block the run, Warn warns (Qt RecordingCapacityPanel severity). */
function gateLevel(status: number): CapacityLevel {
  return status === READINESS_GATE_STATUSES.Fail || status === READINESS_GATE_STATUSES.Unavailable ? "blocked"
    : status === READINESS_GATE_STATUSES.Warn ? "warn" : "ok";
}
const WORD: Record<CapacityLevel, "Blocked" | "Warning" | "OK"> = { blocked: "Blocked", warn: "Warning", ok: "OK" };
const RANK: Record<CapacityLevel, number> = { blocked: 2, warn: 1, ok: 0 };

export function capacityGateLine(g: CapacityGate): CapacityGateLine {
  const level = gateLevel(g.status);
  let text = `${WORD[level]} - ${g.id}: `;
  if (g.status === READINESS_GATE_STATUSES.Pass || !g.reason) text += g.detail;
  else {
    text += g.reason;
    if (g.remediation) text += ` -> ${g.remediation}`;
  }
  return { id: g.id, level, status: WORD[level], text };
}

const fixed = (n: number, digits: number) => n.toFixed(digits);

export function describeRecordingCapacity(view: RecordingCapacity): CapacityText {
  // Worst first; a stable sort keeps the backend's order (window, rate, buffer, throughput) among equals.
  const gates = view.gates.map(capacityGateLine).map((line, index) => ({ line, index }))
    .sort((a, b) => RANK[b.line.level] - RANK[a.line.level] || a.index - b.index).map((x) => x.line);
  if (!view.known) {
    return {
      status: "Unknown", gates,
      facts: ["The camera window is not known yet: start Live View."],
      summary: "Buffer: start Live View to size it",
    };
  }
  const worst = gates.reduce((w, g) => Math.max(w, RANK[g.level]), 0);
  const status: CapacityStatus = worst >= 2 ? "Blocked" : worst === 1 ? "Warning" : "OK";
  const win = view.window ?? { width: 0, height: 0 };
  const facts: string[] = [];
  facts.push(`${view.planned ? "Planned window" : "Window"} ${win.width}x${win.height}, ${view.images_per_frame ?? 0} images per saved frame (${view.bytes_per_frame_text ?? "?"})`);
  const fps = view.requested_fps ?? 0;
  if (fps > 0) {
    let rate = `Rate ${fixed(fps, 0)} fps requested (${view.requested_fps_source ?? ""})`;
    rate += (view.max_fps ?? 0) > 0 ? `, camera max ${fixed(view.max_fps!, 0)} fps`
      : view.planned ? ", camera max read when the Experiment script applies the window" : ", camera max unknown";
    facts.push(rate);
    const stream = view.save_stream;
    if (stream) facts.push(`Save stream: every frame ${stream.every_frame_text}, no cells ${stream.floor_text} (every ${view.invalid_sampling_rate ?? 1}th invalid frame)`);
  } else {
    facts.push("Rate unknown (set target_fps in the profile)");
  }
  const buffer = view.buffer;
  if (buffer) facts.push(`Buffer ${buffer.summary} (${buffer.source})`);
  const destination = view.destination;
  if (destination) {
    facts.push(destination.bytes_per_second > 0
      ? `Destination ${destination.text} (${destination.note})` : `Destination rate unknown (${destination.note})`);
  }
  const seconds = fps > 0 && buffer ? `, ${fixed(buffer.seconds, 2)} s` : "";
  let summary = `Buffer ${buffer?.bytes_text ?? "?"}${seconds} - ${status}`;
  if (worst >= 2) summary += `: ${gates.filter((g) => g.level === "blocked").map((g) => g.id).join(", ")}`;
  return { status, facts, gates, summary };
}
