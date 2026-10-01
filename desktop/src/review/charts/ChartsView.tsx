// The review Charts view (plan 2026-10-01-standalone-review-app, PR 3): the
// Qt HdfReviewTab layout — scatter on the left, the selected cell's frame
// pane over the ring-width histogram on the right. Data comes from the
// review bridge: the columnar scatter, the density job's per-point levels
// (started on open, polled until ready), stored KDE core records from the
// file info, embedded isoelastic curves, and the compute-core job.
import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { ask } from "@tauri-apps/plugin-dialog";
import { reviewBridge, type ReviewDensity, type ReviewInfo, type ReviewScatter as ScatterData } from "../reviewBridge";
import { recordContours } from "./chartMath";
import { Histogram } from "./Histogram";
import { ReviewScatter, type ContourFamily, type IsoCurve, type MenuItem } from "./ReviewScatter";

const FULL_RUN = "#2a78d6";
const LIVE = "#eb6834";
const POLL_MS = 300;
const DEFAULT_CORE_FRACTION = 0.9;

let curvesOnce: Promise<IsoCurve[]> | null = null;
function loadCurves(): Promise<IsoCurve[]> {
  curvesOnce ??= reviewBridge.isoelasticCurves().then((c) => c.curves).catch(() => {
    curvesOnce = null; // retry on the next mount
    return [];
  });
  return curvesOnce;
}

// Per-viewer chart toggles (density colours, isoelastic curves).
const PREFS_KEY = "yofo.review.charts";
function loadPrefs(): { density: boolean; curves: boolean } {
  try {
    const o = JSON.parse(localStorage.getItem(PREFS_KEY) ?? "{}") as Record<string, unknown>;
    return { density: o.density !== false, curves: o.curves !== false };
  } catch {
    return { density: true, curves: true };
  }
}

const pct = (f: number) => Math.round(f * 100);
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

export interface ChartsViewProps {
  info: ReviewInfo;
  fileKey: string;
  /** Selected valid-set position (-1 = none). */
  selected: number;
  /** Size of the valid set (arrow-key stepping wraps over it). */
  validTotal: number;
  onSelect: (validPosition: number) => void;
  /** The frame pane (preview canvas + caption), owned by the panel. */
  framePane: ReactNode;
  /** Re-read the file info (after a core record is saved). */
  refreshInfo: () => Promise<void>;
  log: (line: string) => void;
}

export function ChartsView(props: ChartsViewProps) {
  const { info, fileKey, selected, validTotal, onSelect, framePane, refreshInfo, log } = props;
  const [scatter, setScatter] = useState<ScatterData | null>(null);
  const [density, setDensity] = useState<ReviewDensity | null>(null);
  const [curves, setCurves] = useState<IsoCurve[]>([]);
  const [showCurves, setShowCurves] = useState(() => loadPrefs().curves);
  const [colourByDensity, setColourByDensity] = useState(() => loadPrefs().density);
  const [computedJson, setComputedJson] = useState("");
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState("");
  const resetRef = useRef<(() => void) | null>(null);
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);

  useEffect(() => {
    try {
      localStorage.setItem(PREFS_KEY, JSON.stringify({ density: colourByDensity, curves: showCurves }));
    } catch {
      // Storage unavailable: the choice lasts for this session only.
    }
  }, [colourByDensity, showCurves]);

  useEffect(() => {
    void loadCurves().then((c) => alive.current && setCurves(c));
  }, []);

  // Per file: scatter, then the density job (single-flight; retried while
  // another job runs), polled until its levels are ready.
  useEffect(() => {
    let cancelled = false;
    setScatter(null);
    setDensity(null);
    setComputedJson("");
    setStatus("");
    (async () => {
      try {
        const s = await reviewBridge.scatter();
        if (cancelled) return;
        setScatter(s);
        if (!s.valid || s.area_um2.length === 0) return;
        setStatus("Estimating density…");
        for (let tries = 0; !cancelled; tries++) {
          const r = await reviewBridge.requestDensity(1.0, DEFAULT_CORE_FRACTION, 8, true);
          if (r.ok) break;
          if (tries > 200) {
            setStatus(`Density not available: ${r.message}`);
            return;
          }
          await sleep(POLL_MS * 2); // another job holds the single flight
        }
        while (!cancelled) {
          const d = await reviewBridge.density();
          if (d.valid && d.ready) {
            if (!cancelled) {
              setDensity(d);
              setStatus("");
            }
            return;
          }
          await sleep(POLL_MS);
        }
      } catch (e) {
        if (!cancelled) {
          setStatus("");
          log(`charts error: ${e}`);
        }
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [fileKey, log]);

  const stored = useMemo(() => recordContours(info.kde_analysis_json), [info.kde_analysis_json]);
  const live = useMemo(() => recordContours(info.kde_live_json), [info.kde_live_json]);
  // A record computed this session (context menu), else the density job's
  // full-run record for files that carry none. Neither is in the file yet.
  const computed = useMemo(() => recordContours(computedJson || (stored ? "" : (density?.computed_record_json ?? ""))), [computedJson, density, stored]);
  const computedSource = computedJson || (stored ? "" : (density?.computed_record_json ?? ""));

  const contours = useMemo(() => {
    const out: ContourFamily[] = [];
    if (stored && !computedJson) out.push({ name: `Core ${pct(stored.coreFraction)}% (full run)`, color: FULL_RUN, dashed: false, loops: stored.loops });
    if (computed) out.push({ name: `Core ${pct(computed.coreFraction)}% (full run, not saved)`, color: FULL_RUN, dashed: false, loops: computed.loops });
    if (live) out.push({ name: `Core ${pct(live.coreFraction)}% (live, provisional)`, color: LIVE, dashed: true, loops: live.loops });
    return out;
  }, [stored, live, computed, computedJson]);

  const saveRecord = useCallback(
    async (json: string) => {
      let overwrite = false;
      if (stored) {
        overwrite = await ask("This experiment already has a full-run core contour.\nReplace it with the new one?", {
          title: "Replace core contour",
          kind: "warning",
        });
        if (!overwrite) {
          setStatus("Kept the existing full-run core contour");
          return;
        }
      }
      const r = await reviewBridge.saveCoreRecord(json, overwrite);
      if (!r.ok) {
        setStatus(`Full-run core contour shown, not saved (${r.message})`);
        log(`save core contour refused: ${r.message}`);
        return;
      }
      setComputedJson("");
      await refreshInfo();
      setStatus("Full-run core contour saved");
      log("full-run core contour saved to the file");
    },
    [log, refreshInfo, stored],
  );

  const computeCore = useCallback(async () => {
    setBusy(true);
    const n = scatter?.area_um2.length ?? 0;
    setStatus(`Computing core contour from ${n} cells…`);
    try {
      const r = await reviewBridge.computeCore(stored?.coreFraction ?? DEFAULT_CORE_FRACTION);
      if (!r.ok) {
        setStatus(`Core contour: ${r.message}`);
        return;
      }
      while (await reviewBridge.jobsBusy()) await sleep(POLL_MS);
      const json = await reviewBridge.computedCoreJson();
      const rec = recordContours(json);
      if (!rec || rec.loops.length === 0) {
        setStatus("Core contour: too few cells for a contour");
        return;
      }
      setComputedJson(json);
      setStatus(`Core ${pct(rec.coreFraction)}%: ${rec.loops.length} loop(s) from ${n} cells`);
      if (await ask("Save this full-run core contour into the file?", { title: "Save core contour", kind: "info" })) await saveRecord(json);
      else setStatus("Full-run core contour shown, not saved");
    } catch (e) {
      log(`core contour error: ${e}`);
      setStatus("");
    } finally {
      if (alive.current) setBusy(false);
    }
  }, [log, saveRecord, scatter, stored]);

  const hasPoints = (scatter?.area_um2.length ?? 0) > 0;
  const menu: MenuItem[] = [
    { label: "Compute core contour from full run", onClick: () => void computeCore(), disabled: busy || !hasPoints },
    { label: "Save computed core contour to file", onClick: () => void saveRecord(computedSource), disabled: busy || !computed },
    { label: "Colour by density", onClick: () => setColourByDensity((v) => !v), checked: colourByDensity, disabled: !density },
    { label: "Isoelastic curves", onClick: () => setShowCurves((v) => !v), checked: showCurves },
  ];

  const onKey = (e: React.KeyboardEvent) => {
    if (validTotal <= 0) return;
    const delta = e.key === "ArrowRight" || e.key === "ArrowDown" ? 1 : e.key === "ArrowLeft" || e.key === "ArrowUp" ? -1 : 0;
    if (!delta) return;
    e.preventDefault();
    const from = selected >= 0 ? selected : delta > 0 ? -1 : 0;
    onSelect((((from + delta) % validTotal) + validTotal) % validTotal);
  };

  return (
    <div className="review-charts" tabIndex={0} onKeyDown={onKey}>
      <div className="charts-left">
        <div className="table-toolbar charts-toolbar">
          <label title="Colour each cell by the backend density estimate">
            <input type="checkbox" checked={colourByDensity} disabled={!density} onChange={(e) => setColourByDensity(e.target.checked)} /> Density colours
          </label>
          <label>
            <input type="checkbox" checked={showCurves} onChange={(e) => setShowCurves(e.target.checked)} /> Isoelastic curves
          </label>
          <button onClick={() => void computeCore()} disabled={busy || !hasPoints} title="KDE core contour over every valid cell in this file">
            Compute core contour…
          </button>
          <button onClick={() => resetRef.current?.()} title="Back to the data extent (or double-click empty plot space)">
            Reset zoom
          </button>
          <span className="right mono charts-status">{status}</span>
        </div>
        <ReviewScatter
          data={scatter}
          levels={density?.levels ?? null}
          levelCount={density?.level_count ?? 8}
          colourByDensity={colourByDensity && !!density}
          curves={showCurves ? curves : []}
          contours={contours}
          selected={selected}
          fileKey={fileKey}
          onSelect={onSelect}
          menu={menu}
          resetRef={resetRef}
        />
      </div>
      <div className="charts-right">
        <div className="charts-frame">{framePane}</div>
        <Histogram values={scatter?.ring_ratio ?? []} min={info.ring_ratio_min} max={info.ring_ratio_max} />
      </div>
    </div>
  );
}
