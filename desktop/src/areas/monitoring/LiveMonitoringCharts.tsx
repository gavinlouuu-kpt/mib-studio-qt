// The Monitoring charts with the cell and the gate (epic #866, PR E1): the charts MonitoringPanel shows,
// wired to the studio state. It owns no state of its own beyond what a gesture needs:
//  - a click on a point selects its cell (monSelection in useMonitoringState); the pane docked over the
//    histogram shows it (`cellView`: the cell pinned in the backend and pulled by identity, clickView/useCellView.ts, run by
//    useStudioController so it lives as long as the selection and survives a sub-tab or stage switch), with Previous / Next
//    through the plotted points, Open in window… and ✕ (MON-044, 046, 047, 048);
//  - the draft target-group gate is drawn on the scatter and its handles are dragged into the tune draft
//    (gate/useGateOverlay.ts, MON-064..066, 068); the live count sits under the scatter (MON-067);
//  - the E-modulus table (useEmodulusTable) gives the tooltip its "E x kPa" (MON-043).
import { useMemo } from "react";
import type { MonitoringRow, MonitoringSnapshot } from "../../bridge";
import { MonitoringCharts } from "../../components/MonitoringCharts";
import { useViewerOnly } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";
import { CellPane, CellWindow } from "./clickView/CellPane";
import { stepSelection, stillPlotted } from "./clickView/selection";
import { gateNotice } from "./gate/gateOverlay";
import { useGateOverlay } from "./gate/useGateOverlay";
import { countTargets, previewText, type GateSpec } from "./gate/targetGate";
import { monitoringRowKey, type ScatterData } from "./monitoringChartModel";
import type { EmodulusTableState, PreparedLut } from "./emodulusLut";

/** "Target group preview: N of M plotted valid cells (P%)" under the scatter, hidden while the draft gate is off
 *  (MON-067). E comes from each plotted row's stored modulus; with no table loaded it is not read. */
export function GatePreview({ scatter, spec, lut, table }: { scatter: ScatterData; spec: GateSpec; lut: PreparedLut | null; table: EmodulusTableState }) {
  const tableLoaded = lut !== null;
  const needE = spec.enabled && spec.emodEnabled && tableLoaded;
  const moduli = useMemo(
    () => (needE ? Float64Array.from(scatter.rows, (r) => (typeof r.youngs_modulus === "number" ? r.youngs_modulus : NaN)) : undefined),
    [scatter, needE],
  );
  const text = useMemo(
    () => previewText(countTargets(spec, tableLoaded, scatter.xs, scatter.ys, moduli)),
    [scatter, spec, tableLoaded, moduli],
  );
  if (!spec.enabled) return null;
  const notice = gateNotice({ spec, table: table.status, tableError: table.status === "error" ? table.message : "" });
  return (
    <>
      <p className="mon-gate-count" role="status" data-tour="monitoring-gate-label">{text}</p>
      {notice && <p className="mon-gate-notice" role="status" data-tour="monitoring-gate-no-table">{notice}</p>}
    </>
  );
}

export function LiveMonitoringCharts({ snapshot }: { snapshot: MonitoringSnapshot | null }) {
  const {
    tune, setTuneField, emodulusTable, monSelection, setMonSelection, cellWindowOpen, setCellWindowOpen, validOverlay, cellView: cell,
  } = useStudio();
  const viewerOnly = useViewerOnly();
  const gate = useGateOverlay({ tune, setTuneField, table: emodulusTable, viewerOnly });

  const select = (row: MonitoringRow | null) => {
    if (row) setMonSelection((current) => (current && monitoringRowKey(current) === monitoringRowKey(row) ? current : row));
  };
  const renderPane = (scatter: ScatterData) => {
    if (!monSelection) return null;
    const plotted = scatter.rows;
    const common = {
      row: monSelection,
      state: cell,
      onScatter: stillPlotted(plotted, monSelection),
      overlay: validOverlay,
      stepDisabled: plotted.length === 0,
      onPrev: () => select(stepSelection(plotted, monSelection, -1)),
      onNext: () => select(stepSelection(plotted, monSelection, 1)),
    };
    return (
      <>
        <CellPane {...common} onOpenWindow={() => setCellWindowOpen(true)} onClose={() => { setMonSelection(null); setCellWindowOpen(false); }} />
        {cellWindowOpen && <CellWindow {...common} onClose={() => setCellWindowOpen(false)} />}
      </>
    );
  };
  return (
    <MonitoringCharts
      snapshot={snapshot}
      selectedKey={monSelection ? monitoringRowKey(monSelection) : null}
      onSelectPoint={select}
      overlay={gate.overlay}
      emodulusAt={gate.emodulusAt}
      histogramPane={renderPane}
      scatterFooter={(scatter) => <GatePreview scatter={scatter} spec={gate.spec} lut={gate.lut} table={emodulusTable} />}
    />
  );
}
