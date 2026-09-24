// Device discovery client (bridge schema v14, issue #419).
//
// Discovery is a backend job: start it, poll its bounded snapshot until a
// terminal state, cancel it when the caller gives up. This module is pure
// (the Tauri `invoke` calls are injected) so the polling contract is unit
// tested; bridge.ts wires it to the real commands.
import type { CameraDiscovery, DiscoveredDevice, DiscoverySnapshot, DiscoveryStart } from "./bridge";
import { DISCOVERY_DEVICE_KINDS, DISCOVERY_JOB_STATES } from "./bridgeContract";

export { DISCOVERY_JOB_STATES };

export interface DiscoveryApi {
  start: () => Promise<DiscoveryStart>;
  fetch: (jobId: string) => Promise<DiscoverySnapshot>;
  cancel: (jobId: string) => Promise<boolean>;
  sleep?: (ms: number) => Promise<void>;
}

export interface PollOptions {
  /** Give up (and cancel the job) after this long. Default 65 s: longer than the backend job deadline. */
  timeoutMs?: number;
  intervalMs?: number;
  now?: () => number;
}

export function isTerminalDiscoveryState(state: number): boolean {
  return (
    state === DISCOVERY_JOB_STATES.Completed ||
    state === DISCOVERY_JOB_STATES.Cancelled ||
    state === DISCOVERY_JOB_STATES.Failed
  );
}

const defaultSleep = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

/** Start a discovery job and poll until it ends. Rejects on a refused start
 *  or on timeout (after requesting cancellation). Never blocks the bridge. */
export async function pollDiscovery(api: DiscoveryApi, opts: PollOptions = {}): Promise<DiscoverySnapshot> {
  const timeoutMs = opts.timeoutMs ?? 65_000;
  const intervalMs = opts.intervalMs ?? 100;
  const now = opts.now ?? (() => Date.now());
  const sleep = api.sleep ?? defaultSleep;

  const start = await api.start();
  if (!start.accepted) {
    throw new Error(`discovery refused: ${start.reason || `rejection ${start.rejection}`}`);
  }
  const deadline = now() + timeoutMs;
  for (;;) {
    const snap = await api.fetch(start.job_id);
    if (snap.valid && isTerminalDiscoveryState(snap.state)) return snap;
    if (now() >= deadline) {
      await api.cancel(start.job_id).catch(() => false);
      throw new Error(`discovery job ${start.job_id} timed out after ${timeoutMs} ms`);
    }
    await sleep(intervalMs);
  }
}

function toCamera(d: DiscoveredDevice) {
  return {
    camera_type: d.camera_type,
    camera_index: d.sdk_index,
    interface_index: d.interface_index,
    device_index: d.device_index,
    interface_id: d.interface_id,
    device_id: d.device_id,
    model_name: d.model_name,
    firmware_version: d.firmware_version,
    label: d.label,
  };
}

function toFramegrabber(d: DiscoveredDevice) {
  return {
    interface_index: d.interface_index,
    device_index: d.device_index,
    stream_index: d.stream_index,
    interface_id: d.interface_id,
    device_id: d.device_id,
    stream_id: d.stream_id,
    model_name: d.model_name,
    label: d.label,
  };
}

/** Project a snapshot onto the camera/framegrabber lists the UI renders.
 *  `valid` is false for a cancelled/failed job; `complete` mirrors the
 *  backend's identity-coverage flag so partial results are never shown as
 *  authoritative. */
export function toCameraDiscovery(snap: DiscoverySnapshot): CameraDiscovery {
  const cameras = snap.candidates
    .filter((d) => d.kind === DISCOVERY_DEVICE_KINDS.Camera && d.camera_type >= 0)
    .map(toCamera);
  const framegrabbers = snap.candidates
    .filter((d) => d.kind === DISCOVERY_DEVICE_KINDS.Framegrabber)
    .map(toFramegrabber);
  return {
    valid: snap.valid && snap.state === DISCOVERY_JOB_STATES.Completed,
    complete: snap.complete,
    job_id: snap.job_id,
    cameras,
    framegrabbers,
  };
}

export async function discoverCameras(api: DiscoveryApi, opts: PollOptions = {}): Promise<CameraDiscovery> {
  return toCameraDiscovery(await pollDiscovery(api, opts));
}
