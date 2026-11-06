import { describe, expect, it } from "vitest";
import {
  algoTimeReadout, cameraRateReadouts, cameraStatusReadout, compactStatusLine, flushReadout, formatMetricSample, formatRuntime, runtimeText,
  type MetricSample,
} from "./statsReadouts";

describe("runtime (MON-100)", () => {
  it("is hh:mm:ss like Qt's formatRuntime, zero for nothing", () => {
    expect(formatRuntime(0)).toBe("00:00:00");
    expect(formatRuntime(-3)).toBe("00:00:00");
    expect(formatRuntime(null)).toBe("00:00:00");
    expect(formatRuntime(NaN)).toBe("00:00:00");
    expect(formatRuntime(59.9)).toBe("00:00:59");
    expect(formatRuntime(61)).toBe("00:01:01");
    expect(formatRuntime(3 * 3600 + 25 * 60 + 7)).toBe("03:25:07");
    expect(formatRuntime(100 * 3600)).toBe("100:00:00");
  });
  it("counts only while a run is active", () => {
    expect(runtimeText(true, 125)).toBe("00:02:05");
    expect(runtimeText(false, 125)).toBe("00:00:00");
    expect(runtimeText(true, null)).toBe("00:00:00");
  });
  it("flush status is orange while flushing and grey otherwise", () => {
    expect(flushReadout(true)).toEqual({ text: "Flushing…", cls: "warn" });
    expect(flushReadout(false)).toEqual({ text: "Idle", cls: "dim" });
  });
});

describe("Algo Time (MON-095)", () => {
  it("shows microseconds, plain while fresh", () => {
    expect(algoTimeReadout(85.04, 120)).toEqual({ text: "85.0 µs" });
    expect(algoTimeReadout(85.04, 1000)).toEqual({ text: "85.0 µs" });
    expect(algoTimeReadout(85.04, undefined)).toEqual({ text: "85.0 µs" });
  });
  it("greys a value older than a second with Qt's tooltip", () => {
    expect(algoTimeReadout(85.04, 1500.7)).toEqual({ text: "85.0 µs", cls: "dim", title: "Stale: last update 1500 ms ago" });
  });
  it("is absent when the backend reports no time", () => {
    expect(algoTimeReadout(null, null)).toBeNull();
    expect(algoTimeReadout(undefined, 5)).toBeNull();
    expect(algoTimeReadout(NaN, 5)).toBeNull();
  });
});

describe("camera group (MON-098)", () => {
  const sample = (state: MetricSample["state"], value: number | null, age_ms: number | null = null): MetricSample => ({ state, value, age_ms });
  it("writes the telemetry validity as text, whole numbers like Qt's formatMetric", () => {
    expect(formatMetricSample(sample("valid", 29.6, 10))).toBe("29"); // Qt casts to an unsigned integer: no rounding up
    expect(formatMetricSample(sample("stale", 29.6, 2500))).toBe("29 (stale 2.5 s)");
    expect(formatMetricSample(sample("unavailable", null))).toBe("n/a");
    expect(formatMetricSample(sample("unsupported", null))).toBe("unsupported");
    expect(formatMetricSample(sample("error", null))).toBe("error");
  });
  it("is Running in green or Stopped in grey", () => {
    expect(cameraStatusReadout(true)).toEqual({ text: "Running", cls: "ok" });
    expect(cameraStatusReadout(false)).toEqual({ text: "Stopped", cls: "dim" });
  });
  it("reads zero when the camera is stopped", () => {
    expect(cameraRateReadouts({ running: false, displayFps: 12, dataRateMBs: 4 })).toEqual({ fps: { text: "0.0" }, dataRate: { text: "0.0 MB/s" } });
    expect(cameraRateReadouts({ running: false, displayFps: 12, dataRateMBs: 4, captureFps: sample("valid", 500, 1) }).fps.text).toBe("0.0");
  });
  it("uses the backend's capture sample when there is one, and says what stands in when there is not", () => {
    const own = cameraRateReadouts({
      running: true, displayFps: 12, dataRateMBs: 4, captureFps: sample("valid", 500, 100), captureMBs: sample("stale", 31, 3000),
    });
    expect(own).toEqual({ fps: { text: "500" }, dataRate: { text: "31 (stale 3.0 s) MB/s" } });
    const measured = cameraRateReadouts({ running: true, displayFps: 12.34, dataRateMBs: 4.56 });
    expect(measured.fps.text).toBe("12.3");
    expect(measured.fps.title).toContain("sent no capture rate");
    expect(measured.dataRate.text).toBe("4.6 MB/s");
    expect(cameraRateReadouts({ running: true, displayFps: 1, dataRateMBs: 1, captureFps: null, captureMBs: null }).fps.title).toContain("sent no capture rate");
  });
  it("writes an unsupported or unavailable capture sample instead of a number", () => {
    const r = cameraRateReadouts({ running: true, displayFps: 12, dataRateMBs: 4, captureFps: sample("unsupported", null), captureMBs: sample("unavailable", null) });
    expect(r.fps.text).toBe("unsupported");
    expect(r.dataRate.text).toBe("n/a MB/s");
    expect(r.fps.title).toBeUndefined();
  });
  it("says when the telemetry came but could not be read, instead of passing the stand-in off as the camera's", () => {
    const r = cameraRateReadouts({ running: true, displayFps: 12, dataRateMBs: 4, telemetryError: "camera.frame_rate.state is not a contract metric state" });
    expect(r.fps.text).toBe("12.0");
    expect(r.fps.title).toContain("could not be read (camera.frame_rate.state is not a contract metric state)");
  });
});

describe("the one-line status bar text (MON-104)", () => {
  const base = { running: true, cameraFps: 29.6, hostProcessing: true, validFps: 1.234, invalidFps: 3.456, algoFps: 120, runActive: false, runSeconds: null, buffered: 0n, flushing: false };
  it("lists camera, valid and invalid rates and the algorithm time", () => {
    expect(compactStatusLine({ ...base, algoAvgUs: 84.7 })).toBe("Camera 30 fps · Valid 1.2/s · Invalid 3.5/s · Algo 85 µs");
  });
  it("falls back to the algorithm rate while the backend gives no time", () => {
    expect(compactStatusLine(base)).toBe("Camera 30 fps · Valid 1.2/s · Invalid 3.5/s · Algo 120/s");
  });
  it("adds the run time and the buffered count while a run is active, and (flushing)", () => {
    expect(compactStatusLine({ ...base, algoAvgUs: 85, runActive: true, runSeconds: 12.4, buffered: 40n })).toBe("Camera 30 fps · Valid 1.2/s · Invalid 3.5/s · Algo 85 µs · Run 12 s · buffered 40");
    expect(compactStatusLine({ ...base, algoAvgUs: 85, runActive: true, runSeconds: 12.4, buffered: 40n, flushing: true })).toMatch(/buffered 40 \(flushing\)$/);
  });
  it("says Camera stopped, and names the PL where the host does no processing", () => {
    expect(compactStatusLine({ ...base, running: false, hostProcessing: false })).toBe("Camera stopped · Science on PL");
  });
});
