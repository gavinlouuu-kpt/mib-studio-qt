// Guided tours: the host component an app renders once. It owns the "what's new" notice, the Tours list, the flowchart
// panel and the running tour, and connects the runner to the app through plain callbacks. Generic.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { feedbackStore, safeStore } from "./feedback";
import { FlowchartView } from "./FlowchartView";
import type { TourAction, TourEntry, TourView } from "./registry";
import { startTour, type ConditionState, type RunnerHost, type TourRun } from "./runner";
import { newEntries, seenMarker, ToursList, WhatsNewNotice } from "./WhatsNew";
import "./tours.css";

export interface TourHostProps {
  /** The entries this app offers here (already filtered by their `when`). */
  entries: TourEntry<unknown>[];
  /** localStorage key prefix, e.g. "yofo.zu15eg.tours". */
  storageKey: string;
  navigate: (view: TourView) => void;
  currentView: () => TourView;
  /** Named conditions action steps and canvas targets wait for, read from the app's backend state. */
  conditions: Record<string, () => ConditionState>;
  /** Why action steps are not allowed now, or null. Told the step's action (see `TourAction.needsCamera`). */
  actionBlocked: (action?: TourAction) => string | null;
  /** A tour is about to start (before its first step is shown): the app may forget what it confirmed earlier, so a step waits
   *  for what the operator does during the tour. */
  onTourStart?: (entry: TourEntry<unknown>) => void;
  /** The short detail of a met condition, for the action's `{detail}` placeholder. */
  detail?: (name: string) => string | undefined;
  /** Open a collapsed section that holds a step's element (view state only). */
  reveal?: (el: Element) => void;
  /** Who confirms an action ("the board", the default; "the backend"). */
  confirmedBy?: string;
  /** True while the notice must not show (an experiment Starting or Active, anything recording). */
  quiet: boolean;
  /** The Help menu's "Tours and what's new" entry opens the list. */
  listOpen: boolean;
  onListClose: () => void;
  /** Added to the feedback export (e.g. the instrument). */
  exportMeta?: Record<string, unknown>;
  /** Tests only: overrides passed to the runner. */
  runnerOverrides?: Partial<RunnerHost>;
}

export function TourHost(props: TourHostProps) {
  const store = useMemo(() => safeStore(), []);
  const marker = useMemo(() => seenMarker(store, `${props.storageKey}.seen`), [store, props.storageKey]);
  const feedback = useMemo(() => feedbackStore(store, `${props.storageKey}.feedback`), [store, props.storageKey]);
  const [seen, setSeen] = useState(() => marker.seen());
  const [dismissed, setDismissed] = useState(false);
  const [ownList, setOwnList] = useState(false);
  const [flow, setFlow] = useState<TourEntry<unknown> | null>(null);
  const [touring, setTouring] = useState(false);
  const run = useRef<TourRun | null>(null);
  const live = useRef(props);
  live.current = props;

  // Unmounting (the instrument changed, the page closes) ends a running tour.
  useEffect(() => () => run.current?.stop({ restoreView: false }), []);

  const fresh = newEntries(props.entries, seen);
  const markSeen = useCallback((ids: string[]) => {
    marker.markSeen(ids);
    setSeen(marker.seen());
  }, [marker]);

  const listOpen = (props.listOpen || ownList) && !touring;
  const closeList = () => {
    setOwnList(false);
    if (props.listOpen) props.onListClose();
  };
  // The list marks every tour seen, but keeps the "new" badges it opened with.
  const [listNew, setListNew] = useState<Set<string>>(new Set());
  const freshIds = fresh.map((e) => e.id).join(" ");
  const freshRef = useRef(freshIds);
  freshRef.current = freshIds;
  useEffect(() => {
    if (!listOpen) return;
    setListNew(new Set(freshRef.current.split(" ").filter(Boolean)));
    markSeen(live.current.entries.map((e) => e.id));
  }, [listOpen, markSeen]);

  const play = useCallback(async (entry: TourEntry<unknown>) => {
    run.current?.stop();
    setOwnList(false);
    if (live.current.listOpen) live.current.onListClose();
    setFlow(null);
    setTouring(true);
    markSeen([entry.id]);
    live.current.onTourStart?.(entry);
    const host: RunnerHost = {
      navigate: (v) => live.current.navigate(v),
      currentView: () => live.current.currentView(),
      showFlow: (e) => setFlow(e),
      condition: (name) => live.current.conditions[name]?.() ?? { failed: `unknown condition ${name}` },
      actionBlocked: (action) => live.current.actionBlocked(action),
      detail: (name) => live.current.detail?.(name),
      reveal: (el) => live.current.reveal?.(el),
      confirmedBy: live.current.confirmedBy,
      feedback,
      onEnd: () => { run.current = null; setTouring(false); setFlow(null); },
      ...live.current.runnerOverrides,
    };
    try {
      run.current = await startTour(entry, host);
    } catch (error) {
      console.error("tour failed to start", error);
      setTouring(false);
      setFlow(null);
    }
  }, [feedback, markSeen]);

  const take = () => {
    setDismissed(true);
    if (fresh.length === 1) void play(fresh[0]);
    else setOwnList(true);
  };

  return (
    <>
      {!props.quiet && !touring && !listOpen && !flow && !dismissed && (
        <WhatsNewNotice count={fresh.length} onTake={take} onDismiss={() => { setDismissed(true); markSeen(fresh.map((e) => e.id)); }} />
      )}
      {listOpen && (
        <ToursList entries={props.entries} newIds={listNew} feedback={feedback} exportMeta={props.exportMeta}
          onPlay={(e) => void play(e)} onFlow={(e) => { closeList(); setFlow(e); }} onClose={closeList} />
      )}
      {flow && <FlowchartView entry={flow} touring={touring} close={() => setFlow(null)} />}
    </>
  );
}
