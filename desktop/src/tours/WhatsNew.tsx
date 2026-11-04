// Guided tours: "what's new" (the per-browser marker, the notice) and the Tours list. Generic.
// The marker is the set of tour ids this browser has seen (played, listed or dismissed), so a tour added later with an
// older `introduced` date still counts as new. Never starts a tour by itself.
import type { FeedbackStore, SafeStore } from "./feedback";
import { FLOW_ORDER } from "./FlowchartView";
import type { TourEntry } from "./registry";

export interface SeenMarker {
  seen(): Set<string>;
  markSeen(ids: Iterable<string>): void;
}

export function seenMarker(store: SafeStore, key: string): SeenMarker {
  const read = () => {
    try {
      const v = JSON.parse(store.get(key) ?? "null");
      return new Set<string>(Array.isArray(v?.seen) ? v.seen.filter((x: unknown) => typeof x === "string") : []);
    } catch {
      return new Set<string>();
    }
  };
  return {
    seen: read,
    markSeen(ids) {
      const all = read();
      for (const id of ids) all.add(id);
      store.set(key, JSON.stringify({ v: 1, seen: [...all].sort() }));
    },
  };
}

/** The entries this browser has not seen, newest first. */
export function newEntries<E extends Pick<TourEntry, "id" | "introduced">>(entries: E[], seen: Set<string>): E[] {
  return entries.filter((e) => !seen.has(e.id)).sort((a, b) => b.introduced.date.localeCompare(a.introduced.date));
}

export function WhatsNewNotice({ count, onTake, onDismiss }: { count: number; onTake: () => void; onDismiss: () => void }) {
  if (count <= 0) return null;
  return (
    <div className="tour-notice" role="status" data-testid="tour-notice">
      <span>{count} new: </span>
      <button type="button" className="tour-notice-take" onClick={onTake}>take the tour</button>
      <button type="button" className="tour-notice-dismiss" aria-label="Dismiss" title="Dismiss" onClick={onDismiss}>×</button>
    </div>
  );
}

export function ToursList({ entries, newIds, feedback, exportMeta, onPlay, onFlow, onClose }: {
  entries: TourEntry<unknown>[];
  newIds: Set<string>;
  feedback: FeedbackStore;
  exportMeta?: Record<string, unknown>;
  onPlay: (e: TourEntry<unknown>) => void;
  onFlow: (e: TourEntry<unknown>) => void;
  onClose: () => void;
}) {
  const records = feedback.all();
  const exportHref = `data:application/json;charset=utf-8,${encodeURIComponent(feedback.exportJson(exportMeta))}`;
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal tour-list-modal" role="dialog" aria-label="Tours and what's new" onClick={(e) => e.stopPropagation()}>
        <h3>Tours and what's new</h3>
        <p className="tour-list-intro">Each tour shows where a feature is, or steps through how a part of the system works. A tour never
          sends a command or changes a setting.</p>
        <ul className="tour-list">
          {entries.map((e) => {
            const verdicts = records.filter((r) => r.tour === e.id);
            const last = verdicts[verdicts.length - 1];
            return (
              <li key={e.id} data-tour-entry={e.id} className={newIds.has(e.id) ? "new" : ""}>
                <div className="tour-list-main">
                  <strong>{e.title}</strong>
                  {newIds.has(e.id) && <span className="tour-new-badge">new</span>}
                  <span className="tour-kind">{e.kind === "backend" ? "how it works" : "feature"}</span>
                  <div className="tour-list-summary">{e.summary}</div>
                  <div className="tour-list-meta">Since {e.introduced.date} · {e.introduced.by}{last ? ` · your feedback: ${last.verdict}` : ""}</div>
                </div>
                <div className="tour-list-actions">
                  <button className="btn" onClick={() => onPlay(e)}>Take the tour</button>
                  {FLOW_ORDER.some((r) => e.flows?.[r]) && <button className="btn" onClick={() => onFlow(e)}>Flowcharts</button>}
                </div>
              </li>
            );
          })}
        </ul>
        <div className="tour-list-foot">
          <a className="btn" href={exportHref} download="tour-feedback.json">Export feedback (JSON, {records.length})</a>
          <button className="btn" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}
