import { describe, expect, it } from "vitest";
import { noImagesNotice } from "./noImages";

const base = { file_open: true, recording_file: false, total_valid: 120, total_invalid: 3 };
const none = { count: 0 };

describe("no-images state for metadata-only files (#649)", () => {
  it("explains a PL-science experiment file: rows but no image datasets", () => {
    const text = noImagesNotice({ ...base, valid_images: none, invalid_images: none, recorded_images: none });
    expect(text).toContain("no images or masks");
    expect(text).toContain("metrics table, charts and export work");
  });
  it("says nothing for a file with images, a raw recording, an empty file or no file", () => {
    expect(noImagesNotice({ ...base, valid_images: { count: 5 }, invalid_images: none })).toBeNull();
    expect(noImagesNotice({ ...base, recording_file: true, recorded_images: none })).toBeNull();
    expect(noImagesNotice({ ...base, total_valid: 0, total_invalid: 0, valid_images: none })).toBeNull();
    expect(noImagesNotice({ ...base, file_open: false })).toBeNull();
    expect(noImagesNotice(null)).toBeNull();
  });
});
