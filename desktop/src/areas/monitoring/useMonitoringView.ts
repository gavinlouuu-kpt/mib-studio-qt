// Effects behind the Monitoring view's click-to-view pane and gate overlay (epic #866, PR E1). The state is
// useMonitoringState() (monSelection, cellWindowOpen, emodulusTable); the pin lifecycle of the selected cell is
// clickView/useCellView.ts, run by the charts that show it.
import { useEffect, useRef } from "react";
import { bridge } from "../../bridge";
import type { ShellState } from "../shell/useShell";
import { prepareLut } from "./emodulusLut";
import type { MonitoringState } from "./useMonitoring";

type EmodulusTableDeps = Pick<ShellState, "ready" | "append"> & Pick<MonitoringState, "emodulusTable" | "setEmodulusTable"> & { monitoringVisible: boolean };

/**
 * The E-modulus table behind the gate overlay (iso-E contours, the E tint, E handles), the scatter tooltip's
 * "E x kPa" and the tune panel's hint (MON-043, 064, 066, 068, 077). Read when the Monitoring sub-tab becomes
 * visible and the backend is ready, sending the identity of the table already held so the 200 x 200 grid comes
 * back only when it differs (the backend loads its table once at bootstrap, so in practice once per session).
 * Never on the 500 ms snapshot poll, and not on a hidden sub-tab. A backend that is not initialised yet
 * (`valid: false`) leaves the state as it was and is asked again at the next visit.
 */
export function useEmodulusTable({ ready, append, monitoringVisible, emodulusTable, setEmodulusTable }: EmodulusTableDeps) {
  const known = useRef<string | null>(null);
  known.current = emodulusTable.status === "loaded" ? emodulusTable.table.identity : null;
  useEffect(() => {
    if (!ready || !monitoringVisible) return;
    let disposed = false;
    bridge.fetchEmodulusLut(known.current)
      .then((reply) => {
        if (disposed || !reply.valid) return;
        if (!reply.loaded) return setEmodulusTable({ status: "none" });
        if (reply.unchanged) return; // the table already held is the backend's
        setEmodulusTable({ status: "loaded", table: { identity: reply.identity, info: reply.info, lut: prepareLut(reply.lut) } });
      })
      .catch((e) => {
        if (disposed) return;
        const message = e instanceof Error ? e.message : String(e);
        append(`E-modulus table could not be read: ${message}`);
        // A table already held stays usable; otherwise the view says it could not read one.
        setEmodulusTable((current) => (current.status === "loaded" ? current : { status: "error", message }));
      });
    return () => { disposed = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, monitoringVisible]);
}

type SelectionResetDeps = Pick<MonitoringState, "onMonitoringCleared" | "setMonSelection" | "setCellWindowOpen">;

/** Clear Buffer clears the selected cell and closes its window (MON-048): the buffer the cell came from is gone. */
export function useSelectionReset({ onMonitoringCleared, setMonSelection, setCellWindowOpen }: SelectionResetDeps) {
  useEffect(
    () => onMonitoringCleared(() => { setMonSelection(null); setCellWindowOpen(false); }),
    [onMonitoringCleared, setMonSelection, setCellWindowOpen],
  );
}
