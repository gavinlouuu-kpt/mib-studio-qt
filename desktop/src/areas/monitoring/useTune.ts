// The Monitoring tune panel's behaviour (epic #866, MON-069..081): keeps the draft's baseline in
// step with the running configuration while Monitoring is shown, and applies and reverts it. The
// draft itself is areas/monitoring/tuneDraft.ts (pure); its state lives in useMonitoringState().
//
// Where Apply goes. Qt writes the changed keys to "the active config file" and applies them live. The
// Tauri shell has no single active file (a profile, a central method or an opened document can each
// have set the running configuration), so:
//  - a checked configuration document is open in App config, is not being edited, and agrees with the
//    running configuration over every key it carries: Apply goes through `apply_config_document`
//    (revision check against the file's hash, writes the patch, applies it, reads it back);
//  - otherwise Apply goes to the running configuration only (`apply_processing_config_json`, read
//    back), and the footer says plainly that nothing was saved and why.
// Both paths are accepted while an experiment is Active when the patch names tune criteria only.
import { useCallback, useEffect, useRef } from "react";
import { bridge } from "../../bridge";
import { configDocument } from "../../configDocument";
import type { ConfigDocumentsModel, ConfigSyncModel } from "../config/useConfig";
import type { ShellState } from "../shell/useShell";
import type { MonitoringState } from "./useMonitoring";
import {
  documentDisagreements, readTuneValues, tuneCanApply, tuneDifferences, tunePatch, tunePatchSatisfiedBy, tuneScope,
  type ApplyOutcome, type TuneTarget,
} from "./tuneDraft";

/** How often the draft's baseline is compared with the running configuration while Monitoring is shown. */
export const TUNE_SYNC_MS = 2000;

type TuneDeps = Pick<ShellState, "ready" | "append" | "hostProcessing"> & Pick<MonitoringState, "tune" | "dispatchTune" | "expState">
  & Pick<ConfigSyncModel, "refreshConfig"> & Pick<ConfigDocumentsModel, "checkedConfig"> & { monitoringVisible: boolean };

const baseName = (path: string) => path.split(/[\\/]/).pop() || path;

export function useTunePanel({ ready, append, hostProcessing, tune, dispatchTune, expState, refreshConfig, checkedConfig, monitoringVisible }: TuneDeps) {
  const scope = tuneScope(expState, hostProcessing);
  const latest = useRef({ tune, scope, checkedConfig });
  latest.current = { tune, scope, checkedConfig };
  // Bumped whenever an apply or revert starts or settles: a read that began before it is dropped, so a
  // stale poll can never overwrite what the apply is about to settle.
  const epoch = useRef(0);

  const readRunning = useCallback(async () => {
    const doc = await bridge.fetchProcessingConfigJson();
    if (!doc.valid) return null;
    const section = (JSON.parse(doc.json) as { image_processing?: unknown }).image_processing;
    return { section, values: readTuneValues(section) };
  }, []);

  const sync = useCallback(async () => {
    const mark = epoch.current;
    try {
      const running = await readRunning();
      if (running?.values && mark === epoch.current) dispatchTune({ type: "external", values: running.values });
    } catch { /* backend gone: the monitoring poll surfaces it */ }
  }, [readRunning, dispatchTune]);

  // Visibility-gated like the monitoring poll: nothing runs while another view is shown.
  useEffect(() => {
    if (!ready || !monitoringVisible) return;
    let disposed = false;
    void sync();
    const id = window.setInterval(() => { if (!disposed) void sync(); }, TUNE_SYNC_MS);
    return () => { disposed = true; window.clearInterval(id); };
  }, [ready, monitoringVisible, sync]);

  const onTuneApply = useCallback(async () => {
    const { tune: s, scope: sc, checkedConfig: cfg } = latest.current;
    if (!tuneCanApply(s, sc) || !s.baseline) return;
    const requestId = s.nextRequest;
    const patchJson = JSON.stringify({ image_processing: tunePatch(s) });
    const count = s.changed.length;
    epoch.current++;
    dispatchTune({ type: "begin", requestId });
    const finish = (outcome: ApplyOutcome) => {
      epoch.current++;
      dispatchTune({ type: "result", requestId, outcome });
      if (outcome.kind === "failed") append(`Tune: not applied: ${outcome.error}`);
    };
    try {
      // The newest running configuration: a draft whose exposed values moved since they were loaded is a conflict, nothing is sent.
      const running = await readRunning();
      if (!running?.values) return finish({ kind: "failed", error: "the running configuration could not be read" });
      if (tuneDifferences(running.values, s.baseline).length) {
        dispatchTune({ type: "external", values: running.values });
        return finish({ kind: "failed", conflict: true, error: "the running configuration changed since these values were loaded; nothing was applied" });
      }
      // The open document is used for the save only when it can be trusted to say what runs.
      const doc = cfg.doc;
      let why = "";
      if (!doc) why = "no configuration file is open (App config > Open Config…)";
      else if (cfg.dirty) why = `${baseName(doc.path)} has unsaved edits in the App config editor`;
      else if (cfg.busy) why = `${baseName(doc.path)} is being changed in the App config editor`;
      else {
        try {
          const differs = documentDisagreements((JSON.parse(doc.document_json) as { image_processing?: unknown }).image_processing, running.section);
          if (differs.length) why = `${baseName(doc.path)} differs from the running configuration (${differs.slice(0, 3).join(", ")})`;
        } catch { why = `${baseName(doc.path)} could not be read`; }
      }
      let target: TuneTarget;
      if (doc && !why) {
        const r = await configDocument.apply(doc.path, doc.revision, patchJson);
        if (r.conflict) return finish({ kind: "failed", conflict: true, error: `${baseName(doc.path)} changed since it was opened; nothing was written` });
        if (!r.saved) return finish({ kind: "failed", error: r.error || `${baseName(doc.path)} was not written` });
        // The file changed: keep App config's copy of it in step, so its own next save is not a false conflict.
        try { const fresh = await configDocument.read(doc.path); if (fresh.ok) cfg.adopt(fresh); } catch { /* the editor's Reload / Revert recovers it */ }
        if (!(r.applied && r.verified))
          return finish({ kind: "failed", savedNotApplied: true, error: r.error || "the running configuration did not take the saved values" });
        target = { kind: "file", path: doc.path };
      } else {
        const res = await bridge.applyProcessingTune(patchJson);
        if (!res.ok) return finish({ kind: "failed", error: res.message });
        target = { kind: "runtime", why };
      }
      const after = await readRunning();
      if (!after?.values) return finish({ kind: "failed", savedNotApplied: target.kind === "file", error: "applied, but the running configuration could not be read back" });
      if (!tunePatchSatisfiedBy(s, after.values))
        return finish({ kind: "failed", savedNotApplied: target.kind === "file", error: "the processing service reports different values than were applied" });
      append(`Tune: ${count} change${count === 1 ? "" : "s"} applied${sc.midRun ? " to the running experiment" : ""}${target.kind === "file" ? ` and saved to ${target.path}` : " (not saved to a file: " + target.why + ")"}`);
      finish({ kind: "applied", effective: after.values, target });
      void refreshConfig();
    } catch (e) {
      finish({ kind: "failed", error: String(e) });
    }
  }, [append, dispatchTune, readRunning, refreshConfig]);

  const onTuneRevert = useCallback(async () => {
    epoch.current++;
    dispatchTune({ type: "revert" });
    const cfg = latest.current.checkedConfig;
    // A file conflict is cleared by reading the file again; an editor with unsaved edits is left alone.
    if (cfg.doc && !cfg.dirty && !cfg.busy) {
      try { const fresh = await configDocument.read(cfg.doc.path); if (fresh.ok) cfg.adopt(fresh); } catch { /* keep the old copy */ }
    }
    await sync();
  }, [dispatchTune, sync]);

  return { onTuneApply, onTuneRevert, tuneScopeNow: scope };
}
export type TunePanelModel = ReturnType<typeof useTunePanel>;
