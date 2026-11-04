// O5's read of the calibration records for a calibration run (#716 A7c; bridge ABI 42
// `calibration_records_list`, a read). Never polled: read when the Run summary shows a run that may be a
// calibration run (`key` names it), when that run changes, and on the operator's Refresh. A reply is
// shown only for the run it was read for; a read in flight when the run changes, or after the view is
// gone, is dropped. A failed read says why and shows no list (never a stale one as current). Nothing
// is applied from here: the record's Apply is on Prepare's Calibration card.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge } from "../bridge";
import type { CalibrationRecordsList } from "../calibrationRecords";

export type CalibrationListRead = () => Promise<CalibrationRecordsList>;

export interface RunCalibrationState {
  /** The list read for this run; null before a read, after a failed one, and when not read. */
  list: CalibrationRecordsList | null;
  error: string;
  loading: boolean;
  refresh: () => void;
}

const defaultRead: CalibrationListRead = () => bridge.calibrationRecordsList();

/** `key`: the run the list is read for; null reads nothing (not a calibration run). */
export function useRunCalibration(key: string | null, read: CalibrationListRead = defaultRead): RunCalibrationState {
  const [held, setHeld] = useState<{ key: string | null; list: CalibrationRecordsList | null; error: string }>({ key: null, list: null, error: "" });
  const [loading, setLoading] = useState(false);
  const generation = useRef(0);
  const disposed = useRef(false);
  const args = useRef({ key, read });
  args.current = { key, read };
  useEffect(() => {
    disposed.current = false;
    return () => { disposed.current = true; };
  }, []);

  const doRead = useCallback(async () => {
    const { key: k, read: rd } = args.current;
    if (k === null || disposed.current) return;
    const mine = ++generation.current;
    setLoading(true);
    try {
      const list = await rd();
      if (mine === generation.current && !disposed.current) setHeld({ key: k, list, error: "" });
    } catch (e) {
      if (mine === generation.current && !disposed.current) setHeld({ key: k, list: null, error: String(e).replace(/^Error: /, "") });
    } finally {
      if (mine === generation.current && !disposed.current) setLoading(false);
    }
  }, []);

  // The run last read for (kept across StrictMode's second effect run, so a mount reads once).
  const lastKey = useRef<string | null | undefined>(undefined);
  useEffect(() => {
    if (lastKey.current === key) return;
    lastKey.current = key;
    generation.current += 1; // whatever is in flight was read for another run
    setLoading(false);
    if (key === null) return;
    void doRead();
  }, [key, doRead]);

  const refresh = useCallback(() => { void doRead(); }, [doRead]);
  const mine = held.key === key && key !== null;
  return { list: mine ? held.list : null, error: mine ? held.error : "", loading, refresh };
}
