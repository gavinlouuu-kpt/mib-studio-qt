// The Plan's start request (#716 A5, bridge ABI 39): exact JSON text, the contract's bounds from
// the generated EXPERIMENT_START_REQUEST constants (at the limit and one past it), empty = absent
// (never ""), the experiment id, and the backend's InvalidRequest in plain words.
import { describe, expect, it } from "vitest";
import { EXPERIMENT_START_REQUEST as B } from "../bridgeContract";
import {
  UUID_V4,
  buildStartRequest,
  conditionSlots,
  concentrationText,
  invalidRequestReason,
  issueLine,
  mintExperimentId,
  newPlanDraft,
  parseOperators,
  rememberOperator,
  REMEMBERED_OPERATORS,
  utf8Bytes,
  type PlanDraft,
  type SampleRow,
} from "./planModel";

const ID = "3f2b8c1e-9a4d-4e6f-8b21-0c5d7e9f1a2b";
const plan = (row: Partial<SampleRow> = {}, operator = { name: "", id: "" }): PlanDraft => {
  const p = newPlanDraft(ID, operator);
  return { ...p, rows: [{ ...p.rows[0], ...row }] };
};
const FULL: Partial<SampleRow> = {
  sample: "Donor 1 · vehicle", condition: "Vehicle", experimentalUnit: "D1", concentrationValue: "0.1", concentrationUnit: "µM",
  channel: "C1 · 20 µm", medium: "PBS + 0.5% MC", temperatureC: "22.5", notes: "Line 1\nLine 2",
  free: [{ key: "passage", value: "12" }, { key: "lot", value: "A-17" }],
};
const json = (p: PlanDraft) => buildStartRequest(p).json;
const issues = (p: PlanDraft) => buildStartRequest(p).issues;
/** Every string anywhere in the document (keys and values). */
function strings(v: unknown): string[] {
  if (typeof v === "string") return [v];
  if (Array.isArray(v)) return v.flatMap(strings);
  if (v && typeof v === "object") return Object.entries(v).flatMap(([k, x]) => [k, ...strings(x)]);
  return [];
}
function nulls(v: unknown): number {
  if (v === null) return 1;
  if (v && typeof v === "object") return Object.values(v).reduce<number>((n, x) => n + nulls(x), 0);
  return 0;
}

describe("the start request as JSON text", () => {
  it("a full plan: every declared member, in the contract's order, free key/values in sheet order", () => {
    expect(json(plan(FULL, { name: "A. Operator", id: "op-7" }))).toBe(
      `{"experiment_id":"${ID}","run_index":1,"sample_ref":"S1","operator":{"id":"op-7","name":"A. Operator"},` +
      `"annotation":{"sample":"Donor 1 · vehicle","condition":"Vehicle","experimental_unit":"D1","concentration":{"value":0.1,"unit":"µM"},` +
      `"channel":"C1 · 20 µm","medium":"PBS + 0.5% MC","temperature_c":22.5,"notes":"Line 1\\nLine 2","free":{"passage":"12","lot":"A-17"}}}`,
    );
  });

  it("a minimal plan: only what is declared", () => {
    expect(json(plan({ condition: "Vehicle" }, { name: "A. Operator", id: "" }))).toBe(
      `{"experiment_id":"${ID}","run_index":1,"sample_ref":"S1","operator":{"name":"A. Operator"},"annotation":{"condition":"Vehicle"}}`,
    );
    expect(json(plan({}, { name: "", id: "op-7" }))).toBe(`{"experiment_id":"${ID}","run_index":1,"sample_ref":"S1","operator":{"id":"op-7"}}`);
  });

  it("an empty plan: the identity only; no operator, no annotation block", () => {
    const expected = `{"experiment_id":"${ID}","run_index":1,"sample_ref":"S1"}`;
    expect(json(plan())).toBe(expected);
    // Spaces only are not declared either, and spaces around a value are not part of it.
    expect(json(plan({ sample: "   ", condition: " \t", notes: "\n\n", concentrationValue: " ", concentrationUnit: " ", free: [{ key: " ", value: "" }] }, { name: "  ", id: "" }))).toBe(expected);
    expect(json(plan({ condition: "  Vehicle  " }))).toBe(`{"experiment_id":"${ID}","run_index":1,"sample_ref":"S1","annotation":{"condition":"Vehicle"}}`);
  });

  it("never sends an empty string or a null: not declared is absent", () => {
    const cases = [plan(), plan(FULL, { name: "A. Operator", id: "op-7" }), plan({ medium: "PBS", free: [{ key: "", value: "" }, { key: "lot", value: "A" }] }, { name: "", id: "x" })];
    for (const p of cases) {
      const doc = JSON.parse(json(p)!);
      expect(strings(doc)).not.toContain("");
      expect(nulls(doc)).toBe(0);
    }
  });

  it("numbers are sent as numbers; 0 is a declared value", () => {
    expect(json(plan({ concentrationValue: "0", concentrationUnit: "µM", temperatureC: "-4" }))).toContain(`"concentration":{"value":0,"unit":"µM"},"temperature_c":-4`);
    expect(json(plan({ concentrationValue: "1e-3", concentrationUnit: "M" }))).toContain(`"concentration":{"value":0.001,"unit":"M"}`);
    expect(json(plan({ temperatureC: "+37.0" }))).toContain(`"temperature_c":37`);
  });

  it("the run index and sample ref of a single-sample plan are 1 and the sheet row's ref", () => {
    const doc = JSON.parse(json(plan())!);
    expect(doc.run_index).toBe(1);
    expect(doc.sample_ref).toBe("S1");
    expect(doc).not.toHaveProperty("calibration_record_id"); // A4: not available yet
  });

  it("free keys that are JavaScript property names are sent as keys, in order", () => {
    expect(json(plan({ free: [{ key: "__proto__", value: "a" }, { key: "10", value: "b" }, { key: "2", value: "c" }] })))
      .toContain(`"free":{"__proto__":"a","10":"b","2":"c"}`);
  });
});

/** The issue keys of a plan, or [] when it can be sent. */
const keys = (p: PlanDraft) => issues(p).map((i) => i.key);

describe("bounds mirror the contract exactly (generated constants, bytes of UTF-8)", () => {
  const text: [keyof SampleRow, string][] = [
    ["sample", "annotation.sample"], ["condition", "annotation.condition"], ["experimentalUnit", "annotation.experimental_unit"],
    ["channel", "annotation.channel"], ["medium", "annotation.medium"],
  ];
  it.each(text)("%s: max_text_bytes is accepted, one more byte is refused with the limit named", (field, key) => {
    expect(keys(plan({ [field]: "x".repeat(B.max_text_bytes) }))).toEqual([]);
    const over = plan({ [field]: "x".repeat(B.max_text_bytes + 1) });
    expect(json(over)).toBeNull();
    expect(issues(over)).toEqual([expect.objectContaining({ key, text: `too long: ${B.max_text_bytes + 1} bytes, at most ${B.max_text_bytes}. Shorten it; nothing is cut off.` })]);
  });

  it("counts bytes, not characters: µ takes two", () => {
    const half = "µ".repeat(B.max_text_bytes / 2);
    expect(utf8Bytes(half)).toBe(B.max_text_bytes);
    expect(keys(plan({ condition: half }))).toEqual([]);
    expect(keys(plan({ condition: `${half}x` }))).toEqual(["annotation.condition"]);
  });

  it("notes: max_notes_bytes, line breaks and tabs allowed, other control characters refused", () => {
    expect(keys(plan({ notes: "x".repeat(B.max_notes_bytes) }))).toEqual([]);
    expect(keys(plan({ notes: "x".repeat(B.max_notes_bytes + 1) }))).toEqual(["annotation.notes"]);
    expect(keys(plan({ notes: "a\tb\r\nc" }))).toEqual([]);
    expect(issues(plan({ notes: "a\u0001b" }))[0]).toMatchObject({ key: "annotation.notes", text: "control characters are not allowed (line breaks and tabs are)." });
  });

  it("single-line fields refuse control characters, tabs included", () => {
    expect(issues(plan({ condition: "Vehicle\tB" }))).toEqual([expect.objectContaining({ key: "annotation.condition", text: "control characters, such as tabs, are not allowed." })]);
    expect(keys(plan({ medium: "PBS\u007F" }))).toEqual(["annotation.medium"]);
  });

  it("concentration: unit up to max_unit_bytes; value and unit both or neither; a finite number >= 0", () => {
    expect(keys(plan({ concentrationValue: "1", concentrationUnit: "u".repeat(B.max_unit_bytes) }))).toEqual([]);
    expect(keys(plan({ concentrationValue: "1", concentrationUnit: "u".repeat(B.max_unit_bytes + 1) }))).toEqual(["annotation.concentration.unit"]);
    expect(keys(plan({ concentrationValue: "1" }))).toEqual(["annotation.concentration"]);
    expect(keys(plan({ concentrationUnit: "µM" }))).toEqual(["annotation.concentration"]);
    expect(issues(plan({ concentrationValue: "-1", concentrationUnit: "µM" }))).toEqual([expect.objectContaining({ key: "annotation.concentration.value", text: "must be 0 or more." })]);
    for (const bad of ["1,5", "abc", "0x10", "1e400", "Infinity", "NaN"])
      expect(keys(plan({ concentrationValue: bad, concentrationUnit: "µM" }))).toEqual(["annotation.concentration.value"]);
  });

  it("temperature: temperature_c_min and temperature_c_max are accepted, one past either is refused", () => {
    expect(keys(plan({ temperatureC: String(B.temperature_c_max) }))).toEqual([]);
    expect(keys(plan({ temperatureC: String(B.temperature_c_min) }))).toEqual([]);
    expect(issues(plan({ temperatureC: String(B.temperature_c_max + 1) }))).toEqual([expect.objectContaining({
      key: "annotation.temperature_c", text: `${B.temperature_c_max + 1} °C is outside ${B.temperature_c_min} to ${B.temperature_c_max} °C.`,
    })]);
    expect(keys(plan({ temperatureC: String(B.temperature_c_min - 1) }))).toEqual(["annotation.temperature_c"]);
    expect(keys(plan({ temperatureC: `${B.temperature_c_max}.5` }))).toEqual(["annotation.temperature_c"]);
    expect(keys(plan({ temperatureC: "warm" }))).toEqual(["annotation.temperature_c"]);
  });

  it("free key/values: max_free_entries, max_free_key_bytes and max_free_value_bytes", () => {
    const entries = (n: number) => Array.from({ length: n }, (_, i) => ({ key: `k${i}`, value: "v" }));
    expect(keys(plan({ free: entries(B.max_free_entries) }))).toEqual([]);
    expect(issues(plan({ free: entries(B.max_free_entries + 1) }))).toEqual([expect.objectContaining({
      key: "annotation.free", text: `${B.max_free_entries + 1} entries, at most ${B.max_free_entries}.`,
    })]);
    expect(keys(plan({ free: [{ key: "k".repeat(B.max_free_key_bytes), value: "v" }] }))).toEqual([]);
    expect(keys(plan({ free: [{ key: "k".repeat(B.max_free_key_bytes + 1), value: "v" }] }))).toEqual(["annotation.free key"]);
    expect(keys(plan({ free: [{ key: "lot", value: "v".repeat(B.max_free_value_bytes) }] }))).toEqual([]);
    expect(issues(plan({ free: [{ key: "lot", value: "v".repeat(B.max_free_value_bytes + 1) }] }))).toEqual([expect.objectContaining({
      key: `annotation.free["lot"]`, label: "Key/value “lot”", row: 0,
    })]);
  });

  it("free key/values: a half-filled row and a repeated key are refused; an empty row is ignored", () => {
    expect(issues(plan({ free: [{ key: "lot", value: "" }] }))).toEqual([expect.objectContaining({ key: `annotation.free["lot"]`, text: "give the key a value, or clear the row.", row: 0 })]);
    expect(issues(plan({ free: [{ key: "", value: "A-17" }] }))).toEqual([expect.objectContaining({ key: "annotation.free key", text: "give the value a key, or clear the row.", row: 0 })]);
    expect(issues(plan({ free: [{ key: "lot", value: "A" }, { key: " lot ", value: "B" }] }))).toEqual([expect.objectContaining({ text: "this key is already used: keys are unique.", row: 1 })]);
    expect(json(plan({ free: [{ key: "", value: "" }, { key: "lot", value: "A" }] }))).toContain(`"free":{"lot":"A"}`);
  });

  it("operator name and id: max_id_bytes each", () => {
    expect(keys(plan({}, { name: "n".repeat(B.max_id_bytes), id: "i".repeat(B.max_id_bytes) }))).toEqual([]);
    expect(keys(plan({}, { name: "n".repeat(B.max_id_bytes + 1), id: "" }))).toEqual(["operator.name"]);
    expect(keys(plan({}, { name: "", id: "i".repeat(B.max_id_bytes + 1) }))).toEqual(["operator.id"]);
  });

  it("the whole request: max_request_bytes of the text as sent (escapes included) is accepted, one more is refused", () => {
    // Free values of quotes take two bytes each once escaped: find a sheet just under the bound,
    // then fill the notes to land exactly on it.
    const limit = B.max_request_bytes;
    let base: PlanDraft | null = null, bytes = 0;
    for (let v = 0; v <= B.max_free_value_bytes && !base; v++) {
      const p = plan({ notes: "x", free: Array.from({ length: B.max_free_entries }, (_, i) => ({ key: `k${String(i).padStart(2, "0")}`, value: '"'.repeat(v) })) });
      const b = buildStartRequest(p);
      if (b.issues.length === 0 && b.bytes <= limit && b.bytes + B.max_notes_bytes - 1 > limit) { base = p; bytes = b.bytes; }
    }
    expect(base).not.toBeNull();
    const withNotes = (n: number) => ({ ...base!, rows: [{ ...base!.rows[0], notes: "x".repeat(n) }] });
    const at = buildStartRequest(withNotes(1 + limit - bytes));
    expect(at.bytes).toBe(limit);
    expect(at.issues).toEqual([]);
    expect(utf8Bytes(at.json!)).toBe(limit);
    const over = buildStartRequest(withNotes(2 + limit - bytes));
    expect(over.json).toBeNull();
    expect(over.issues).toEqual([expect.objectContaining({ key: "startRequest", text: expect.stringContaining(`at most ${limit.toLocaleString("en-US")}`) })]);
  });

  it("names the field and the limit in one line", () => {
    const [i] = issues(plan({ condition: "x".repeat(B.max_text_bytes + 1) }));
    expect(issueLine(i)).toBe(`Condition: too long: ${B.max_text_bytes + 1} bytes, at most ${B.max_text_bytes}. Shorten it; nothing is cut off.`);
    const [f] = issues(plan({ free: [{ key: "a", value: "1" }, { key: "lot", value: "" }] }));
    expect(issueLine(f)).toBe("Key/value “lot” (row 2): give the key a value, or clear the row.");
  });
});

describe("the experiment id", () => {
  it("is a lowercase UUID v4", () => {
    const seen = new Set<string>();
    for (let i = 0; i < 200; i++) {
      const id = mintExperimentId();
      expect(id).toMatch(UUID_V4);
      expect(id).toBe(id.toLowerCase());
      seen.add(id);
    }
    expect(seen.size).toBe(200);
  });

  it("sets the version and variant bits from any random bytes", () => {
    expect(mintExperimentId((b) => b.fill(0xff))).toBe("ffffffff-ffff-4fff-bfff-ffffffffffff");
    expect(mintExperimentId((b) => b.fill(0))).toBe("00000000-0000-4000-8000-000000000000");
  });

  it("a plan's id that is not a lowercase UUID is refused before sending", () => {
    expect(keys({ ...plan(), experimentId: ID.toUpperCase() })).toEqual(["experiment_id"]);
    expect(keys({ ...plan(), experimentId: "" })).toEqual(["experiment_id"]);
  });
});

describe("the sheet's preview", () => {
  it("one colour per condition, slots 1-4 in sheet order", () => {
    const slots = conditionSlots([{ condition: "Vehicle" }, { condition: " CytoD " }, { condition: "Vehicle" }, { condition: "" }, { condition: "A" }, { condition: "B" }, { condition: "C" }]);
    expect([...slots]).toEqual([["Vehicle", 1], ["CytoD", 2], ["A", 3], ["B", 4]]);
  });
  it("concentration text", () => {
    expect(concentrationText({ concentrationValue: "0.1", concentrationUnit: "µM" })).toBe("0.1 µM");
    expect(concentrationText({ concentrationValue: "", concentrationUnit: "" })).toBe("");
    expect(concentrationText({ concentrationValue: "1", concentrationUnit: "" })).toBe("1 ?");
  });
});

describe("the backend's refusal (InvalidRequest, start outcome 7)", () => {
  it("names the field in the sheet's words", () => {
    expect(invalidRequestReason("invalid start request: annotation.condition: 300 bytes, at most 256")).toEqual({ key: "annotation.condition", label: "Condition", text: "300 bytes, at most 256" });
    expect(invalidRequestReason(`invalid start request: annotation.free["lot"]: values are strings only`)).toEqual({ key: `annotation.free["lot"]`, label: "Key/value “lot”", text: "values are strings only" });
    expect(invalidRequestReason("invalid start request: annotation.free key: empty; use null when it is not declared")?.label).toBe("Key/value key");
    expect(invalidRequestReason("invalid start request: startRequest: duplicate key \"sample\"")).toEqual({ key: "startRequest", label: "The plan as sent", text: "duplicate key \"sample\"" });
    expect(invalidRequestReason("invalid start request: something else")).toEqual({ key: "", label: "The plan as sent", text: "something else" });
  });
  it("any other message is not an InvalidRequest", () => {
    expect(invalidRequestReason("Experiment is already running")).toBeNull();
    expect(invalidRequestReason("")).toBeNull();
  });
});

describe("remembered operators", () => {
  it("most recent first, without duplicates, bounded; nothing for an empty selection", () => {
    let list = rememberOperator([], { name: "A", id: "1" });
    list = rememberOperator(list, { name: "B", id: "" });
    list = rememberOperator(list, { name: " A ", id: "1" });
    expect(list).toEqual([{ name: "A", id: "1" }, { name: "B", id: "" }]);
    expect(rememberOperator(list, { name: " ", id: "" })).toEqual(list);
    for (let i = 0; i < 20; i++) list = rememberOperator(list, { name: `N${i}`, id: "" });
    expect(list).toHaveLength(REMEMBERED_OPERATORS);
  });
  it("reads back only well-formed entries", () => {
    expect(parseOperators([{ name: "A", id: "1" }, { name: 3 }, null, "x", { name: "B", id: "" }])).toEqual([{ name: "A", id: "1" }, { name: "B", id: "" }]);
    expect(parseOperators({})).toEqual([]);
  });
});
