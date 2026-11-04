// Guided tours: the registry types and their validation. Generic: nothing here knows an instrument. An app supplies its
// entries (TourEntry[]), the context its `when` predicates read, and a host for the runner (runner.ts).
// The API is described in studio/TOURS.md (zu15eg-linux) and in docs/howto/adding-a-tour.md (MIB Studio port of this module).

/** A view of the host app (for example `{ tab: "experiment", expTab: "preview" }`). The runner asks the host to show it
 *  before it looks for a step's element: view state only, never a command or a setting. */
export type TourView = Readonly<Record<string, string>>;

export type TourSide = "top" | "right" | "bottom" | "left" | "over";

/** A rectangle in image coordinates (pixels of the drawn image, origin top left). `width`/`height` omitted: to the edge. */
export interface CanvasRegion {
  x: number;
  y: number;
  width?: number;
  height?: number;
}

/** What a step points at. Every element target is a `data-tour` value the app sets, never a CSS class chain. */
export type TourTarget =
  /** `[data-tour="<ui>"]`. */
  | { ui: string; view?: TourView }
  /** A named region of the entry's `canvasRegions`, mapped from image coordinates onto `[data-tour="<canvas>"]`, a canvas
   *  whose intrinsic size is the image. `requires`: a condition that must be met (for example a drawn frame). */
  | { canvas: string; region: string; requires?: string; view?: TourView }
  /** A node or edge ("from->to") of one of the entry's flowcharts; neither: the whole chart. */
  | { flow: FlowRole; node?: string; edge?: string }
  /** The entry's evidence panel. */
  | { evidence: true }
  /** No element: a message in the middle of the screen. */
  | { none: true };

/** A step that asks the operator to use a real control. The tour itself sends nothing: it waits until the host reports
 *  the condition `await` met (the backend confirmed it), up to `timeoutMs`, then says what happened. */
export interface TourAction {
  await: string;
  /** What the popover says while it waits, e.g. "Press Start Camera." */
  prompt: string;
  /** Shown once the condition is met. */
  success: string;
  /** Shown when the condition fails or times out, with what the host reports. */
  recovery: string;
  timeoutMs: number;
  /** The step is done while an experiment is running or being started (for example "press Stop"), so the host's
   *  `actionBlocked()` is not consulted for it, at any time. Default: the step is blocked while the host says so. */
  duringRun?: true;
  /** The control only works with the camera running (a camera feature write needs an open camera session), so the camera
   *  acquiring is no reason to block the step. The host's `actionBlocked(action)` is told, and still blocks for anything
   *  else (an experiment, a recording). */
  needsCamera?: true;
}

export interface TourStep {
  target: TourTarget;
  title: string;
  text: string;
  side?: TourSide;
  /** The sources the step's sentences come from; each must be one of the entry's `source`. */
  cite: string[];
  action?: TourAction;
  /** The text asks the operator to press the highlighted control (a `ui` target), so the runner leaves it usable, as it does for
   *  an action step. For a control that only reads or previews: the step waits for nothing and the tour still sends nothing. */
  interactive?: true;
}

export type FlowRole = "before" | "after" | "planned";

export interface FlowNode {
  id: string;
  label: string;
  sub?: string;
  /** Grid position: column 0..cols-1, row 0..n. */
  col: number;
  row: number;
  tone?: "a" | "b" | "c" | "d" | "warn" | "neutral";
}

export interface FlowEdge {
  from: string;
  to: string;
  label?: string;
  dashed?: boolean;
}

export interface FlowSpec {
  title: string;
  cols: number;
  nodes: FlowNode[];
  edges: FlowEdge[];
  /** One line under the chart: what the colours mean, or a caveat. */
  legend?: string;
}

export interface EvidenceItem {
  text: string;
  /** One of the entry's `source`. */
  source: string;
  /** Demonstrated: the date it was shown (ISO), on the evidence class the item's text names (for example the hardware). */
  date?: string;
  /** Tested: the test's name. */
  test?: string;
}

/** Kept apart on purpose: a diagram is not proof of behaviour. */
export interface TourEvidence {
  /** The heading of the `demonstrated` list. Default: "Demonstrated (on the hardware)". An app whose demonstrations are not on
   *  hardware says what they are on. */
  demonstratedLabel?: string;
  demonstrated: EvidenceItem[];
  tested: EvidenceItem[];
  uncertain: EvidenceItem[];
}

export interface TourEntry<Ctx = unknown> {
  id: string;
  title: string;
  /** One line for the Tours list. */
  summary: string;
  /** `feature`: anchored on the UI. `backend`: before/after flowcharts, evidence, then where the outcome shows in the UI. */
  kind: "feature" | "backend";
  /** When it arrived: an ISO date and the patch, job or commit. */
  introduced: { date: string; by: string };
  /** Where the text comes from: repository paths, or `<root>:<path>` for another root the app names (see TOURS.md). */
  source: string[];
  when: (ctx: Ctx) => boolean;
  /** Before/after (and planned) flowcharts. Something only planned is `planned`, never `after`. */
  flows?: Partial<Record<FlowRole, FlowSpec>>;
  evidence?: TourEvidence;
  canvasRegions?: Record<string, CanvasRegion>;
  steps: TourStep[];
}

export const edgeId = (e: Pick<FlowEdge, "from" | "to">) => `${e.from}->${e.to}`;
/** The `data-tour` value of a flowchart node or edge: `flow:<role>:<node id or from->to>`. */
export const flowTarget = (role: FlowRole, id?: string) => (id ? `flow:${role}:${id}` : `flow:${role}`);
export const EVIDENCE_TARGET = "flow:evidence";

const ISO_DATE = /^\d{4}-\d{2}-\d{2}$/;

/** Everything wrong with a set of entries: unique ids, dates, citations, flowchart and region targets, action
 *  conditions the host knows, and the backend tour shape. Empty: valid. */
export function validateEntries<Ctx>(entries: TourEntry<Ctx>[], opts: { conditions?: readonly string[] } = {}): string[] {
  const problems: string[] = [];
  const ids = new Set<string>();
  for (const e of entries) {
    const at = (m: string) => problems.push(`${e.id}: ${m}`);
    if (!/^[a-z0-9][a-z0-9-]*$/.test(e.id)) at("id must be lower-case kebab");
    if (ids.has(e.id)) at("duplicate id");
    ids.add(e.id);
    if (!ISO_DATE.test(e.introduced.date)) at(`introduced.date ${e.introduced.date} is not YYYY-MM-DD`);
    if (!e.introduced.by.trim()) at("introduced.by is empty");
    if (!e.source.length) at("no source");
    if (new Set(e.source).size !== e.source.length) at("a source is listed twice");
    if (!e.steps.length) at("no steps");
    for (const [role, spec] of Object.entries(e.flows ?? {}) as [FlowRole, FlowSpec][]) {
      problems.push(...validateFlow(spec).map((p) => `${e.id}: ${role} flowchart: ${p}`));
    }
    if (e.kind === "backend") {
      if (!e.flows?.before || !e.flows?.after) at("a backend tour shows a before and an after flowchart");
      if (!e.evidence) at("a backend tour has an evidence panel");
      if (!e.steps.some((s) => "ui" in s.target || "canvas" in s.target)) at("a backend tour walks to where the outcome shows in the UI");
    }
    for (const group of ["demonstrated", "tested", "uncertain"] as const) {
      for (const item of e.evidence?.[group] ?? []) {
        if (!e.source.includes(item.source)) at(`evidence (${group}) cites ${item.source}, not in source`);
        if (group === "demonstrated" && !(item.date && ISO_DATE.test(item.date))) at("demonstrated evidence needs a date");
        if (group === "tested" && !item.test) at("tested evidence needs the test's name");
      }
    }
    e.steps.forEach((s, i) => {
      const step = (m: string) => at(`step ${i + 1} (${s.title}): ${m}`);
      if (!s.title.trim() || !s.text.trim()) step("empty title or text");
      if (!s.cite.length) step("cites no source");
      for (const c of s.cite) if (!e.source.includes(c)) step(`cites ${c}, not in source`);
      const t = s.target;
      if ("flow" in t) {
        const spec = e.flows?.[t.flow];
        if (!spec) step(`no ${t.flow} flowchart`);
        else if (t.node && !spec.nodes.some((n) => n.id === t.node)) step(`no node ${t.node} in the ${t.flow} flowchart`);
        else if (t.edge && !spec.edges.some((x) => edgeId(x) === t.edge)) step(`no edge ${t.edge} in the ${t.flow} flowchart`);
      }
      if ("evidence" in t && !e.evidence) step("points at an evidence panel the entry does not have");
      if ("canvas" in t) {
        if (!e.canvasRegions?.[t.region]) step(`no canvas region ${t.region}`);
        if (t.requires && opts.conditions && !opts.conditions.includes(t.requires)) step(`unknown condition ${t.requires}`);
      }
      if (s.interactive && !("ui" in t)) step("an interactive step points at a real control (a ui target)");
      if (s.interactive && s.action) step("an action step is already interactive");
      if (s.action) {
        if (!("ui" in t)) step("an action step points at a real control (a ui target)");
        if (opts.conditions && !opts.conditions.includes(s.action.await)) step(`unknown condition ${s.action.await}`);
        if (!(s.action.timeoutMs > 0)) step("an action step needs a timeout");
        if (!s.action.recovery.trim()) step("an action step needs a recovery text");
      }
    });
  }
  return problems;
}

export function validateFlow(spec: FlowSpec): string[] {
  const problems: string[] = [];
  const ids = new Set<string>();
  const cells = new Set<string>();
  for (const n of spec.nodes) {
    if (!/^[a-z0-9][a-z0-9-]*$/.test(n.id)) problems.push(`node id ${n.id} must be lower-case kebab`);
    if (ids.has(n.id)) problems.push(`duplicate node ${n.id}`);
    ids.add(n.id);
    if (n.col < 0 || n.col >= spec.cols || n.row < 0) problems.push(`node ${n.id} is off the grid`);
    const cell = `${n.col},${n.row}`;
    if (cells.has(cell)) problems.push(`node ${n.id} overlaps another at ${cell}`);
    cells.add(cell);
  }
  const edges = new Set<string>();
  for (const e of spec.edges) {
    if (!ids.has(e.from) || !ids.has(e.to)) problems.push(`edge ${edgeId(e)} names a missing node`);
    if (edges.has(edgeId(e))) problems.push(`duplicate edge ${edgeId(e)}`);
    edges.add(edgeId(e));
  }
  return problems;
}

/** Every source an entry names (tour, steps, evidence), for a check that each one exists. */
export const sourcesOf = (e: TourEntry<unknown>) => [...new Set([...e.source, ...e.steps.flatMap((s) => s.cite), ...(["demonstrated", "tested", "uncertain"] as const).flatMap((g) => (e.evidence?.[g] ?? []).map((i) => i.source))])];
