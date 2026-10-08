import { useEffect, useState } from "react";
import type { CmdResult, LedLimits } from "../bridge";

// PZ7035 LED in Service / Commissioning mode (#501 P1). Operators get the per-mode presets (the
// backend applies them on every mode switch); raw delay/width is for commissioning only, and the
// backend refuses it outside Service mode and outside the mode's limits as well.

// Align: 100/135 µs at 400 fps (whole frames, results8 on); 0/125 µs for the banded fallback.
export const LED_PRESETS = { run: { delay: 7, width: 60 }, align: { delay: 100, width: 135 }, alignBands: { delay: 0, width: 125 } } as const;
const STEP_US = 0.5;

type Props = {
  mode: "run" | "align";
  /** Align on images before results8 uses the banded preview and its own preset. */
  alignBands?: boolean;
  limits: LedLimits | undefined;
  current: { delay_us: number; width_us: number; on: boolean; guard_fault?: boolean; guard_trips?: number } | undefined;
  disabled: boolean;
  apply: (delayUs: number, widthUs: number) => Promise<CmdResult>;
  append: (line: string) => void;
};

export function InstrumentLedControls({ mode, alignBands = false, limits, current, disabled, apply, append }: Props) {
  const preset = LED_PRESETS[mode === "align" && alignBands ? "alignBands" : mode];
  const [delay, setDelay] = useState(String(preset.delay));
  const [width, setWidth] = useState(String(preset.width));
  // A mode switch restores the preset on the board; follow it here.
  useEffect(() => { setDelay(String(preset.delay)); setWidth(String(preset.width)); }, [preset]);

  const send = async (d: number, w: number) => {
    setDelay(String(d)); setWidth(String(w));
    const r = await apply(d, w);
    append(r.ok ? r.message : `LED: ${r.message}`);
  };
  const clamp = (v: number, lo: number | undefined, hi: number | undefined) => Math.min(Math.max(v, lo ?? -Infinity), hi ?? Infinity);
  const nudge = (field: "delay" | "width", step: number) => {
    const snap = (v: number) => Math.round(v / STEP_US) * STEP_US;
    if (field === "width") void send(Number(delay), clamp(snap(Number(width) + step), limits?.width_min_us, limits?.width_max_us));
    else void send(clamp(snap(Number(delay) + step), limits?.delay_min_us, limits?.delay_max_us), Number(width));
  };
  const range = limits
    ? `delay ${limits.delay_min_us}–${limits.delay_max_us} µs, width ${limits.width_min_us}–${limits.width_max_us} µs`
    : "";

  return (
    <fieldset className="instrument-led" aria-label="LED (Service mode)">
      <legend>LED for {mode === "run" ? "Run" : "Align"} (Service mode)</legend>
      <p className="mono">
        Board: {current ? (current.on ? `${current.delay_us.toFixed(1)} / ${current.width_us.toFixed(1)} µs` : "off") : "—"}
        {" · "}preset {preset.delay} / {preset.width} µs{range ? ` · limits ${range}` : ""}
      </p>
      {/* Read-only: the strobe control S[0], the guard and its trip count, as the PL reports them. */}
      <p className="mono" aria-label="LED readout">
        Strobe (S[0]): {current ? (current.on ? "on (1)" : "off (0)") : "—"}
        {" · "}guard: {current ? (current.guard_fault ? "FAULT" : "ok") : "—"}
        {" · "}trips: {current?.guard_trips ?? "—"}
      </p>
      {current?.guard_fault && <p role="alert">The strobe guard tripped: the LED is held off. Switching a camera mode clears it.</p>}
      <label>Delay (µs)<input type="number" step={STEP_US} value={delay} disabled={disabled} onChange={(e) => setDelay(e.target.value)} /></label>
      <label>Width (µs)<input type="number" step={STEP_US} value={width} disabled={disabled} onChange={(e) => setWidth(e.target.value)} /></label>
      <button disabled={disabled} onClick={() => nudge("delay", -STEP_US)}>Delay − {STEP_US} µs</button>
      <button disabled={disabled} onClick={() => nudge("delay", STEP_US)}>Delay + {STEP_US} µs</button>
      <button disabled={disabled} onClick={() => nudge("width", -STEP_US)}>Width − {STEP_US} µs</button>
      <button disabled={disabled} onClick={() => nudge("width", STEP_US)}>Width + {STEP_US} µs</button>
      <button disabled={disabled} onClick={() => void send(Number(delay), Number(width))}>Apply</button>
      <button disabled={disabled} onClick={() => void send(preset.delay, preset.width)}>Restore preset</button>
    </fieldset>
  );
}
