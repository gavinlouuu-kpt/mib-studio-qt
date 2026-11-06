import { describe, expect, it } from "vitest";
import { POLL_FAILURE_THRESHOLD, POLL_HEALTHY, pollHealthAfter, pollNotice } from "./pollHealth";

describe("poll health", () => {
  it("stays quiet for a single failed tick and says nothing while polls succeed", () => {
    let h = pollHealthAfter(POLL_HEALTHY, new Error("timeout"));
    expect(h).toEqual({ failures: 1, lastError: "timeout" });
    expect(pollNotice("Monitoring data", h)).toBe("");
    h = pollHealthAfter(h);
    expect(h).toBe(POLL_HEALTHY);
    expect(pollHealthAfter(POLL_HEALTHY)).toBe(POLL_HEALTHY);
  });

  it("raises the notice with the last reason once the failures persist, and clears it on the next success", () => {
    let h = POLL_HEALTHY;
    for (let i = 0; i < POLL_FAILURE_THRESHOLD; i++) h = pollHealthAfter(h, i === POLL_FAILURE_THRESHOLD - 1 ? "backend gone" : new Error("timeout"));
    expect(pollNotice("Monitoring data", h)).toBe("Monitoring data could not be read from the backend (3 attempts in a row): backend gone");
    h = pollHealthAfter(h, new Error("still gone"));
    expect(pollNotice("Trigger status", h)).toContain("(4 attempts in a row): still gone");
    expect(pollNotice("Monitoring data", pollHealthAfter(h))).toBe("");
  });

  it("bounds the reason it keeps", () => {
    expect(pollHealthAfter(POLL_HEALTHY, "x".repeat(500)).lastError.length).toBeLessThanOrEqual(201);
  });
});
