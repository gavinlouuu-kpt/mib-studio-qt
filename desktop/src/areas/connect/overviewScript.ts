// Connect and Overview (epic #863, CONNECT-056..061): the Overview's own camera script, overviewConfig.js (Qt
// OverviewTab.cpp:166-167, 300-407, 495-507). It is the full-sensor script an eGrabber camera runs on the Overview
// stage; the Experiment script is another file (Config > Camera script). Pure rules, storage and the camera_document
// calls live here; the hook (useOverviewScript.ts) and the view (OverviewScriptEditor.tsx) use them.
import type { CameraSelection } from "../../bridge";
import { CAMERA_SELECTION_MODES } from "../../bridgeContract";
import { invoke } from "../../transport";
import { joinPath, profileFile } from "./cameraStage";

/** localStorage key of the external Overview script path (Qt QSettings `Config/ExternalOverviewScriptPath`). */
export const OVERVIEW_SCRIPT_KEY = "mib.overview.script-path";
export const OVERVIEW_SCRIPT_FILE = "overviewConfig.js";

// Qt's texts (OverviewTab::onSaveJs, onApplyJs, onClearJs, resources/ui/OverviewTab.ui).
export const MSG_SAVED = "Saved.";
export const MSG_APPLIED = "Applied to camera. Capture remains stopped.";
export const MSG_UNSAVED = "Unsaved changes – click Save to apply.";
export const MSG_CLEARED_QUESTION = "External Camera script path cleared.\nReset from default include path now?\n\nNote: Save to apply any changes.";
export const MSG_CLEARED_TITLE = "Camera Script Path Cleared";
export const MSG_NO_HARDWARE_CAMERA = "No hardware camera selected";
export const MSG_DISCARD_QUESTION = "Discard the unsaved changes to the Overview script?";

export const failedToLoad = (reason: string) => `Failed to load: ${reason}`;
export const failedToSave = (reason: string) => `Failed to save: ${reason}`;
export const failedToSaveScript = (reason: string) => `Failed to save script: ${reason}`;
export const failedToApply = (reason: string) => `Failed to apply script: ${reason}`;

// ---- the persisted external path ----------------------------------------------------------------------------------

/** The external script the operator browsed to, kept across reloads; "" when none. Storage may be blocked. */
export function readExternalPath(): string {
  try { return window.localStorage?.getItem(OVERVIEW_SCRIPT_KEY)?.trim() ?? ""; } catch { return ""; }
}
/** Remember the external path ("" forgets it). The choice lasts this session at least when storage is blocked. */
export function writeExternalPath(path: string): void {
  try {
    if (path) window.localStorage?.setItem(OVERVIEW_SCRIPT_KEY, path);
    else window.localStorage?.removeItem(OVERVIEW_SCRIPT_KEY);
  } catch { /* storage blocked: the choice lasts this session only */ }
}

// ---- paths and visibility -----------------------------------------------------------------------------------------

/** The default Overview script (Qt `<include folder>/overviewConfig.js`, the folder that holds the active config.json):
 *  next to the active profile's config.json; with no profile applied, in the application's config folder
 *  (`app_paths().app_config`), the place the shell keeps its own settings. "" while neither is known. */
export function overviewDefaultPath(s: { profileConfigPath: string; appConfigDir: string }): string {
  return profileFile(s.profileConfigPath, OVERVIEW_SCRIPT_FILE) || (s.appConfigDir ? joinPath(s.appConfigDir, OVERVIEW_SCRIPT_FILE) : "");
}

/** Qt `currentJsPath`: the external file when one is set, else the default. */
export const overviewScriptPath = (external: string, defaultPath: string): string => external.trim() || defaultPath;

/** The whole script area belongs to an eGrabber setup: Qt hides it for MindVision (its JSON profile is used instead, OverviewTab::
 *  refreshCameraMode), and the platform has none without EGrabber scripts (the PZ7035). Shown for every other selection, so the file
 *  can be prepared before a camera is connected. */
export function overviewScriptVisible(s: { eGrabberScripts: boolean; selection: Pick<CameraSelection, "mode"> | null }): boolean {
  return s.eGrabberScripts && s.selection?.mode !== CAMERA_SELECTION_MODES.MindVision;
}

/** Why Apply to Camera cannot start now; "" when it can. The camera itself is checked at the click, against a fresh selection. */
export function overviewApplyBlock(s: { ready: boolean; expActive: boolean; recording: boolean }): string {
  if (!s.ready) return "Backend is not initialized.";
  if (s.expActive) return "Stop and finalize the experiment before changing camera settings.";
  if (s.recording) return "Stop the raw recording before applying a camera script.";
  return "";
}

/** The camera is an eGrabber hardware camera, the only one an Overview script can be applied to. */
export const isHardwareCamera = (selection: Pick<CameraSelection, "valid" | "configured" | "mode">): boolean =>
  selection.valid && selection.configured && selection.mode === CAMERA_SELECTION_MODES.Hardware;

// ---- line endings ---------------------------------------------------------------------------------------------------

const toLf = (text: string) => text.replace(/\r\n/g, "\n");

/** A textarea reports its text with LF line breaks. A script that was written with CRLF (a Windows profile, the bench's
 *  C:\MIBTest\include) keeps them when it is saved after an edit; a script with LF or mixed endings is saved as typed. */
export function withLineEndings(edited: string, original: string): string {
  if (!original.includes("\r\n") || /(^|[^\r])\n/.test(original) || edited.includes("\r")) return edited;
  return edited.replace(/\n/g, "\r\n");
}

/** The editor text differs from the file (ignoring the line-ending style the textarea normalizes). */
export const differsFromFile = (edited: string, fileText: string): boolean => toLf(edited) !== toLf(fileText);

// ---- the camera_document calls ----------------------------------------------------------------------------------------

export interface ScriptDoc { path: string; text: string; revision: string }

const camDoc = (action: string, path: string, baseline = "", text = "") =>
  invoke<ScriptDoc>("camera_document", { action, path, kind: "overview", baseline, text });

export const readOverviewScript = (path: string) => camDoc("read", path);
export const saveOverviewScript = (doc: ScriptDoc, text: string) => camDoc("save", doc.path, doc.revision, withLineEndings(text, doc.text));

/** The default file, made from the bundled resources/defaults/overviewConfig.js (Qt FileIOUtils::ensureDefaultsFile). The
 *  backend makes the folder too and never replaces an existing file. */
export async function createOverviewDefault(path: string): Promise<ScriptDoc> {
  const bundled = await camDoc("default", "");
  return camDoc("create", path, "", bundled.text);
}

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

/** Load a script for the editor. For the default path a missing file is made from the bundled default (Qt `ensureDefaultsFile`
 *  before the load); the reason shown is the read error when the file exists but cannot be read. `created` says it was made. */
export async function loadOverviewScript(path: string, isDefault: boolean): Promise<{ doc: ScriptDoc; created: boolean }> {
  try {
    return { doc: await readOverviewScript(path), created: false };
  } catch (readError) {
    if (!isDefault) throw new Error(errorText(readError));
    try {
      return { doc: await createOverviewDefault(path), created: true };
    } catch {
      throw new Error(errorText(readError));
    }
  }
}

/** The stage change needs the default file even when the editor has not loaded it yet (Qt builds the Overview tab, which runs
 *  ensureDefaultsFile, before any tab change): true when the file is there afterwards. */
export async function ensureOverviewDefault(path: string): Promise<boolean> {
  try { await createOverviewDefault(path); return true; } catch { return false; }
}
