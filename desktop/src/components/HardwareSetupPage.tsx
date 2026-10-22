// The classic Hardware setup page (#798; board "Classic · Hardware setup"): one page, one card per
// device, the connection state as a chip, fields grouped, dangerous actions set apart in a gated
// tile. Reached from a preflight row's "Set up …" (#796) and from Settings → Hardware setup…;
// "Back to preflight" returns. Nothing is deleted: every field, select and button of today's
// forms exists here, placed from the parts `HardwareControls` hands out through its `layout`
// prop (one instance: one poll, one busy state, one command owner), the camera sources App
// renders, and `StageControls` as is. Every backend command keeps its arming and mode rules.
//
// `HardwareSetupPage` is the header (title, subtitle, Back to preflight) and the Mode notice, whose
// button is the existing Mode switch; it sits outside the ViewerGate so a viewer can still leave.
// `HardwareSetupCards` is the body: it is what App passes as the `layout` of HardwareControls,
// inside the ViewerGate (a viewer watches, never operates).
//
// Kit gaps (#715 follow-ups), styled locally with `--mib-*` tokens (HardwareSetupPage.css): plain
// text inputs, selects and checkboxes, and the gated-actions tile.
import { useEffect, type ReactNode } from "react";
import { Button, Card, Notice, StateChip } from "@mib/ui";
import type { OperatingMode } from "../commissioning";
import type { SetupSection } from "../workspace/PrepareSummary";
import type { AutofocusParts, HardwareParts, PumpParts } from "./HardwareControls";
import type { PulseGeneratorParts } from "./PulseGeneratorControls";
import "./HardwareSetupPage.css";

export const SETUP_SUBTITLE = "Reached from a preflight row's Set up… and from Settings · nothing here is required for the mock camera";

/** The Mode notice's words: what is locked in Operator mode, what Service mode needs. */
export function modeNotice(mode: OperatingMode, armed: boolean): { tone: "unknown" | "warn"; title: string; text: string; button: string } {
  if (mode !== "service") {
    return {
      tone: "unknown",
      title: "Operator mode · run, purge, jog and move are locked",
      text: "Connecting devices and reading their status is always allowed. Dangerous actions need Service / Commissioning mode and arming.",
      button: "Enter Service mode…",
    };
  }
  return {
    tone: "warn",
    title: `Service / Commissioning mode · ${armed ? "armed for one action" : "not armed"}`,
    text: armed
      ? "The next run, purge, jog or move is sent and consumes the arming. Not for routine operation."
      : "Arm one hardware action in the banner above before a run, purge, jog or move. Not for routine operation.",
    button: "Exit to Operator",
  };
}

export interface HardwareSetupPageProps {
  onBack: () => void;
  mode: OperatingMode;
  armed: boolean;
  /** The existing Mode switch (App's enterMode): the notice's button, not a new mechanism. */
  onMode: (next: OperatingMode) => void;
  children: ReactNode;
}

export function HardwareSetupPage(p: HardwareSetupPageProps) {
  const notice = modeNotice(p.mode, p.armed);
  return (
    <div className="mib-root hw-setup" data-testid="hardware-setup-page">
      <header className="hw-setup__head">
        <div className="hw-setup__title">
          <h2>Hardware setup</h2>
          <p className="hw-setup__sub">{SETUP_SUBTITLE}</p>
        </div>
        <Button onClick={p.onBack} data-testid="hardware-setup-back">Back to preflight</Button>
      </header>
      <Notice tone={notice.tone} title={notice.title} data-testid="hardware-setup-mode"
        actions={<Button size="small" variant={p.mode === "service" ? "secondary" : "primary"} onClick={() => p.onMode(p.mode === "service" ? "operator" : "service")}>{notice.button}</Button>}>
        {notice.text}
      </Notice>
      {p.children}
    </div>
  );
}

/** The chip of a gated tile from the actuate gate's reason (hardwareGate / canActuate words). */
export function lockChip(reason: string): { tone: "unknown" | "confirmed"; text: string } {
  if (!reason) return { tone: "confirmed", text: "Unlocked · armed" };
  if (/not ready/i.test(reason)) return { tone: "unknown", text: "Locked · backend not ready" };
  if (/experiment/i.test(reason)) return { tone: "unknown", text: "Locked · experiment active" };
  if (/not connected|not attached/i.test(reason)) return { tone: "unknown", text: "Locked · not connected" };
  if (/arm/i.test(reason)) return { tone: "unknown", text: "Locked · not armed" };
  return { tone: "unknown", text: "Locked · Service / Commissioning mode" };
}

/** The gated-actions tile (kit gap #715): grey tile, a Locked / Unlocked chip, the reason in one
 *  line, the gated buttons (disabled by their own gate when locked) and the Stops, always enabled. */
export function GatedTile({ reason, note, children, testId }: { reason: string; note: string; children: ReactNode; testId: string }) {
  const chip = lockChip(reason);
  return (
    <div className="hw-gate" data-testid={testId} data-locked={reason ? "locked" : "unlocked"}>
      <div className="hw-gate__head">
        <StateChip size="small" tone={chip.tone}>{chip.text}</StateChip>
        <span className="hw-gate__why">{reason || note}</span>
      </div>
      <div className="hw-gate__actions">{children}</div>
    </div>
  );
}

/** A Stop: always available, styled as destructive. */
const Destructive = ({ children }: { children: ReactNode }) => <span className="hw-destructive">{children}</span>;

const connectionChip = (connected: boolean) => <StateChip tone={connected ? "confirmed" : "neutral"}>{connected ? "Connected" : "Not connected"}</StateChip>;

/** One pump card, rendered once per pump from the slot's parts. */
export function PumpCard({ pump }: { pump: PumpParts }) {
  return (
    <Card title={pump.title} data-testid="setup-pump" data-setup-card="pump" status={connectionChip(pump.connected)}
      subtitle={pump.peristaltic ? "Peristaltic · model, serial port, baud · address" : "Syringe · model, serial port, baud · address"}>
      <div className="hw-status">{pump.status}</div>
      {pump.setup}
      <div className="hardware-actions">{pump.connection}</div>
      <h4 className="hw-group">Rate, direction and syringe volume</h4>
      {pump.rate}
      <GatedTile reason={pump.actuateReason} note="Run and purge need Service / Commissioning mode and arming; Stop stays available" testId={`setup-pump-gate-${pump.id}`}>
        {pump.actions.run}{pump.actions.purge}
        <Destructive>{pump.actions.stopPump}</Destructive>
        <Destructive>{pump.actions.stopPurge}</Destructive>
      </GatedTile>
    </Card>
  );
}

function FocusCard({ focus }: { focus: AutofocusParts }) {
  return (
    <Card title="Focus · autofocus / nanopositioner" data-testid="setup-focus" data-setup-card="focus" status={connectionChip(focus.connected)}
      subtitle="Backend (CoreMOR / OEABT), COM port, endpoint id">
      <div className="hw-status">{focus.status}</div>
      {focus.setup}
      <div className="hardware-actions">{focus.connection}</div>
      <h4 className="hw-group">Autofocus</h4>
      {focus.config}
      <GatedTile reason={focus.actuateReason} note="Jog and Enable need Service / Commissioning mode and arming; Disable stays available" testId="setup-focus-gate">
        {focus.actions.jogDown}{focus.actions.jogUp}{focus.actions.enable}
        <Destructive>{focus.actions.disable}</Destructive>
      </GatedTile>
    </Card>
  );
}

/** The pulse generator's card: the layout handed to PulseGeneratorControls. */
export function pulseCard(parts: PulseGeneratorParts): ReactNode {
  return (
    <Card title="Acquisition pulse generator (RS485)" data-testid="setup-pulse" data-setup-card="pulse" status={connectionChip(parts.connected)}
      subtitle="Serial port, device address · channel timing (frequency, duty). Camera acquisition pulses, not sorter trigger pulses.">
      <div className="hw-status">{parts.status}</div>
      {parts.error && <p role="alert">{parts.error}</p>}
      <div className="hardware-fields">{parts.setup}</div>
      <div className="hardware-actions">{parts.connection}</div>
      <h4 className="hw-group">Timing</h4>
      <p className="hw-note">Disconnect does not stop an existing output; disable each channel first.</p>
      <div className="hardware-fields">{parts.timing}</div>
      <GatedTile reason={parts.actuateReason} note="Start pulses needs Service / Commissioning mode and arming; Stop pulses stays available" testId="setup-pulse-gate">
        {parts.actions.enable}
        <Destructive>{parts.actions.disable}</Destructive>
      </GatedTile>
    </Card>
  );
}

export interface HardwareSetupCardsProps {
  parts: HardwareParts;
  /** The camera sources (App's "Available devices" sub-tabs, Refresh / Connect / Configure Mock…). */
  cameraSources: ReactNode;
  /** The Z stage panel as is; null on the PZ7035. */
  stage: ReactNode;
  /** The card to scroll into view, and a counter that grows on every visit (the same section twice). */
  setupSection: SetupSection | null;
  visit: number;
}

const SECTION_CARD: Readonly<Record<SetupSection, string>> = { camera: "camera", focus: "focus", fluidics: "pumps" };

export function HardwareSetupCards(p: HardwareSetupCardsProps) {
  useEffect(() => {
    if (!p.setupSection) return;
    const card = document.querySelector<HTMLElement>(`.hw-setup [data-setup-card="${SECTION_CARD[p.setupSection]}"]`);
    card?.scrollIntoView?.({ block: "start" });
  }, [p.setupSection, p.visit]);
  const n = p.parts.notices;
  return (
    <div className="hw-setup__cards" data-testid="hardware-setup-cards">
      <Card title="Camera sources and startup selection" data-testid="setup-camera" data-setup-card="camera"
        subtitle="The found devices, the camera to use, and what is selected at startup">
        {p.cameraSources}
        {p.parts.startup}
      </Card>
      {n.configureReason && <Notice tone="info" title={n.configureReason} />}
      {n.error && <Notice tone="blocked" title="Last command" data-testid="setup-error"><span role="alert">{n.error}</span></Notice>}
      {n.statusError && <Notice tone="unknown" title="Status unavailable" data-testid="setup-status-error">{n.statusError}</Notice>}
      {p.parts.instrumentPumpText && <p className="hw-note">{p.parts.instrumentPumpText}</p>}
      <div className="hw-setup__pumps" data-setup-card="pumps" data-testid="setup-pumps">
        {p.parts.pumps.map((pump) => <PumpCard key={pump.id} pump={pump} />)}
      </div>
      {p.parts.autofocus && <FocusCard focus={p.parts.autofocus} />}
      {p.stage && (
        <Card title="Z stage" data-testid="setup-stage" data-setup-card="stage" subtitle="Connection port, stage profile (ZC300, no Home) · Position · Zero · Move">
          {p.stage}
        </Card>
      )}
      {p.parts.pulseGenerator?.(pulseCard)}
    </div>
  );
}
