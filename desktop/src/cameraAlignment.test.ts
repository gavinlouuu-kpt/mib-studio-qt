import {describe, expect, it} from "vitest";
import type {CameraGeometry} from "./bridge";
import {initialWindow, rateSummary, snapWindow} from "./cameraAlignment";

const pz: CameraGeometry = {
  supported: true, overview: true, camera: "aravis", sensor_width: 816, sensor_height: 624,
  roi: {x: 0, y: 0, width: 0, height: 0}, width_increment: 8, height_increment: 1,
  offset_x_increment: 8, offset_y_increment: 1, min_width: 8, min_height: 1,
  session: {overview: true, region: {x: 0, y: 0, width: 816, height: 624}, frame_rate_hz: 830,
    frame_rate_max_hz: 1665, frame_rate_limit: "SensorGeometry", band_count: 11,
    delivered_frame_rate_hz: 25.2, delivered_limit: "BandReadout"},
};

describe("camera alignment", () => {
  it("snaps the window to 8-pixel columns and keeps it on the sensor", () => {
    expect(snapWindow({x: 155, y: 256.4, width: 510, height: 96}, pz)).toEqual({x: 152, y: 256, width: 512, height: 96});
    expect(snapWindow({x: 700, y: 600, width: 512, height: 96}, pz)).toEqual({x: 304, y: 528, width: 512, height: 96});
    expect(snapWindow({x: -20, y: -5, width: 2000, height: 1}, pz)).toEqual({x: 0, y: 0, width: 816, height: 1});
  });

  it("starts from the saved window, else the centre at the processing size", () => {
    expect(initialWindow(pz)).toEqual({x: 152, y: 264, width: 512, height: 96});
    expect(initialWindow({...pz, roi: {x: 8, y: 10, width: 64, height: 32}})).toEqual({x: 8, y: 10, width: 64, height: 32});
  });

  it("states the sensor rate, the delivered rate and what limits each", () => {
    expect(rateSummary(pz)).toBe(
      "Sensor 830 Hz (max 1665 Hz: window height) → ≥ 25.2 images/s here (limit: band readout to the PS, 11 bands per image)");
    expect(rateSummary({...pz, session: {}})).toBe("");
  });
});
