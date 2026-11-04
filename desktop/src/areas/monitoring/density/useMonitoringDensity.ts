// The Monitoring density (KDE) behaviour (epic #866 PR E2: MON-058..063, 083..086): the effects and the
// callbacks behind the toggle, the contours and the reference. The state is in useMonitoringState() (the
// `PR E2: density (KDE)` block); this named hook is called from studio/useStudioController.ts and returns the
// two things the views read through StudioContext: `kdeLayer` (the Monitoring charts) and
// `kdeReferenceControls` (the Monitoring Settings dialog).
//
// The backend owns the estimate (MonitoringDensityService on its own lowest-priority worker); the shell
//  - pushes the settings with `monitoring_density_set_settings` whenever one of them changes: enabled is the
//    toggle AND the Monitoring sub-tab shown (false when hidden, as Qt does), the interval, the bandwidth
//    factor, the core fraction and the scatter's fixed axes;
//  - polls `fetch_monitoring_density(since_generation)` every 500 ms while enabled, one request at a time; a
//    reply of `unchanged` changes nothing and costs no render;
//  - keeps the reference contour (pinned, or read from an experiment file) in memory and its path in the
//    preferences.
// A viewer (another client controls the instrument) sends nothing and polls nothing: the settings are one set
// per instrument and a control command; the toggle is disabled for it and the chart is the plain one.
import { useCallback, useEffect, useMemo, useRef } from "react";
import { bridge } from "../../../bridge";
import { POLL_FAILURE_THRESHOLD, POLL_HEALTHY, pollHealthAfter, pollNotice, type PollHealth } from "../pollHealth";
import { open, message } from "../../../transport/dialogs";
import type { KdeReference } from "../../../monitoringDensity";
import type { ShellState } from "../../shell/useShell";
import type { MonitoringState } from "../useMonitoring";
import {
  densityTooltip, estimateOfReply, pinnedLabel, type Loop, type MonitoringDensityLayer,
} from "./densityModel";
import { densityPrefsOf, fractionOfPercent, saveDensityPrefs } from "./densityPrefs";
import {
  REFERENCE_ERROR_TITLE, REFERENCE_FILE_DIALOG_TITLE, REFERENCE_FILE_FILTERS, referenceErrorText, referenceFailureReason, referenceOfReply,
  referenceStartDir, restoreOutcome, restoreOutcomeOfError, type RestoreOutcome,
} from "./densityReference";

/** The density poll: the same spacing as the Monitoring snapshot poll, never faster. */
export const DENSITY_POLL_MS = 500;

/** What the Monitoring Settings dialog's reference row needs (DensityReferenceControls.tsx). */
export interface DensityReferenceControlsModel {
  /** The loaded reference's label (Qt's), or null when there is none. */
  label: string | null;
  /** Qt: KDE on and a core contour to keep. */
  canPin: boolean;
  /** A reference is loaded, or a remembered path is waiting to be loaded. */
  canClear: boolean;
  /** Why From file… cannot be used now (the instrument only reads HDF5 files while idle); undefined = it can. */
  fromFileBlocked?: string;
  onPin: () => void;
  /** The picker, the read and, on failure, the error dialog; resolves when that is over. */
  onFromFile: () => Promise<void>;
  onClear: () => void;
}

export const FROM_FILE_BUSY_REASON = "An experiment file is read only while the instrument is idle: not while an experiment runs or starts.";

type DensityDeps = Pick<ShellState, "ready" | "append">
  & Pick<MonitoringState, "monSettings" | "expActive" | "onMonitoringCleared" | "kdeEnabled" | "setKdeEnabled" | "kdeReferencePath"
    | "setKdeReferencePath" | "kdeReference" | "setKdeReference" | "kdeEstimate" | "setKdeEstimate" | "kdeNotice" | "setKdeNotice">
  & { monitoringVisible: boolean; viewerOnly: boolean };

const NO_LOOPS: readonly Loop[] = [];

export function useMonitoringDensity({
  ready, append, monitoringVisible, viewerOnly, expActive, monSettings, onMonitoringCleared, kdeEnabled, setKdeEnabled, kdeReferencePath,
  setKdeReferencePath, kdeReference, setKdeReference, kdeEstimate, setKdeEstimate, kdeNotice, setKdeNotice,
}: DensityDeps) {
  const { kdeBandwidthFactor, kdeIntervalMs, kdeCorePercent, scatterXMin, scatterXMax, scatterYMin, scatterYMax } = monSettings;
  // The layer is on for the operator who controls the instrument; the service computes only while Monitoring is shown.
  const on = kdeEnabled && !viewerOnly;
  const active = ready && on && monitoringVisible;

  const appendRef = useRef(append);
  appendRef.current = append;
  const activeRef = useRef(active);
  activeRef.current = active;
  const onRef = useRef(on);
  onRef.current = on;

  // ---- one notice at a time: a refused or failing command / poll, said once ----
  const noticeKind = useRef<"" | "push" | "poll">("");
  const lastLogged = useRef("");
  const tell = useCallback((kind: "push" | "poll", text: string) => {
    if (text) {
      noticeKind.current = kind;
      setKdeNotice(text);
      if (lastLogged.current !== text) {
        lastLogged.current = text;
        appendRef.current(text);
      }
    } else if (noticeKind.current === kind) {
      noticeKind.current = "";
      lastLogged.current = "";
      setKdeNotice("");
    }
  }, [setKdeNotice]);

  // ---- preferences (MON-063): the toggle, the three numbers and the reference path survive a restart ----
  const savedPrefs = useRef("");
  useEffect(() => {
    const prefs = densityPrefsOf(kdeEnabled, kdeReferencePath, { kdeBandwidthFactor, kdeIntervalMs, kdeCorePercent });
    const text = JSON.stringify(prefs);
    if (savedPrefs.current === "") {
      // The first run holds what was loaded; storing it again would only create a record for the defaults.
      savedPrefs.current = text;
      return;
    }
    if (text === savedPrefs.current) return;
    savedPrefs.current = text;
    saveDensityPrefs(prefs);
  }, [kdeEnabled, kdeReferencePath, kdeBandwidthFactor, kdeIntervalMs, kdeCorePercent]);

  // ---- push the settings (control command) whenever one changes; `enabled` false while hidden ----
  useEffect(() => {
    if (!ready || viewerOnly) return;
    let current = true;
    bridge
      .monitoringDensitySetSettings({
        enabled: active,
        intervalMs: kdeIntervalMs,
        bandwidthFactor: kdeBandwidthFactor,
        coreFraction: fractionOfPercent(kdeCorePercent),
        x0: scatterXMin,
        x1: scatterXMax,
        y0: scatterYMin,
        y1: scatterYMax,
      })
      .then((reply) => {
        // Said only to an operator who has the density switched on: the push of "off" at start-up is routine.
        if (current) tell("push", reply.ok || !onRef.current ? "" : `Density (KDE) settings were not accepted by the backend (${reply.message || "refused"}): the colouring may not follow them.`);
      })
      .catch((e) => {
        if (current) tell("push", onRef.current ? `Density (KDE) settings could not be sent to the backend (${e instanceof Error ? e.message : String(e)}): the colouring may not follow them.` : "");
      });
    return () => { current = false; };
  }, [ready, viewerOnly, active, kdeIntervalMs, kdeBandwidthFactor, kdeCorePercent, scatterXMin, scatterXMax, scatterYMin, scatterYMax, tell]);

  // ---- poll the result while enabled (reads); Clear Buffer bumps the epoch so a reply in flight is not drawn ----
  const generation = useRef<string | null>(null);
  const epoch = useRef(0);
  useEffect(() => {
    if (!active) {
      // Off (or hidden): drop the densities and contours; asking again later returns whatever the service holds.
      generation.current = null;
      setKdeEstimate(null);
      return;
    }
    let disposed = false, polling = false;
    let health: PollHealth = POLL_HEALTHY;
    const poll = async () => {
      if (polling) return;
      polling = true;
      const mine = epoch.current;
      try {
        const reply = await bridge.fetchMonitoringDensity(generation.current);
        if (disposed || mine !== epoch.current) return;
        health = pollHealthAfter(health);
        tell("poll", "");
        if (!reply.valid || reply.unchanged) return; // nothing newer: no state, no render
        generation.current = reply.generation;
        setKdeEstimate(estimateOfReply(reply));
      } catch (e) {
        if (disposed) return;
        health = pollHealthAfter(health, e === undefined || e === null ? "unknown error" : e);
        // Said once, when the failures reach the threshold: not on every further tick.
        if (health.failures === POLL_FAILURE_THRESHOLD) tell("poll", pollNotice("Density (KDE)", health));
      } finally {
        polling = false;
      }
    };
    void poll();
    const id = window.setInterval(() => void poll(), DENSITY_POLL_MS);
    return () => { disposed = true; window.clearInterval(id); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [active]);

  // ---- Clear Buffer drops the KDE state like Qt (the densities and contours; a reference stays) ----
  useEffect(() => onMonitoringCleared(() => {
    epoch.current += 1;
    setKdeEstimate(null);
    // The service drops its result for the empty ring on its next wake; wake it now.
    if (activeRef.current) void bridge.monitoringDensityRequestUpdate().catch(() => undefined);
  }), [onMonitoringCleared, setKdeEstimate]);

  // ---- the reference contour ----
  const pathRef = useRef(kdeReferencePath);
  pathRef.current = kdeReferencePath;

  // Reload the file chosen in a previous session once the backend is ready and idle (MON-062). A file that is gone
  // (or has no contour) is dropped with one log line; a refusal because the instrument is busy keeps the path and
  // tries again when it is idle (a run ended).
  const restoring = useRef(false);
  const waitingLogged = useRef("");
  const pendingPath = kdeReference === null ? kdeReferencePath : "";
  useEffect(() => {
    if (!ready || !pendingPath || expActive || restoring.current) return;
    restoring.current = true;
    void (async () => {
      let outcome: RestoreOutcome;
      try {
        outcome = restoreOutcome(await bridge.fetchKdeReferenceFromFile(pendingPath));
      } catch (e) {
        outcome = restoreOutcomeOfError(e);
      }
      restoring.current = false;
      if (pathRef.current !== pendingPath) return; // pinned, cleared or replaced meanwhile
      if (outcome.kind === "restored") {
        setKdeReference(outcome.reference);
      } else if (outcome.kind === "drop") {
        setKdeReferencePath("");
        appendRef.current(`Monitoring KDE: previous reference contour ${pendingPath} not restored: ${outcome.why}`);
      } else if (waitingLogged.current !== pendingPath) {
        waitingLogged.current = pendingPath;
        appendRef.current(`Monitoring KDE: previous reference contour ${pendingPath} not restored yet: ${outcome.why}; it is tried again when the instrument is idle`);
      }
    })();
  }, [ready, pendingPath, expActive, setKdeReference, setKdeReferencePath]);

  const contours = kdeEstimate?.contours;
  const pin = useCallback(() => {
    if (!on || !contours || contours.length === 0) return;
    // A pinned contour lives for the session only (Qt: the path is forgotten).
    const reference = { label: pinnedLabel(new Date()), contours };
    setKdeReference(reference);
    setKdeReferencePath("");
    appendRef.current(`KDE reference contour: ${contours.length} loop(s) (${reference.label})`);
  }, [on, contours, setKdeReference, setKdeReferencePath]);

  const clear = useCallback(() => {
    setKdeReference(null);
    setKdeReferencePath("");
    appendRef.current("KDE reference contour cleared");
  }, [setKdeReference, setKdeReferencePath]);

  const fromFile = useCallback(async () => {
    let picked: unknown;
    try {
      picked = await open({
        title: REFERENCE_FILE_DIALOG_TITLE, filters: REFERENCE_FILE_FILTERS, multiple: false, directory: false,
        defaultPath: referenceStartDir(pathRef.current),
      });
    } catch (e) {
      appendRef.current(`KDE reference contour: the file picker failed: ${e instanceof Error ? e.message : e}`);
      return;
    }
    if (typeof picked !== "string" || picked === "") return; // cancelled
    const path = picked;
    const fail = async (why: string) => {
      appendRef.current(`KDE reference contour: none loaded from ${path}: ${why}`);
      await message(referenceErrorText(path, why), { title: REFERENCE_ERROR_TITLE, kind: "error" });
    };
    let reply: KdeReference;
    try {
      reply = await bridge.fetchKdeReferenceFromFile(path);
    } catch (e) {
      return fail(e instanceof Error ? e.message : String(e));
    }
    const reference = reply.ok ? referenceOfReply(reply) : null;
    if (!reference) return fail(referenceFailureReason(reply));
    // Nothing changed on a failure above: Qt keeps the previous reference and its path.
    setKdeReference(reference);
    setKdeReferencePath(path);
    appendRef.current(`KDE reference contour: ${reference.contours.length} loop(s) (${reference.label})`);
  }, [setKdeReference, setKdeReferencePath]);

  const tooltip = densityTooltip({ on, estimate: kdeEstimate, intervalMs: kdeIntervalMs, corePercent: kdeCorePercent, reference: kdeReference });
  const kdeLayer = useMemo<MonitoringDensityLayer>(() => ({
    on,
    byRow: on ? kdeEstimate?.byRow ?? null : null,
    contours: on ? contours ?? NO_LOOPS : NO_LOOPS,
    reference: on ? kdeReference?.contours ?? NO_LOOPS : NO_LOOPS,
    toggle: { onChange: setKdeEnabled, tooltip, notice: kdeNotice },
  }), [on, kdeEstimate, contours, kdeReference, setKdeEnabled, tooltip, kdeNotice]);

  const canPin = on && !!contours && contours.length > 0;
  const hasReference = kdeReference !== null;
  const kdeReferenceControls = useMemo<DensityReferenceControlsModel>(() => ({
    label: kdeReference?.label ?? null,
    canPin,
    canClear: hasReference || kdeReferencePath !== "",
    fromFileBlocked: expActive ? FROM_FILE_BUSY_REASON : undefined,
    onPin: pin,
    onFromFile: fromFile,
    onClear: clear,
  }), [kdeReference, canPin, hasReference, kdeReferencePath, expActive, pin, fromFile, clear]);

  return { kdeLayer, kdeReferenceControls };
}
