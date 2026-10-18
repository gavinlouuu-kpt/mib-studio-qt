// A quick control's state over time (P3): the pure machine of quickControlModel.ts driven by the
// readback the shell polls and by the operator's explicit Write. The write goes through the
// caller's function (the classic tab's command path); a reply after WRITE_TIMEOUT_MS is stale.
import { useCallback, useEffect, useReducer, useRef } from "react";
import {
  initialControl,
  stepControl,
  WRITE_TIMEOUT_MS,
  type Checked,
  type Codec,
  type ControlEvent,
  type ControlState,
  type WriteResult,
} from "./quickControlModel";

export interface QuickControlOptions<V, D> {
  codec: Codec<V, D>;
  /** An empty draft, for a control with neither readback nor initial value. */
  empty: D;
  /** The device's current value (polled by the shell); null when it reports none. */
  readback: V | null;
  /** The draft before any readback (a default or a placement); null for none. */
  initial: V | null;
  /** Bounds and increments, checked before anything is sent. */
  check: (draft: D) => Checked<V>;
  /** The write through the existing command path, with the readback read after it. */
  write: (value: V) => Promise<WriteResult<V>>;
  timeoutMs?: number;
}

export function useQuickControl<V, D>(o: QuickControlOptions<V, D>) {
  const codecRef = useRef(o.codec);
  codecRef.current = o.codec;
  const [state, dispatch] = useReducer(
    (s: ControlState<V, D>, e: ControlEvent<V, D>) => stepControl(s, e, codecRef.current),
    undefined,
    () => initialControl(o.readback, o.initial, o.codec, o.empty),
  );
  const stateRef = useRef(state);
  stateRef.current = state;
  // Set again on every mount: StrictMode mounts, unmounts and mounts again in development.
  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  // Follow the readback by value (a new object with the same value is no change).
  const readbackKey = o.readback === null ? "" : JSON.stringify(o.readback);
  useEffect(() => { dispatch({ type: "observe", readback: o.readback }); }, [readbackKey]); // eslint-disable-line react-hooks/exhaustive-deps

  const write = o.write, check = o.check, timeoutMs = o.timeoutMs ?? WRITE_TIMEOUT_MS;
  const submit = useCallback(() => {
    const s = stateRef.current;
    if (s.phase === "pending") return;
    const checked = check(s.draft);
    if (!checked.ok) { dispatch({ type: "invalid", reason: checked.reason }); return; }
    const token = s.token + 1; // the token "submit" assigns
    dispatch({ type: "submit", value: checked.value, note: checked.note });
    const timer = window.setTimeout(() => { if (mounted.current) dispatch({ type: "timeout", token, ms: timeoutMs }); }, timeoutMs);
    void write(checked.value)
      .catch((e): WriteResult<V> => ({ ok: false, reason: String(e).replace(/^Error: /, "") }))
      .then((result) => {
        window.clearTimeout(timer);
        if (mounted.current) dispatch({ type: "settled", token, result });
      });
  }, [check, write, timeoutMs]);

  return {
    state,
    edit: useCallback((draft: D) => dispatch({ type: "edit", draft }), []),
    revert: useCallback(() => dispatch({ type: "revert" }), []),
    submit,
  };
}
