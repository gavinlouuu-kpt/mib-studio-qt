// A disclosure row for the summary-first preflight (O1b, approved by Gavin 2026-10-22). Kit gap
// #715: @mib/ui has no disclosure component, so this is a native <details> styled with the kit's
// `--mib-*` tokens (disclosure.css), kept local until the kit has one.
//
// The summary click is controlled, so an operator's open is told apart from an automatic one
// (`data-open`: issue, auto, user or closed):
// - `issue`: the row has a counted issue. It is open and cannot be collapsed while the issue lasts.
// - `auto`: it opens by itself (e.g. Calibration with a proposed record), and the operator may close it.
// - The operator's choice (`choice`) wins over `auto`; once an issue clears, the row returns to it.
// The body is always rendered; a closed <details> hides it. textContent therefore includes hidden
// content: a test of what is shown checks each element against its closed <details> ancestors
// (`isShown`).
import type { ReactNode } from "react";
import { StateChip, type StateTone } from "@mib/ui";
import type { RowOpenState } from "./prepareSummaryModel";
import "./disclosure.css";

export interface DisclosureRowProps {
  /** `data-row`, for tests and the harness. */
  rowKey: string;
  title: ReactNode;
  /** One line after the title. */
  summary: ReactNode;
  chip?: { tone: StateTone; text: string } | null;
  /** A quiet row (optional hardware that is not connected). */
  quiet?: boolean;
  state: Partial<RowOpenState> & { issue: boolean };
  /** The operator's last choice; undefined while they never opened or closed it. */
  choice: boolean | undefined;
  onChoice: (open: boolean) => void;
  children: ReactNode;
}

/** Whether the row is open: an issue keeps it open; else the operator's choice, else `auto`. */
export const rowOpen = (state: DisclosureRowProps["state"], choice: boolean | undefined): boolean =>
  state.issue || (choice ?? !!state.auto);

export function DisclosureRow(p: DisclosureRowProps) {
  const open = rowOpen(p.state, p.choice);
  const how = p.state.issue ? "issue" : p.choice !== undefined ? (p.choice ? "user" : "closed") : p.state.auto ? "auto" : "closed";
  return (
    <details className={`ws-row${p.quiet ? " ws-row--quiet" : ""}`} open={open} data-row={p.rowKey} data-open={how}
      onToggle={(e) => {
        // The browser may open a row itself (find in page): keep that as the operator's choice, and
        // never let a row with an issue close.
        const el = e.currentTarget;
        if (el.open === open) return;
        if (p.state.issue) el.open = true;
        else p.onChoice(el.open);
      }}>
      <summary aria-disabled={p.state.issue || undefined} title={p.state.issue ? "Open while it has an issue" : undefined}
        onClick={(e) => { e.preventDefault(); if (!p.state.issue) p.onChoice(!open); }}>
        <span className="ws-row__chev" aria-hidden="true">{open ? "▼" : "▶"}</span>
        <span className="ws-row__title">{p.title}</span>
        <span className="ws-row__summary">{p.summary}</span>
        {p.chip ? <StateChip tone={p.chip.tone} size={p.quiet ? "small" : "default"}>{p.chip.text}</StateChip> : <span />}
      </summary>
      <div className="ws-row__body">{p.children}</div>
    </details>
  );
}

/** Whether an element is shown: no closed <details> hides it (a closed row still shows its own
 *  summary). For tests and checks; textContent counts hidden content too. */
export function isShown(el: Element): boolean {
  for (let d = el.closest("details"); d; d = d.parentElement?.closest("details") ?? null) {
    if (!d.open && !d.querySelector(":scope > summary")?.contains(el)) return false;
  }
  return true;
}
