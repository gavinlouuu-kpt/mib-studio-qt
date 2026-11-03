// MIB Studio tours: the facts store. Facts are what a component reports after a backend reply; conditions read them.
import { describe, expect, it } from "vitest";
import { CONDITION_NAMES, createFactsStore, FACTS } from "./facts";

describe("facts store", () => {
  it("a condition is pending until a success is reported, then met with the reported detail", () => {
    const f = createFactsStore();
    expect(f.condition(FACTS.discovery)).toBe("pending");
    expect(f.detail(FACTS.discovery)).toBeUndefined();
    f.report(FACTS.discovery, { ok: true, detail: "job 1, instance abc" });
    expect(f.condition(FACTS.discovery)).toBe("met");
    expect(f.detail(FACTS.discovery)).toBe("job 1, instance abc");
    expect(f.condition(FACTS.runStart)).toBe("pending");  // facts are independent
  });

  it("start accepted and first frame are two facts: one does not satisfy the other", () => {
    const f = createFactsStore();
    f.report(FACTS.runStart, { ok: true, detail: "run generation 1" });
    expect(f.condition(FACTS.runStart)).toBe("met");
    expect(f.condition(FACTS.runFirstFrame)).toBe("pending");
    f.report(FACTS.runFirstFrame, { ok: true, detail: "frames buffered" });
    expect(f.condition(FACTS.runFirstFrame)).toBe("met");
  });

  it("a refusal is handed out once as failed and then forgotten, so Try again waits afresh", () => {
    const f = createFactsStore();
    f.report(FACTS.featureWrite, { ok: false, reason: "Read-only in the current context." });
    expect(f.detail(FACTS.featureWrite)).toBeUndefined();
    expect(f.condition(FACTS.featureWrite)).toEqual({ failed: "Read-only in the current context." });
    expect(f.condition(FACTS.featureWrite)).toBe("pending");
  });

  it("a later report replaces an earlier one", () => {
    const f = createFactsStore();
    f.report(FACTS.workspaceSaved, { ok: false, reason: "conflict" });
    f.report(FACTS.workspaceSaved, { ok: true, detail: "ws · revision 2" });
    expect(f.condition(FACTS.workspaceSaved)).toBe("met");
    f.report(FACTS.workspaceSaved, { ok: false, reason: "conflict" });
    expect(f.condition(FACTS.workspaceSaved)).toEqual({ failed: "conflict" });
  });

  it("reset forgets everything: a tour waits for what the operator does during it", () => {
    const f = createFactsStore();
    for (const name of CONDITION_NAMES) f.report(name, { ok: true, detail: "x" });
    f.reset();
    for (const name of CONDITION_NAMES) expect(f.condition(name)).toBe("pending");
  });

  it("names every condition once, in kebab case", () => {
    expect(new Set(CONDITION_NAMES).size).toBe(CONDITION_NAMES.length);
    expect(CONDITION_NAMES).toHaveLength(Object.keys(FACTS).length);
    for (const n of CONDITION_NAMES) expect(n).toMatch(/^[a-z]+(-[a-z]+)*$/);
  });
});
