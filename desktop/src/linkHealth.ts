// PZ7035 sensor link, sensor and latency readouts for the sidebar and preflight (#501). Pure
// formatting of fetch_instrument_status; the thresholds come from the backend
// (docs/YOFO_HOST_INTERFACE.md: warn above 10 errors/s and 1 resync/s; the rates are not valid
// for about 1.5 s after a mode switch, so nothing warns then).
import type { InstrumentStatus } from "./bridge";

export interface Readout { text: string; cls: "" | "dim" | "warn" }

const rate = (v: number) => (v < 10 ? v.toFixed(1) : v.toFixed(0));

/** "816×624 @ 400.1 fps", or "closed" while no XVS runs (the sensor is not open yet). */
export function sensorReadout(s: InstrumentStatus | null | undefined): Readout {
  if (!s?.available || !s.sensor) return { text: "—", cls: "dim" };
  if (!s.sensor.xvs_period_clocks) return { text: "closed", cls: "dim" };
  return { text: `${s.sensor.width}×${s.sensor.height} @ ${s.sensor.fps.toFixed(1)} fps`, cls: "" };
}

/** Errors, resyncs, bad and dropped frames per second. Warns above the backend's thresholds for
 *  errors and resyncs, and when the backend reports sustained bad or dropped frames (above 1% / 0.1% of
 *  the sensor's frame rate for 5 s: a drop in Run is data loss). A baseline of a few per second must
 *  not warn, so those two are judged relative to the frame rate, by the backend. */
export function linkReadout(s: InstrumentStatus | null | undefined): Readout {
  if (!s?.available || !s.link) return { text: "—", cls: "dim" };
  const l = s.link;
  if (!l.rates_valid) return { text: "measuring…", cls: "dim" };
  const text = `${rate(l.ingress_errors_per_s)} err/s · ${rate(l.resyncs_per_s)} resync/s · ${rate(l.bad_frames_per_s)} bad/s · ${rate(l.dropped_per_s)} drop/s`;
  const warn = !!l.ingress_errors_warn || l.resyncs_per_s > l.resyncs_warn_per_s || !!l.bad_frames_warn || !!l.dropped_warn;
  return { text, cls: warn ? "warn" : "" };
}

/** The latency monitor: max in µs and how many of the frames went over the budget. */
export function latencyReadout(s: InstrumentStatus | null | undefined): Readout {
  const l = s?.available ? s.latency : undefined;
  if (!l || l.frames <= 0) return { text: "—", cls: "dim" };
  return {
    text: `${l.max_us.toFixed(1)} µs max · ${l.over_budget} over budget of ${l.frames.toLocaleString("en-US")}`,
    cls: l.over_budget > 0 ? "warn" : "",
  };
}
