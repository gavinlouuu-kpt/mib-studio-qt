// The camera's GenApi tree for Align (#806 E-3): read on open and whenever the caller's `epoch`
// changes (the camera started or stopped), again after every write reply, after a stale reply
// (with a bounded backoff, STALE_RETRY_DELAYS_MS; then it stops and the view says to Refresh), and
// on the operator's Refresh. A write goes out only on commit, with the token of the tree the draft
// was made against; the backend's reply is kept as it came. A write call that is rejected without
// a backend reply (VIEWER_ONLY and the server's other refusal codes, or a transport failure) is
// kept as a WriteFailure, never as a device outcome. Nothing here decides a value, an access or
// an outcome.
import { useCallback, useEffect, useRef, useState } from "react";
import type {
  CameraFeatureModule,
  CameraFeatureReply,
  CameraFeatureTreeReply,
  CameraFeatureTreeRequest,
  CameraFeatureValue,
  CameraFeatureWriteRequest,
} from "../bridge";
import { isWriteFailure, resolveBinding, writeFailure, writeSummary, type FeatureWriteResult } from "./cameraFeatures";
import { FACTS, reportFact, reportFactRefused } from "../tours/mib/facts";

export interface CameraFeatureApi {
  tree: (request: CameraFeatureTreeRequest) => Promise<CameraFeatureTreeReply>;
  write: (request: CameraFeatureWriteRequest) => Promise<CameraFeatureReply>;
}

/** The tree the quick controls and the browser read: the remote device (the camera itself). */
export const TREE_REQUEST: CameraFeatureTreeRequest = { modules: ["remote"] };

/** After a stale tree, read again after each of these delays, then stop (Refresh reads again). */
export const STALE_RETRY_DELAYS_MS: readonly number[] = [0, 250, 1000];

const failedTree = (reason: string): CameraFeatureTreeReply =>
  ({ schema_version: 1, outcome: "error", reason, evidence: "", session: null, modules: [] });

const sleep = (ms: number) => new Promise<void>((resolve) => { setTimeout(resolve, ms); });

export function useCameraFeatures(api: CameraFeatureApi | undefined, epoch: unknown) {
  const [tree, setTree] = useState<CameraFeatureTreeReply | null>(null);
  const [loading, setLoading] = useState(false);
  const [replies, setReplies] = useState<Record<string, FeatureWriteResult>>({});
  const [pending, setPending] = useState<Record<string, boolean>>({});
  const seq = useRef(0);
  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  const refresh = useCallback(async () => {
    if (!api) return;
    const mine = ++seq.current;
    setLoading(true);
    // A stale tree (the session moved while it was walked) is read again with a bounded backoff;
    // `loading` stays true while a retry is due, so the view says "reading again" only then.
    for (let attempt = 0; ; attempt++) {
      let reply: CameraFeatureTreeReply;
      try {
        reply = (await api.tree(TREE_REQUEST)) ?? failedTree("no reply from the backend");
      } catch (e) {
        reply = failedTree(String(e).replace(/^Error: /, ""));
      }
      if (!mounted.current || mine !== seq.current) return; // a newer read owns the view
      setTree(reply);
      if (reply.outcome !== "stale" || attempt >= STALE_RETRY_DELAYS_MS.length) break;
      await sleep(STALE_RETRY_DELAYS_MS[attempt]);
      if (!mounted.current || mine !== seq.current) return;
    }
    setLoading(false);
  }, [api]);

  useEffect(() => { void refresh(); }, [refresh, epoch]);

  /** Write one feature's value with the token of the tree it was drafted against. */
  const commit = useCallback(async (key: string, module: CameraFeatureModule, feature: string, value: CameraFeatureValue) => {
    if (!api || pending[key]) return;
    const r = resolveBinding(tree, module, feature);
    if (r.state !== "writable") return;
    setPending((p) => ({ ...p, [key]: true }));
    let reply: FeatureWriteResult;
    try {
      const answer: CameraFeatureReply | null | undefined = await api.write({ session: r.session, module, feature, value });
      reply = answer ?? writeFailure("no reply from the backend", value);
    } catch (e) {
      // Refused by the server before the backend acted (VIEWER_ONLY, INVALID_ARGUMENTS, ...) or a
      // transport failure: not a device outcome.
      reply = writeFailure(e, value);
    }
    if (!mounted.current) return;
    setReplies((all) => ({ ...all, [key]: reply }));
    setPending((p) => ({ ...p, [key]: false }));
    // Tours (E02): what the backend answered, as the write summary words it. Only a reply with a readback counts as done.
    const summary = writeSummary(reply);
    if (!isWriteFailure(reply) && reply.outcome === "readback") reportFact(FACTS.featureWrite, `${feature}: ${summary.label} · requested ${summary.requested} · ${summary.readback}`);
    else reportFactRefused(FACTS.featureWrite, `${feature}: ${summary.label}${summary.reason ? ` · ${summary.reason}` : ""}`);
    // After every write reply: access and values may have moved (the write itself, dependent
    // nodes, a new session), and a refusal re-reads the descriptor the backend refused against.
    void refresh();
  }, [api, tree, pending, refresh]);

  return { tree, loading, replies, pending, refresh, commit };
}
