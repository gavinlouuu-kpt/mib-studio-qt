// Connect and Overview (epic #863, CONNECT-056..061): the state of the Overview's overviewConfig.js editor (Qt
// OverviewTab onReloadJs / onSaveJs / onApplyJs / onBrowseJs / onClearJs). Owned by the studio controller, not the
// visible tab, so the unsaved text and an operation in flight survive a stage change; the stage change reads the
// resolved path through ScriptSources (syncScriptSources).
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type CameraSelection } from "../../bridge";
import { ask, confirm, open } from "../../transport/dialogs";
import type { ConfigDocumentsModel } from "../config/useConfig";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { PreviewState } from "../preview/usePreview";
import type { ShellState } from "../shell/useShell";
import type { CameraConnectionModel, CameraWindowingModel, ConnectState } from "./useConnect";
import {
  differsFromFile, failedToApply, failedToLoad, failedToSave, failedToSaveScript, isHardwareCamera, loadOverviewScript,
  MSG_APPLIED, MSG_CLEARED_QUESTION, MSG_CLEARED_TITLE, MSG_DISCARD_QUESTION, MSG_NO_HARDWARE_CAMERA, MSG_SAVED, overviewApplyBlock,
  overviewDefaultPath, overviewScriptPath, overviewScriptVisible, readExternalPath, saveOverviewScript, writeExternalPath,
  type ScriptDoc,
} from "./overviewScript";

export interface OverviewScriptContext {
  ready: boolean;
  expActive: boolean;
  /** Raw recording is on: the camera cannot be stopped underneath it. */
  recording: boolean;
  /** The platform has EGrabber camera scripts (caps.egrabber_script). */
  eGrabberScripts: boolean;
  selection: CameraSelection | null;
  /** The active profile's config.json ("" when none is applied). */
  profileConfigPath: string;
  append: (line: string) => void;
  refreshCameraState: () => Promise<void>;
  refreshGeometry: () => Promise<unknown>;
  setRunning: (running: boolean) => void;
}

type ContextDeps = Pick<ShellState, "ready" | "caps" | "append"> & Pick<MonitoringState, "expActive"> & Pick<PreviewState, "recording">
  & Pick<ConnectState, "camSelection" | "setRunning"> & Pick<CameraConnectionModel, "refreshCameraState">
  & Pick<CameraWindowingModel, "refreshCameraGeometry"> & Pick<ConfigDocumentsModel, "profiles">;

/** The editor's inputs from the studio's state (called by the controller, after the config documents hook). */
export function overviewScriptContext(s: ContextDeps): OverviewScriptContext {
  return {
    ready: s.ready, expActive: s.expActive, recording: s.recording, eGrabberScripts: s.caps.egrabber_script, selection: s.camSelection,
    profileConfigPath: s.profiles.activeProfile?.path ?? "", append: s.append, refreshCameraState: s.refreshCameraState,
    refreshGeometry: s.refreshCameraGeometry, setRunning: s.setRunning,
  };
}

/** The last thing an operation said: a plain status, or a failure (read out as an alert). */
export interface OverviewScriptMessage { level: "info" | "error"; text: string }

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

export function useOverviewScript(ctx: OverviewScriptContext) {
  const [appConfigDir, setAppConfigDir] = useState("");
  const [external, setExternal] = useState(readExternalPath);
  const [doc, setDoc] = useState<ScriptDoc | null>(null);
  // The path the document was asked for. The backend answers with the canonical path (a `\\?\` path on Windows), so the
  // editor compares and shows this one, and the stage change applies the same string.
  const [docFor, setDocFor] = useState("");
  const [text, setText] = useState("");
  const [dirty, setDirty] = useState(false);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<OverviewScriptMessage | null>(null);
  const pending = useRef(false);
  const live = useRef(ctx);
  live.current = ctx;

  const visible = overviewScriptVisible({ eGrabberScripts: ctx.eGrabberScripts, selection: ctx.selection });
  const defaultPath = overviewDefaultPath({ profileConfigPath: ctx.profileConfigPath, appConfigDir });
  const path = overviewScriptPath(external, defaultPath);
  const defaultPathRef = useRef(defaultPath);
  defaultPathRef.current = defaultPath;
  const info = (text: string) => setMessage({ level: "info", text });
  const fail = (text: string) => setMessage({ level: "error", text });

  // The folder for the default when no profile is applied.
  useEffect(() => {
    if (!ctx.ready || !ctx.eGrabberScripts) return;
    let alive = true;
    bridge.appPaths().then((p) => { if (alive) setAppConfigDir(p.app_config); }).catch(() => undefined);
    return () => { alive = false; };
  }, [ctx.ready, ctx.eGrabberScripts]);

  /** Load `target` into the editor (Qt loadFileToEditor); the default file is made from the bundled one when missing. */
  const load = useCallback(async (target: string): Promise<boolean> => {
    if (!target) { fail("No Overview script path yet."); return false; }
    try {
      const { doc: loaded, created } = await loadOverviewScript(target, target === defaultPathRef.current);
      setDoc(loaded);
      setDocFor(target);
      setText(loaded.text);
      setDirty(false);
      if (created) live.current.append(`Overview script ${loaded.path} created from the bundled default`);
      return true;
    } catch (e) {
      fail(failedToLoad(errorText(e)));
      live.current.append(`Overview script: failed to load ${target}: ${errorText(e)}`);
      return false;
    }
  }, []);

  // Qt loads the script when the tab is built; here once the backend is ready and the path is known, and again when the
  // path changes (another profile applied, another file) unless the operator has unsaved edits.
  const failedFor = useRef("");
  useEffect(() => {
    if (!ctx.ready || !visible || !path || dirty || pending.current) return;
    if (docFor === path || failedFor.current === path) return;
    failedFor.current = path;
    void load(path);
  }, [ctx.ready, visible, path, dirty, docFor, load]);

  const guarded = async (work: () => Promise<void>) => {
    if (pending.current) return;
    pending.current = true;
    setBusy(true);
    try { await work(); } finally { pending.current = false; setBusy(false); }
  };
  const discardEdits = async () => !dirty || await confirm(MSG_DISCARD_QUESTION);

  /** The editor shows the file of the resolved path (a profile change while the operator edits leaves it behind). */
  const loaded = doc !== null && docFor === path;

  /** Reset (Qt onReloadJs): reload from the file, making the default when it is missing. */
  const reset = () => guarded(async () => {
    if (!await discardEdits()) return;
    setMessage(null);
    failedFor.current = "";
    await load(path);
  });

  const write = async (failed: (reason: string) => string): Promise<ScriptDoc | null> => {
    if (!doc || !loaded) { fail(failed("the script is not loaded")); return null; }
    try {
      const saved = await saveOverviewScript(doc, text);
      setDoc(saved);
      setText(saved.text);
      setDirty(false);
      return saved;
    } catch (e) {
      fail(failed(errorText(e)));
      live.current.append(`Overview script: failed to save ${doc.path}: ${errorText(e)}`);
      return null;
    }
  };

  /** Save (Qt onSaveJs): written to disk, confirmed with "Saved." (an inline status; Qt opens an information box). */
  const save = () => guarded(async () => {
    setMessage(null);
    const saved = await write(failedToSave);
    if (saved) { info(MSG_SAVED); live.current.append(`Overview script saved: ${docFor}`); }
  });

  /** Apply to Camera (Qt onApplyJs): save first, then the Overview role. The camera is stopped first (the Rust guard
   *  refuses a running one; Qt's backend stops it itself) and stays stopped, as in Qt. */
  const apply = () => guarded(async () => {
    const c = live.current;
    setMessage(null);
    const block = overviewApplyBlock({ ready: c.ready, expActive: c.expActive, recording: c.recording });
    if (block) { fail(failedToApply(block)); return; }
    if (!await write(failedToSaveScript)) return;
    try {
      // A fresh selection: the poll behind ctx.selection may be stale.
      const selection = await bridge.fetchCameraSelection();
      if (!isHardwareCamera(selection)) { fail(failedToApply(MSG_NO_HARDWARE_CAMERA)); return; }
      if (selection.running) {
        const stopped = await bridge.stopCapture();
        if (!stopped.ok) { fail(failedToApply(`the camera could not be stopped: ${stopped.message}`)); return; }
        c.setRunning(false);
      }
      const result = await bridge.applyCameraScriptWithRole(docFor, true);
      if (!result.ok) {
        fail(failedToApply(result.message));
        c.append(`Overview script not applied: ${result.message}`);
      } else {
        info(MSG_APPLIED);
        c.append(`Overview script applied from ${docFor}`);
      }
      await c.refreshCameraState();
      await c.refreshGeometry();
    } catch (e) {
      fail(failedToApply(errorText(e)));
      c.append(`Overview script not applied: ${errorText(e)}`);
    }
  });

  /** Browse... (Qt onBrowseJs): pick a script anywhere; the choice is kept across reloads. */
  const browse = () => guarded(async () => {
    if (!await discardEdits()) return;
    const picked = await open({
      multiple: false, directory: false, defaultPath: path || undefined, title: "Select Camera script (overviewConfig.js)",
      filters: [{ name: "JavaScript files", extensions: ["js"] }, { name: "All Files", extensions: ["*"] }],
    });
    if (typeof picked !== "string" || !picked) return;
    setMessage(null);
    // Qt remembers the choice before it loads it.
    writeExternalPath(picked);
    setExternal(picked);
    live.current.append(`External Overview script set to ${picked}`);
    failedFor.current = "";
    await load(picked);
  });

  /** Clear (Qt onClearJs): forget the external path, then offer to reload the default. */
  const clear = () => guarded(async () => {
    writeExternalPath("");
    setExternal("");
    live.current.append("External Overview script cleared; reverting to the default path");
    let reload = false;
    try { reload = await ask(MSG_CLEARED_QUESTION, { title: MSG_CLEARED_TITLE, kind: "info", okLabel: "Yes", cancelLabel: "No" }); } catch { reload = false; }
    setMessage(null);
    failedFor.current = "";
    if (reload) { await load(defaultPath); return; }
    // "No": the editor keeps the text it has, now aimed at the default file (Qt: "Save to apply any changes"). Take that
    // file's revision so Save can write it, and mark the text as unsaved when it differs from it.
    try {
      const { doc: target } = await loadOverviewScript(defaultPath, true);
      setDoc(target);
      setDocFor(defaultPath);
      setDirty(differsFromFile(text, target.text));
    } catch (e) {
      fail(failedToLoad(errorText(e)));
    }
  });

  const edit = (value: string) => {
    setText(value);
    setDirty(doc ? differsFromFile(value, doc.text) : true);
    setMessage(null);
  };

  const applyBlock = overviewApplyBlock({ ready: ctx.ready, expActive: ctx.expActive, recording: ctx.recording });
  return {
    visible, path, defaultPath, external, text, dirty, busy, message, loaded, applyBlock,
    /** The file the editor shows (the label); differs from `path` while the operator keeps edits across a profile change. */
    shownPath: docFor || path,
    edit, reset, save, apply, browse, clear,
  };
}
export type OverviewScriptModel = ReturnType<typeof useOverviewScript>;
