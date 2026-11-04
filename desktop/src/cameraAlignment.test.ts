import {describe, expect, it} from "vitest";
import type {CameraGeometry} from "./bridge";
import {displayedWindow, dragStoredWindow, egrabberWindowLine, initialWindow, mirrorOf, NO_MIRROR, pointInDisplayedWindow, rateSummary, snapRunWindow, snapWindow, windowInputLimits} from "./cameraAlignment";

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

describe("PZ7035 Run window (#501 P1)", () => {
  it("is 512x96 with x on 8 and y on 4, inside the 816x624 sensor", () => {
    expect(snapRunWindow({x: 157, y: 203})).toEqual({x: 160, y: 204, width: 512, height: 96});
    expect(snapRunWindow({x: 151, y: 201})).toEqual({x: 152, y: 200, width: 512, height: 96});
    expect(snapRunWindow({x: 999, y: 999})).toEqual({x: 304, y: 528, width: 512, height: 96});
    expect(snapRunWindow({x: -5, y: -5})).toEqual({x: 0, y: 0, width: 512, height: 96});
  });
});

describe("eGrabber camera window (#846, Qt Overview snapping)", () => {
  const eg: CameraGeometry = {
    supported: true, overview: true, camera: "egrabber", sensor_width: 1920, sensor_height: 1080,
    roi: {x: 0, y: 0, width: 0, height: 0}, width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4,
    min_width: 64, min_height: 16, session: {}, window_source: "none",
  };
  // The same cases as tests/backend/camera_window_profile_test.cpp: the UI and the backend snap alike.
  it("snaps sizes down and offsets down, as the backend does when it saves", () => {
    expect(snapWindow({x: 371, y: 203, width: 1000, height: 130}, eg)).toEqual({x: 368, y: 200, width: 992, height: 128});
    expect(snapWindow({x: 0, y: 0, width: 10, height: 3}, eg)).toEqual({x: 0, y: 0, width: 64, height: 16});
    expect(snapWindow({x: 0, y: 0, width: 4000, height: 4000}, eg)).toEqual({x: 0, y: 0, width: 1920, height: 1080});
    expect(snapWindow({x: 719, y: 503, width: 512, height: 96}, eg)).toEqual({x: 704, y: 500, width: 512, height: 96});
    expect(snapWindow({x: 15, y: 3, width: 512, height: 96}, eg)).toEqual({x: 0, y: 0, width: 512, height: 96});
    expect(snapWindow({x: -40, y: -9, width: 512, height: 96}, eg)).toEqual({x: 0, y: 0, width: 512, height: 96});
    expect(snapWindow({x: 1900, y: 1070, width: 512, height: 96}, eg)).toEqual({x: 1408, y: 984, width: 512, height: 96});
    expect(snapWindow({x: 1000, y: 900, width: 1216, height: 208}, eg)).toEqual({x: 704, y: 872, width: 1216, height: 208});
    expect(snapWindow({x: 368, y: 616, width: 1216, height: 208}, eg)).toEqual({x: 368, y: 616, width: 1216, height: 208});
  });

  it("rounds a dragged position to a pixel and then snaps down (Qt SimpleImageCanvas), not to the nearest step", () => {
    expect(snapWindow({x: 157.6, y: 203.7, width: 512, height: 96}, eg)).toEqual({x: 144, y: 204, width: 512, height: 96});
    // the Aravis/PZ7035 snapping is unchanged: nearest step
    expect(snapWindow({x: 155, y: 256.4, width: 510, height: 96}, pz)).toEqual({x: 152, y: 256, width: 512, height: 96});
  });

  it("uses the camera's own sensor when it is known and Qt's 1920x1080 when it is not", () => {
    expect(snapWindow({x: 4000, y: 4000, width: 4000, height: 4000}, {...eg, sensor_width: 1936, sensor_height: 1216}))
      .toEqual({x: 0, y: 0, width: 1936, height: 1216});
    expect(snapWindow({x: 0, y: 0, width: 4000, height: 4000}, {...eg, sensor_width: 0, sensor_height: 0})).toEqual({x: 0, y: 0, width: 1920, height: 1080});
  });

  it("starts from the profile's or script's window, else Qt's default 512x96 at (704, 500)", () => {
    expect(initialWindow({...eg, roi: {x: 368, y: 616, width: 1216, height: 208}, window_source: "profile"})).toEqual({x: 368, y: 616, width: 1216, height: 208});
    expect(initialWindow(eg)).toEqual({x: 704, y: 500, width: 512, height: 96});
  });

  it("limits the number boxes like Qt's spin boxes, and leaves the other cameras' boxes alone", () => {
    const window = {x: 704, y: 500, width: 512, height: 96};
    expect(windowInputLimits(eg, "width", window)).toEqual({min: 64, max: 1920, step: 16});
    expect(windowInputLimits(eg, "height", window)).toEqual({min: 16, max: 1080, step: 4});
    expect(windowInputLimits(eg, "x", window)).toEqual({min: 0, max: 1408, step: 16});
    expect(windowInputLimits(eg, "y", window)).toEqual({min: 0, max: 984, step: 4});
    expect(windowInputLimits({...eg, sensor_width: 1936, sensor_height: 1216}, "width", null)).toEqual({min: 64, max: 1936, step: 16});
    expect(windowInputLimits(pz, "width", window)).toBeUndefined();
  });

  it("says where the window came from", () => {
    const roi = {x: 368, y: 616, width: 1216, height: 208};
    expect(egrabberWindowLine({...eg, roi, window_source: "profile"})).toBe("Window 1216×208 at (368, 616): the applied profile's camera.window, applied after the Experiment script.");
    expect(egrabberWindowLine({...eg, roi, window_source: "script", experiment_script_path: "C:\\MIBTest\\include\\egrabberConfig.js"}))
      .toBe("Window 1216×208 at (368, 616): from the Experiment script egrabberConfig.js.");
    expect(egrabberWindowLine(eg)).toContain("No camera window is saved yet");
  });
});

describe("MindVision mirroring (#863 A2, Qt SimpleImageCanvas::setRoiTransform)", () => {
  const mv: CameraGeometry = {...pz, camera: "mindvision", min_width: 16, min_height: 4, width_increment: 1, height_increment: 1,
    offset_x_increment: 1, offset_y_increment: 1, flip_horizontal: true, flip_vertical: false};
  const image = {width: 816, height: 624};
  const stored = {x: 100, y: 120, width: 256, height: 128};

  it("reads the mirroring from a MindVision geometry only", () => {
    expect(mirrorOf(mv)).toEqual({flipHorizontal: true, flipVertical: false});
    expect(mirrorOf({...mv, flip_horizontal: false})).toBe(NO_MIRROR);
    expect(mirrorOf({...pz, flip_horizontal: true})).toBe(NO_MIRROR); // Aravis has no profile mirroring
    expect(mirrorOf(null)).toBe(NO_MIRROR);
    expect(mirrorOf({...mv, flip_horizontal: undefined, flip_vertical: undefined})).toBe(NO_MIRROR); // a backend without the members
  });

  it("draws the stored window mirrored across the image on a flipped axis only", () => {
    expect(displayedWindow(stored, image, NO_MIRROR)).toEqual(stored);
    expect(displayedWindow(stored, image, {flipHorizontal: true, flipVertical: false})).toEqual({x: 460, y: 120, width: 256, height: 128}); // 816 - 256 - 100
    expect(displayedWindow(stored, image, {flipHorizontal: false, flipVertical: true})).toEqual({x: 100, y: 376, width: 256, height: 128}); // 624 - 128 - 120
    expect(displayedWindow(stored, image, {flipHorizontal: true, flipVertical: true})).toEqual({x: 460, y: 376, width: 256, height: 128});
  });

  it("is its own inverse, so a displayed window gives back the stored (sensor) one", () => {
    for (const mirror of [{flipHorizontal: true, flipVertical: false}, {flipHorizontal: false, flipVertical: true}, {flipHorizontal: true, flipVertical: true}]) {
      expect(displayedWindow(displayedWindow(stored, image, mirror), image, mirror)).toEqual(stored);
    }
    // a window that fills the axis does not move
    expect(displayedWindow({x: 0, y: 0, width: 816, height: 624}, image, {flipHorizontal: true, flipVertical: true})).toEqual({x: 0, y: 0, width: 816, height: 624});
  });

  it("moves the stored offset against the pointer on a mirrored axis (the box moves with the pointer on screen)", () => {
    const delta = {x: 40, y: 10};
    expect(dragStoredWindow(stored, delta, NO_MIRROR)).toEqual({...stored, x: 140, y: 130});
    expect(dragStoredWindow(stored, delta, {flipHorizontal: true, flipVertical: false})).toEqual({...stored, x: 60, y: 130});
    expect(dragStoredWindow(stored, delta, {flipHorizontal: true, flipVertical: true})).toEqual({...stored, x: 60, y: 110});
    // the displayed box really followed the pointer to the right
    const before = displayedWindow(stored, image, {flipHorizontal: true, flipVertical: false});
    const after = displayedWindow(dragStoredWindow(stored, delta, {flipHorizontal: true, flipVertical: false}), image, {flipHorizontal: true, flipVertical: false});
    expect(after.x - before.x).toBe(40);
  });

  it("hits the box where it is drawn", () => {
    const flipped = {flipHorizontal: true, flipVertical: false};
    expect(pointInDisplayedWindow({x: 150, y: 150}, stored, image, flipped)).toBe(false); // the unmirrored place
    expect(pointInDisplayedWindow({x: 500, y: 150}, stored, image, flipped)).toBe(true);
    expect(pointInDisplayedWindow({x: 150, y: 150}, stored, image, NO_MIRROR)).toBe(true);
    expect(pointInDisplayedWindow({x: 500, y: 300}, stored, image, flipped)).toBe(false); // below the box
  });

  it("limits the MindVision boxes like Qt's: the SDK minimum up to the sensor, step 1, offsets that keep the window on it", () => {
    expect(windowInputLimits(mv, "width", stored)).toEqual({min: 16, max: 816, step: 1});
    expect(windowInputLimits(mv, "height", stored)).toEqual({min: 4, max: 624, step: 1});
    expect(windowInputLimits(mv, "x", stored)).toEqual({min: 0, max: 560, step: 1});
    expect(windowInputLimits(mv, "y", stored)).toEqual({min: 0, max: 496, step: 1});
    // the sensor is not known before the first start: Qt allows up to 65535, and a minimum of at least 1
    const unknown = {...mv, sensor_width: 0, sensor_height: 0, min_width: 0, min_height: 0};
    expect(windowInputLimits(unknown, "width", null)).toEqual({min: 1, max: 65535, step: 1});
    expect(windowInputLimits(unknown, "height", null)).toEqual({min: 1, max: 65535, step: 1});
  });
});
