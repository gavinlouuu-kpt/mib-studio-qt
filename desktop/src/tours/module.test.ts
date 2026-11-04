// Guided tours, generic module: validation, the per-browser storage (including a localStorage that throws), the
// "what's new" marker and the canvas geometry adapter. No app and no instrument.
import { describe, expect, it } from "vitest";
import { feedbackStore, safeStore } from "./feedback";
import { flowTarget, sourcesOf, validateEntries, validateFlow, type FlowSpec, type TourEntry } from "./registry";
import { regionRect } from "./runner";
import { newEntries, seenMarker } from "./WhatsNew";

const FLOW: FlowSpec = { title: "t", cols: 2, nodes: [{ id: "a", label: "A", col: 0, row: 0 }, { id: "b", label: "B", col: 1, row: 1 }], edges: [{ from: "a", to: "b" }] };
const entry = (over: Partial<TourEntry> = {}): TourEntry => ({
  id: "x", title: "X", summary: "s", kind: "feature", introduced: { date: "2026-11-03", by: "test" }, source: ["doc.md"], when: () => true,
  steps: [{ target: { ui: "thing" }, title: "T", text: "Text.", cite: ["doc.md"] }], ...over,
});
const backend = (over: Partial<TourEntry> = {}): TourEntry => entry({
  kind: "backend", flows: { before: FLOW, after: FLOW }, evidence: { demonstrated: [{ text: "shown", date: "2026-11-02", source: "doc.md" }], tested: [{ text: "ok", test: "a.test.ts", source: "doc.md" }], uncertain: [] },
  steps: [{ target: { flow: "after", node: "a" }, title: "N", text: "n", cite: ["doc.md"] }, { target: { flow: "after", edge: "a->b" }, title: "E", text: "e", cite: ["doc.md"] },
    { target: { ui: "place" }, title: "U", text: "u", cite: ["doc.md"] }], ...over,
});

function throwingStorage(): Storage {
  const no = () => { throw new DOMException("denied", "SecurityError"); };
  return { getItem: no, setItem: no, removeItem: no, clear: no, key: no, length: 0 } as unknown as Storage;
}
function memoryStorage(): Storage {
  const m = new Map<string, string>();
  return { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => void m.set(k, v), removeItem: (k: string) => void m.delete(k), clear: () => m.clear(), key: () => null, get length() { return m.size; } } as Storage;
}

describe("validateEntries", () => {
  it("accepts a valid feature and backend entry", () => {
    expect(validateEntries([entry(), backend({ id: "y" })], { conditions: [] })).toEqual([]);
  });
  it("finds duplicate ids, bad dates, uncited and unknown citations", () => {
    const p = validateEntries([entry(), entry({ introduced: { date: "3 Nov", by: "x" }, steps: [{ target: { ui: "t" }, title: "T", text: "x", cite: ["other.md"] }] })]);
    expect(p).toEqual(expect.arrayContaining(["x: duplicate id", "x: introduced.date 3 Nov is not YYYY-MM-DD", expect.stringContaining("cites other.md, not in source")]));
  });
  it("checks flowchart targets against the spec", () => {
    const p = validateEntries([backend({ steps: [{ target: { flow: "after", node: "zz" }, title: "N", text: "n", cite: ["doc.md"] }, { target: { flow: "before", edge: "b->a" }, title: "E", text: "e", cite: ["doc.md"] }, { target: { ui: "p" }, title: "U", text: "u", cite: ["doc.md"] }] })]);
    expect(p).toEqual([expect.stringContaining("no node zz"), expect.stringContaining("no edge b->a")]);
  });
  it("asks a backend tour for before and after, evidence and a UI step", () => {
    const p = validateEntries([entry({ kind: "backend", steps: [{ target: { none: true }, title: "T", text: "t", cite: ["doc.md"] }] })]);
    expect(p).toEqual(expect.arrayContaining([expect.stringContaining("before and an after"), expect.stringContaining("evidence panel"), expect.stringContaining("where the outcome shows")]));
  });
  it("keeps evidence honest: dated demonstrations, named tests, cited sources", () => {
    const p = validateEntries([backend({ evidence: { demonstrated: [{ text: "x", source: "doc.md" }], tested: [{ text: "y", source: "doc.md" }], uncertain: [{ text: "z", source: "nope.md" }] } })]);
    expect(p).toEqual(expect.arrayContaining([expect.stringContaining("needs a date"), expect.stringContaining("test's name"), expect.stringContaining("cites nope.md")]));
  });
  it("action steps: a real control, a known condition, a timeout and a recovery", () => {
    const action = { await: "nope", prompt: "Press it.", success: "ok", recovery: "", timeoutMs: 0 };
    const p = validateEntries([entry({ steps: [{ target: { none: true }, title: "A", text: "a", cite: ["doc.md"], action }] })], { conditions: ["camera-running"] });
    expect(p).toEqual(expect.arrayContaining([expect.stringContaining("real control"), expect.stringContaining("unknown condition nope"), expect.stringContaining("timeout"), expect.stringContaining("recovery")]));
  });
  it("interactive steps: a real control, and not an action step as well", () => {
    const action = { await: "ok", prompt: "Press it.", success: "ok", recovery: "r", timeoutMs: 1000 };
    const p = validateEntries([entry({ steps: [
      { target: { none: true }, title: "I", text: "i", cite: ["doc.md"], interactive: true },
      { target: { ui: "t" }, title: "J", text: "j", cite: ["doc.md"], interactive: true, action },
      { target: { ui: "t" }, title: "K", text: "k", cite: ["doc.md"], interactive: true },
    ] })], { conditions: ["ok"] });
    expect(p).toEqual([expect.stringContaining("step 1 (I): an interactive step points at a real control"), expect.stringContaining("step 2 (J): an action step is already interactive")]);
  });
  it("a canvas target names a region of the entry", () => {
    const p = validateEntries([entry({ steps: [{ target: { canvas: "img", region: "frame" }, title: "C", text: "c", cite: ["doc.md"] }] })]);
    expect(p).toEqual([expect.stringContaining("no canvas region frame")]);
  });
  it("collects every source an entry names", () => {
    expect(sourcesOf(backend())).toEqual(["doc.md"]);
  });
});

describe("validateFlow", () => {
  it("finds missing nodes, overlaps and off-grid nodes", () => {
    const bad: FlowSpec = { title: "t", cols: 1, nodes: [{ id: "a", label: "A", col: 0, row: 0 }, { id: "b", label: "B", col: 0, row: 0 }, { id: "c", label: "C", col: 3, row: 0 }], edges: [{ from: "a", to: "q" }] };
    expect(validateFlow(bad)).toEqual(expect.arrayContaining([expect.stringContaining("overlaps"), expect.stringContaining("off the grid"), expect.stringContaining("missing node")]));
  });
  it("names targets as flow:<role>:<id>", () => {
    expect(flowTarget("after", "a->b")).toBe("flow:after:a->b");
    expect(flowTarget("before")).toBe("flow:before");
  });
});

describe("storage without localStorage", () => {
  it("a throwing localStorage: the marker and feedback work for the page, nothing throws", () => {
    const store = safeStore(throwingStorage);
    const marker = seenMarker(store, "k.seen");
    expect(newEntries([entry()], marker.seen()).map((e) => e.id)).toEqual(["x"]);
    expect(() => marker.markSeen(["x"])).not.toThrow();
    expect(marker.seen().has("x")).toBe(true);
    const fb = feedbackStore(store, "k.fb", () => new Date("2026-11-03T10:00:00Z"));
    fb.add({ tour: "x", verdict: "unclear", note: "n", tourVersion: "2026-11-03" });
    expect(fb.all()).toHaveLength(1);
    expect(JSON.parse(fb.exportJson({ instrument: "t" }))).toMatchObject({ kind: "tour-feedback", instrument: "t", records: [{ tour: "x", verdict: "unclear", note: "n", at: "2026-11-03T10:00:00.000Z" }] });
  });
  it("no localStorage at all", () => {
    const store = safeStore(() => undefined);
    expect(store.set("a", "1")).toBe(false);
    expect(store.get("a")).toBe("1");
  });
  it("a working localStorage persists across stores; garbage reads as empty", () => {
    const ls = memoryStorage();
    seenMarker(safeStore(() => ls), "k").markSeen(["b", "a"]);
    expect([...seenMarker(safeStore(() => ls), "k").seen()]).toEqual(["a", "b"]);
    ls.setItem("k", "{not json");
    expect(seenMarker(safeStore(() => ls), "k").seen().size).toBe(0);
    ls.setItem("fb", "[{\"tour\":1}]");
    expect(feedbackStore(safeStore(() => ls), "fb").all()).toEqual([]);
  });
});

describe("what's new", () => {
  it("lists unseen entries, newest first", () => {
    const es = [entry({ id: "a", introduced: { date: "2026-10-26", by: "p" } }), entry({ id: "b", introduced: { date: "2026-11-02", by: "p" } }), entry({ id: "c", introduced: { date: "2026-10-28", by: "p" } })];
    expect(newEntries(es, new Set(["c"])).map((e) => e.id)).toEqual(["b", "a"]);
    expect(newEntries(es, new Set(["a", "b", "c"]))).toEqual([]);
  });
});

describe("canvas geometry adapter", () => {
  it("maps a region in image pixels onto the canvas box", () => {
    const box = { left: 100, top: 50, width: 1024, height: 192 };  // a 512x96 image drawn at 2x
    expect(regionRect(box, { width: 512, height: 96 }, { x: 0, y: 0 })).toEqual({ left: 100, top: 50, width: 1024, height: 192 });
    expect(regionRect(box, { width: 512, height: 96 }, { x: 256, y: 48, width: 128, height: 24 })).toEqual({ left: 612, top: 146, width: 256, height: 48 });
  });
});
