// Ring-width histogram for the review Charts view — the Qt tab's
// generateHistogram: valid cells with ringRatio > 0, 0.5-wide bins over the
// ring-ratio threshold range (here the file's recorded config, not the live
// one), y axis to ceil(1.1 × max count).
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { niceTicks, ringRatioHistogram } from "./chartMath";

const MARGIN = { left: 48, top: 28, right: 12, bottom: 40 };
const BAR = "#209fdf";

export function Histogram(props: { values: number[]; min: number; max: number }) {
  const { values, min, max } = props;
  const wrap = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState({ w: 400, h: 240 });
  const hist = useMemo(() => ringRatioHistogram(values, min, max), [values, min, max]);

  useLayoutEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const measure = () => setSize({ w: Math.max(160, el.clientWidth), h: Math.max(120, el.clientHeight) });
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  useEffect(() => {
    const c = canvas.current;
    const g = c?.getContext("2d");
    if (!c || !g) return;
    const dpr = window.devicePixelRatio || 1;
    c.width = Math.round(size.w * dpr);
    c.height = Math.round(size.h * dpr);
    c.style.width = `${size.w}px`;
    c.style.height = `${size.h}px`;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = "#fff";
    g.fillRect(0, 0, size.w, size.h);
    const pw = size.w - MARGIN.left - MARGIN.right;
    const ph = size.h - MARGIN.top - MARGIN.bottom;
    const yMax = Math.max(1, Math.ceil(hist.maxCount * 1.1));
    const sx = pw / (hist.max - hist.min);
    const sy = ph / yMax;

    g.font = "11px system-ui, sans-serif";
    g.lineWidth = 1;
    g.fillStyle = "#444";
    g.textAlign = "right";
    g.textBaseline = "middle";
    for (const t of niceTicks({ min: 0, max: yMax }, Math.max(2, Math.floor(ph / 40))).filter(Number.isInteger)) {
      const y = Math.round(MARGIN.top + ph - t * sy) + 0.5;
      g.strokeStyle = "#e6e6e6";
      g.beginPath();
      g.moveTo(MARGIN.left, y);
      g.lineTo(MARGIN.left + pw, y);
      g.stroke();
      g.fillText(String(t), MARGIN.left - 6, y);
    }
    g.fillStyle = BAR;
    hist.counts.forEach((n, k) => {
      if (n === 0) return;
      const x = MARGIN.left + k * hist.binWidth * sx;
      const w = Math.min(hist.binWidth, hist.max - (hist.min + k * hist.binWidth)) * sx;
      const h = n * sy;
      g.fillRect(x + 0.5, MARGIN.top + ph - h, Math.max(1, w - 1), h);
    });
    g.strokeStyle = "#999";
    g.strokeRect(MARGIN.left + 0.5, MARGIN.top + 0.5, pw, ph);
    g.fillStyle = "#444";
    g.textAlign = "center";
    g.textBaseline = "top";
    // Six ticks over the fixed range (Qt setTickCount(6)).
    for (let k = 0; k <= 5; k++) {
      const v = hist.min + ((hist.max - hist.min) * k) / 5;
      g.fillText(v.toFixed(1), MARGIN.left + (v - hist.min) * sx, MARGIN.top + ph + 4);
    }
    g.fillStyle = "#222";
    g.textBaseline = "alphabetic";
    g.font = "bold 13px system-ui, sans-serif";
    g.fillText("Ring Width Distribution", MARGIN.left + pw / 2, 18);
    g.font = "12px system-ui, sans-serif";
    g.fillText("Ring ratio", MARGIN.left + pw / 2, size.h - 6);
    g.save();
    g.translate(12, MARGIN.top + ph / 2);
    g.rotate(-Math.PI / 2);
    g.fillText("Frequency", 0, 0);
    g.restore();
    if (hist.maxCount === 0) {
      g.fillStyle = "#888";
      g.fillText("No ring ratios", MARGIN.left + pw / 2, MARGIN.top + ph / 2);
    }
  }, [size, hist]);

  return (
    <div ref={wrap} className="histogram-wrap">
      <canvas ref={canvas} aria-label="Ring width histogram" />
    </div>
  );
}
