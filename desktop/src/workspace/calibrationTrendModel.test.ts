import { describe, expect, it } from "vitest";
import { calRecord } from "./calibrationFixtures";
import { calibrationTrend, latestTrendRecords, trendSetupKey } from "./calibrationTrendModel";

describe("recorded calibration history, no science in the shell", () => {
  it("orders measurements by derivation, not later apply time, and preserves failed records", () => {
    const later = calRecord("later", "insufficient", { created_at_utc: "2026-10-22T00:00:00Z" });
    const earlier = calRecord("earlier", "applied", { created_at_utc: "2026-10-20T00:00:00Z", status_at_utc: "2026-10-23T00:00:00Z" });
    const before = JSON.stringify([later, earlier]);
    const points = calibrationTrend([later, earlier], "diameter_cv");
    expect(points.map(p => p.record.record_id)).toEqual(["earlier", "later"]);
    expect(points[1]).toMatchObject({ value: 14.2, limit: 10, passed: false });
    expect(JSON.stringify([later, earlier])).toBe(before);
  });
  it("uses the backend result and recorded verdict even if a client would infer something else", () => {
    const r = calRecord("a", "proposed");
    r.gates = [{ id: "max_cv_percent", value: 0, limit: 1, pass: true, reason: "backend recorded reason" }];
    expect(calibrationTrend([r], "diameter_cv")[0]).toMatchObject({ value: 3.1, limit: 1, passed: true, gateReason: "backend recorded reason" });
  });
  it("does not replace a missing metric with the gate's fallback zero", () => {
    const r = calRecord("a", "insufficient"); r.results.cv_diameter_percent = null;
    r.gates = [{ id: "max_cv_percent", value: 0, limit: 10, pass: false, reason: "no evidence" }];
    expect(calibrationTrend([r], "diameter_cv")[0]).toMatchObject({ value: null, unavailable: "Not recorded", passed: false });
  });
  it("keeps legitimate zero, null, NaN and invalid dates distinct", () => {
    const a = calRecord("a", "proposed"); a.results.cv_diameter_percent = 0;
    const b = calRecord("b", "proposed", { created_at_utc: "invalid" }); b.results.cv_diameter_percent = NaN;
    const points = calibrationTrend([b, a], "diameter_cv");
    expect(points[0].value).toBe(0);
    expect(points[1]).toMatchObject({ value: null, timestamp: null });
  });
  it("does not fabricate focus or flow, and preserves their backend missing reasons", () => {
    const r = calRecord("a", "proposed");
    r.results.focus = { status: "not recorded", median: 900 };
    r.results.event_rate = { status: "too short", cv_percent: 2 };
    expect(calibrationTrend([r], "focus")[0]).toMatchObject({ value: null, unavailable: "not recorded" });
    expect(calibrationTrend([r], "flow")[0]).toMatchObject({ value: null, unavailable: "too short" });
    r.results.focus = { status: "recorded", median: 0 };
    r.results.event_rate = { status: "computed", cv_percent: 2 };
    expect(calibrationTrend([r], "focus")[0].value).toBe(0);
    expect(calibrationTrend([r], "flow")[0].value).toBe(2);
  });
  it("keeps the latest record version without counting an apply as another measurement", () => {
    const a = calRecord("a", "proposed", { version: 1, sequence: 1 });
    const b = calRecord("a", "applied", { version: 2, sequence: 2 });
    expect(latestTrendRecords([b, a])).toEqual([b]);
    expect(calibrationTrend([a, b], "objects")).toHaveLength(1);
  });
  it("does not group unknown identities, different methods, or different method revisions as compatible", () => {
    const a = calRecord("a", "proposed"), b = calRecord("b", "proposed");
    expect(trendSetupKey(a)).not.toBe(trendSetupKey(b));
    a.identity = { ...a.identity, chip: "chip-1", objective: "20x" }; b.identity = { ...a.identity };
    expect(trendSetupKey(a)).toBe(trendSetupKey(b));
    b.method = { ...b.method, revision_id: "r4" };
    expect(trendSetupKey(a)).not.toBe(trendSetupKey(b));
  });
  it("retains each record's own limit; never recalibrates history from the last point", () => {
    const a = calRecord("a", "proposed"), b = calRecord("b", "proposed");
    b.gates = [{ id: "max_cv_percent", value: 3.1, limit: 5, pass: true, reason: null }];
    expect(calibrationTrend([a, b], "diameter_cv").map(p => p.limit)).toEqual([10, 5]);
  });
});
