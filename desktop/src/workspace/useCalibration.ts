// The workspace's read of the calibration records (#716 A4b, bridge ABI 42) and its one explicit
// action, calibration_apply.
//
// - Reads: `calibration_records_list`, polled with Prepare's readiness (PREPARE_READINESS_POLL_MS)
//   while Prepare or the Experiment plan is shown and no run is active, one request at a time; a
//   read asked for while one is in flight runs once it ends. A failed read clears the list (never a
//   stale list shown as current). `calibration_record_get` only when the operator selects a record
//   in the list. Listing, getting and polling never apply anything.
// - Apply: only `apply(recordId, expectedFactor)`, from a button, with the factor the page last read
//   (a compare-and-set in the backend). Nothing is sent without control (a viewer), during a run or
//   while an apply is pending. Whatever the answer, the page reads the list (and the gates) again; a
//   list read that started before the apply answered is dropped, so the factor shown is never older
//   than the apply.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge } from "../bridge";
import type { CalibrationRecordGet, CalibrationRecordsList } from "../calibrationRecords";
import { applyErrorNotice, applyReplyNotice, type CalibrationNotice } from "./calibrationModel";
import { PREPARE_READINESS_POLL_MS } from "./prepareModel";

export interface CalibrationHistory {
  recordId: string;
  reply: CalibrationRecordGet | null;
  error: string;
}

export interface CalibrationController {
  /** The last list read; null before the first read, after a failed one, and while not polled. */
  list: CalibrationRecordsList | null;
  error: string;
  /** Read again now (a read; it never applies anything). */
  refresh: () => void;
  /** The record whose apply is pending, null when none is. */
  applying: string | null;
  /** The last apply's outcome, until the next apply. */
  outcome: CalibrationNotice | null;
  /** The explicit apply: calibration_apply(recordId, expectedFactor). */
  apply: (recordId: string, expectedFactor: number) => Promise<void>;
  /** The selected record's versions (calibration_record_get), null when none is selected. */
  history: CalibrationHistory | null;
  select: (recordId: string | null) => void;
}

export interface CalibrationOptions {
  /** Whether this client controls the instrument (a viewer sends nothing). */
  controlHeld: boolean;
  experimentActive: boolean;
  /** Read the readiness gates again after an apply. */
  afterApply?: () => void;
}

export function useCalibration(active: boolean, options: CalibrationOptions): CalibrationController {
  const [list, setList] = useState<CalibrationRecordsList | null>(null);
  const [error, setError] = useState("");
  const [applying, setApplying] = useState<string | null>(null);
  const [outcome, setOutcome] = useState<CalibrationNotice | null>(null);
  const [history, setHistory] = useState<CalibrationHistory | null>(null);
  const mounted = useRef(true);
  const inFlight = useRef(false);
  const again = useRef(false);
  // Bumped when an apply answers: a list read started before it is dropped.
  const epoch = useRef(0);
  const activeRef = useRef(active);
  activeRef.current = active;
  const opts = useRef(options);
  opts.current = options;
  const applyPending = useRef(false);
  const selected = useRef<string | null>(null);
  // Each record_get gets a token; only the newest request's reply is shown.
  const historyToken = useRef(0);
  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  const read = useCallback(async () => {
    if (inFlight.current) { again.current = true; return; }
    inFlight.current = true;
    try {
      do {
        again.current = false;
        const started = epoch.current;
        try {
          const l = await bridge.calibrationRecordsList();
          if (mounted.current && activeRef.current && started === epoch.current) { setList(l); setError(""); }
        } catch (e) {
          if (mounted.current && activeRef.current && started === epoch.current) { setList(null); setError(String(e)); }
        }
        if (started !== epoch.current) again.current = true; // an apply answered meanwhile: read once more
      } while (again.current && mounted.current && activeRef.current);
    } finally {
      inFlight.current = false;
    }
  }, []);

  useEffect(() => {
    // A read started in an earlier active period (before a run, or another view) is not current.
    epoch.current += 1;
    if (!active) { setList(null); setError(""); return; }
    void read();
    const id = window.setInterval(() => void read(), PREPARE_READINESS_POLL_MS);
    return () => window.clearInterval(id);
  }, [active, read]);

  const refresh = useCallback(() => { if (activeRef.current) void read(); }, [read]);

  const select = useCallback((recordId: string | null) => {
    selected.current = recordId;
    const token = ++historyToken.current;
    if (recordId === null) { setHistory(null); return; }
    setHistory((h) => (h?.recordId === recordId ? h : { recordId, reply: null, error: "" }));
    const current = () => mounted.current && historyToken.current === token;
    bridge.calibrationRecordGet(recordId).then(
      (reply) => { if (current()) setHistory({ recordId, reply, error: "" }); },
      (e) => { if (current()) setHistory({ recordId, reply: null, error: String(e) }); },
    );
  }, []);

  const apply = useCallback(async (recordId: string, expectedFactor: number) => {
    const o = opts.current;
    if (applyPending.current || !o.controlHeld || o.experimentActive) return;
    applyPending.current = true;
    setApplying(recordId);
    setOutcome(null);
    let notice: CalibrationNotice;
    try {
      notice = applyReplyNotice(await bridge.calibrationApply(recordId, expectedFactor), expectedFactor);
    } catch (e) {
      notice = applyErrorNotice(e);
    } finally {
      applyPending.current = false;
      epoch.current += 1;
    }
    if (!mounted.current) return;
    setApplying(null);
    setOutcome(notice);
    // Re-read: the list (and the selected record's versions) and the gates show what changed.
    void read();
    if (selected.current) select(selected.current);
    opts.current.afterApply?.();
  }, [read, select]);

  return { list, error, refresh, applying, outcome, apply, history, select };
}
