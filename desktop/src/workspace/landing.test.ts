import { describe, expect, it } from "vitest";
import { DEFAULT_LANDING, LANDING_STORAGE_KEY, initialWorkspaceStage, readLanding, writeLanding } from "./landing";

const memory = () => {
  const saved = new Map<string, string>();
  return { saved, getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v) };
};

describe("landing (which view the app opens in)", () => {
  it("opens in the workspace by default on this branch", () => {
    expect(DEFAULT_LANDING).toBe("workspace");
    expect(readLanding(memory())).toBe("workspace");
    expect(readLanding(null)).toBe("workspace");
    expect(initialWorkspaceStage("workspace")).toBe(0); // Prepare
    expect(initialWorkspaceStage("classic")).toBeNull();
  });

  it("remembers the last choice, and ignores a value it does not know", () => {
    const s = memory();
    writeLanding("classic", s);
    expect(s.saved.get(LANDING_STORAGE_KEY)).toBe("classic");
    expect(readLanding(s)).toBe("classic");
    writeLanding("workspace", s);
    expect(readLanding(s)).toBe("workspace");
    s.saved.set(LANDING_STORAGE_KEY, "review");
    expect(readLanding(s)).toBe("workspace");
  });

  it("storage that throws (blocked site data) falls back to the default and never throws", () => {
    const broken = { getItem: () => { throw new Error("SecurityError"); }, setItem: () => { throw new Error("QuotaExceededError"); } };
    expect(readLanding(broken)).toBe("workspace");
    expect(() => writeLanding("classic", broken)).not.toThrow();
  });
});
