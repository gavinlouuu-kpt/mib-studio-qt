// The reference contour row of the Monitoring Settings dialog's Density (KDE) group (epic #866 PR E2: MON-061,
// MON-062, MON-086): Pin current, From file… and Clear, as Qt's `kdePinReferenceBtn`, `kdeReferenceFromFileBtn`
// and `kdeClearReferenceBtn`. They act at once; they are not values the dialog's Apply sets.
//
// Not inside a ViewerGate: none of them sends a control command. Pin and Clear only keep or drop contours in
// this client's memory, and From file… is a read (`fetch_kde_reference_from_file`, open to every client).
import { useEffect, useRef, useState } from "react";
import type { DensityReferenceControlsModel } from "./useMonitoringDensity";

export function DensityReferenceControls({ controls }: { controls: DensityReferenceControlsModel }) {
  // From file… stays pressed until the picker, the read and any error dialog are over.
  const [busy, setBusy] = useState(false);
  const mounted = useRef(true);
  useEffect(() => () => { mounted.current = false; }, []);
  const fromFile = async () => {
    setBusy(true);
    try {
      await controls.onFromFile();
    } finally {
      if (mounted.current) setBusy(false);
    }
  };
  return (
    <div className="mon-density-reference" data-tour="monitoring-settings-kde-reference">
      <span className="mon-settings-label">Reference contour</span>
      <div className="mon-density-reference-buttons">
        <button
          type="button" className="btn" onClick={controls.onPin} disabled={!controls.canPin} data-tour="monitoring-settings-kde-pin"
          title={controls.canPin ? "Keep the current core contour on the scatter as a dashed reference to compare against" : "Needs Density (KDE) on and a core contour"}
        >
          Pin current
        </button>
        <button
          type="button" className="btn" onClick={() => void fromFile()} disabled={busy || !!controls.fromFileBlocked} data-tour="monitoring-settings-kde-from-file"
          title={controls.fromFileBlocked ?? "Show the core contour stored in a previous experiment file (full-run if computed, else the live estimate) as the dashed reference"}
        >
          From file…
        </button>
        <button
          type="button" className="btn" onClick={controls.onClear} disabled={!controls.canClear} data-tour="monitoring-settings-kde-clear"
        >
          Clear
        </button>
      </div>
      {controls.label && (
        <p className="mon-density-reference-label" data-tour="monitoring-settings-kde-reference-label">
          Reference: {controls.label}
        </p>
      )}
    </div>
  );
}
