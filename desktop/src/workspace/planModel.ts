// Pure model of the workspace's Experiment · Plan sub-view (O3, #716 A5; ADR 0015 items 1, 4, 5):
// the sample format, the sample sheet with its fixed schema, the run's identity and the selected
// operator, and the start request sent with Start (bridge ABI 39, #712).
//
// The sheet maps 1:1 to `/experiment_annotation` (RunStartIdentity.h): sample, condition,
// experimental unit, concentration (value + unit), channel, medium, temperature (°C), notes and
// free key/values. `buildStartRequest` turns a plan into the `startRequestJson` text. It checks the
// contract's bounds itself, with the generated `EXPERIMENT_START_REQUEST` constants (bytes of
// UTF-8, as the backend counts them), and refuses an over-bound value with the limit named; it
// never truncates. An empty field is not declared: its key is left out, never sent as "". Spaces
// around a value are not part of it. The backend checks the same request again and stays the
// authority: a refusal there (InvalidRequest, "invalid start request: <key>: <why>") is shown with
// `invalidRequestReason`.
//
// Pure module: no React or transport imports (the kit types are types only).

import type { ConditionSlot } from "@mib/ui";
import { EXPERIMENT_START_REQUEST as B } from "../bridgeContract";

export type SampleFormat = "single" | "queue";

/** One free key/value row of the sheet, as typed. */
export interface FreeEntry {
  key: string;
  value: string;
}

/** One sample-sheet row, as typed (text, so nothing is coerced before it is checked). */
export interface SampleRow {
  /** The row's id in the sheet, recorded as `sample_ref`. */
  ref: string;
  sample: string;
  condition: string;
  experimentalUnit: string;
  concentrationValue: string;
  concentrationUnit: string;
  channel: string;
  medium: string;
  temperatureC: string;
  notes: string;
  free: FreeEntry[];
}

/** The operator the client selected (#393 phase 1): a name and an optional id. Not authentication. */
export interface OperatorChoice {
  name: string;
  id: string;
}

export interface PlanDraft {
  /** UUID v4, lowercase, minted when the plan is created. */
  experimentId: string;
  format: SampleFormat;
  rows: SampleRow[];
  operator: OperatorChoice;
}

/** The text fields of a row, in sheet order. */
export type RowTextField = Exclude<keyof SampleRow, "ref" | "free">;

/** A run index in the contract: a whole number from 0 to 4294967295. */
const RUN_INDEX_MAX = 4294967295;

/** The run index of a single-sample plan: its one and only run. */
export const SINGLE_SAMPLE_RUN_INDEX = 1;

export function emptyRow(ref: string): SampleRow {
  return {
    ref, sample: "", condition: "", experimentalUnit: "", concentrationValue: "", concentrationUnit: "",
    channel: "", medium: "", temperatureC: "", notes: "", free: [],
  };
}

/** A new single-sample plan: a fresh experiment id, one empty row ("S1"), the given operator. */
export function newPlanDraft(experimentId: string, operator: OperatorChoice = { name: "", id: "" }): PlanDraft {
  return { experimentId, format: "single", rows: [emptyRow("S1")], operator: { ...operator } };
}

// ---- experiment id ------------------------------------------------------------------------------

/** The canonical lowercase form of a UUID v4. */
export const UUID_V4 = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
/** What the backend accepts as an experiment id: 8-4-4-4-12 lowercase hex (it lowercases first). */
const CANONICAL_UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

/** A random UUID v4 in lowercase. Uses `getRandomValues`, which, unlike `randomUUID`, also exists
 *  outside a secure context (the PZ7035's instrument UI is served over plain HTTP). */
export function mintExperimentId(fill: (bytes: Uint8Array) => void = (bytes) => { globalThis.crypto.getRandomValues(bytes); }): string {
  const b = new Uint8Array(16);
  fill(b);
  b[6] = (b[6] & 0x0f) | 0x40; // version 4
  b[8] = (b[8] & 0x3f) | 0x80; // RFC 4122 variant
  const h = [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

// ---- checks --------------------------------------------------------------------------------------

/** A reason the plan cannot be sent, named by its contract key and the field's label. */
export interface PlanIssue {
  /** The contract key, e.g. "annotation.condition" ("startRequest" for the whole request). */
  key: string;
  /** The field as the sheet names it, e.g. "Condition". */
  label: string;
  /** Why, in plain words, with the limit named. */
  text: string;
  /** For a key/value row: its position in the sheet (0-based). */
  row?: number;
}

const encoder = new TextEncoder();
/** Bytes of UTF-8, as the backend counts them. */
export const utf8Bytes = (s: string): number => encoder.encode(s).length;

const LONE_SURROGATE = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/;
const CONTROL = /[\u0000-\u001F\u007F]/;
// Notes may hold tab, line feed and carriage return; nothing else below 0x20.
const CONTROL_MULTILINE = /[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/;
const DECIMAL = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/;

const group = (n: number) => n.toLocaleString("en-US");

/** "Too long: 257 bytes, at most 256." */
export function tooLongText(bytes: number, max: number): string {
  return `too long: ${group(bytes)} bytes, at most ${group(max)}. Shorten it; nothing is cut off.`;
}

class Checker {
  readonly issues: PlanIssue[] = [];
  add(key: string, label: string, text: string, row?: number) {
    this.issues.push(row === undefined ? { key, label, text } : { key, label, text, row });
  }
  /** A declared string: trimmed; empty = not declared (undefined); otherwise within `max` bytes. */
  text(key: string, label: string, raw: string, max: number, multiline = false, row?: number): string | undefined {
    const v = raw.trim();
    if (v === "") return undefined;
    if (LONE_SURROGATE.test(v)) this.add(key, label, "holds a character that is not valid text.", row);
    else if ((multiline ? CONTROL_MULTILINE : CONTROL).test(v))
      this.add(key, label, multiline ? "control characters are not allowed (line breaks and tabs are)." : "control characters, such as tabs, are not allowed.", row);
    else {
      const n = utf8Bytes(v);
      if (n > max) this.add(key, label, tooLongText(n, max), row);
    }
    return v;
  }
  /** A declared number: undefined when empty, NaN after an issue. */
  number(key: string, label: string, raw: string): number | undefined {
    const v = raw.trim();
    if (v === "") return undefined;
    const n = DECIMAL.test(v) ? Number(v) : NaN;
    if (!DECIMAL.test(v)) this.add(key, label, "not a number (use a point for decimals, e.g. 0.1).");
    else if (!Number.isFinite(n)) this.add(key, label, "too large to record.");
    return n;
  }
}

/** Labels of the contract keys, for the sheet and for the backend's refusals. */
export const KEY_LABELS: Readonly<Record<string, string>> = {
  startRequest: "The plan as sent",
  experiment_id: "Experiment id",
  run_index: "Run index",
  sample_ref: "Sample ref",
  operator: "Operator",
  "operator.id": "Operator id",
  "operator.name": "Operator name",
  calibration_record_id: "Calibration record",
  annotation: "Sample sheet",
  "annotation.sample": "Sample",
  "annotation.condition": "Condition",
  "annotation.experimental_unit": "Experimental unit",
  "annotation.concentration": "Concentration",
  "annotation.concentration.value": "Concentration value",
  "annotation.concentration.unit": "Concentration unit",
  "annotation.channel": "Channel",
  "annotation.medium": "Medium",
  "annotation.temperature_c": "Temperature (°C)",
  "annotation.notes": "Notes",
  "annotation.free": "Key/values",
  "annotation.free key": "Key/value key",
};

/** The fixed single-line text fields of a row, in sheet order, with their contract keys. */
export const ROW_TEXT_FIELDS: readonly { field: RowTextField; key: string; max: number; hint?: string }[] = [
  { field: "sample", key: "annotation.sample", max: B.max_text_bytes, hint: "the sample as you name it" },
  { field: "condition", key: "annotation.condition", max: B.max_text_bytes, hint: "e.g. Vehicle; one colour per condition" },
  { field: "experimentalUnit", key: "annotation.experimental_unit", max: B.max_text_bytes, hint: "e.g. the donor" },
  { field: "channel", key: "annotation.channel", max: B.max_text_bytes },
  { field: "medium", key: "annotation.medium", max: B.max_text_bytes },
];

/** The contract key a row field is recorded under (the sheet shows the issue next to it). */
export const FIELD_KEYS: Readonly<Record<RowTextField, string>> = {
  sample: "annotation.sample",
  condition: "annotation.condition",
  experimentalUnit: "annotation.experimental_unit",
  concentrationValue: "annotation.concentration.value",
  concentrationUnit: "annotation.concentration.unit",
  channel: "annotation.channel",
  medium: "annotation.medium",
  temperatureC: "annotation.temperature_c",
  notes: "annotation.notes",
};

// ---- the start request ---------------------------------------------------------------------------

type Fragment = string | undefined;
const str = (s: string): string => JSON.stringify(s);
/** A JSON object from ordered entries; undefined members are left out (not declared). */
function object(entries: readonly (readonly [string, Fragment])[]): string {
  return `{${entries.filter(([, v]) => v !== undefined).map(([k, v]) => `${str(k)}:${v}`).join(",")}}`;
}
const objectOrAbsent = (entries: readonly (readonly [string, Fragment])[]): Fragment =>
  entries.some(([, v]) => v !== undefined) ? object(entries) : undefined;
const optStr = (v: string | undefined): Fragment => (v === undefined ? undefined : str(v));
const num = (n: number): string => JSON.stringify(n);

export interface StartRequestBuild {
  /** The startRequestJson text, or null when an issue prevents sending it. */
  json: string | null;
  issues: PlanIssue[];
  /** UTF-8 bytes of the text (of what it would be, with issues). */
  bytes: number;
}

/** The row's annotation members, checked; undefined when the row declares nothing. */
function annotation(c: Checker, row: SampleRow): Fragment {
  const t = (field: RowTextField, max: number, multiline = false) =>
    c.text(FIELD_KEYS[field], KEY_LABELS[FIELD_KEYS[field]], row[field], max, multiline);
  const sample = t("sample", B.max_text_bytes);
  const condition = t("condition", B.max_text_bytes);
  const unitOfExperiment = t("experimentalUnit", B.max_text_bytes);

  // Concentration: a value and a unit, or neither.
  let concentration: Fragment;
  const valueText = row.concentrationValue.trim(), unitText = row.concentrationUnit.trim();
  if (valueText !== "" || unitText !== "") {
    if (valueText === "" || unitText === "") {
      c.add("annotation.concentration", KEY_LABELS["annotation.concentration"], valueText === ""
        ? "give a value with the unit, or clear the unit when it is not declared."
        : "give a unit with the value (e.g. µM), or clear the value when it is not declared.");
    }
    const value = c.number("annotation.concentration.value", KEY_LABELS["annotation.concentration.value"], row.concentrationValue);
    if (value !== undefined && Number.isFinite(value) && value < 0)
      c.add("annotation.concentration.value", KEY_LABELS["annotation.concentration.value"], "must be 0 or more.");
    const unit = t("concentrationUnit", B.max_unit_bytes);
    if (value !== undefined && unit !== undefined) concentration = object([["value", num(value)], ["unit", str(unit)]]);
  }

  const channel = t("channel", B.max_text_bytes);
  const medium = t("medium", B.max_text_bytes);

  let temperature: Fragment;
  const celsius = c.number("annotation.temperature_c", KEY_LABELS["annotation.temperature_c"], row.temperatureC);
  if (celsius !== undefined && Number.isFinite(celsius)) {
    if (celsius < B.temperature_c_min || celsius > B.temperature_c_max)
      c.add("annotation.temperature_c", KEY_LABELS["annotation.temperature_c"],
        `${celsius} °C is outside ${B.temperature_c_min} to ${B.temperature_c_max} °C.`);
    else temperature = num(celsius);
  }

  const notes = t("notes", B.max_notes_bytes, true);

  // Free key/values: a row with neither is ignored; keys unique; strings only.
  const used = row.free.map((e, i) => ({ ...e, i })).filter((e) => e.key.trim() !== "" || e.value.trim() !== "");
  if (used.length > B.max_free_entries)
    c.add("annotation.free", KEY_LABELS["annotation.free"], `${used.length} entries, at most ${B.max_free_entries}.`);
  const seen = new Set<string>();
  const free: [string, string][] = [];
  for (const e of used) {
    const before = c.issues.length;
    const key = c.text("annotation.free key", KEY_LABELS["annotation.free key"], e.key, B.max_free_key_bytes, false, e.i);
    const valueKey = `annotation.free["${key ?? ""}"]`, valueLabel = key === undefined ? "Key/value value" : `Key/value “${key}”`;
    const value = c.text(valueKey, valueLabel, e.value, B.max_free_value_bytes, false, e.i);
    if (key === undefined) c.add("annotation.free key", KEY_LABELS["annotation.free key"], "give the value a key, or clear the row.", e.i);
    else if (value === undefined) c.add(valueKey, valueLabel, "give the key a value, or clear the row.", e.i);
    else if (seen.has(key)) c.add(valueKey, valueLabel, "this key is already used: keys are unique.", e.i);
    else if (c.issues.length === before) free.push([key, value]);
    if (key !== undefined) seen.add(key);
  }
  const freeObject = free.length ? object(free.map(([k, v]) => [k, str(v)] as const)) : undefined;

  return objectOrAbsent([
    ["sample", optStr(sample)],
    ["condition", optStr(condition)],
    ["experimental_unit", optStr(unitOfExperiment)],
    ["concentration", concentration],
    ["channel", optStr(channel)],
    ["medium", optStr(medium)],
    ["temperature_c", temperature],
    ["notes", optStr(notes)],
    ["free", freeObject],
  ]);
}

/** The startRequestJson of one run of the plan (ABI 39, the contract's `experiment_start_request`):
 *  experiment_id, run_index, sample_ref (the row's ref), the selected operator {id, name} when one
 *  is given, and the row's annotation when anything is declared. Nothing undeclared is sent; no
 *  calibration_record_id yet (calibration records come in A4). Members in the contract's order;
 *  free key/values in sheet order. */
export function buildStartRequest(plan: PlanDraft, rowIndex = 0, runIndex = SINGLE_SAMPLE_RUN_INDEX): StartRequestBuild {
  const c = new Checker();
  const row = plan.rows[rowIndex];
  if (!CANONICAL_UUID.test(plan.experimentId)) c.add("experiment_id", KEY_LABELS.experiment_id, "not a lowercase UUID (8-4-4-4-12 hex digits).");
  if (!Number.isInteger(runIndex) || runIndex < 0 || runIndex > RUN_INDEX_MAX)
    c.add("run_index", KEY_LABELS.run_index, `expected a whole number from 0 to ${RUN_INDEX_MAX}.`);
  if (!row) {
    c.add("sample_ref", KEY_LABELS.sample_ref, "the sheet has no such row.");
    return { json: null, issues: c.issues, bytes: 0 };
  }
  const ref = c.text("sample_ref", KEY_LABELS.sample_ref, row.ref, B.max_id_bytes);
  const operator = objectOrAbsent([
    ["id", optStr(c.text("operator.id", KEY_LABELS["operator.id"], plan.operator.id, B.max_id_bytes))],
    ["name", optStr(c.text("operator.name", KEY_LABELS["operator.name"], plan.operator.name, B.max_id_bytes))],
  ]);
  const text = object([
    ["experiment_id", str(plan.experimentId)],
    ["run_index", num(runIndex)],
    ["sample_ref", optStr(ref)],
    ["operator", operator],
    ["annotation", annotation(c, row)],
  ]);
  const bytes = utf8Bytes(text);
  if (bytes > B.max_request_bytes)
    c.add("startRequest", KEY_LABELS.startRequest, `${group(bytes)} bytes as sent, at most ${group(B.max_request_bytes)}. Shorten the notes or the key/values; nothing is cut off.`);
  return { json: c.issues.length ? null : text, issues: c.issues, bytes };
}

/** The issues of one sheet field (by contract key), or of one key/value row. */
export function issuesFor(issues: readonly PlanIssue[], keys: readonly string[]): PlanIssue[] {
  return issues.filter((i) => keys.includes(i.key) && i.row === undefined);
}
export function freeRowIssues(issues: readonly PlanIssue[], row: number): PlanIssue[] {
  return issues.filter((i) => i.row === row);
}

/** "Condition: too long: …", the one-line form of an issue. */
export const issueLine = (i: PlanIssue): string => `${i.label}${i.row === undefined ? "" : ` (row ${i.row + 1})`}: ${i.text}`;

// ---- the sheet's preview row ---------------------------------------------------------------------

/** One colour per condition: distinct declared conditions take slots 1–4 in sheet order (slot 5
 *  is the kit's calibration / reference colour). A condition past the fourth has no slot. */
export function conditionSlots(rows: readonly Pick<SampleRow, "condition">[]): Map<string, ConditionSlot> {
  const slots = new Map<string, ConditionSlot>();
  for (const r of rows) {
    const c = r.condition.trim();
    if (c === "" || slots.has(c) || slots.size >= 4) continue;
    slots.set(c, (slots.size + 1) as ConditionSlot);
  }
  return slots;
}

/** "0.1 µM", or "" when the concentration is not declared. */
export function concentrationText(row: Pick<SampleRow, "concentrationValue" | "concentrationUnit">): string {
  const v = row.concentrationValue.trim(), u = row.concentrationUnit.trim();
  return v && u ? `${v} ${u}` : v || u ? `${v || "?"} ${u || "?"}` : "";
}

// ---- the backend's answer ------------------------------------------------------------------------

/** The prefix of the backend's InvalidRequest message (start outcome 7; the contract's
 *  `experiment_start_request` document): "invalid start request: <key>: <why>". */
export const INVALID_REQUEST_PREFIX = "invalid start request: ";

export interface InvalidRequestReason {
  /** The key the backend named, e.g. "annotation.condition". */
  key: string;
  /** Its label in the sheet, or the key itself when the sheet has none. */
  label: string;
  /** The backend's reason, e.g. "300 bytes, at most 256". */
  text: string;
}

/** The backend's refusal of the start request in plain words, or null for any other message. */
export function invalidRequestReason(message: string): InvalidRequestReason | null {
  if (!message.startsWith(INVALID_REQUEST_PREFIX)) return null;
  const rest = message.slice(INVALID_REQUEST_PREFIX.length);
  const m = /^(annotation\.free\[".*?"\]|annotation\.free key|[A-Za-z_.]+): ([\s\S]*)$/.exec(rest);
  if (!m) return { key: "", label: KEY_LABELS.startRequest, text: rest };
  const [, key, text] = m;
  const free = /^annotation\.free\["(.*)"\]$/.exec(key);
  return { key, label: free ? `Key/value “${free[1]}”` : KEY_LABELS[key] ?? key, text };
}

// ---- remembered operators ------------------------------------------------------------------------

/** How many selected operators the workspace remembers locally. */
export const REMEMBERED_OPERATORS = 8;

const same = (a: OperatorChoice, b: OperatorChoice) => a.name.trim() === b.name.trim() && a.id.trim() === b.id.trim();

/** The remembered list with `op` first (most recent), without duplicates, bounded. An operator
 *  with neither a name nor an id is not remembered. */
export function rememberOperator(list: readonly OperatorChoice[], op: OperatorChoice): OperatorChoice[] {
  const entry = { name: op.name.trim(), id: op.id.trim() };
  if (!entry.name && !entry.id) return [...list];
  return [entry, ...list.filter((o) => !same(o, entry))].slice(0, REMEMBERED_OPERATORS);
}

/** A remembered list read back from storage; anything malformed is dropped. */
export function parseOperators(raw: unknown): OperatorChoice[] {
  if (!Array.isArray(raw)) return [];
  return raw
    .filter((o): o is OperatorChoice => !!o && typeof o === "object" && typeof (o as OperatorChoice).name === "string" && typeof (o as OperatorChoice).id === "string")
    .map((o) => ({ name: o.name, id: o.id }))
    .slice(0, REMEMBERED_OPERATORS);
}

/** "A. Operator (op-7)", "A. Operator", "op-7". */
export const operatorText = (o: OperatorChoice): string => {
  const name = o.name.trim(), id = o.id.trim();
  return name && id ? `${name} (${id})` : name || id;
};
