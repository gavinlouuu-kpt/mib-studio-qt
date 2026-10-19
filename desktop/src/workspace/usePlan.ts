// The workspace's Experiment plan (O3, #716 A5), held by App so it survives stage changes and
// leaving the workspace. The plan is local: creating or editing it sends nothing. Only `start`
// acts, and only through the caller's Start flow (App's `runExperimentStart`, the classic
// `experimentStart.ts` flow with the plan's startRequestJson).
//
// - The experiment id is minted when the plan is created and kept while the plan is edited and
//   while a Start is refused or cancelled (no run was recorded). Once its run has started the plan
//   is locked: its identity is recorded in that run, and a single-sample plan has no second run.
//   "New plan" mints a new id.
// - The selected operator (#393 phase 1) is remembered locally, in localStorage, when a run
//   starts with it, and is the next plan's default. It is a selection, not authentication.
import { useCallback, useRef, useState } from "react";
import type { StartOutcome } from "../experimentStart";
import {
  mintExperimentId,
  newPlanDraft,
  parseOperators,
  rememberOperator,
  type OperatorChoice,
  type PlanDraft,
} from "./planModel";

/** Where the remembered operators are kept (this machine only). */
export const OPERATORS_STORAGE_KEY = "mib.workspace.operators";

/** What a Start from the plan came to: the flow's outcome, a transport error, or nothing sent
 *  because a start request was already pending. */
export type PlanStartResult = StartOutcome | { kind: "error"; message: string } | { kind: "busy" };

export interface PlanStarted {
  path: string;
  notice: string;
  runIndex: number;
}

export interface PlanController {
  draft: PlanDraft;
  /** Edit the draft (ignored while a Start is pending and once its run has started). */
  edit: (change: (draft: PlanDraft) => PlanDraft) => void;
  /** Remembered operators, most recent first. */
  operators: OperatorChoice[];
  /** The last Start's result, until the next Start or a new plan. */
  result: PlanStartResult | null;
  /** Set once the plan's run has started: the plan is locked. */
  started: PlanStarted | null;
  starting: boolean;
  /** Run the Start flow with this startRequestJson. */
  start: (startRequestJson: string, runIndex: number) => Promise<PlanStartResult | null>;
  /** A new plan: a new experiment id, an empty sheet, the last operator. */
  newPlan: () => void;
}

function readOperators(): OperatorChoice[] {
  try {
    const raw = window.localStorage.getItem(OPERATORS_STORAGE_KEY);
    return raw === null ? [] : parseOperators(JSON.parse(raw));
  } catch {
    return [];
  }
}

function writeOperators(list: OperatorChoice[]) {
  try {
    window.localStorage.setItem(OPERATORS_STORAGE_KEY, JSON.stringify(list));
  } catch {
    /* storage unavailable: the operator is just not remembered */
  }
}

export function usePlan(runStart: (startRequestJson: string) => Promise<PlanStartResult>): PlanController {
  const [operators, setOperators] = useState<OperatorChoice[]>(readOperators);
  const [draft, setDraft] = useState<PlanDraft>(() => newPlanDraft(mintExperimentId(), operators[0]));
  const [result, setResult] = useState<PlanStartResult | null>(null);
  const [started, setStarted] = useState<PlanStarted | null>(null);
  const [starting, setStarting] = useState(false);
  const pending = useRef(false);
  const startedRef = useRef(false);
  // The draft and list as last rendered: the operator remembered is the one the Start sent (the
  // draft cannot change while a Start is pending).
  const draftRef = useRef(draft);
  draftRef.current = draft;
  const operatorsRef = useRef(operators);
  operatorsRef.current = operators;

  const edit = useCallback((change: (d: PlanDraft) => PlanDraft) => {
    if (startedRef.current || pending.current) return;
    setDraft(change);
  }, []);

  const start = useCallback(async (json: string, runIndex: number): Promise<PlanStartResult | null> => {
    if (pending.current || startedRef.current) return null;
    pending.current = true;
    setStarting(true);
    setResult(null);
    try {
      const out = await runStart(json);
      setResult(out);
      if (out.kind === "started") {
        startedRef.current = true;
        setStarted({ path: out.path, notice: out.notice, runIndex });
        const next = rememberOperator(operatorsRef.current, draftRef.current.operator);
        writeOperators(next);
        setOperators(next);
      }
      return out;
    } catch (e) {
      const out: PlanStartResult = { kind: "error", message: String(e) };
      setResult(out);
      return out;
    } finally {
      pending.current = false;
      setStarting(false);
    }
  }, [runStart]);

  const newPlan = useCallback(() => {
    if (pending.current) return;
    startedRef.current = false;
    setStarted(null);
    setResult(null);
    setDraft((d) => newPlanDraft(mintExperimentId(), d.operator));
  }, []);

  return { draft, edit, operators, result, started, starting, start, newPlan };
}
