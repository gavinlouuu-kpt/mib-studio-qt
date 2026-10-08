import { describe, expect, it } from "vitest";
import { MIB_STUDIO, productName, YOFO_STUDIO } from "./brand";

describe("product name (#550 m13)", () => {
  it("is YOFO Studio on the PZ7035 and in the remote browser UI", () => {
    expect(productName({ pz7035: true, remote: false })).toBe(YOFO_STUDIO);
    expect(productName({ pz7035: false, remote: true })).toBe(YOFO_STUDIO);
    expect(productName({ pz7035: true, remote: true })).toBe(YOFO_STUDIO);
  });
  it("stays MIB Studio on the desktop", () => {
    expect(productName({ pz7035: false, remote: false })).toBe(MIB_STUDIO);
  });
});
