// O1 Prepare (#713, track A step A1): the instrument's readiness, read-only. The hardware
// checklist (preflight.ts) and the authoritative backend gates read without a destination,
// summed up in one verdict (prepareVerdict). Nothing here acts on hardware: the only action is
// the operator's explicit Preflight confirmation, which stores a signature and sends nothing to
// the backend.
import { Button, Card, FactList, Notice, StateChip, Toolbar, type Fact } from "@mib/ui";
import type { StageStatus } from "../bridge";
import type { ReadinessGate } from "../eventAdapter";
import type { PreflightCheck, PreflightInput, PreflightReport } from "../preflight";
import {
  CONFIRMATION_TEXT,
  GATE_LABELS,
  READINESS_FOOTNOTE,
  checkFactTone,
  checkText,
  checkTone,
  confirmBlockers,
  confirmationState,
  gateFactTone,
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
import type { PrepareReadiness } from "./usePrepareReadiness";

export interface PrepareViewProps {
  /** The same snapshot the classic checklist is derived from. */
  preflightInput: PreflightInput;
  /** derivePreflight(preflightInput), computed once by the workspace. */
  report: PreflightReport;
  readiness: PrepareReadiness;
  /** The workspace's one verdict (also in its header chip). */
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
  /** Leave the workspace for the classic Connect tab, where devices are set up. */
  onOpenSetup: () => void;
}

const rows = (checks: PreflightCheck[]): Fact[] =>
  checks.map((c) => ({
    label: c.requirement === "required" ? `${c.label} · required` : c.label,
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

/** The gate's state and reason, then its remediation when there is one. */
function GateValue({ g, chip }: { g: ReadinessGate; chip: boolean }) {
  const fix = gateRemediation(g);
  return (
    <span className="ws-gate">
      {chip ? <StateChip size="small" tone={gateTone(g.status)}>{gateText(g)}</StateChip> : gateText(g)}
      {fix ? <span className="ws-gate-fix">{`Fix: ${fix}`}</span> : null}
    </span>
  );
}

/** The worst state among a card's checks (and extra facts), as one chip. */
function cardChip(checks: Pick<PreflightCheck, "status">[]) {
  const worst = worstState(checks);
  return worst ? <StateChip tone={checkTone(worst.status)}>{worst.text}</StateChip> : null;
}

export function PrepareView(p: PrepareViewProps) {
  const gates = p.readiness.snapshot?.valid ? p.readiness.snapshot.gates : [];
  const report = p.report;
  const byId = (ids: string[]) => report.checks.filter((c) => ids.includes(c.id));
  const camera = byId(["camera", "core", "plCore", "sensorLink", "led", "capture", "trigger"]);
  const focus = byId(["autofocus"]);
  const fluidics = byId(["samplePump", "sheathPump"]);
  const storage = byId(["storage"]);
  const required = summarizeRequired(report);
  const confirmation = confirmationState(p.signature, p.confirmedFor, report.criticalPassed);
  const confirmText = CONFIRMATION_TEXT[confirmation];
  // Why Confirm is unavailable, in plain words next to the button (never only a tooltip).
  const checklist = confirmBlockers(report);
  const locks = [p.confirmLockedReason, p.signature === "" ? "No camera is configured: there is nothing to confirm yet." : ""].filter(Boolean);
  const confirmWhy = [...locks, ...(checklist?.lines ?? [])];
  const confirmDisabled = confirmWhy.length > 0 || confirmation === "confirmed";
  const pz = p.preflightInput.capabilities?.instrument === "pz7035";
  const zStage = zStageFact(p.zStage);
  const storageGates = gates.filter((g) => g.id.startsWith("storage."));
  // Confirm follows the checklist; when it is available but Start would still be refused, say so.
  const startRefusal = startRefusalText(p.verdict.startBlocked);

  return (
    <div className="ws-view" data-testid="workspace-prepare">
      <Toolbar title="Instrument readiness">
        <Button variant="selected" aria-current="page">Overview</Button>
        <Button onClick={() => { p.onRefreshChecks(); p.readiness.refresh(); }}>Refresh</Button>
      </Toolbar>
      <div className="ws-body">
        {p.experimentActive && (
          <Notice tone="info" title="Readiness paused · a run is active">
            The gates are read again when the run has ended. Start always re-evaluates them.
          </Notice>
        )}
        <Card
          title={required.title}
          subtitle="Gates are evaluated by the backend and shown here read-only, without a destination: no file is written while you look. Start re-evaluates every gate with the chosen file."
          status={required.attention.length ? <>{required.attention.slice(0, 2).map((c) => (
            <StateChip key={c.id} tone={checkTone(c.status)}>{`${c.label}: ${checkText(c)}`}</StateChip>
          ))}</> : undefined}
        />
        <div className="ws-grid">
          <Card title="Camera and acquisition" subtitle={pz ? "Camera, PL core, sensor link, LED strobe, trigger" : "Camera, processing core, frame stream, trigger"}
            status={cardChip(camera)} actions={<Button onClick={p.onOpenSetup}>Camera setup</Button>}>
            <FactList facts={rows(camera)} />
          </Card>
          <Card title="Focus system" subtitle="Coarse Z stage + fine nanopositioner"
            status={pz ? <StateChip tone="unsupported">Unsupported</StateChip> : cardChip([...focus, zStage])}
            actions={pz ? undefined : <Button onClick={p.onOpenSetup}>Focus setup</Button>}>
            <FactList facts={pz
              ? [{ label: "Focus stages", value: "Unsupported · not on this instrument" }]
              : [{ label: "Coarse Z stage", value: zStage.value, tone: zStage.tone }, ...rows(focus)]} />
          </Card>
          <Card title="Fluidics" subtitle="Sample + sheath pumps" status={cardChip(fluidics)}
            actions={<Button onClick={p.onOpenSetup}>Pump setup</Button>}>
            <FactList facts={rows(fluidics)} />
          </Card>
          <Card title="Data destination" subtitle="The file is chosen at Start; the backend checks it then" status={cardChip(storage)}>
            <FactList facts={[
              ...rows(storage),
              ...storageGates.map((g) => ({ label: <GateLabel id={g.id} />, value: <GateValue g={g} chip={false} />, tone: gateFactTone(g.status) })),
            ]} />
          </Card>
          <Card
            title="Ready to operate?"
            subtitle="The checklist and the backend's gates, read without a destination"
            status={<StateChip tone={p.verdict.tone}>{p.verdict.title}</StateChip>}
            actions={<>
              <Button onClick={p.readiness.refresh} disabled={p.experimentActive} title={p.experimentActive ? "Paused while a run is active" : undefined}>Read gates again</Button>
              <Button variant="primary" onClick={p.onConfirm} disabled={confirmDisabled}>Confirm readiness</Button>
            </>}
          >
            {p.verdict.detail ? <p className="ws-note" data-testid="workspace-verdict-detail">{p.verdict.detail}</p> : null}
            <FactList facts={[{ label: "Preflight confirmation", value: confirmText.text, tone: confirmText.tone }]} />
            {gates.length ? (
              <FactList data-testid="workspace-gates" facts={gates.map((g) => ({ label: <GateLabel id={g.id} />, value: <GateValue g={g} chip /> }))} />
            ) : (
              <FactList facts={[{ label: "Backend gates", value: p.readiness.error ? `Unknown · ${p.readiness.error}` : "Unknown · not read yet", tone: "unknown" }]} />
            )}
            <p className="ws-note" data-testid="workspace-footnote">{READINESS_FOOTNOTE}</p>
            {confirmWhy.length > 0 && confirmation !== "confirmed" ? (
              <Notice tone={checklist && !locks.length ? checklist.tone : "info"} title="Confirm readiness is unavailable" data-testid="workspace-confirm-why">
                {confirmWhy.join(". ") + "."}
              </Notice>
            ) : null}
            {startRefusal ? (
              <Notice tone="warn" title={startRefusal.title} data-testid="workspace-start-refused">
                {startRefusal.text}
              </Notice>
            ) : null}
          </Card>
        </div>
      </div>
    </div>
  );
}
