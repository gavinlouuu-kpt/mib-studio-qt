// The scatter's gate overlay as a hook (epic #866, PR E1: MON-064..066, MON-068): turns the tune draft, the
// E-modulus table and the pointer into the `overlay` the Monitoring scatter takes, and writes a dragged handle
// back through `setTuneField`, so a gate dragged on the chart is the panel's own draft and still needs Apply.
// The drawing is gateOverlay.ts, the maths targetGate.ts, the handle rules gateHandles.ts.
import { useCallback, useMemo, useRef, useState } from "react";
import type { Viewport } from "../../../review/charts/chartMath";
import type { OverlayHover, ScatterOverlay } from "../charts/MonitoringScatter";
import type { PressInterceptor } from "../charts/useChartView";
import { emodulusReadout, tableLut, type EmodulusTableState, type PreparedLut } from "../emodulusLut";
import type { TuneFieldId, TuneState } from "../tuneDraft";
import { GateOverlayModel, type GateView } from "./gateOverlay";
import { gateUnapplied, handleCursor, handleEditable, handleToolTip } from "./gateHandles";
import {
  beginDrag, dragValue, fieldLimits, gateSpecFromValues, handleField, type DragSession, type GateSpec, type HandleKind,
} from "./targetGate";

const GATE_OFF: GateSpec = { enabled: false, areaMin: 0, areaMax: 0, deformMin: 0, deformMax: 0, emodEnabled: false, emodMin: 0, emodMax: 0 };

export interface GateOverlayInput {
  tune: TuneState;
  setTuneField: (field: TuneFieldId, value: unknown) => void;
  table: EmodulusTableState;
  /** Another client controls the instrument: the panel's controls are disabled, so are the handles. */
  viewerOnly: boolean;
}

export interface GateOverlayResult {
  overlay: ScatterOverlay;
  /** The draft gate (disabled while the running configuration is not known). */
  spec: GateSpec;
  lut: PreparedLut | null;
  /** The scatter tooltip's E readout (null: no table or no value there). */
  emodulusAt: (area: number, deformability: number) => number | null;
}

export function useGateOverlay({ tune, setTuneField, table, viewerOnly }: GateOverlayInput): GateOverlayResult {
  const model = useRef<GateOverlayModel>();
  model.current ??= new GateOverlayModel();
  const [hover, setHover] = useState<HandleKind>("none");
  const [dragging, setDragging] = useState<HandleKind>("none");

  const draft = tune.draft;
  const spec = useMemo(
    () => (draft ? gateSpecFromValues(draft) : GATE_OFF),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [draft?.targetGroupEnabled, draft?.targetAreaMin, draft?.targetAreaMax, draft?.targetDeformabilityMin, draft?.targetDeformabilityMax,
      draft?.targetEmodulusEnabled, draft?.targetEmodulusMin, draft?.targetEmodulusMax],
  );
  const lut = tableLut(table);
  const unapplied = gateUnapplied(tune);
  const dragText = draft && dragging !== "none" ? handleToolTip(dragging, draft, true) : "";
  const view: GateView = {
    spec, unapplied, lut, table: table.status, tableError: table.status === "error" ? table.message : "", hover, dragging, dragText,
  };

  // Latest values for the callbacks the scatter holds (they must not change identity with every edit).
  const live = useRef({ view, tune, viewerOnly, setTuneField });
  live.current = { view, tune, viewerOnly, setTuneField };
  const press = useRef<{ kind: Exclude<HandleKind, "none">; session: DragSession } | null>(null);

  const draw = useCallback((g: CanvasRenderingContext2D, vp: Viewport) => model.current!.draw(g, vp, live.current.view), []);

  const interceptor = useMemo<PressInterceptor>(() => ({
    press: (px, py, vp) => {
      press.current = null;
      const { view: v, tune: t, viewerOnly: viewer } = live.current;
      const hit = model.current!.hit(v, vp, px, py);
      if (hit.kind === "none" || !handleEditable(hit.kind, t, viewer)) return false;
      press.current = { kind: hit.kind, session: beginDrag(hit, v.spec, v.lut, vp, px, py) };
      return true;
    },
    move: (px, py, vp) => {
      const p = press.current;
      if (!p) return;
      const { view: v, tune: t, viewerOnly: viewer } = live.current;
      setDragging(p.kind);
      setHover("none");
      if (!handleEditable(p.kind, t, viewer)) return; // locked meanwhile (an apply began)
      const field = handleField(p.kind);
      const value = dragValue(p.session, v.spec, v.lut, vp, px, py, fieldLimits(field));
      if (value === null) return; // no E at this position: keep the last value
      live.current.setTuneField(field, value);
    },
    end: (px, py, vp, completed) => {
      press.current = null;
      setDragging("none");
      if (completed) {
        const { view: v, tune: t, viewerOnly: viewer } = live.current;
        const hit = model.current!.hit(v, vp, px, py);
        setHover(hit.kind !== "none" && handleEditable(hit.kind, t, viewer) ? hit.kind : "none");
      } else setHover("none");
    },
  }), []);

  const onHover = useCallback((px: number, py: number, vp: Viewport): OverlayHover | false => {
    const { view: v, tune: t, viewerOnly: viewer } = live.current;
    const hit = model.current!.hit(v, vp, px, py);
    const kind = hit.kind !== "none" && handleEditable(hit.kind, t, viewer) ? hit.kind : "none";
    setHover(kind);
    if (kind === "none" || !t.draft) return false;
    return { cursor: handleCursor(kind), tooltip: handleToolTip(kind, t.draft, false) };
  }, []);
  const onLeave = useCallback(() => setHover("none"), []);

  const version = [
    spec.enabled, spec.areaMin, spec.areaMax, spec.deformMin, spec.deformMax, spec.emodEnabled, spec.emodMin, spec.emodMax,
    unapplied, table.status === "loaded" ? table.table.identity : table.status, hover, dragging, dragText,
  ].join("|");
  const overlay = useMemo<ScatterOverlay>(
    () => ({ draw, interceptor, hover: onHover, onLeave, version }),
    [draw, interceptor, onHover, onLeave, version],
  );
  const emodulusAt = useCallback((area: number, deformability: number) => emodulusReadout(table, area, deformability), [table]);
  return { overlay, spec, lut, emodulusAt };
}
