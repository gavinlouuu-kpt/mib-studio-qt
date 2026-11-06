import { describe, expect, it } from "vitest";
import {
  DEFAULT_REGEN_CONFIG,
  REGEN_CONFIG_FIELDS,
  backgroundLabel,
  coerceConfig,
  coerceField,
  configFromImageProcessing,
  coreMismatchText,
  frameLabel,
  imageProcessingOverride,
  joinDir,
  previewWindow,
  remaskedDefault,
  roiError,
  roiFromDrag,
  roiLabel,
  toOdd,
} from "./regenerateForm";

const field = (key: string) => REGEN_CONFIG_FIELDS.find((f) => f.key === key)!;

describe("Processing Config (Qt BatchMaskDialog, REVIEW-061)", () => {
  it("has Qt's eight fields with its ranges", () => {
    expect(REGEN_CONFIG_FIELDS.map((f) => [f.label, f.min, f.max])).toEqual([
      ["Blur kernel", 1, 99],
      ["BG threshold", 0, 255],
      ["Morph kernel", 1, 99],
      ["Morph iters", 1, 20],
      ["Area min", 0, 10000],
      ["Area max", 0, 10000],
      ["Deform min", 0, 1],
      ["Deform max", 0, 1],
    ]);
  });
  it("coerces kernels odd and values into range, as the spin boxes do", () => {
    expect(toOdd(4)).toBe(5);
    expect(toOdd(5)).toBe(5);
    expect(coerceField(field("gaussian_blur_size"), 4, 3)).toBe(5);
    expect(coerceField(field("gaussian_blur_size"), 100, 3)).toBe(99);
    expect(coerceField(field("morph_kernel_size"), 0, 3)).toBe(1);
    expect(coerceField(field("bg_subtract_threshold"), 300, 8)).toBe(255);
    expect(coerceField(field("morph_iterations"), 2.6, 1)).toBe(3);
    expect(coerceField(field("deformability_threshold_max"), 0.123456, 1)).toBe(0.1235);
    expect(coerceField(field("area_threshold_min"), Number.NaN, 60)).toBe(60);
    expect(coerceConfig({ ...DEFAULT_REGEN_CONFIG, gaussian_blur_size: 6, morph_kernel_size: 2 })).toMatchObject({ gaussian_blur_size: 7, morph_kernel_size: 3 });
  });
  it("reads the eight fields from an image_processing object and sends only them", () => {
    const live = { gaussian_blur_size: 5, bg_subtract_threshold: 12, area_threshold_min: 30, ring_ratio_min: 2, filters: { enable_border_check: true } };
    const config = configFromImageProcessing(live);
    expect(config).toEqual({ ...DEFAULT_REGEN_CONFIG, gaussian_blur_size: 5, bg_subtract_threshold: 12, area_threshold_min: 30 });
    expect(configFromImageProcessing({ gaussian_blur_size: "x" }).gaussian_blur_size).toBe(DEFAULT_REGEN_CONFIG.gaussian_blur_size);
    expect(configFromImageProcessing(null)).toEqual(DEFAULT_REGEN_CONFIG);
    expect(Object.keys(imageProcessingOverride(config)).length).toBe(8);
    expect(imageProcessingOverride(config)).not.toHaveProperty("ring_ratio_min");
  });
});

describe("Preview & ROI (REVIEW-059)", () => {
  it("draws an ROI from a drag in either direction, clamped to the frame", () => {
    expect(roiFromDrag([10, 20], [4, 5], 32, 24)).toEqual({ x: 4, y: 5, w: 6, h: 15 });
    expect(roiFromDrag([-5, -5], [50, 50], 32, 24)).toEqual({ x: 0, y: 0, w: 32, h: 24 });
    expect(roiFromDrag([3, 3], [3, 9], 32, 24)).toBeNull();
  });
  it("checks an ROI against the frame", () => {
    expect(roiError(null)).toBe("");
    expect(roiError({ x: 0, y: 0, w: 0, h: 0 }, 32, 24)).toBe("");
    expect(roiError({ x: 2, y: 2, w: 28, h: 20 }, 32, 24)).toBe("");
    expect(roiError({ x: 5, y: 2, w: 28, h: 20 }, 32, 24)).toBe("The ROI reaches past the 32 × 24 frame");
    expect(roiError({ x: -1, y: 0, w: 2, h: 2 })).toBe("ROI values must be whole numbers, 0 or more");
    expect(roiLabel(null)).toBe("ROI: full frame");
    expect(roiLabel({ x: 1, y: 2, w: 3, h: 4 })).toBe("ROI 1, 2, 3 × 4");
  });
  it("steps through the whole set or Start/Count clamped to it (Qt getSourceFrameCount)", () => {
    expect(previewWindow(300, true, 50, 10)).toEqual({ offset: 0, total: 300 });
    expect(previewWindow(300, false, 50, 100)).toEqual({ offset: 50, total: 100 });
    expect(previewWindow(300, false, 250, 100)).toEqual({ offset: 250, total: 50 });
    expect(previewWindow(300, false, 250, 0)).toEqual({ offset: 250, total: 50 });
    expect(previewWindow(300, false, 300, 10)).toEqual({ offset: 300, total: 0 });
    expect(frameLabel(0, 300)).toBe("Frame 1 / 300");
    expect(frameLabel(4, null)).toBe("Frame 5");
    expect(frameLabel(0, 0)).toBe("No frames");
  });
});

describe("background, output and core (REVIEW-060, -062, -063)", () => {
  it("labels the background as Qt does", () => {
    expect(backgroundLabel({ kind: "frame", index: 40, dataset: "/valid_frames/images", label: 3 }, true)).toBe("Background: Frame 3");
    expect(backgroundLabel({ kind: "none" }, false)).toBe("Background: none");
    expect(backgroundLabel({ kind: "none" }, true)).toBe("Background: none (a synthetic one is built)");
    expect(backgroundLabel({ kind: "recorded" }, true)).toBe("Background: the file's saved background");
  });
  it("proposes <base>_remasked.h5 beside the source, inside a folder (Qt computeAutoOutputPath)", () => {
    expect(remaskedDefault("hdf", "/data/run_7.h5")).toEqual({ dir: "/data", base: "run_7" });
    expect(remaskedDefault("avi", "C:\\clips\\a.avi")).toEqual({ dir: "C:\\clips", base: "a" });
    expect(remaskedDefault("folder", "/data/frames/")).toEqual({ dir: "/data/frames", base: "frames" });
    expect(remaskedDefault("hdf", "")).toEqual({ dir: "", base: "masks" });
    expect(joinDir("/data", "run_remasked.h5")).toBe("/data/run_remasked.h5");
    expect(joinDir("C:\\clips", "a_remasked.h5")).toBe("C:\\clips\\a_remasked.h5");
  });
  it("words the core mismatch as Qt", () => {
    expect(coreMismatchText({ version: "1.0.0", contract: 2 }, { version: "1.1.0", contract: 3 })).toBe(
      "This file was recorded with processing core 1.0.0 (contract 2), but regeneration will use the currently active core 1.1.0 (contract 3).\n\n" +
        "The application will not switch cores automatically; the regenerated output records the active identity.",
    );
    expect(coreMismatchText({ version: "1.0.0" }, { version: "1.1.0", contract: 3 })).toContain("(contract unknown)");
  });
});
