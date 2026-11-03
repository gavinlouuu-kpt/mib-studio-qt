// MIB Studio tours: the six fold-in entries (pure checks: shape, validation, conditions, flowchart text, step text). The checks that read
// files and git history (sources exist, data-tour attributes exist, tested items name real tests) are in scripts/tours-sources.test.ts:
// they use Node APIs and `tsc` has no Node types.
import { describe, expect, it } from "vitest";
import { validateEntries, type TourEntry } from "../registry";
import { CONDITION_NAMES, FACTS } from "./facts";
import { MIB_TOURS } from "./entries";

const EXPECTED = [
  "fold-in-e01-discovery", "fold-in-e02-features", "fold-in-e04-e06-runs", "fold-in-i02-recipes", "fold-in-f04-population", "fold-in-i12-f12-workspace",
];

describe("the six fold-in tours", () => {
  it("are exactly the toured goals, each a backend tour introduced on 2026-11-03", () => {
    expect(MIB_TOURS.map((e) => e.id)).toEqual(EXPECTED);
    for (const e of MIB_TOURS) {
      expect(e.kind).toBe("backend");
      expect(e.introduced).toEqual({ date: "2026-11-03", by: "fold-in tours (claude/fold-in-tours)" });
    }
    // F09 (comparison and fits) has no production view, only a JSON probe: a probe gets no tour.
    expect(MIB_TOURS.some((e) => /f09|comparison|4pl/i.test(e.id))).toBe(false);
  });

  it("validate against the registry rules and the app's condition names", () => {
    expect(validateEntries(MIB_TOURS, { conditions: CONDITION_NAMES })).toEqual([]);
  });

  it("flowchart text fits its box (label and sub-label length)", () => {
    for (const e of MIB_TOURS) {
      for (const [role, spec] of Object.entries(e.flows ?? {})) {
        for (const n of spec!.nodes) {
          expect(n.label.length, `${e.id} ${role} ${n.id} label`).toBeLessThanOrEqual(18);
          expect((n.sub ?? "").length, `${e.id} ${role} ${n.id} sub`).toBeLessThanOrEqual(22);
        }
      }
    }
  });

  it("every action step awaits a known condition and has a sensible timeout; the run steps are duringRun", () => {
    const actions = MIB_TOURS.flatMap((e) => e.steps.filter((s) => s.action).map((s) => ({ e, s, a: s.action! })));
    expect(actions.map(({ a }) => a.await)).toEqual([
      FACTS.discovery, FACTS.featureWrite, FACTS.runStart, FACTS.runFirstFrame, FACTS.runAccounting,
      FACTS.recipeCreated, FACTS.reviewOpened, FACTS.populationEvaluated, FACTS.workspaceSaved,
    ]);
    for (const { a } of actions) {
      expect(a.timeoutMs).toBeGreaterThanOrEqual(30_000);
      expect(a.success).toContain("{detail}");
    }
    const during = actions.filter(({ a }) => a.duringRun).map(({ a }) => a.await);
    expect(during).toEqual([FACTS.runStart, FACTS.runFirstFrame, FACTS.runAccounting]);
  });

  it("only the camera feature write needs the camera; the steps that ask for a press on a read or a preview are interactive, not actions", () => {
    const needs = MIB_TOURS.flatMap((e) => e.steps.filter((s) => s.action?.needsCamera).map((s) => s.action!.await));
    expect(needs).toEqual([FACTS.featureWrite]);
    const interactive = MIB_TOURS.flatMap((e) => e.steps.filter((s) => s.interactive).map((s) => "ui" in s.target ? s.target.ui : "?"));
    expect(interactive).toEqual(["align-feature-toggle", "recipe-profile", "recipe-compare", "recipe-preview-apply", "review-population-gallery", "saved-plan-history"]);
    for (const e of MIB_TOURS) for (const s of e.steps) if (s.interactive) expect(s.action).toBeUndefined();
  });

  it("the recipe form step points at the whole form, so the operator can fill it while it is highlighted", () => {
    const create = MIB_TOURS.find((e) => e.id === "fold-in-i02-recipes")!.steps.find((s) => s.action)!;
    expect(create.target).toEqual({ ui: "recipe-form" });
  });

  it("E04's start accepted and first frame are two separate action steps", () => {
    const e = MIB_TOURS.find((x) => x.id === "fold-in-e04-e06-runs")!;
    const awaits = e.steps.map((s) => s.action?.await).filter(Boolean);
    expect(awaits.indexOf(FACTS.runStart)).toBeGreaterThanOrEqual(0);
    expect(awaits.indexOf(FACTS.runFirstFrame)).toBe(awaits.indexOf(FACTS.runStart) + 1);
    expect(JSON.stringify(e.steps.map((s) => s.text))).toMatch(/counts can be zero|can be zero/);
  });
});

describe("steps", () => {
  const entry = (id: string): TourEntry => MIB_TOURS.find((e) => e.id === id)!;
  it("step text is short and plain", () => {
    for (const e of MIB_TOURS) {
      for (const s of e.steps) {
        expect(s.text.length, `${e.id}: ${s.title}`).toBeLessThanOrEqual(400);
        expect(s.title.length, `${e.id}: ${s.title}`).toBeLessThanOrEqual(60);
      }
    }
  });
  it("the stale refusal after a restart is shown only in the chart and evidence", () => {
    const i12 = entry("fold-in-i12-f12-workspace");
    expect(i12.steps.some((s) => /cannot restart the server/.test(s.text))).toBe(true);
    expect(i12.steps.filter((s) => s.action).map((s) => s.action!.await)).toEqual([FACTS.workspaceSaved]);
  });
  it("the apply preview is said not to apply or actuate", () => {
    const i02 = entry("fold-in-i02-recipes");
    expect(i02.steps.map((s) => s.text).join(" ")).toMatch(/hardware_actuated false/);
  });
});
