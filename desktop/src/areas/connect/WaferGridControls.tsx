// Wafer Grid on the Overview (CONNECT-054): the toolbar button and the notice under the toolbar. Qt: the
// "overviewWaferGridBtn" and the failure dialog of OverviewTab::onToggleDotGrid. The state and behaviour
// are useWaferGrid.ts; the overlay on the canvas is drawn by usePreview's draw().
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";
import { waferGridButtonLabel } from "./waferGrid";

export const WAFER_GRID_TOOLTIP =
  "Decode the wafer dot-grid fiducial pattern in the live image and show the design, chip and absolute position on the wafer (Overview only)";

export function WaferGridToggle() {
  const { waferGrid, waferGridSupported, onToggleWaferGrid, ready, expActive } = useStudio();
  if (!waferGridSupported) return null;
  const unavailable = !waferGrid.available && !waferGrid.enabled;
  return (
    <ViewerGate>
      <button
        className="btn"
        data-testid="wafer-grid-toggle"
        aria-pressed={waferGrid.enabled}
        onClick={() => void onToggleWaferGrid()}
        disabled={!ready || expActive || unavailable}
        title={unavailable ? "Dot-grid localization is switched off in this build" : expActive ? "Not during an experiment" : WAFER_GRID_TOOLTIP}
      >
        {waferGridButtonLabel(waferGrid.enabled)}
      </button>
    </ViewerGate>
  );
}

// A refused toggle stays visible (text, not colour alone) until the next one works.
export function WaferGridNotice() {
  const { waferGridSupported, waferGridError } = useStudio();
  if (!waferGridSupported || !waferGridError) return null;
  return <p className="pending-note warn" role="alert" data-testid="wafer-grid-error">{waferGridError}</p>;
}
