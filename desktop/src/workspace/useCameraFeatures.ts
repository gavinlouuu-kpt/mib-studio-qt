// The camera's GenApi tree for Align (#806 E-3): read on open and whenever the caller's `epoch`
// changes (the camera started or stopped), again after every write reply and after a stale reply,
// and on the operator's Refresh. A write goes out only on commit, with the token of the tree the
// draft was made against; the backend's reply is kept as it came. Nothing here decides a value,
// an access or an outcome.
import { useCallback, useEffect, useRef, useState } from "react";
import type {
  CameraFeatureModule,
  CameraFeatureReply,
  CameraFeatureTreeReply,
  CameraFeatureTreeRequest,
  CameraFeatureValue,
  CameraFeatureWriteRequest,
} from "../bridge";
import { resolveBinding } from "./cameraFeatures";

export interface CameraFeatureApi {
  tree: (request: CameraFeatureTreeRequest) => Promise<CameraFeatureTreeReply>;
  write: (request: CameraFeatureWriteRequest) => Promise<CameraFeatureReply>;
}

/** The tree the quick controls and the browser read: the remote device (the camera itself). */
export const TREE_REQUEST: CameraFeatureTreeRequest = { modules: ["remote"] };

const failedTree = (reason: string): CameraFeatureTreeReply =>
  ({ schema_version: 1, outcome: "error", reason, evidence: "", session: null, modules: [] });

/** A transport failure as a reply, so the view shows it like any other outcome. */
const failedWrite = (module: CameraFeatureModule, feature: string, value: CameraFeatureValue, reason: string): CameraFeatureReply =>
  ({ schema_version: 1, outcome: "unknown_write_outcome", reason, evidence: "", session: null,
    address: { module, feature, selector: null }, requested: value, readback: null });

export function useCameraFeatures(api: CameraFeatureApi | undefined, epoch: unknown) {
  const [tree, setTree] = useState<CameraFeatureTreeReply | null>(null);
  const [loading, setLoading] = useState(false);
  const [replies, setReplies] = useState<Record<string, CameraFeatureReply>>({});
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
    // A stale tree (the session moved while it was walked) is read once more at once.
    for (let attempt = 0; attempt < 2; attempt++) {
      let reply: CameraFeatureTreeReply;
      try {
        reply = (await api.tree(TREE_REQUEST)) ?? failedTree("no reply from the backend");
      } catch (e) {
        reply = failedTree(String(e).replace(/^Error: /, ""));
      }
      if (!mounted.current || mine !== seq.current) return; // a newer read owns the view
      setTree(reply);
      if (reply.outcome !== "stale") break;
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
    let reply: CameraFeatureReply;
    try {
      reply = (await api.write({ session: r.session, module, feature, value }))
        ?? failedWrite(module, feature, value, "no reply from the backend");
    } catch (e) {
      reply = failedWrite(module, feature, value, `no reply: ${String(e).replace(/^Error: /, "")}`);
    }
    if (!mounted.current) return;
    setReplies((all) => ({ ...all, [key]: reply }));
    setPending((p) => ({ ...p, [key]: false }));
    // After every write reply: access and values may have moved (the write itself, dependent
    // nodes, a new session), and a refusal re-reads the descriptor the backend refused against.
    void refresh();
  }, [api, tree, pending, refresh]);

  return { tree, loading, replies, pending, refresh, commit };
}
