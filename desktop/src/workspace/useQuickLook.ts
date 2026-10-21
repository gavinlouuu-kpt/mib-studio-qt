// The Summary sub-view's reads of the run quick look (#716 A7b; bridge ABI 44 `run_quick_look`, a read
// any viewer may make). No polling: the quick look is read
//
// - when the Summary is shown, and when what it reads changes (a run of the plan closed, Repeat
//   sample kept one, or the operator chose or cleared the reference run);
// - once the instrument turns idle (an active run ended) when the last reply had a run, or the
//   reference, the backend deferred ("computed when the instrument is idle"): the transition App
//   already observes (also when it happens while a read is being answered);
// - and on the operator's Refresh.
//
// A reply is shown only for what it was read for: a change invalidates every read in flight, so a
// late answer for an earlier reference never shows its deltas under a newer one, and an older reply
// never replaces a newer one. While the reference changes, the last reply of the same paths keeps
// showing the runs' own numbers, without its deltas and its reference. A read that fails says why
// and keeps the last reply.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type RunQuickLookReference, type RunQuickLookReply } from "../bridge";
import { hasDeferred } from "./quickLookModel";

export type QuickLookRead = (paths: string[], reference?: RunQuickLookReference | null) => Promise<RunQuickLookReply>;

export interface QuickLookState {
  /** The last reply read for the current paths and reference; null before one. */
  reply: RunQuickLookReply | null;
  /** The last read for them failed: why ("" when it did not). */
  error: string;
  loading: boolean;
  refresh: () => void;
}

const defaultRead: QuickLookRead = (paths, reference) => bridge.runQuickLook(paths, reference);
const plain = (e: unknown) => String(e).replace(/^Error: /, "");

interface Held { key: string; pathsKey: string; reply: RunQuickLookReply | null; error: string }

/** A reply read for another reference: the runs' own numbers stand, its deltas and reference do not. */
const withoutReference = (r: RunQuickLookReply): RunQuickLookReply => ({ ...r, reference: null, runs: r.runs.map((run) => ({ ...run, delta: null })) });

export function useQuickLook(paths: readonly string[], reference: RunQuickLookReference | null, experimentActive: boolean,
  read: QuickLookRead = defaultRead): QuickLookState {
  const key = JSON.stringify([paths, reference]);
  const pathsKey = JSON.stringify(paths);
  const [held, setHeld] = useState<Held>({ key, pathsKey, reply: null, error: "" });
  const [loading, setLoading] = useState(false);
  const args = useRef({ key, pathsKey, paths, reference, read });
  args.current = { key, pathsKey, paths, reference, read };
  const reply = held.key === key ? held.reply : held.pathsKey === pathsKey && held.reply ? withoutReference(held.reply) : null;
  const error = held.pathsKey === pathsKey ? held.error : "";
  const replyRef = useRef<RunQuickLookReply | null>(null);
  replyRef.current = reply;
  // Bumped by every read and by every change of what is read: only the newest read may answer.
  const generation = useRef(0);
  const lastKey = useRef<string | null>(null);
  const wasActive = useRef(experimentActive);
  // Bumped each time the instrument turns idle: a read begun before it may hold a deferral that the
  // idle instrument now computes.
  const idleEpoch = useRef(0);
  const activeRef = useRef(experimentActive);
  activeRef.current = experimentActive;

  const doRead = useCallback(async (retryOnIdle = true) => {
    const { key: k, pathsKey: pk, paths: p, reference: r, read: rd } = args.current;
    if (p.length === 0) return;
    const mine = ++generation.current;
    const epoch = idleEpoch.current;
    setLoading(true);
    try {
      const out = await rd([...p], r);
      if (mine !== generation.current) return;
      setHeld({ key: k, pathsKey: pk, reply: out, error: out.ok ? "" : out.message });
      // The instrument turned idle while this read was answered: read once more for its deferrals.
      if (retryOnIdle && epoch !== idleEpoch.current && !activeRef.current && hasDeferred(out)) void doRead(false);
    } catch (e) {
      if (mine === generation.current) setHeld((h) => ({ ...h, error: plain(e) }));
    } finally {
      if (mine === generation.current) setLoading(false);
    }
  }, []);

  useEffect(() => {
    const idleNow = wasActive.current && !experimentActive;
    wasActive.current = experimentActive;
    if (idleNow) idleEpoch.current += 1;
    if (lastKey.current !== key) {
      lastKey.current = key;
      generation.current += 1; // whatever is in flight was read for something else
      setLoading(false);
      if (args.current.paths.length === 0) { setHeld({ key, pathsKey, reply: null, error: "" }); return; }
      void doRead();
      return;
    }
    if (idleNow && hasDeferred(replyRef.current)) void doRead();
  }, [key, pathsKey, experimentActive, doRead]);

  const refresh = useCallback(() => { void doRead(); }, [doRead]);
  return { reply, error, loading, refresh };
}
