// Guided tours: the flowchart panel. Draws an entry's before/after (and planned) flowcharts from their specs as plain SVG,
// with its evidence panel, and marks every node and edge with `data-tour="flow:<role>:<id>"` for the runner. Generic.
import { useEffect } from "react";
import { edgeId, EVIDENCE_TARGET, flowTarget, type EvidenceItem, type FlowRole, type FlowSpec, type TourEntry } from "./registry";

const COL = 118;   // grid column width
const ROW = 60;    // grid row height
const BOX_W = 106;
const BOX_H = 40;
const PAD = 6;
const TEXT_W = BOX_W - 8;
/** Long text is squeezed to the box (SVG textLength) instead of running into the next box; the estimate is generous. */
const fit = (text: string, px: number) => (text.length * px * 0.6 > TEXT_W ? { textLength: TEXT_W, lengthAdjust: "spacingAndGlyphs" as const } : {});

const ROLE_LABEL: Record<FlowRole, string> = { before: "Before", after: "After", planned: "Planned (not built)" };
export const FLOW_ORDER: FlowRole[] = ["before", "after", "planned"];

type Box = { x: number; y: number; cx: number; cy: number };

function boxOf(col: number, row: number): Box {
  const x = PAD + col * COL + (COL - BOX_W) / 2;
  const y = PAD + row * ROW;
  return { x, y, cx: x + BOX_W / 2, cy: y + BOX_H / 2 };
}

/** The path of an edge between two boxes, and where its label goes. */
export function edgePath(a: Box, b: Box): { d: string; lx: number; ly: number } {
  if (b.y > a.y) {
    const sx = a.cx, sy = a.y + BOX_H, tx = b.cx, ty = b.y;
    const my = (sy + ty) / 2;
    return { d: sx === tx ? `M${sx},${sy} L${tx},${ty}` : `M${sx},${sy} C${sx},${my} ${tx},${my} ${tx},${ty}`, lx: (sx + tx) / 2, ly: my };
  }
  if (b.y < a.y) {
    const sx = a.cx, sy = a.y, tx = b.cx, ty = b.y + BOX_H;
    const my = (sy + ty) / 2;
    return { d: sx === tx ? `M${sx},${sy} L${tx},${ty}` : `M${sx},${sy} C${sx},${my} ${tx},${my} ${tx},${ty}`, lx: (sx + tx) / 2, ly: my };
  }
  const right = b.x > a.x;
  const sx = right ? a.x + BOX_W : a.x, tx = right ? b.x : b.x + BOX_W;
  return { d: `M${sx},${a.cy} L${tx},${b.cy}`, lx: (sx + tx) / 2, ly: a.cy - 4 };
}

export function FlowChart({ spec, role, chartId }: { spec: FlowSpec; role: FlowRole; chartId: string }) {
  const rows = Math.max(...spec.nodes.map((n) => n.row)) + 1;
  const width = spec.cols * COL + 2 * PAD;
  const height = (rows - 1) * ROW + BOX_H + 2 * PAD;
  const byId = new Map(spec.nodes.map((n) => [n.id, boxOf(n.col, n.row)]));
  const marker = `tour-arrow-${chartId}-${role}`;
  return (
    <figure className={`tour-flow tour-flow-${role}`} data-tour={flowTarget(role)}>
      <figcaption><span className={`tour-flow-role ${role}`}>{ROLE_LABEL[role]}</span> {spec.title}</figcaption>
      <svg viewBox={`0 0 ${width} ${height}`} width={width} role="img" aria-label={`${ROLE_LABEL[role]}: ${spec.title}`}>
        <defs>
          <marker id={marker} viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
            <path d="M0,0 L10,5 L0,10 z" className="tour-arrowhead" />
          </marker>
        </defs>
        {spec.edges.map((e) => {
          const a = byId.get(e.from), b = byId.get(e.to);
          if (!a || !b) return null;
          const p = edgePath(a, b);
          return (
            <g key={edgeId(e)} className={`tour-edge${e.dashed ? " dashed" : ""}`} data-tour={flowTarget(role, edgeId(e))}>
              <path d={p.d} className="tour-edge-hit" />
              <path d={p.d} className="tour-edge-line" markerEnd={`url(#${marker})`} />
              {e.label && <text x={p.lx + 4} y={p.ly} className="tour-edge-label">{e.label}</text>}
            </g>
          );
        })}
        {spec.nodes.map((n) => {
          const b = byId.get(n.id)!;
          return (
            <g key={n.id} className={`tour-node tone-${n.tone ?? "neutral"}`} data-tour={flowTarget(role, n.id)}>
              <rect x={b.x} y={b.y} width={BOX_W} height={BOX_H} rx="5" />
              <text x={b.cx} y={n.sub ? b.y + 16 : b.cy + 4} className="tour-node-label" {...fit(n.label, 11.5)}>{n.label}</text>
              {n.sub && <text x={b.cx} y={b.y + 31} className="tour-node-sub" {...fit(n.sub, 9.5)}>{n.sub}</text>}
            </g>
          );
        })}
      </svg>
      {spec.legend && <p className="tour-flow-legend">{spec.legend}</p>}
    </figure>
  );
}

function EvidenceList({ label, items, kind }: { label: string; items: EvidenceItem[]; kind: string }) {
  return (
    <section className={`tour-evidence-group ${kind}`}>
      <h4>{label}</h4>
      {items.length === 0 ? <p className="tour-evidence-none">None recorded in the sources.</p> : (
        <ul>
          {items.map((i, n) => (
            <li key={n}>
              {i.date && <strong>{i.date}: </strong>}
              {i.test && <code>{i.test}</code>}{i.test && ": "}
              {i.text} <span className="tour-evidence-source">({i.source})</span>
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}

export function EvidencePanel({ entry }: { entry: TourEntry<unknown> }) {
  if (!entry.evidence) return null;
  return (
    <div className="tour-evidence" data-tour={EVIDENCE_TARGET} aria-label="Evidence">
      <EvidenceList label={entry.evidence.demonstratedLabel ?? "Demonstrated (on the hardware)"} items={entry.evidence.demonstrated} kind="demonstrated" />
      <EvidenceList label="Tested" items={entry.evidence.tested} kind="tested" />
      <EvidenceList label="Uncertain" items={entry.evidence.uncertain} kind="uncertain" />
      <p className="tour-evidence-note">A diagram is not proof of behaviour: only the first two lists are.</p>
    </div>
  );
}

/** The modal: every flowchart of the entry side by side (stacked on a narrow screen), then the evidence. While a tour
 *  runs (`touring`), Driver.js owns Esc and the close button is hidden. */
export function FlowchartView({ entry, touring, close }: { entry: TourEntry<unknown>; touring: boolean; close: () => void }) {
  useEffect(() => {
    if (touring) return;
    const onKey = (e: KeyboardEvent) => { if (e.key === "Escape") close(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [touring, close]);
  const roles = FLOW_ORDER.filter((r) => entry.flows?.[r]);
  return (
    <div className="modal-backdrop tour-flow-backdrop" onClick={touring ? undefined : close}>
      <div className="modal tour-flow-modal" role="dialog" aria-label={entry.title} onClick={(e) => e.stopPropagation()}>
        <div className="tour-flow-head">
          <h3>{entry.title}</h3>
          {!touring && <button className="btn" onClick={close}>Close</button>}
        </div>
        <div className={`tour-flow-grid n${roles.length}`}>
          {roles.map((r) => <FlowChart key={r} spec={entry.flows![r]!} role={r} chartId={entry.id} />)}
        </div>
        <EvidencePanel entry={entry} />
      </div>
    </div>
  );
}
