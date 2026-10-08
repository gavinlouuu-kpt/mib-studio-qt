import { describe, expect, it } from "vitest";
import type { InstrumentStatus } from "./bridge";
import { latencyReadout, linkReadout, sensorReadout } from "./linkHealth";

const status = (over: Partial<InstrumentStatus> = {}): InstrumentStatus => ({
  available: true,
  link: { rates_valid: true, ingress_errors_per_s: 0.2, resyncs_per_s: 0.1, bad_frames_per_s: 0, dropped_per_s: 0, ingress_errors_warn_per_s: 10, resyncs_warn_per_s: 1 },
  sensor: { xvs_period_clocks: 249966, fps: 400.0544, width: 816, height: 624 },
  latency: { last_us: 100, max_us: 111.0, over_budget: 0, frames: 300118 },
  ...over,
});

describe("sensor link, sensor and latency readouts (#501)", () => {
  it("shows the sensor as geometry at the actual fps, or closed while no XVS runs", () => {
    expect(sensorReadout(status())).toEqual({ text: "816×624 @ 400.1 fps", cls: "" });
    expect(sensorReadout(status({ sensor: { xvs_period_clocks: 0, fps: 0, width: 512, height: 96 } }))).toEqual({ text: "closed", cls: "dim" });
    expect(sensorReadout(null).text).toBe("—");
    expect(sensorReadout({ available: false }).text).toBe("—");
  });

  it("shows the four link rates and warns only above the backend's thresholds", () => {
    expect(linkReadout(status())).toEqual({ text: "0.2 err/s · 0.1 resync/s · 0.0 bad/s · 0.0 drop/s", cls: "" });
    const hot = status({ link: { ...status().link!, ingress_errors_per_s: 25, resyncs_per_s: 0.5 } });
    expect(linkReadout(hot)).toEqual({ text: "25 err/s · 0.5 resync/s · 0.0 bad/s · 0.0 drop/s", cls: "warn" });
    expect(linkReadout(status({ link: { ...status().link!, resyncs_per_s: 2 } })).cls).toBe("warn");
    // bad and dropped frames are shown, and warn only when the backend reports them sustained above
    // their share of the frame rate (a baseline of a few per second does not)
    expect(linkReadout(status({ link: { ...status().link!, bad_frames_per_s: 3, dropped_per_s: 0.2 } }))).toEqual({ text: "0.2 err/s · 0.1 resync/s · 3.0 bad/s · 0.2 drop/s", cls: "" });
    expect(linkReadout(status({ link: { ...status().link!, bad_frames_per_s: 8, bad_frames_warn: true } })).cls).toBe("warn");
    expect(linkReadout(status({ link: { ...status().link!, dropped_per_s: 1, dropped_warn: true } })).cls).toBe("warn");
    // inside the settle window after a mode switch the backend reports no valid rates: nothing warns
    expect(linkReadout(status({ link: { ...status().link!, rates_valid: false, ingress_errors_per_s: 500 } }))).toEqual({ text: "measuring…", cls: "dim" });
  });

  it("shows the latency max and how many frames went over the budget", () => {
    expect(latencyReadout(status())).toEqual({ text: "111.0 µs max · 0 over budget of 300,118", cls: "" });
    expect(latencyReadout(status({ latency: { last_us: 1, max_us: 150, over_budget: 3, frames: 1000 } })).cls).toBe("warn");
    expect(latencyReadout(status({ latency: { last_us: 0, max_us: 0, over_budget: 0, frames: 0 } }))).toEqual({ text: "—", cls: "dim" });
  });
});

describe("sensor link in preflight (#501)", () => {
  it("lists all four rates, the sensor geometry and fps, and warns above the thresholds", async () => {
    const { derivePreflight } = await import("./preflight");
    const { DESKTOP_CAPABILITIES } = await import("./platformCapabilities");
    const PZ7035_CAPABILITIES = { ...DESKTOP_CAPABILITIES, instrument: "pz7035" as const, pl_identity: true, led_strobe: true, align_mode: true, run_mode: true };
    const base = {
      backendReady: true, cameraConfigured: true, cameraRunning: true, cameraIdentity: "x", cameraExpected: "",
      coreValid: true, corePinSatisfied: true, coreVersion: "", requiredCoreVersion: "",
      autofocus: { valid: true, connected: false, identity: "" }, samplePump: { valid: true, connected: false, identity: "" },
      sheathPump: { valid: true, connected: false, identity: "" }, trigger: { valid: true, cameraAttached: false },
      storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
      capabilities: PZ7035_CAPABILITIES,
    } as unknown as Parameters<typeof derivePreflight>[0];
    const link = (input: InstrumentStatus) => derivePreflight({ ...base, instrument: input }).checks.find((c) => c.id === "sensorLink")!;
    const ok = link(status());
    expect(ok.status).toBe("passed");
    expect(ok.detected).toBe("0.2 err/s · 0.1 resync/s · 0.0 bad/s · 0.0 drop/s");
    expect(ok.detail).toContain("816×624 @ 400.1 fps");
    const hot = link(status({ link: { ...status().link!, resyncs_per_s: 3 } }));
    expect(hot.status).toBe("warning");
    expect(hot.detail).toContain("resyncs above 1/s");
    const loss = link(status({ link: { ...status().link!, dropped_warn: true, bad_frames_warn: true } }));
    expect(loss.status).toBe("warning");
    expect(loss.detail).toContain("dropped frames above 0.1% of the frame rate");
    expect(loss.detail).toContain("bad frames above 1% of the frame rate");
  });
});
