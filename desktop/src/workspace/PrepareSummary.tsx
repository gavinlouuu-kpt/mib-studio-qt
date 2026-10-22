// The summary-first hardware preflight (O1b, approved by Gavin 2026-10-22; the classic boards of
// the same day): one verdict card (a headline, the "detected and selected" line, the existing
// verdict chip, a neutral "Next · Align" line, the Preflight confirmation and Confirm with a one-line
// Start-refusal note) and "What was checked", whose disclosure rows hold everything else: Camera and
// acquisition, Focus, Fluidics, Calibration, Method, Data destination, All checks. A row with a
// counted issue opens by itself, shows its Fix and cannot be collapsed (prepareSummaryModel.ts says
// what counts). No configuration lives here: a row has at most one "Set up …" button that leaves
// for the Hardware setup page. Shared by the classic Hardware Preflight stage and the workspace's
// Prepare. Nothing here acts on hardware: Confirm stores a signature, Refresh and Read gates again
// read, and the Calibration row's Apply / Re-apply (#716 A4b) is an explicit compare-and-set.
import { useState } from "react";
import { Button, Card, FactList, Notice, StateChip, type Fact, type StateTone } from "@mib/ui";
import { READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { StageStatus } from "../bridge";
import type { ReadinessGate } from "../eventAdapter";
import type { PreflightCheck, PreflightInput, PreflightReport } from "../preflight";
import {
  CONFIRMATION_TEXT,
  GATE_LABELS,
  checkFactTone,
  checkText,
  checkTone,
  confirmBlockers,
  confirmationState,
  gateLabel,
  gateRemediation,
  gateText,
  gateTone,
  startRefusalText,
  summarizeRequired,
  worstState,
  zStageFact,
  type Verdict,
} from "./prepareModel";
import {
  CAMERA_CHECKS,
  DESTINATION_CHECKS,
  DESTINATION_NOTE,
  HOW_PREPARE_READS,
  NEXT_ALIGN_NOTE,
  NEXT_ALIGN_WARN_NOTE,
  SIMULATED_NOTE,
  allChecksSummary,
  calibrationRowState,
  cameraName,
  checkIssue,
  coreText,
  countedIssues,
  detectedLine,
  fallbackCamera,
  firstNames,
  gateRole,
  gateRow,
  isCounted,
  issueChip,
  nextAlignText,
  prepareHeadline,
  startRefusalLine,
  type CountedIssue,
  type RowKey,
} from "./prepareSummaryModel";
import { DisclosureRow } from "./DisclosureRow";
import type { PrepareReadiness } from "./usePrepareReadiness";
import { CalibrationCard } from "./CalibrationCard";
import { ageText, factorText, methodCalibrationOf, recordFactor } from "./calibrationModel";
import type { CalibrationController } from "./useCalibration";

/** Which section of the Hardware setup page a row's "Set up …" button opens (App's `setupSection`). */
export type SetupSection = "camera" | "focus" | "fluidics";

export interface PrepareSummaryProps {
  /** The same snapshot the classic checklist is derived from. */
  preflightInput: PreflightInput;
  /** derivePreflight(preflightInput), computed once by the host. */
  report: PreflightReport;
  readiness: PrepareReadiness;
  /** The one verdict (`prepareVerdict`: the same words in the workspace header chip and the Plan). */
  verdict: Verdict;
  zStage: StageStatus | null;
  /** The current Preflight signature and the one the operator confirmed. */
  signature: string;
  confirmedFor: string;
  onConfirm: () => void;
  /** Why the confirmation is unavailable apart from the checks (e.g. a run is active); "" when it is. */
  confirmLockedReason: string;
  experimentActive: boolean;
  /** Re-read the checklist devices (reads only). */
  onRefreshChecks: () => void;
  /** Open the Hardware setup page at a part; absent when there is no such page to open. */
  onSetup?: (section: SetupSection) => void;
  /** Whether this client controls the instrument (a viewer's calibration Apply sends nothing). */
  controlHeld: boolean;
  /** The calibration records and their explicit apply (#716 A4b), and the calibration gate. */
  calibration: CalibrationController;
  calibrationGate: ReadinessGate | null;
  /** This computer's clock, for the records' ages. */
  now: number;
  /** The remembered nanopositioner endpoint (startup preference), "" for none; undefined = not read. */
  rememberedEndpoint?: string;
}

const checkFacts = (checks: PreflightCheck[], rename: Readonly<Record<string, string>> = {}): Fact[] =>
  checks.map((c) => ({
    label: `${rename[c.id] ?? c.label}${c.requirement === "required" ? " · required" : ""}`,
    value: checkText(c),
    tone: checkFactTone(c.status),
  }));

/** A gate's plain label, with its id as secondary text (and title) so support can match logs. */
function GateLabel({ id }: { id: string }) {
  return (
    <span data-gate-id={id} title={id}>
      {gateLabel(id)}
      {GATE_LABELS[id] ? <span className="ws-gate-id">{id}</span> : null}
    </span>
  );
}

/** The kit tone a gate is shown in: a gate that is not counted (Next · Align, the chosen mock
 *  camera) is grey with its real words; every other gate keeps its backend tone. */
function shownTone(g: ReadinessGate): StateTone {
  const role = gateRole(g);
  return role === "next" || role === "simulated" ? "neutral" : gateTone(g.status);
}

/** The note added under a gate that is treated specially; "" for none. */
function gateNote(g: ReadinessGate): string {
  const role = gateRole(g);
  if (role === "next") return g.status === G.Warn ? NEXT_ALIGN_WARN_NOTE : NEXT_ALIGN_NOTE;
  if (role === "simulated") return SIMULATED_NOTE;
  if (role === "destination") return DESTINATION_NOTE;
  return "";
}

/** The gate's state and reason as a chip, then the note and its remediation ("Fix: …"). */
function GateValue({ g }: { g: ReadinessGate }) {
  const role = gateRole(g);
  const note = gateNote(g);
  // The destination's remediation ("choose a destination") is what Start does; the chosen mock
  // camera's ("expected for development/tests") is in the note.
  const fix = role === "destination" || role === "simulated" ? "" : gateRemediation(g);
  return (
    <span className="ws-gate" data-gate-role={role}>
      <StateChip size="small" tone={shownTone(g)}>{gateText(g)}</StateChip>
      {note ? <span className="ws-gate-note">{note}</span> : null}
      {fix ? <span className="ws-gate-fix">{`Fix: ${fix}`}</span> : null}
    </span>
  );
}

const gateFact = (g: ReadinessGate): Fact => ({ label: <GateLabel id={g.id} />, value: <GateValue g={g} /> });

/** A row's facts: with counted issues, those first and the rest behind "Show all". */
function RowFacts({ issue, all, more, shown, onShow }: { issue: Fact[]; all: Fact[]; more: string; shown: boolean; onShow: () => void }) {
  if (!issue.length || shown) return <FactList className="ws-row-facts" facts={all} />;
  return (
    <>
      <FactList className="ws-row-facts" facts={issue} />
      <button type="button" className="ws-row-more" onClick={onShow}>{more ? `${more} · Show all` : "Show all"}</button>
    </>
  );
}

const PASS_CHIP = (text: string) => ({ tone: "confirmed" as StateTone, text });
const offStatus = (s: string) => s === "not-required" || s === "unsupported";

export function PrepareSummary(p: PrepareSummaryProps) {
  const [choice, setChoice] = useState<Partial<Record<RowKey, boolean>>>({});
  const [shown, setShown] = useState<Partial<Record<RowKey, boolean>>>({});
  const choose = (k: RowKey) => (open: boolean) => setChoice((c) => ({ ...c, [k]: open }));
  const show = (k: RowKey) => () => setShown((s) => ({ ...s, [k]: true }));

  const snapshot = p.readiness.snapshot;
  const gates = snapshot?.valid ? snapshot.gates : [];
  const report = p.report;
  const byId = (ids: string[]) => report.checks.filter((c) => ids.includes(c.id));
  const pz = p.preflightInput.capabilities?.instrument === "pz7035";
  const zStage = zStageFact(p.zStage);
  const summaryInput = { report, readiness: snapshot, error: p.readiness.error, paused: p.experimentActive, zStage };
  const issues = countedIssues(summaryInput);
  const headline = prepareHeadline(summaryInput, issues);
  const rowIssues = (k: RowKey): CountedIssue[] => issues.filter((i) => i.row === k);
  const setup = (section: SetupSection, label: string) => p.onSetup
    ? <div className="ws-row-actions"><Button size="small" onClick={() => p.onSetup!(section)}>{label}</Button></div> : null;

  // Confirm follows the checklist (advisory, per tab); its reasons in plain words next to it.
  const confirmation = confirmationState(p.signature, p.confirmedFor, report.criticalPassed);
  const confirmText = CONFIRMATION_TEXT[confirmation];
  const checklist = confirmBlockers(report);
  const locks = [p.confirmLockedReason, p.signature === "" ? "No camera is configured: there is nothing to confirm yet." : ""].filter(Boolean);
  const confirmWhy = [...locks, ...(checklist?.lines ?? [])];
  const confirmDisabled = confirmWhy.length > 0 || confirmation === "confirmed";
  // When Confirm is available but Start would still be refused, one line beside it says why; the
  // full list (with each Fix) is its title.
  const refusal = p.verdict.startBlocked.length ? startRefusalLine(snapshot) : "";
  const refusalFull = startRefusalText(p.verdict.startBlocked);

  // Calibration: the record in effect, for the detected line and the row's summary.
  const c = p.calibration;
  const calList = c.list?.valid && !c.list.store?.error ? c.list : null;
  const inEffect = calList ? (calList.records ?? []).find((r) => r.record_id === calList.in_effect_record_id) ?? null : null;
  const calDetected = !calList ? "" : inEffect ? `calibration ${inEffect.record_id} in effect` : "no calibration in effect";
  const method = methodCalibrationOf(snapshot);
  const calGate = p.calibrationGate;
  const calState = calibrationRowState(calGate, method, calList?.records);
  const calChip = calState.issue ? (() => {
    const g = calGate!; const reason = g.reason.split(" · ")[0];
    return { tone: gateTone(g.status), text: reason ? `${gateText({ status: g.status, reason: "" })} · ${reason}` : gateText(g) };
  })() : calGate ? (calGate.status === G.Pass ? PASS_CHIP(inEffect ? "Pass · in effect" : "Pass")
      // Not required: the reason ("the method's calibration policy is none") is in the row.
      : { tone: gateTone(calGate.status), text: gateText({ status: calGate.status, reason: "" }) })
    : { tone: "unknown" as StateTone, text: snapshot?.valid ? "Unknown · gate not reported" : "Unknown · gates not read yet" };
  const calSummary = inEffect
    ? `${inEffect.record_id} · ${factorText(recordFactor(inEffect))} · applied ${ageText(inEffect.status_at_utc, p.now)} ago`
    : calList ? "No record in effect" : c.error ? `Unknown · ${c.error}` : "Records not read yet";

  // Camera and acquisition: its checks and gates; the source gate first, as drawn.
  const cameraChecks = byId(CAMERA_CHECKS);
  const cameraGates = gates.filter((g) => gateRow(g.id) === "camera").sort((a, b) => Number(b.id === "camera.source") - Number(a.id === "camera.source"));
  const cameraIssue = rowIssues("camera");
  const simulated = cameraGates.some((g) => gateRole(g) === "simulated");
  const sourceGate = cameraGates.find((g) => g.id === "camera.source");
  // The camera session is the one a running camera resolves; the frames follow it.
  const sessionPending = cameraGates.some((g) => g.id === "camera.session" && gateRole(g) === "next");
  const fallback = fallbackCamera(sourceGate);
  const trigger = cameraChecks.find((x) => x.id === "trigger");
  const cameraSummary = [cameraName(report, snapshot), coreText(report),
    trigger ? (trigger.status === "passed" ? "trigger attached" : trigger.status === "unsupported" ? "" : "trigger not attached") : "",
    fallback !== null ? `requested ${fallback ? `${fallback} ` : ""}camera unavailable`
      : !snapshot?.valid ? "gates not read yet" : sessionPending ? "session and frames are checked in Align" : "capture stream running"].filter(Boolean).join(" · ");
  const cameraPassed = cameraChecks.filter((x) => x.status === "passed").length;
  const cameraGatesPassed = cameraGates.filter((g) => g.status === G.Pass).length;
  const cameraChip = issueChip(cameraIssue) ?? (simulated ? PASS_CHIP("Pass · simulated camera")
    : cameraChecks.some((x) => x.status === "unknown" && x.requirement === "required") ? { tone: "unknown" as StateTone, text: "Unknown" }
    : PASS_CHIP(`Pass · ${cameraPassed} checks${cameraGates.length ? ` · ${cameraGatesPassed} gates` : ""}`));
  const cameraAll = [...checkFacts(cameraChecks), ...cameraGates.map(gateFact)];
  const cameraIssueFacts = [...checkFacts(cameraChecks.filter((x) => checkIssue(x))), ...cameraGates.filter((g) => isCounted(gateRole(g))).map(gateFact)];
  const passingRequired = cameraChecks.filter((x) => x.requirement === "required" && x.status === "passed").map((x) => x.label);
  const waiting = cameraGates.filter((g) => gateRole(g) === "next").length;
  const cameraMore = [passingRequired.length ? `${firstNames(passingRequired, passingRequired.length)} pass` : "",
    waiting ? `${waiting} ${waiting === 1 ? "gate waits" : "gates wait"} for Align` : ""].filter(Boolean).join(" · ");

  // Focus: the autofocus controller, the remembered nanopositioner and the Z stage. Fluidics: the
  // pumps. Optional hardware: one quiet line while none is connected.
  const focusChecks = byId(["autofocus"]);
  const focusIssue = issues.filter((i) => i.row === "optional" && (i.check === "autofocus" || i.check === "zStage"));
  const zOff = pz || offStatus(zStage.status);
  const focusOff = zOff && focusChecks.every((x) => offStatus(x.status) && x.requirement !== "required");
  const remembered = p.rememberedEndpoint;
  const focusFacts: Fact[] = pz
    ? [{ label: "Focus stages", value: "Unsupported · not on this instrument" }]
    : [
      ...checkFacts(focusChecks, { autofocus: "Autofocus controller" }),
      ...(remembered === undefined ? [] : [{
        label: "Nanopositioner",
        value: (
          <span className="ws-gate">
            <span>{remembered ? `Remembered endpoint ${remembered}` : "Not required · Optional — no endpoint remembered"}</span>
            {focusOff ? <span className="ws-gate-fix">Fix: optional. To use autofocus, connect the controller in Hardware setup (Set up focus…); nothing on this page edits hardware</span> : null}
          </span>
        ),
      }]),
      { label: "Coarse Z stage", value: zStage.value, tone: zStage.tone },
    ];
  const focusWorst = worstState([...focusChecks, ...(pz ? [] : [zStage])]);
  const focusChip = issueChip(focusIssue)
    ?? (pz ? { tone: "unsupported" as StateTone, text: "Unsupported" }
      : focusOff ? { tone: "neutral" as StateTone, text: "Not connected · optional" }
      : focusWorst ? { tone: checkTone(focusWorst.status), text: focusWorst.text } : null);
  const focusSummary = pz ? "Not on this instrument"
    : focusOff ? "Autofocus controller, nanopositioner and Z stage · not connected"
    : [focusChecks.some((x) => x.status === "passed") ? "autofocus controller connected" : "autofocus controller not connected",
      zStage.status === "passed" || zStage.status === "warning" || zStage.status === "failed" ? `Z stage ${zStage.value.toLowerCase()}` : "Z stage not connected"]
      .join(" · ").replace(/^./, (ch) => ch.toUpperCase());
  const focusIssueFacts = checkFacts(focusChecks.filter((x) => checkIssue(x)), { autofocus: "Autofocus controller" });
  if (!pz && (zStage.status === "failed" || zStage.status === "warning")) focusIssueFacts.unshift({ label: "Coarse Z stage", value: zStage.value, tone: zStage.tone });

  const pumpChecks = byId(["samplePump", "sheathPump"]);
  const pumpIssue = issues.filter((i) => i.row === "optional" && (i.check === "samplePump" || i.check === "sheathPump"));
  const pumpsOff = pumpChecks.every((x) => offStatus(x.status) && x.requirement !== "required");
  const pumpsWorst = worstState(pumpChecks);
  const pumpsChip = issueChip(pumpIssue)
    ?? (pumpsOff ? { tone: "neutral" as StateTone, text: "Not connected · optional" }
      : pumpsWorst ? { tone: checkTone(pumpsWorst.status), text: pumpsWorst.text } : null);
  const connectedPumps = pumpChecks.filter((x) => x.status === "passed").map((x) => x.label.toLowerCase());
  const pumpsSummary = pumpsOff ? "Sample and sheath pumps · not connected"
    : connectedPumps.length === 2 ? "Sample and sheath pumps · connected"
    : connectedPumps.length === 1 ? `${firstNames(connectedPumps, 2)} connected · the other not`.replace(/^./, (ch) => ch.toUpperCase())
    : "Sample and sheath pumps";

  // Method: the gates that belong to no device, apart from the Next · Align ones.
  const methodGates = gates.filter((g) => gateRow(g.id) === "method" && gateRole(g) !== "next");
  const methodIssue = rowIssues("method");
  const methodChip = issueChip(methodIssue) ?? (!snapshot?.valid ? { tone: "unknown" as StateTone, text: "Unknown · gates not read yet" }
    : PASS_CHIP(`Pass · ${methodGates.filter((g) => g.status === G.Pass).length} gates`));
  const methodSummary = methodIssue.length ? firstNames(methodIssue.map((i) => i.name), 3) : "Processing settings, method revision, wall clock, previous run";
  const methodPass = methodGates.filter((g) => g.status === G.Pass).length;

  // Data destination: the file is chosen at Start; storage.output is never counted.
  const storageChecks = byId(DESTINATION_CHECKS);
  const storageGates = gates.filter((g) => gateRow(g.id) === "destination");
  const destIssue = rowIssues("destination");
  const destChip = issueChip(destIssue) ?? { tone: "unknown" as StateTone, text: "Unknown · checked at Start" };
  const recordingPass = ["storage.hdf5", "storage.buffer"].every((id) => storageGates.some((g) => g.id === id && g.status === G.Pass));
  const destSummary = destIssue.length ? firstNames(destIssue.map((i) => i.name), 3)
    : `File chosen and checked at Start${recordingPass ? " · recording file and budget pass" : ""}`;
  const destAll = [...checkFacts(storageChecks), ...storageGates.map(gateFact)];
  const destIssueFacts = [...checkFacts(storageChecks.filter((x) => checkIssue(x))), ...storageGates.filter((g) => isCounted(gateRole(g))).map(gateFact)];

  const allIssue = rowIssues("all");
  const required = summarizeRequired(report);
  const half = Math.ceil(gates.length / 2);

  return (
    <div className="ws-prepare-summary" data-testid="prepare-summary">
      {p.experimentActive && (
        <Notice tone="info" title="Readiness paused · a run is active">
          The gates are read again when the run has ended. Start always re-evaluates them.
        </Notice>
      )}
      <Card title={<span data-testid="workspace-headline" data-tone={headline.tone}>{headline.text}</span>}
        subtitle={<span data-testid="workspace-detected">{detectedLine(report, snapshot, zStage, calDetected)}</span>}
        data-testid="workspace-prepare-verdict"
        status={<>
          <StateChip tone={p.verdict.tone} data-testid="workspace-prepare-verdict-chip">{p.verdict.title}</StateChip>
          {headline.counts ? <StateChip tone="unknown" data-testid="workspace-issue-counts">{headline.counts}</StateChip> : null}
        </>}
        actions={<>
          <Button variant="primary" onClick={p.onConfirm} disabled={confirmDisabled}>Confirm hardware preflight</Button>
          {refusal ? <span className="ws-refusal" data-testid="workspace-start-refused" title={refusalFull ? refusalFull.text : undefined}>{refusal}</span> : null}
        </>}>
        <div className="ws-next" data-testid="workspace-next-align">
          <StateChip size="small" tone="neutral">Next · Align</StateChip>
          <span>{p.experimentActive ? "not read while a run is active" : nextAlignText(snapshot)}</span>
        </div>
        <FactList className="ws-verdict-facts" facts={[{ label: "Preflight confirmation", value: confirmText.text, tone: confirmText.tone }]} />
        {confirmWhy.length > 0 && confirmation !== "confirmed" ? (
          <Notice tone={checklist && !locks.length ? checklist.tone : "info"} title="Confirm hardware preflight is unavailable" data-testid="workspace-confirm-why">
            {confirmWhy.join(". ") + "."}
          </Notice>
        ) : null}
      </Card>

      <Card title="What was checked" className="ws-checked" data-testid="workspace-checked"
        subtitle="Auto-detected and selected. Open a row for its checks; a row with a counted issue opens by itself.">
        <DisclosureRow rowKey="camera" title="Camera and acquisition" summary={cameraSummary} chip={cameraChip}
          state={{ issue: cameraIssue.length > 0 }} choice={choice.camera} onChoice={choose("camera")}>
          <RowFacts issue={cameraIssueFacts} all={cameraAll} more={cameraMore} shown={!!shown.camera} onShow={show("camera")} />
          {setup("camera", "Set up camera…")}
        </DisclosureRow>

        <DisclosureRow rowKey="focus" quiet={focusOff} title="Focus" summary={focusSummary} chip={focusChip}
          state={{ issue: focusIssue.length > 0 }} choice={choice.focus} onChoice={choose("focus")}>
          <RowFacts issue={focusIssueFacts} all={focusFacts} more="" shown={!!shown.focus} onShow={show("focus")} />
          {pz ? null : setup("focus", "Set up focus…")}
        </DisclosureRow>

        <DisclosureRow rowKey="fluidics" quiet={pumpsOff} title="Fluidics" summary={pumpsSummary} chip={pumpsChip}
          state={{ issue: pumpIssue.length > 0 }} choice={choice.fluidics} onChoice={choose("fluidics")}>
          <RowFacts issue={checkFacts(pumpChecks.filter((x) => checkIssue(x)))} all={checkFacts(pumpChecks)} more="" shown={!!shown.fluidics} onShow={show("fluidics")} />
          {setup("fluidics", "Set up pumps…")}
        </DisclosureRow>

        <DisclosureRow rowKey="calibration" title="Calibration" summary={calSummary} chip={calChip}
          state={calState} choice={choice.calibration} onChoice={choose("calibration")}>
          <CalibrationCard embedded calibration={p.calibration} gate={calGate} gatesRead={!!snapshot?.valid} method={method}
            controlHeld={p.controlHeld} experimentActive={p.experimentActive} now={p.now} />
        </DisclosureRow>

        <DisclosureRow rowKey="method" title="Method" summary={methodSummary} chip={methodChip}
          state={{ issue: methodIssue.length > 0 }} choice={choice.method} onChoice={choose("method")}>
          {methodGates.length ? (
            <RowFacts issue={methodGates.filter((g) => isCounted(gateRole(g))).map(gateFact)} all={methodGates.map(gateFact)}
              more={methodPass ? `${methodPass} ${methodPass === 1 ? "gate passes" : "gates pass"}` : ""} shown={!!shown.method} onShow={show("method")} />
          ) : <p className="ws-note">{snapshot?.valid ? "No method gates reported." : "Unknown · gates not read yet"}</p>}
        </DisclosureRow>

        <DisclosureRow rowKey="destination" title="Data destination" summary={destSummary} chip={destChip}
          state={{ issue: destIssue.length > 0 }} choice={choice.destination} onChoice={choose("destination")}>
          <RowFacts issue={destIssueFacts} all={destAll} more="" shown={!!shown.destination} onShow={show("destination")} />
        </DisclosureRow>

        <DisclosureRow rowKey="all" title="All checks" summary={allChecksSummary(snapshot, p.readiness.error, p.experimentActive)}
          state={{ issue: allIssue.length > 0 }} choice={choice.all} onChoice={choose("all")}>
          {gates.length ? (
            <div className="ws-row-gates" data-testid="workspace-gates">
              {[gates.slice(0, half), gates.slice(half)].map((col, i) => <FactList key={i} className="ws-row-facts ws-row-facts--gates" facts={col.map(gateFact)} />)}
            </div>
          ) : (
            <FactList className="ws-row-facts" facts={[{ label: "Backend gates",
              value: p.experimentActive ? "Unknown · not read while a run is active" : p.readiness.error ? `Unknown · ${p.readiness.error}` : "Unknown · not read yet", tone: "unknown" }]} />
          )}
          <p className="ws-note" data-testid="workspace-required">{required.title}</p>
          {p.verdict.detail ? <p className="ws-note" data-testid="workspace-verdict-detail">{p.verdict.detail}</p> : null}
          <Card variant="tile" title="How Prepare reads readiness" className="ws-how" data-testid="workspace-how">
            {HOW_PREPARE_READS.map((l, i) => <p key={l} className="ws-note" data-testid={i === 1 ? "workspace-footnote" : undefined}>{l}</p>)}
          </Card>
          <div className="ws-row-actions">
            <Button size="small" onClick={() => { p.onRefreshChecks(); p.readiness.refresh(); }}>Refresh</Button>
            <Button size="small" onClick={p.readiness.refresh} disabled={p.experimentActive} title={p.experimentActive ? "Paused while a run is active" : undefined}>Read gates again</Button>
          </div>
        </DisclosureRow>
      </Card>
    </div>
  );
}
