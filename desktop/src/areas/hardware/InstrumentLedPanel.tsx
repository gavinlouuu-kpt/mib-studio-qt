// Hardware (epic #868): the PZ7035 LED / strobe controls under the Preview canvas (Service mode only).
// Moved from App.tsx without change (#869 wave 0).
import { bridge } from "../../bridge";
import { InstrumentLedControls } from "../../components/InstrumentLedControls";
import { useStudio } from "../../studio/StudioContext";

export function InstrumentLedPanel() {
  const { instrumentModes, operatingMode, runMode, instrument, caps, ready, expActive, viewerOnly, append } = useStudio();
  return (
    <>
      {instrumentModes && operatingMode === "service" && (runMode || instrument?.mode?.name === "align") && (
        <InstrumentLedControls mode={runMode ? "run" : "align"} alignBands={instrument?.mode?.align_source === "bands"} limits={caps.led_limits?.[runMode ? "run" : "align"]}
          current={instrument?.led} disabled={!ready || expActive || viewerOnly} apply={bridge.setInstrumentLed} append={append} />
      )}
    </>
  );
}
