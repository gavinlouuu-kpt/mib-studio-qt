// The Qt BatchMaskDialog's "Preview & ROI" and "Processing Config" groups (#867 REVIEW-059,
// -060, -061) as two presentational components shared by both Tauri Review surfaces (YOFO
// Review's Regenerate masks dialog and MIB Studio's Review stage panel). Each surface owns
// the state and the bridge calls; these only draw and report.
import { useEffect, useRef, useState, type ReactNode } from "react";
import {
  REGEN_CONFIG_FIELDS,
  coerceField,
  frameLabel,
  roiFromDrag,
  type RegenConfig,
  type RegenConfigField,
  type Roi,
} from "./regenerateForm";
import "./regenerate.css";

export interface PreviewFrame {
  width: number;
  height: number;
  image: ImageData;
}

/** Preview & ROI: the source frame with the ROI (drag to draw, Qt's RoiDrawCanvas), Prev /
 * Next and "Frame i / n", the ROI as numbers with Full frame, the background row (Set as
 * Background, Clear, `extraBackground`, its label) and the synthetic-background box. */
export function RoiPreviewPanel(props: {
  frame: PreviewFrame | null;
  loading: boolean;
  /** Why the frame could not be shown ("" = none). */
  error: string;
  /** 0-based preview index and the window size (null = unknown: Next stays on until a frame fails). */
  index: number;
  total: number | null;
  onIndex: (index: number) => void;
  roi: Roi | null;
  onRoi: (roi: Roi | null) => void;
  /** Where the ROI came from, e.g. "recorded in the file". */
  roiNote?: string;
  backgroundLabel: string;
  onSetBackground: () => void;
  onClearBackground: () => void;
  extraBackground?: ReactNode;
  synthetic: boolean;
  onSynthetic: (on: boolean) => void;
  disabled: boolean;
}) {
  const { frame, index, total, roi, disabled } = props;
  const canvas = useRef<HTMLCanvasElement>(null);
  const [drag, setDrag] = useState<{ a: [number, number]; b: [number, number] } | null>(null);
  const shown = drag && frame ? roiFromDrag(drag.a, drag.b, frame.width, frame.height) : roi;

  useEffect(() => {
    const node = canvas.current;
    if (!node) return;
    const ctx = node.getContext?.("2d");
    if (!frame) {
      node.width = 1;
      node.height = 1;
      ctx?.clearRect(0, 0, 1, 1);
      return;
    }
    node.width = frame.width;
    node.height = frame.height;
    if (!ctx) return;
    ctx.putImageData(frame.image, 0, 0);
    if (shown && shown.w > 0 && shown.h > 0) {
      ctx.strokeStyle = "#facc15";
      ctx.lineWidth = Math.max(1, Math.round(Math.max(frame.width, frame.height) / 400));
      ctx.strokeRect(shown.x + 0.5, shown.y + 0.5, shown.w - 1, shown.h - 1);
    }
  }, [frame, shown?.x, shown?.y, shown?.w, shown?.h]); // eslint-disable-line react-hooks/exhaustive-deps

  const toImage = (e: React.PointerEvent<HTMLCanvasElement>): [number, number] => {
    const r = e.currentTarget.getBoundingClientRect();
    const w = frame?.width ?? 1, h = frame?.height ?? 1;
    return [((e.clientX - r.left) / (r.width || 1)) * w, ((e.clientY - r.top) / (r.height || 1)) * h];
  };
  const atEnd = total !== null && index >= total - 1;
  const setRoiField = (key: keyof Roi, raw: string) => {
    const v = raw === "" ? 0 : Math.max(0, Math.floor(Number(raw)));
    if (!Number.isFinite(v)) return;
    props.onRoi({ ...(roi ?? { x: 0, y: 0, w: 0, h: 0 }), [key]: v });
  };

  return (
    <fieldset className="regen-group regen-preview" data-tour="regen-preview-roi">
      <legend>Preview &amp; ROI</legend>
      <div className="regen-canvas-wrap">
        <canvas
          ref={canvas}
          className="regen-canvas"
          aria-label="Source preview: drag to draw the ROI"
          style={{ display: frame ? undefined : "none", touchAction: "none" }}
          onPointerDown={(e) => {
            if (disabled || !frame) return;
            const p = toImage(e);
            e.currentTarget.setPointerCapture?.(e.pointerId);
            setDrag({ a: p, b: p });
          }}
          onPointerMove={(e) => drag && setDrag({ a: drag.a, b: toImage(e) })}
          onPointerUp={(e) => {
            if (!drag || !frame) return;
            const next = roiFromDrag(drag.a, toImage(e), frame.width, frame.height);
            setDrag(null);
            if (next) props.onRoi(next);
          }}
          onPointerCancel={() => setDrag(null)}
        />
        {!frame && <div className="regen-canvas-empty">{props.loading ? "Loading…" : props.error || (total === 0 ? "No frames" : "No preview")}</div>}
      </div>
      <div className="regen-nav" data-tour="regen-preview-nav">
        <button className="btn" disabled={disabled || props.loading || index <= 0} onClick={() => props.onIndex(index - 1)}>
          ← Prev
        </button>
        <span className="regen-frame-label" aria-live="polite">
          {frameLabel(index, total)}
        </span>
        <button className="btn" disabled={disabled || props.loading || atEnd || total === 0 || (total === null && !!props.error)} onClick={() => props.onIndex(index + 1)}>
          Next →
        </button>
      </div>
      {frame && props.error && <p className="form-error">{props.error}</p>}
      <div className="regen-roi" data-tour="regen-roi">
        {(["x", "y", "w", "h"] as const).map((k) => (
          <label key={k}>
            {k === "w" ? "Width" : k === "h" ? "Height" : k.toUpperCase()}
            <input type="number" min={0} step={1} value={roi ? roi[k] : 0} disabled={disabled} onChange={(e) => setRoiField(k, e.target.value)} aria-label={`ROI ${k}`} />
          </label>
        ))}
        <button className="btn" disabled={disabled || !roi} onClick={() => props.onRoi(null)} title="Process the whole frame">
          Full frame
        </button>
      </div>
      <p className="hint">{frame ? `${frame.width} × ${frame.height} px. ` : ""}Drag on the image to draw the ROI{props.roiNote ? `; ${props.roiNote}` : ""}.</p>
      <div className="regen-bg" data-tour="regen-background">
        <button className="btn" disabled={disabled || !frame} onClick={props.onSetBackground}>
          Set as Background
        </button>
        <button className="btn" disabled={disabled} onClick={props.onClearBackground}>
          Clear
        </button>
        {props.extraBackground}
        <span className="regen-bg-label">{props.backgroundLabel}</span>
      </div>
      <label className="check-row">
        <input type="checkbox" checked={props.synthetic} disabled={disabled} onChange={(e) => props.onSynthetic(e.target.checked)} /> Use synthetic background when none
        selected
      </label>
    </fieldset>
  );
}

function ConfigInput(props: { field: RegenConfigField; value: number; disabled: boolean; onChange: (v: number) => void }) {
  const { field, value } = props;
  // A draft while typing; coerced (Qt's spin box rules) on blur or Enter.
  const [draft, setDraft] = useState<string | null>(null);
  const commit = () => {
    if (draft === null) return;
    props.onChange(coerceField(field, Number(draft), value));
    setDraft(null);
  };
  return (
    <label className="regen-field">
      <span>{field.label}:</span>
      <input
        type="number"
        min={field.min}
        max={field.max}
        step={field.step}
        disabled={props.disabled}
        value={draft ?? (field.decimals ? value.toFixed(field.decimals) : String(value))}
        aria-label={field.label}
        onChange={(e) => {
          // A value the spin box would keep as is applies at once (spinner arrows, a
          // finished number); anything else (an even kernel, out of range) waits for blur.
          setDraft(e.target.value);
          const n = Number(e.target.value);
          if (e.target.value !== "" && Number.isFinite(n) && coerceField(field, n, value) === n) props.onChange(n);
        }}
        onBlur={commit}
        onKeyDown={(e) => e.key === "Enter" && commit()}
      />
      {field.suffix && <span className="hint">{field.suffix}</span>}
    </label>
  );
}

/** Processing Config: Image Processing (blur, BG threshold, morph kernel / iterations) and
 * Validation (area µm², deformability), local to this run, and the reset button. */
export function ProcessingConfigGroup(props: {
  config: RegenConfig;
  onChange: (config: RegenConfig) => void;
  onReset: () => void;
  resetLabel: string;
  /** Where the values came from, under the fields. */
  note: string;
  disabled: boolean;
}) {
  const field = (f: RegenConfigField) => (
    <ConfigInput key={f.key} field={f} value={props.config[f.key]} disabled={props.disabled} onChange={(v) => props.onChange({ ...props.config, [f.key]: v })} />
  );
  return (
    <fieldset className="regen-group regen-config" data-tour="regen-config">
      <legend>Processing Config</legend>
      <div className="regen-subgroup">
        <div className="regen-subtitle">Image Processing</div>
        {REGEN_CONFIG_FIELDS.filter((f) => f.group === "image").map(field)}
      </div>
      <div className="regen-subgroup">
        <div className="regen-subtitle">Validation</div>
        {REGEN_CONFIG_FIELDS.filter((f) => f.group === "validation").map(field)}
      </div>
      <button className="btn" disabled={props.disabled} onClick={props.onReset} data-tour="regen-config-reset">
        {props.resetLabel}
      </button>
      <p className="hint">{props.note}</p>
    </fieldset>
  );
}
