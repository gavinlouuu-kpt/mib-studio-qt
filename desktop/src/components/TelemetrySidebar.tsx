// The classic left column (#797; board "Classic · Hardware Preflight · summary-first · redesigned
// left column"): one summary per subsystem, with a plain props model (no App internals). Top to
// bottom: a header chip mirroring the stage tabs with one context line; Camera; Processing; the
// PZ7035's PL core (unchanged); Focus (one section: the old "Autofocus" and "Nanopositioner
// Autofocus" were both `afStatus`); Fluidics; Experiment with grouped counters. Unavailable values
// read "—". A compact telemetry column is a kit gap (#715): styled locally with `--mib-*` tokens
// (TelemetrySidebar.css), inside a `mib-root` wrapper so the kit's font and colours apply.
//
// The aside's `aria-label`, `collapsed` class and the collapse toggle stay in App; this is the
// aside's body. Nothing here acts on hardware: every value is read by App or the shared pump poll.
import { StateChip, type StateTone } from "@mib/ui";
import type { AutofocusStatus, InstrumentStatus, PumpStatus } from "../bridge";
import { latencyReadout, linkReadout, sensorReadout, type Readout } from "../linkHealth";
import type { StageStatus } from "../workflow";
import {
  cameraFacts, contextLine, experimentFacts, fluidicsFacts, focusFacts, processingFacts, stageChipText, stageTone,
  type ExperimentInput, type ProcessingInput, type RunContext, type SideFact, type SideValue,
} from "./telemetrySidebarModel";
import "./TelemetrySidebar.css";

export interface TelemetrySidebarProps {
  /** The stage of the classic tab shown, as the stage tabs read it (index among the tabs). */
  stage: { index: number; title: string; status: StageStatus; statusLabel: string };
  /** The camera source as named ("Mock camera", a device label); "" when none is selected. */
  source: string;
  /** The run in progress (its file name, the queue's "sample n of m"); null while nothing runs. */
  run: RunContext | null;
  camera: { running: boolean; status: string; displayFps: number | null; dataRateMBs: number | null };
  processing: ProcessingInput;
  /** The PZ7035's PL core section, exactly as before; absent on the desktop. */
  plCore?: { instrument: InstrumentStatus | null } | null;
  /** The autofocus controller; `supported` false hides the section (the PZ7035). */
  focus: { supported: boolean; status: AutofocusStatus | null };
  /** The pumps from the shared poll (usePumpStatus). */
  fluidics: { pumps: readonly [PumpStatus | null, PumpStatus | null] };
  experiment: ExperimentInput;
}

function Value({ v }: { v: SideValue }) {
  const cls = ["tsb-row__v", v.tone ? `tsb-row__v--${v.tone}` : "", v.mute ? "tsb-row__v--mute" : ""].filter(Boolean).join(" ");
  return <span className={cls}>{v.text}</span>;
}

function Rows({ facts }: { facts: SideFact[] }) {
  return (
    <div className="tsb-rows">
      {facts.map((f) => (
        <div className="tsb-row" key={f.label}>
          <span className="tsb-row__k">{f.label}</span>
          <Value v={f.value} />
        </div>
      ))}
    </div>
  );
}

function Section({ title, children, testId }: { title: string; children: React.ReactNode; testId: string }) {
  return (
    <section className="tsb-section" data-testid={testId} aria-label={title}>
      <h4 className="tsb-section__title">{title}</h4>
      {children}
    </section>
  );
}

/** One dim line for optional hardware that is not connected. */
function Quiet({ text, testId }: { text: string; testId: string }) {
  return <p className="tsb-quiet" data-testid={testId}>{text}</p>;
}

const readoutValue = (r: Readout): SideValue => (r.cls === "warn" ? { text: r.text, tone: "warn" } : { text: r.text, mute: r.cls === "dim" });

/** The PZ7035 PL core and health rows (#501), as the column showed them before. */
function plCoreFacts(instrument: InstrumentStatus | null): SideFact[] {
  const noDevice = instrument && !instrument.available ? "no PL device" : "—";
  const id = (value: string | undefined) => (instrument?.core ? value?.slice(0, 8) || "—" : noDevice);
  return [
    { label: "Build", value: { text: id(instrument?.core?.build_id), tone: instrument?.core?.build_match === "match" ? "confirmed" : undefined, mute: instrument?.core?.build_match !== "match" } },
    { label: "Weights", value: { text: id(instrument?.core?.profile_id), tone: instrument?.core?.profile_match === "match" ? "confirmed" : undefined, mute: instrument?.core?.profile_match !== "match" } },
    { label: "LED", value: instrument?.led
      ? (instrument.led.guard_fault ? { text: "GUARD TRIPPED", tone: "blocked" } : instrument.led.on ? { text: `${instrument.led.preset} ${instrument.led.delay_us}/${instrument.led.width_us} µs` } : { text: "off", mute: true })
      : { text: "—", mute: true } },
    { label: "Sensor", value: readoutValue(sensorReadout(instrument)) },
    { label: "Link", value: readoutValue(linkReadout(instrument)) },
    { label: "Latency", value: readoutValue(latencyReadout(instrument)) },
  ];
}

export function TelemetrySidebar(p: TelemetrySidebarProps) {
  const chipTone: StateTone = stageTone(p.stage.status);
  const focus = p.focus.supported ? focusFacts(p.focus.status) : null;
  const fluidics = fluidicsFacts(p.fluidics.pumps);
  const plTitle = p.plCore?.instrument && !p.plCore.instrument.available && p.plCore.instrument.error ? p.plCore.instrument.error : "PZ7035 PL core and health (#501)";
  return (
    <div className="mib-root tsb" data-testid="telemetry-sidebar">
      <header className="tsb-head" data-testid="telemetry-head">
        <StateChip size="small" tone={chipTone} data-testid="telemetry-stage">{stageChipText(p.stage.index, p.stage.title, p.stage.statusLabel)}</StateChip>
        <p className="tsb-context" data-testid="telemetry-context">{contextLine(p.source, p.camera.running, p.run)}</p>
      </header>
      <Section title="Camera" testId="telemetry-camera"><Rows facts={cameraFacts(p.camera)} /></Section>
      <Section title="Processing" testId="telemetry-processing"><Rows facts={processingFacts(p.processing)} /></Section>
      {p.plCore ? (
        <section className="tsb-section" data-testid="telemetry-pl-core" aria-label="PL core" title={plTitle}>
          <h4 className="tsb-section__title">PL core</h4>
          <Rows facts={plCoreFacts(p.plCore.instrument)} />
        </section>
      ) : null}
      {p.focus.supported ? (focus
        ? <Section title="Focus" testId="telemetry-focus"><Rows facts={focus} /></Section>
        : <Quiet text="Focus · not connected" testId="telemetry-focus" />) : null}
      {fluidics
        ? <Section title="Fluidics" testId="telemetry-fluidics"><Rows facts={fluidics} /></Section>
        : <Quiet text="Fluidics · not connected" testId="telemetry-fluidics" />}
      <Section title="Experiment" testId="telemetry-experiment"><Rows facts={experimentFacts(p.experiment)} /></Section>
    </div>
  );
}
