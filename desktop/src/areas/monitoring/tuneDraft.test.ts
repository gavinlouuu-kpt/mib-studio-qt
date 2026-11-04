import { describe, expect, it } from "vitest";
import { EXPERIMENT_STATES } from "../../bridgeContract";
import {
  RUN_LOCKED_FIELDS, TUNE_FIELDS, documentDisagreements, formatTuneValue, initialTuneState, normalizeTuneValue, readTuneValues,
  tuneApplyBlocker, tuneCanApply, tuneCanRevert, tuneDifferences, tuneIssues, tunePatch, tunePatchSatisfiedBy, tuneReducer,
  tuneScope, tuneSpec, tuneStateText, tuneValidationList, tuneValuesEqual,
  type TuneAction, type TuneFieldId, type TuneState, type TuneValues,
} from "./tuneDraft";

/** The image_processing section the backend serializes (ProcessingConfigJson.cpp toJson), defaults of ProcessingConfig. */
const section = () => ({
  gaussian_blur_size: 3, area_threshold_min: 60, area_threshold_max: 290, deformability_threshold_min: 0, deformability_threshold_max: 1,
  area_ratio_threshold_max: 1.5, ring_ratio_min: 15, ring_ratio_max: 25,
  filters: { enable_border_check: true, enable_area_range_check: true, enable_deformability_range_check: false, enable_area_ratio_check: false,
    enable_ring_ratio_check: true, enable_laplacian_variance_check: false, require_single_inner_contour: true },
  target_group: { enabled: false, area_min: 72, area_max: 191, deformability_min: 0, deformability_max: 0.3, emodulus_enabled: false, emodulus_min: 0, emodulus_max: 10 },
  multi_image: { enabled: false, count: 1 },
});
const values = (): TuneValues => readTuneValues(section())!;
const run = (s: TuneState, ...actions: TuneAction[]) => actions.reduce(tuneReducer, s);
const loaded = (v: TuneValues = values()) => run(initialTuneState(), { type: "external", values: v });
const edit = (field: TuneFieldId, value: unknown): TuneAction => ({ type: "edit", field, value });
const withValue = (field: TuneFieldId, value: number | boolean): TuneValues => ({ ...values(), [field]: value });
const idle = tuneScope(EXPERIMENT_STATES.Idle, true);
const running = tuneScope(EXPERIMENT_STATES.Active, true);

describe("the field table", () => {
  it("has the Qt panel's 23 fields in presentation order, each once", () => {
    expect(TUNE_FIELDS.map((s) => s.id)).toEqual([
      "areaEnabled", "areaMin", "areaMax", "deformabilityEnabled", "deformabilityMin", "deformabilityMax", "ringRatioEnabled", "ringRatioMin",
      "ringRatioMax", "areaRatioEnabled", "areaRatioMax", "borderEnabled", "singleInnerContourEnabled", "targetGroupEnabled", "targetAreaMin",
      "targetAreaMax", "targetDeformabilityMin", "targetDeformabilityMax", "targetEmodulusEnabled", "targetEmodulusMin", "targetEmodulusMax",
      "multiImageEnabled", "multiImageCount",
    ]);
  });

  it("uses the backend's JSON key paths (ProcessingConfigJson.cpp:40-95)", () => {
    expect(Object.fromEntries(TUNE_FIELDS.map((s) => [s.id, s.path.join(".")]))).toEqual({
      areaEnabled: "filters.enable_area_range_check", areaMin: "area_threshold_min", areaMax: "area_threshold_max",
      deformabilityEnabled: "filters.enable_deformability_range_check", deformabilityMin: "deformability_threshold_min",
      deformabilityMax: "deformability_threshold_max", ringRatioEnabled: "filters.enable_ring_ratio_check", ringRatioMin: "ring_ratio_min",
      ringRatioMax: "ring_ratio_max", areaRatioEnabled: "filters.enable_area_ratio_check", areaRatioMax: "area_ratio_threshold_max",
      borderEnabled: "filters.enable_border_check", singleInnerContourEnabled: "filters.require_single_inner_contour",
      targetGroupEnabled: "target_group.enabled", targetAreaMin: "target_group.area_min", targetAreaMax: "target_group.area_max",
      targetDeformabilityMin: "target_group.deformability_min", targetDeformabilityMax: "target_group.deformability_max",
      targetEmodulusEnabled: "target_group.emodulus_enabled", targetEmodulusMin: "target_group.emodulus_min",
      targetEmodulusMax: "target_group.emodulus_max", multiImageEnabled: "multi_image.enabled", multiImageCount: "multi_image.count",
    });
  });

  it("has the Qt ranges, steps, decimals and units (ExperimentMonitoringTab.cpp:741-826)", () => {
    const row = (id: TuneFieldId) => { const s = tuneSpec(id); return [s.kind, s.min, s.max, s.step, s.decimals, s.unit]; };
    for (const id of ["areaMin", "areaMax", "targetAreaMin", "targetAreaMax"] as const) expect(row(id)).toEqual(["int", 0, 100000, 10, 0, " µm²"]);
    for (const id of ["deformabilityMin", "deformabilityMax", "targetDeformabilityMin", "targetDeformabilityMax"] as const) expect(row(id)).toEqual(["float", 0, 1, 0.01, 2, ""]);
    for (const id of ["ringRatioMin", "ringRatioMax"] as const) expect(row(id)).toEqual(["float", 0, 100, 0.5, 1, ""]);
    expect(row("areaRatioMax")).toEqual(["float", 0, 10, 0.1, 2, ""]);
    for (const id of ["targetEmodulusMin", "targetEmodulusMax"] as const) expect(row(id)).toEqual(["float", 0, 1000, 0.1, 2, " kPa"]);
    expect(row("multiImageCount")).toEqual(["int", 1, 32, 1, 0, ""]);
    expect(TUNE_FIELDS.filter((s) => s.kind === "bool")).toHaveLength(9);
  });

  it("locks exactly the multi-image fields for a run", () => {
    expect([...RUN_LOCKED_FIELDS]).toEqual(["multiImageEnabled", "multiImageCount"]);
  });
});

describe("values", () => {
  it("reads the exposed fields from the backend's section and refuses an incomplete or mistyped one", () => {
    const v = values();
    expect(v.areaMin).toBe(60);
    expect(v.targetEmodulusMax).toBe(10);
    expect(v.borderEnabled).toBe(true);
    expect(v.multiImageCount).toBe(1);
    expect(readTuneValues(undefined)).toBeNull();
    const missing = section(); delete (missing.filters as Partial<typeof missing.filters>).enable_border_check;
    expect(readTuneValues(missing)).toBeNull();
    expect(readTuneValues({ ...section(), area_threshold_min: "60" })).toBeNull();
    expect(readTuneValues({ ...section(), multi_image: { enabled: "no", count: 1 } })).toBeNull();
  });

  it("rounds like a spin box and clamps to its range", () => {
    const n = (id: TuneFieldId, raw: unknown) => normalizeTuneValue(tuneSpec(id), raw);
    expect(n("areaMin", 12.6)).toBe(13);
    expect(n("areaMin", -5)).toBe(0);
    expect(n("areaMax", 100001)).toBe(100000);
    expect(n("areaMax", "250")).toBe(250);
    expect(n("deformabilityMax", 1.234)).toBe(1);
    expect(n("deformabilityMin", 0.126)).toBe(0.13);
    expect(n("ringRatioMax", 25.04)).toBe(25);
    expect(n("ringRatioMax", 101)).toBe(100);
    expect(n("areaRatioMax", 10.5)).toBe(10);
    expect(n("targetEmodulusMax", 1000.5)).toBe(1000);
    expect(n("multiImageCount", 0)).toBe(1);
    expect(n("multiImageCount", 33)).toBe(32);
    expect(n("areaMin", NaN)).toBeUndefined();
    expect(n("areaMin", "")).toBeUndefined();
    expect(n("areaMin", "abc")).toBeUndefined();
    expect(n("areaMin", Infinity)).toBeUndefined();
    expect(n("borderEnabled", true)).toBe(true);
    expect(n("borderEnabled", 1)).toBeUndefined();
  });

  it("compares at display precision (re-typing the displayed value is not an edit)", () => {
    const d = tuneSpec("deformabilityMin");
    expect(tuneValuesEqual(d, 0.123456, 0.12)).toBe(true);
    expect(tuneValuesEqual(d, 0.125, 0.12)).toBe(false);
    expect(tuneValuesEqual(tuneSpec("areaMin"), 60.2, 60)).toBe(true);
    expect(tuneValuesEqual(tuneSpec("borderEnabled"), true, true)).toBe(true);
    expect(tuneValuesEqual(tuneSpec("borderEnabled"), true, false)).toBe(false);
    expect(formatTuneValue(d, 0.5)).toBe("0.50");
    expect(formatTuneValue(tuneSpec("areaMin"), 60)).toBe("60");
    expect(formatTuneValue(tuneSpec("ringRatioMin"), 15)).toBe("15.0");
  });
});

describe("validation (Qt validateTuneConfig)", () => {
  it("checks a range only while its criterion is enabled", () => {
    expect(tuneIssues(values())).toEqual([]);
    expect(tuneIssues({ ...values(), areaMin: 500 })).toEqual(["Area: minimum (500 µm²) exceeds maximum (290 µm²)"]);
    expect(tuneIssues({ ...values(), areaMin: 500, areaEnabled: false })).toEqual([]);
    expect(tuneIssues({ ...values(), deformabilityEnabled: true, deformabilityMin: 0.6, deformabilityMax: 0.5 })).toEqual(["Deformability: minimum exceeds maximum"]);
    expect(tuneIssues({ ...values(), ringRatioMin: 30 })).toEqual(["Ring ratio: minimum exceeds maximum"]);
  });

  it("checks the target group, and its Young's modulus range only while that is on too", () => {
    const base = { ...values(), targetAreaMin: 200, targetAreaMax: 100, targetDeformabilityMin: 0.5, targetDeformabilityMax: 0.2, targetEmodulusMin: 9, targetEmodulusMax: 3 };
    expect(tuneIssues(base)).toEqual([]);
    expect(tuneIssues({ ...base, targetGroupEnabled: true })).toEqual(["Target group: area minimum exceeds maximum", "Target group: deformability minimum exceeds maximum"]);
    expect(tuneIssues({ ...base, targetGroupEnabled: true, targetEmodulusEnabled: true })).toContain("Target group: Young's modulus minimum exceeds maximum");
    expect(tuneIssues({ ...base, targetEmodulusEnabled: true })).toEqual([]);
  });
});

describe("the draft", () => {
  it("is empty until the running configuration is read, and then Applied", () => {
    const s = initialTuneState();
    expect(s.draft).toBeNull();
    expect(tuneStateText(s)).toBe("Waiting for the running configuration…");
    expect(tuneCanApply(s, idle)).toBe(false);
    expect(run(s, edit("areaMin", 80))).toBe(s);
    const l = loaded();
    expect(l.draft).toEqual(values());
    expect(tuneStateText(l)).toBe("Applied");
    expect(tuneCanApply(l, idle)).toBe(false);
    expect(tuneCanRevert(l)).toBe(false);
  });

  it("marks only the fields the operator changed, and counts them", () => {
    let s = run(loaded(), edit("areaMin", 80));
    expect(s.changed).toEqual(["areaMin"]);
    expect(s.draft!.areaMin).toBe(80);
    expect(tuneStateText(s)).toBe("1 unapplied change");
    s = run(s, edit("borderEnabled", false), edit("ringRatioMax", 30));
    expect(s.changed).toEqual(["areaMin", "borderEnabled", "ringRatioMax"]);
    expect(tuneStateText(s)).toBe("3 unapplied changes");
    expect(tuneCanApply(s, idle)).toBe(true);
    expect(tuneCanRevert(s)).toBe(true);
  });

  it("unmarks a field edited back to its applied value and restores the exact value, never a rounded copy", () => {
    const precise = withValue("deformabilityMin", 0.12345678);
    let s = run(loaded(precise), edit("deformabilityMin", 0.12));
    expect(s.changed).toEqual([]); // 0.12 is what the control shows for 0.12345678
    expect(s.draft!.deformabilityMin).toBe(0.12345678);
    s = run(s, edit("deformabilityMin", 0.2));
    expect(s.changed).toEqual(["deformabilityMin"]);
    expect(s.draft!.deformabilityMin).toBe(0.2);
    s = run(s, edit("deformabilityMin", 0.12));
    expect(s.changed).toEqual([]);
    expect(s.draft!.deformabilityMin).toBe(0.12345678);
    expect(tunePatch(s)).toEqual({});
  });

  it("normalises what it is given and ignores what is not a number", () => {
    const s = run(loaded(), edit("areaMax", 250000), edit("areaMin", "abc"), edit("multiImageCount", 0));
    expect(s.draft!.areaMax).toBe(100000);
    expect(s.draft!.areaMin).toBe(60);
    expect(s.draft!.multiImageCount).toBe(1);
    expect(s.changed).toEqual(["areaMax"]);
  });

  it("builds a patch of the changed fields only, in the backend's nested layout", () => {
    const s = run(loaded(), edit("areaMin", 80), edit("borderEnabled", false), edit("targetGroupEnabled", true), edit("targetEmodulusMax", 8.5), edit("multiImageCount", 4));
    expect(tunePatch(s)).toEqual({
      area_threshold_min: 80,
      filters: { enable_border_check: false },
      target_group: { enabled: true, emodulus_max: 8.5 },
      multi_image: { count: 4 },
    });
  });

  it("shows the first issue as Invalid, lists them all, and does not allow Apply", () => {
    const s = run(loaded(), edit("areaMin", 400), edit("ringRatioMin", 40));
    expect(tuneStateText(s)).toBe("Invalid: Area: minimum (400 µm²) exceeds maximum (290 µm²)");
    expect(tuneValidationList(s)).toEqual(["Area: minimum (400 µm²) exceeds maximum (290 µm²)", "Ring ratio: minimum exceeds maximum"]);
    expect(tuneCanApply(s, idle)).toBe(false);
    expect(tuneCanRevert(s)).toBe(true);
    // A clean draft lists nothing even if the running configuration were inconsistent.
    expect(tuneValidationList(loaded({ ...values(), areaMin: 400 }))).toEqual([]);
  });

  it("goes back to the running configuration on Revert", () => {
    const s = run(loaded(), edit("areaMin", 80), { type: "revert" });
    expect(s.changed).toEqual([]);
    expect(s.draft).toEqual(values());
    expect(tuneStateText(s)).toBe("Applied");
  });

  it("keeps a switched-off criterion's values (they stay editable data, the view only disables them)", () => {
    const s = run(loaded(), edit("areaMin", 80), edit("areaEnabled", false));
    expect(s.draft!.areaMin).toBe(80);
    expect(s.draft!.areaEnabled).toBe(false);
    expect(s.changed).toEqual(["areaMin", "areaEnabled"]);
  });
});

describe("a newer configuration arriving (Qt noteExternalBaseline)", () => {
  it("is adopted silently while the draft is clean, and changes nothing when it says the same", () => {
    const l = loaded();
    expect(run(l, { type: "external", values: values() })).toBe(l);
    const s = run(l, { type: "external", values: withValue("areaMin", 70) });
    expect(s.draft!.areaMin).toBe(70);
    expect(s.baseline!.areaMin).toBe(70);
    expect(s.conflict).toBe(false);
    expect(s.changed).toEqual([]);
  });

  it("keeps editing against the same values when nothing exposed differs", () => {
    const edited = run(loaded(), edit("areaMin", 80));
    expect(run(edited, { type: "external", values: values() })).toBe(edited);
  });

  it("is a Conflict when an exposed field differs under a dirty draft: draft kept, Apply blocked, until Revert", () => {
    const edited = run(loaded(), edit("areaMin", 80));
    const s = run(edited, { type: "external", values: withValue("ringRatioMax", 40) });
    expect(s.conflict).toBe(true);
    expect(s.draft!.areaMin).toBe(80);
    expect(s.changed).toEqual(["areaMin"]);
    expect(tuneStateText(s)).toMatch(/^Conflict: the configuration changed elsewhere\. Revert to load it/);
    expect(tuneCanApply(s, idle)).toBe(false);
    expect(tuneCanRevert(s)).toBe(true);
    // The same newer set again changes nothing (the panel polls).
    expect(run(s, { type: "external", values: withValue("ringRatioMax", 40) })).toBe(s);
    const reverted = run(s, { type: "revert" });
    expect(reverted.conflict).toBe(false);
    expect(reverted.draft!.ringRatioMax).toBe(40);
    expect(reverted.draft!.areaMin).toBe(60);
    expect(reverted.changed).toEqual([]);
  });

  it("is Conflict even when the draft was edited back to clean after one (until Revert)", () => {
    const edited = run(loaded(), edit("areaMin", 80));
    const conflicted = run(edited, { type: "external", values: withValue("ringRatioMax", 40) }, edit("areaMin", 60));
    expect(conflicted.changed).toEqual([]);
    expect(conflicted.conflict).toBe(true);
    expect(tuneCanRevert(conflicted)).toBe(true);
    const next = run(conflicted, { type: "external", values: withValue("ringRatioMax", 41) });
    expect(next.conflict).toBe(false); // a clean draft refreshes silently and clears the conflict
    expect(next.draft!.ringRatioMax).toBe(41);
  });
});

describe("Apply", () => {
  const dirty = () => run(loaded(), edit("areaMin", 80), edit("borderEnabled", false));
  const begin = (s: TuneState) => run(s, { type: "begin", requestId: s.nextRequest });

  it("holds the draft read-only while in flight and ignores a result for another request", () => {
    const s = begin(dirty());
    expect(s.applying).toBe(true);
    expect(s.activeRequest).toBe(1);
    expect(s.nextRequest).toBe(2);
    expect(tuneStateText(s)).toBe("Applying…");
    expect(tuneCanApply(s, idle)).toBe(false);
    expect(tuneCanRevert(s)).toBe(false);
    expect(run(s, edit("areaMin", 90))).toBe(s);
    expect(run(s, { type: "revert" })).toBe(s);
    expect(run(s, { type: "result", requestId: 99, outcome: { kind: "failed", error: "late" } })).toBe(s);
    expect(run(s, { type: "begin", requestId: 5 })).toBe(s);
  });

  it("adopts the effective values and clears the draft when confirmed, remembering where it went", () => {
    const s = begin(dirty());
    const effective = { ...values(), areaMin: 80, borderEnabled: false };
    const done = run(s, { type: "result", requestId: 1, outcome: { kind: "applied", effective, target: { kind: "file", path: "/c/config.json" } } });
    expect(done.applying).toBe(false);
    expect(done.changed).toEqual([]);
    expect(done.baseline).toEqual(effective);
    expect(done.draft).toEqual(effective);
    expect(done.target).toEqual({ kind: "file", path: "/c/config.json" });
    expect(tuneStateText(done)).toBe("Applied");
  });

  it("keeps the draft and says why when it did not apply", () => {
    const failed = run(begin(dirty()), { type: "result", requestId: 1, outcome: { kind: "failed", error: "Processing settings are locked during an experiment" } });
    expect(failed.applying).toBe(false);
    expect(failed.changed).toEqual(["areaMin", "borderEnabled"]);
    expect(tuneStateText(failed)).toBe("Not applied: Processing settings are locked during an experiment");
    expect(tuneCanApply(failed, idle)).toBe(true); // the operator may retry
    // The next edit clears the message.
    expect(tuneStateText(run(failed, edit("areaMin", 90)))).toBe("2 unapplied changes");
  });

  it("says Saved, not applied when only the file took it", () => {
    const failed = run(begin(dirty()), { type: "result", requestId: 1, outcome: { kind: "failed", savedNotApplied: true, error: "" } });
    expect(tuneStateText(failed)).toBe("Saved, not applied: Saved to the configuration file but not applied by the processing service");
    const named = run(begin(dirty()), { type: "result", requestId: 1, outcome: { kind: "failed", savedNotApplied: true, error: "the processing service reports different values" } });
    expect(tuneStateText(named)).toBe("Saved, not applied: the processing service reports different values");
  });

  it("goes to Conflict when the file or the running configuration changed under it, and keeps the draft", () => {
    const conflicted = run(begin(dirty()), { type: "result", requestId: 1, outcome: { kind: "failed", conflict: true, error: "config.json changed since it was opened; nothing was written" } });
    expect(conflicted.conflict).toBe(true);
    expect(conflicted.changed).toHaveLength(2);
    expect(tuneStateText(conflicted)).toMatch(/^Conflict:/);
    expect(tuneCanApply(conflicted, idle)).toBe(false);
  });

  it("parks a configuration that arrives while applying and reconciles it with the result", () => {
    const applying = begin(dirty());
    const parked = run(applying, { type: "external", values: withValue("ringRatioMax", 40) });
    expect(parked.pending).toEqual(withValue("ringRatioMax", 40));
    expect(parked.conflict).toBe(false);
    // Confirmed: the effective values are newer than anything parked.
    const effective = { ...values(), areaMin: 80, borderEnabled: false };
    const done = run(parked, { type: "result", requestId: 1, outcome: { kind: "applied", effective, target: { kind: "runtime", why: "" } } });
    expect(done.pending).toBeNull();
    expect(done.baseline).toEqual(effective);
    // Failed: the parked value is judged as an external change under the retained draft.
    const failed = run(parked, { type: "result", requestId: 1, outcome: { kind: "failed", error: "boom" } });
    expect(failed.conflict).toBe(true);
    expect(failed.pending).toEqual(withValue("ringRatioMax", 40));
    expect(failed.changed).toHaveLength(2);
  });

  it("checks the read-back at display precision", () => {
    const s = dirty();
    expect(tunePatchSatisfiedBy(s, { ...values(), areaMin: 80, borderEnabled: false })).toBe(true);
    expect(tunePatchSatisfiedBy(s, { ...values(), areaMin: 80, borderEnabled: true })).toBe(false);
    expect(tunePatchSatisfiedBy(s, { ...values(), areaMin: 70, borderEnabled: false })).toBe(false);
    expect(tuneDifferences(values(), withValue("areaMin", 61))).toEqual(["areaMin"]);
  });
});

describe("what the experiment allows", () => {
  it("follows the backend's rule per state", () => {
    expect(tuneScope(EXPERIMENT_STATES.Idle, true)).toEqual({ allowed: true, midRun: false, reason: "" });
    const active = tuneScope(EXPERIMENT_STATES.Active, true);
    expect(active.allowed).toBe(true);
    expect(active.midRun).toBe(true);
    expect(active.reason).toMatch(/apply live/);
    for (const state of [EXPERIMENT_STATES.Starting, EXPERIMENT_STATES.Stopping, EXPERIMENT_STATES.Failed]) {
      const scope = tuneScope(state, true);
      expect(scope.allowed).toBe(false);
      expect(scope.reason).not.toBe("");
    }
    expect(tuneScope(EXPERIMENT_STATES.Starting, true).reason).toMatch(/starting/);
    expect(tuneScope(EXPERIMENT_STATES.Stopping, true).reason).toMatch(/stopping/);
    expect(tuneScope(EXPERIMENT_STATES.Failed, true).reason).toMatch(/failed/);
    expect(tuneScope(99, true).allowed).toBe(false);
  });

  it("refuses a run on a PL instrument (the PL runs with the profile compiled at Start) but tunes it between runs", () => {
    const pl = tuneScope(EXPERIMENT_STATES.Active, false);
    expect(pl.allowed).toBe(false);
    expect(pl.reason).toMatch(/PL/);
    expect(tuneScope(EXPERIMENT_STATES.Idle, false).allowed).toBe(true);
  });

  it("holds Apply back for a dirty draft the experiment will not take", () => {
    const dirty = run(loaded(), edit("areaMin", 80));
    expect(tuneApplyBlocker(dirty, idle)).toBe("");
    expect(tuneCanApply(dirty, running)).toBe(true);
    const starting = tuneScope(EXPERIMENT_STATES.Starting, true);
    expect(tuneApplyBlocker(dirty, starting)).toBe(starting.reason);
    expect(tuneCanApply(dirty, starting)).toBe(false);
    // Revert stays available whatever the experiment does.
    expect(tuneCanRevert(dirty)).toBe(true);
  });

  it("keeps multi-image changes waiting for the end of a run, and applies them when idle", () => {
    const multi = run(loaded(), edit("multiImageEnabled", true), edit("multiImageCount", 4));
    expect(tuneApplyBlocker(multi, running)).toMatch(/Multi-image changes wait until the experiment ends/);
    expect(tuneCanApply(multi, running)).toBe(false);
    expect(tuneCanApply(multi, idle)).toBe(true);
    const both = run(multi, edit("areaMin", 80));
    expect(tuneCanApply(both, running)).toBe(false); // the whole patch waits: nothing is sent partially
    const criteriaOnly = run(loaded(), edit("areaMin", 80));
    expect(tuneCanApply(criteriaOnly, running)).toBe(true);
  });
});

describe("an open configuration file against the running one", () => {
  it("names the keys both carry that differ, and ignores the rest", () => {
    const running = section();
    expect(documentDisagreements(section(), running)).toEqual([]);
    const file = { ...section(), unknown_key: 3, gaussian_blur_size: 7, filters: { ...section().filters, enable_border_check: false }, target_group: { area_max: 191 } };
    expect(documentDisagreements(file, running)).toEqual(["gaussian_blur_size", "filters.enable_border_check"]);
    expect(documentDisagreements({}, running)).toEqual([]);
    expect(documentDisagreements(undefined, running)).toEqual([]);
    expect(documentDisagreements({ area_threshold_min: 61 }, running)).toEqual(["area_threshold_min"]);
    expect(documentDisagreements({ area_threshold_min: 60.0 }, running)).toEqual([]);
  });
});
