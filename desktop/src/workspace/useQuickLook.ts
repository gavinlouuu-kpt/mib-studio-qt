// The Summary sub-view's reads of the run quick look (#716 A7b; bridge ABI 44 `run_quick_look`, a read
// any viewer may make). No polling: the quick look is read
//
// - when the Summary is shown, and when what it reads changes (a run of the plan closed, Repeat
//   sample kept one, or the operator chose or cleared the reference run);
// - once the instrument turns idle (an active run ended) when the last reply had a run the backend
//   deferred ("computed when the instrument is idle"): the transition App already observes;
// - and on the operator's Refresh.
//
// An older reply never replaces a newer one. A read that fails keeps the last reply and says why.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type RunQuickLookReference, type RunQuickLookReply } from "../bridge";
import { hasDeferred } from "./quickLookModel";

export type QuickLookRead = (paths: string[], reference?: RunQuickLookReference | null) => Promise<RunQuickLookReply>;

export interface QuickLookState {
  reply: RunQuickLookReply | null;
  /** The last read failed: why ("" when it did not). */
  error: string;
  loading: boolean;
  refresh: () => void;
}

const defaultRead: QuickLookRead = (paths, reference) => bridge.runQuickLook(paths, reference);
const plain = (e: unknown) => String(e).replace(/^Error: /, "");

export function useQuickLook(paths: readonly string[], reference: RunQuickLookReference | null, experimentActive: boolean,
  read: QuickLookRead = defaultRead): QuickLookState {
  const [reply, setReply] = useState<RunQuickLookReply | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  const key = JSON.stringify([paths, reference]);
  const args = useRef({ paths, reference, read });
  args.current = { paths, reference, read };
  const replyRef = useRef(reply);
  replyRef.current = reply;
  const generation = useRef(0);
  const lastKey = useRef<string | null>(null);
  const wasActive = useRef(experimentActive);
  // Bumped each time the instrument turns idle: a read begun before it may hold a deferral that the
  // idle instrument now computes.
  const idleEpoch = useRef(0);
  const activeRef = useRef(experimentActive);
  activeRef.current = experimentActive;

  const doRead = useCallback(async (retryOnIdle = true) => {
    const { paths: p, reference: r, read: rd } = args.current;
    if (p.length === 0) return;
    const mine = ++generation.current;
    const epoch = idleEpoch.current;
    setLoading(true);
    try {
      const out = await rd([...p], r);
      if (mine !== generation.current) return;
      setReply(out);
      setError(out.ok ? "" : out.message);
      // The instrument turned idle while this read was answered: read once more for its deferrals.
      if (retryOnIdle && epoch !== idleEpoch.current && !activeRef.current && hasDeferred(out)) void doRead(false);
    } catch (e) {
      if (mine === generation.current) setError(plain(e));
    } finally {
      if (mine === generation.current) setLoading(false);
    }
  }, []);

  useEffect(() => {
    const idleNow = wasActive.current && !experimentActive;
    wasActive.current = experimentActive;
    if (idleNow) idleEpoch.current += 1;
    if (args.current.paths.length === 0) { lastKey.current = key; setReply(null); setError(""); return; }
    if (lastKey.current !== key) { lastKey.current = key; void doRead(); return; }
    if (idleNow && hasDeferred(replyRef.current)) void doRead();
  }, [key, experimentActive, doRead]);

  const refresh = useCallback(() => { void doRead(); }, [doRead]);
  return { reply, error, loading, refresh };
}
