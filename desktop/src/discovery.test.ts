// Device discovery client (bridge schema v14, issue #419): polling a job to a
// terminal state, cancelling on timeout, and mapping a snapshot onto the
// camera/framegrabber shape the UI renders.
import { describe, expect, it } from "vitest";
import {
  discoverCameras,
  DISCOVERY_JOB_STATES,
  isTerminalDiscoveryState,
  pollDiscovery,
  toCameraDiscovery,
  type DiscoveryApi,
} from "./discovery";
import type { DiscoveredDevice, DiscoverySnapshot, DiscoveryStart } from "./bridge";

function device(partial: Partial<DiscoveredDevice>): DiscoveredDevice {
  return {
    kind: 0,
    provider_id: "p",
    display_name: "",
    system_path: "",
    persistent_id: "",
    sdk_index: -1,
    interface_index: -1,
    device_index: -1,
    stream_index: -1,
    bus_address: -1,
    stable_identity: "",
    identity_strength: 1,
    identification: 0,
    claimed_by: [],
    capabilities: [],
    synthetic: false,
    camera_type: -1,
    interface_id: "",
    device_id: "",
    stream_id: "",
    model_name: "",
    firmware_version: "",
    label: "",
    ...partial,
  };
}

function snapshot(partial: Partial<DiscoverySnapshot>): DiscoverySnapshot {
  return {
    valid: true,
    job_id: "7",
    generation: "7",
    instance: "instance-1",
    stale: false,
    state: DISCOVERY_JOB_STATES.Running,
    complete: false,
    overflow: false,
    attempt: 1,
    max_attempts: 1,
    kinds: [0, 1],
    candidates: [],
    errors: [],
    providers_run: [],
    origin: "test",
    ...partial,
  };
}

describe("toCameraDiscovery", () => {
  it("splits cameras and framegrabbers and keeps the legacy fields", () => {
    const snap = snapshot({
      state: DISCOVERY_JOB_STATES.Completed,
      complete: true,
      candidates: [
        device({ kind: 0, camera_type: 1, sdk_index: 2, label: "MV (index 2)", model_name: "MV" }),
        device({ kind: 1, interface_index: 0, device_index: 0, stream_index: 0, stream_id: "S0", label: "FG" }),
        device({ kind: 0, camera_type: 2, synthetic: true, label: "Mock camera (folder frame stream)" }),
        device({ kind: 2, label: "nanopositioner (ignored here)" }),
      ],
    });
    const out = toCameraDiscovery(snap);
    expect(out.valid).toBe(true);
    expect(out.cameras.map((c) => c.camera_type)).toEqual([1, 2]);
    expect(out.cameras[0].camera_index).toBe(2);
    expect(out.cameras[0].label).toBe("MV (index 2)");
    expect(out.framegrabbers).toHaveLength(1);
    expect(out.framegrabbers[0].stream_id).toBe("S0");
    expect(out.complete).toBe(true);
  });

  it("marks an incomplete or cancelled snapshot so the UI never treats it as authoritative", () => {
    const partial = toCameraDiscovery(snapshot({ state: DISCOVERY_JOB_STATES.Completed, complete: false }));
    expect(partial.complete).toBe(false);
    const cancelled = toCameraDiscovery(snapshot({ state: DISCOVERY_JOB_STATES.Cancelled }));
    expect(cancelled.valid).toBe(false);
  });
});

describe("pollDiscovery", () => {
  function api(
    states: number[],
    opts: { accepted?: boolean; restartAfter?: number } = {},
  ): DiscoveryApi & { cancelled: string[]; fetches: number; instances: string[] } {
    let i = 0;
    // The backend's current instance; `restartAfter` fetches simulate a backend restart.
    let current = "instance-1";
    const self = {
      cancelled: [] as string[],
      instances: [] as string[],
      fetches: 0,
      start: async (): Promise<DiscoveryStart> => ({
        accepted: opts.accepted ?? true,
        coalesced: false,
        job_id: "42",
        rejection: opts.accepted === false ? 1 : 0,
        reason: opts.accepted === false ? "no kinds" : "",
        instance: current,
      }),
      fetch: async (jobId: string, instance: string): Promise<DiscoverySnapshot> => {
        self.fetches += 1;
        self.instances.push(instance);
        if (opts.restartAfter !== undefined && self.fetches > opts.restartAfter) current = "instance-2";
        // The reply shape of a backend that refused another instance's job ID.
        if (instance !== current) {
          return snapshot({ valid: false, stale: true, job_id: "0", generation: "0", instance: current,
                            state: DISCOVERY_JOB_STATES.Failed });
        }
        const state = states[Math.min(i, states.length - 1)];
        i += 1;
        return snapshot({ job_id: jobId, state, instance: current });
      },
      cancel: async (jobId: string) => {
        self.cancelled.push(jobId);
        return true;
      },
      sleep: async () => {},
    };
    return self;
  }

  it("polls until the job reaches a terminal state", async () => {
    const a = api([DISCOVERY_JOB_STATES.Queued, DISCOVERY_JOB_STATES.Running, DISCOVERY_JOB_STATES.Completed]);
    const snap = await pollDiscovery(a, { timeoutMs: 1000, intervalMs: 1 });
    expect(snap.state).toBe(DISCOVERY_JOB_STATES.Completed);
    expect(a.fetches).toBe(3);
    expect(a.cancelled).toEqual([]);
  });

  it("cancels the job and rejects when the timeout elapses", async () => {
    const a = api([DISCOVERY_JOB_STATES.Running]);
    let now = 0;
    await expect(
      pollDiscovery(a, { timeoutMs: 50, intervalMs: 10, now: () => (now += 20) }),
    ).rejects.toThrow(/timed out/);
    expect(a.cancelled).toEqual(["42"]);
  });

  it("passes the start's backend instance on every fetch", async () => {
    const a = api([DISCOVERY_JOB_STATES.Running, DISCOVERY_JOB_STATES.Completed]);
    await pollDiscovery(a, { timeoutMs: 1000, intervalMs: 1 });
    expect(a.instances).toEqual(["instance-1", "instance-1"]);
  });

  it("stops on a stale reply after a backend restart instead of polling another job", async () => {
    // The restarted backend reuses job 42; its reply to the old instance is stale.
    const a = api([DISCOVERY_JOB_STATES.Running], { restartAfter: 1 });
    await expect(pollDiscovery(a, { timeoutMs: 1000, intervalMs: 1 })).rejects.toThrow(/stale/);
    expect(a.fetches).toBe(2);
    expect(a.cancelled).toEqual([]);
  });

  it("surfaces a rejected start without polling", async () => {
    const a = api([DISCOVERY_JOB_STATES.Completed], { accepted: false });
    await expect(pollDiscovery(a, { timeoutMs: 100, intervalMs: 1 })).rejects.toThrow(/no kinds/);
    expect(a.fetches).toBe(0);
  });

  it("discoverCameras returns the UI shape", async () => {
    const a = api([DISCOVERY_JOB_STATES.Completed]);
    const out = await discoverCameras(a, { timeoutMs: 100, intervalMs: 1 });
    expect(out.valid).toBe(true);
    expect(out.cameras).toEqual([]);
  });

  it("knows the terminal states", () => {
    expect(isTerminalDiscoveryState(DISCOVERY_JOB_STATES.Completed)).toBe(true);
    expect(isTerminalDiscoveryState(DISCOVERY_JOB_STATES.Cancelled)).toBe(true);
    expect(isTerminalDiscoveryState(DISCOVERY_JOB_STATES.Failed)).toBe(true);
    expect(isTerminalDiscoveryState(DISCOVERY_JOB_STATES.Running)).toBe(false);
  });
});
