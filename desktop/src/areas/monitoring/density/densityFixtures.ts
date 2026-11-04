// Test support: replies of the density commands in the contract's shapes, run through the real decoders
// (monitoringDensity.ts), so a test feeds the shell what the bridge would.
import { decodeKdeReference, decodeMonitoringDensity, type KdeReference, type MonitoringDensityReply } from "../../../monitoringDensity";

export type NewDensityReply = Extract<MonitoringDensityReply, { valid: true; unchanged: false }>;

export interface PointSpec {
  frame: number;
  object?: number;
  density: number | null;
  start?: string;
  epoch?: string;
}

/** A `fetch_monitoring_density` reply with a newer generation. */
export function densityReply(points: PointSpec[], over: { generation?: string; result?: Record<string, unknown> | null; refreshMs?: number } = {}): NewDensityReply {
  const contexts: { start_generation: string; capture_epoch: string }[] = [];
  const ctxOf = (start: string, epoch: string) => {
    let at = contexts.findIndex((c) => c.start_generation === start && c.capture_epoch === epoch);
    if (at < 0) {
      contexts.push({ start_generation: start, capture_epoch: epoch });
      at = contexts.length - 1;
    }
    return at;
  };
  const context = points.map((p) => ctxOf(p.start ?? "1", p.epoch ?? "2"));
  const result = over.result === null ? null : {
    points: {
      count: points.length, contexts, context, frame_index: points.map((p) => String(p.frame)), object_id: points.map((p) => p.object ?? 0),
      density: points.map((p) => p.density),
    },
    bandwidth: { x: 12.345, y: 0.01234 }, bandwidth_factor: 1, core_fraction: 0.9, core_level: 0.4, core_count: 3,
    contours: [[[1, 0.1], [2, 0.2], [1, 0.1]]], pixel_to_micron: 0.5,
    grid: { x0: 0, x1: 1000, y0: 0, y1: 1, nx: 128, ny: 64 }, compute_ms: 21, compute_us: 21000, wall_ms: 22, computed_at_ns: "12345",
    ...over.result,
  };
  const refresh = over.refreshMs ?? 3000;
  return decodeMonitoringDensity({
    valid: true, unchanged: false, generation: over.generation ?? "7",
    settings: { enabled: true, interval_ms: 2000, bandwidth_factor: 1, core_fraction: 0.9, x0: 0, x1: 1000, y0: 0, y1: 1 },
    stats: { next_interval_ms: refresh, refresh_ms: refresh, estimates: "4", skipped_unchanged: "0", skipped_under_load: "0", busy: false, priority_lowered: true },
    result,
  }) as NewDensityReply;
}

export const unchangedReply = (generation = "7"): MonitoringDensityReply => decodeMonitoringDensity({ valid: true, unchanged: true, generation });

export const LOOP: [number, number][] = [[100, 0.1], [300, 0.1], [300, 0.3], [100, 0.1]];

/** A `fetch_kde_reference_from_file` reply that loaded. */
export const referenceLoaded = (over: Record<string, unknown> = {}): KdeReference => decodeKdeReference({
  ok: true, path: "/data/run7.h5", file_name: "run7.h5", error: null, reason: null, source: "full-run", core_fraction: 0.9, core_percent: 90,
  loops: 1, contours: [LOOP], ...over,
});

/** … and one that did not, with the reason the bridge gives. */
export const referenceFailed = (reason: string, path = "/data/run7.h5"): KdeReference => decodeKdeReference({
  ok: false, path, file_name: path.split("/").pop(), error: `No reference contour loaded from ${path}: ${reason}`, reason,
  source: null, core_fraction: null, core_percent: null, loops: 0, contours: [],
});

/** A digest of a list of canvas calls, for "drawn exactly as before" checks: cyrb53 over its JSON, hex. (Not a
 *  security hash; the project's TypeScript has no Node types for node:crypto.) */
export function callsDigest(calls: unknown): string {
  const text = JSON.stringify(calls);
  let h1 = 0xdeadbeef, h2 = 0x41c6ce57;
  for (let i = 0; i < text.length; i += 1) {
    const ch = text.charCodeAt(i);
    h1 = Math.imul(h1 ^ ch, 2654435761);
    h2 = Math.imul(h2 ^ ch, 1597334677);
  }
  h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909);
  h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909);
  return (4294967296 * (2097151 & h2) + (h1 >>> 0)).toString(16).padStart(14, "0");
}
