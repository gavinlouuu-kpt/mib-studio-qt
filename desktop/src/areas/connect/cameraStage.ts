// Connect and Overview (epic #863, bug #846): what entering the Overview or Experiment stage does to an
// eGrabber camera, as the Qt app's MainWindow::onTabChanged does it, and where the scripts come from.
// Pure decisions and one orchestration over a small IO interface, so the sequence is unit-tested
// without a camera: stop a running camera, apply the stage's script with its role (Overview = the full
// sensor as is; Experiment = the script followed by the profile's camera.window), restart it, and on
// Overview start a stopped, configured camera. Never during an experiment.
import type { CameraGeometry, CameraSelection } from "../../bridge";
import { CAMERA_SELECTION_MODES } from "../../bridgeContract";

export type StageName = "overview" | "experiment";

/** What the operator is told about a stage change; shown in the stage it concerns. */
export interface StageNotice { stage: StageName; level: "info" | "warn" | "error"; text: string }

export const NO_EXPERIMENT_SCRIPT = "No Experiment camera script: the camera keeps its current window";
export const NO_OVERVIEW_SCRIPT = "No Overview camera script: the camera keeps its current mode. Choose one in Camera & Alignment (overviewConfig.js).";
/** The Overview editor's file is gone (Qt: "Script file does not exist", nothing applied). */
export const overviewScriptMissing = (path: string) => `Overview camera script not found: ${path}. The camera keeps its current mode.`;
/** Added to the no-script notice on a remote (browser) client: the server reads no script file by path, so the
 *  profile folder's egrabberConfig.js / overviewConfig.js is not looked for there (desktop-only, #892 review). */
export const REMOTE_NO_PROFILE_SCRIPT = "A remote client does not read script files on the instrument, so the profile folder's script is not used.";
export function noScriptText(overview: boolean, remote: boolean): string {
  const text = overview ? NO_OVERVIEW_SCRIPT : NO_EXPERIMENT_SCRIPT;
  return remote ? `${text}${text.endsWith(".") ? "" : "."} ${REMOTE_NO_PROFILE_SCRIPT}` : text;
}
/** Qt's wording when the Overview's auto-start fails (MainWindow::onTabChanged). */
export const START_FAILED = "Failed to start camera. Please check camera connection and try again.";
export const OVERVIEW_DISABLED_TITLE = "Overview tab is disabled while an experiment is active. Please stop the experiment first.";

// ---- script paths -------------------------------------------------------------------------------

/** Where the scripts may come from. Filled from the config area's state each render
 *  (studio/useStudioController.ts) so the stage change reads the operator's current choice. */
export interface ScriptSources {
  /** Config › Camera script: the file the operator chose to apply. */
  cameraScriptPath: string;
  /** Config › the EGrabber script editor's file. */
  cameraDocumentPath: string;
  /** The applied profile's config.json (its folder holds egrabberConfig.js and overviewConfig.js); "" when none. */
  profileConfigPath: string;
  /** The Overview script editor's resolved path (Camera & Alignment; Qt OverviewTab::currentJsPath): the external file the
   *  operator browsed to, else the default; "" until the editor knows it. */
  overviewScriptPath: string;
  /** The editor's default path (<profile folder>/overviewConfig.js, else the app config folder): the one file that is made
   *  from the bundled default when it is missing (Qt ensureDefaultsFile). */
  overviewDefaultPath: string;
}

export const NO_SOURCES: ScriptSources = { cameraScriptPath: "", cameraDocumentPath: "", profileConfigPath: "", overviewScriptPath: "", overviewDefaultPath: "" };

const separatorOf = (path: string) => (path.includes("\\") && !path.includes("/") ? "\\" : "/");

/** The folder of a file path, written with the path's own separator; "" when it has none. */
export function folderOf(path: string): string {
  const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return cut > 0 ? path.slice(0, cut) : "";
}

export function joinPath(folder: string, name: string): string {
  if (!folder) return name;
  return /[\\/]$/.test(folder) ? folder + name : folder + separatorOf(folder) + name;
}

/** A profile's files sit next to its config.json. */
export function profileFile(profileConfigPath: string, name: string): string {
  const folder = folderOf(profileConfigPath);
  return folder ? joinPath(folder, name) : "";
}

export type ScriptSource = "selection" | "document" | "profile" | "overview-editor";
export interface ResolvedScript { path: string; source: ScriptSource }

/** The Experiment script, in the order the task fixes: the Config › Camera script selection if set,
 *  else the camera document editor's file if set, else the active profile's egrabberConfig.js if it exists. */
export async function resolveExperimentScript(
  sources: ScriptSources, exists: (path: string) => Promise<boolean>,
): Promise<ResolvedScript | null> {
  if (sources.cameraScriptPath) return { path: sources.cameraScriptPath, source: "selection" };
  if (sources.cameraDocumentPath) return { path: sources.cameraDocumentPath, source: "document" };
  const profile = profileFile(sources.profileConfigPath, "egrabberConfig.js");
  if (profile && await exists(profile)) return { path: profile, source: "profile" };
  return null;
}

/** The Overview script: the Overview editor's resolved path (Qt `currentJsPath`) when the file is there, or when it is the
 *  default path and can be made from the bundled default (Qt `ensureDefaultsFile`, which the Overview tab runs when it is
 *  built); an external file that is gone is not replaced. Without an editor path, the active profile's overviewConfig.js
 *  if it exists. */
export async function resolveOverviewScript(
  sources: ScriptSources, exists: (path: string) => Promise<boolean>, createDefault?: (path: string) => Promise<boolean>,
): Promise<ResolvedScript | null> {
  const editor = sources.overviewScriptPath;
  if (editor) {
    if (await exists(editor)) return { path: editor, source: "overview-editor" };
    if (createDefault && editor === sources.overviewDefaultPath && await createDefault(editor)) return { path: editor, source: "overview-editor" };
    return null;
  }
  const profile = profileFile(sources.profileConfigPath, "overviewConfig.js");
  if (profile && await exists(profile)) return { path: profile, source: "profile" };
  return null;
}

// ---- decisions ----------------------------------------------------------------------------------

/** Qt MainWindow::onTabChanged: entering Overview starts a stopped, configured camera. Not a MindVision
 *  camera: Qt's MindVision branch switches the preset and returns before the auto-start (a MindVision
 *  stage change restarts only a camera that was running), so navigation never energizes an illuminated
 *  rig. Not the PZ7035 either: it starts through its own Align switch. */
export function overviewAutoStart(geometry: CameraGeometry | null, selection: Pick<CameraSelection, "configured" | "running">, instrumentModes: boolean): boolean {
  if (instrumentModes || !selection.configured || selection.running) return false;
  if (geometry?.camera === "mindvision") return false;
  return true;
}

/** An eGrabber camera: a selected hardware camera whose geometry says "egrabber". */
export const isEgrabber = (geometry: CameraGeometry | null | undefined): boolean => !!geometry?.supported && geometry.camera === "egrabber";

/** The line under the stage change for an Experiment apply: what window the camera ended up with. */
export function experimentWindowNotice(geometry: CameraGeometry | null): StageNotice | null {
  const apply = geometry?.window_apply;
  if (!apply) return null;
  const failed = !!apply.refusal || !!apply.error;
  return { stage: "experiment", level: failed ? "error" : "info", text: `Camera window: ${apply.text}${failed ? ` (${apply.refusal || apply.error})` : ""}` };
}

// ---- the sequence -------------------------------------------------------------------------------

export interface CmdLike { ok: boolean; message: string }

export interface StageIo {
  fetchSelection(): Promise<CameraSelection>;
  stopCapture(): Promise<CmdLike>;
  startCapture(): Promise<CmdLike>;
  applyScript(path: string, overview: boolean): Promise<CmdLike>;
  refreshGeometry(): Promise<CameraGeometry | null>;
  scriptExists(path: string): Promise<boolean>;
  /** Make the default Overview script from the bundled default; true when the file is there afterwards. */
  createOverviewDefault?(path: string): Promise<boolean>;
  /** A remote (browser) client: the profile folder is not probed (see REMOTE_NO_PROFILE_SCRIPT). */
  remote?: boolean;
  /** The camera is running (true) or stopped (false) now. */
  setRunning(running: boolean): void;
  log(line: string): void;
}

export interface StageInput {
  stage: StageName;
  expActive: boolean;
  /** Raw recording is on: the camera cannot be stopped underneath it. */
  recording: boolean;
  sources: ScriptSources;
  /** The geometry that said "egrabber" (read for the auto-start rule only). */
  geometry: CameraGeometry | null;
}

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

/** Enter a stage with an eGrabber camera. Resolves with what the operator should read (or null);
 *  never throws. The camera is stopped only when a script is about to be applied, and restarted only
 *  when it was running (or, on Overview, when it was stopped and configured: the auto-start). */
export async function runEgrabberStage(input: StageInput, io: StageIo): Promise<StageNotice | null> {
  const { stage } = input;
  const overview = stage === "overview";
  const label = overview ? "Overview" : "Experiment";
  try {
    if (input.expActive) return null; // never during an experiment
    if (input.recording) {
      return { stage, level: "warn", text: `Stop the raw recording before the camera can change to the ${label} script.` };
    }
    const selection = await io.fetchSelection();
    if (!selection.valid || !selection.configured || selection.mode !== CAMERA_SELECTION_MODES.Hardware) return null;
    const script = overview ? await resolveOverviewScript(input.sources, io.scriptExists, io.createOverviewDefault) : await resolveExperimentScript(input.sources, io.scriptExists);
    const wasRunning = selection.running;
    const autoStart = overviewAutoStart(input.geometry, selection, false);

    if (!script) {
      // Qt logs it and leaves the camera as it is; Overview still starts a stopped camera.
      // A remote client cannot tell a missing editor file from one it cannot read: it says why instead.
      const remote = io.remote === true;
      const text = overview && input.sources.overviewScriptPath && !remote
        ? overviewScriptMissing(input.sources.overviewScriptPath) : noScriptText(overview, remote);
      const notice: StageNotice = { stage, level: "warn", text };
      io.log(notice.text);
      if (overview && autoStart) return (await startCamera(io, stage, false)) ?? notice;
      return notice;
    }

    if (wasRunning) {
      const stopped = await io.stopCapture();
      if (!stopped.ok) {
        const text = `Camera not stopped, ${label} script not applied: ${stopped.message}`;
        io.log(text);
        return { stage, level: "error", text };
      }
      io.setRunning(false);
    }
    io.log(`applying the ${label} camera script ${script.path}`);
    const applied = await io.applyScript(script.path, overview);
    if (!applied.ok) {
      const text = `${label} camera script not applied: ${applied.message}`;
      io.log(text);
      await io.refreshGeometry();
      return { stage, level: "error", text }; // Qt leaves the capture stopped here as well
    }
    io.log(`${label} camera script applied`);
    const geometry = await io.refreshGeometry();
    let notice: StageNotice | null = overview ? null : experimentWindowNotice(geometry);
    if (notice) io.log(notice.text);

    if (wasRunning || (overview && autoStart)) {
      const failure = await startCamera(io, stage, wasRunning);
      if (failure) notice = failure;
    }
    return notice;
  } catch (e) {
    const text = `Camera ${label} change failed: ${errorText(e)}`;
    io.log(text);
    return { stage, level: "error", text };
  }
}

/** Start the camera; null on success, the notice to show on failure. */
async function startCamera(io: StageIo, stage: StageName, restart: boolean): Promise<StageNotice | null> {
  const started = await io.startCapture();
  if (started.ok) {
    io.setRunning(true);
    io.log(restart ? "camera restarted after the script change" : "camera started on entering Overview");
    return null;
  }
  const text = restart ? `Camera script applied, but restart failed: ${started.message}` : START_FAILED;
  io.log(`${text} (${started.message})`);
  return { stage, level: "error", text };
}

/** Overview for a camera without scripts (mock, MindVision, Aravis): start a stopped, configured
 *  camera, once per entry. Null when started, not eligible, or nothing to say. */
export async function runOverviewAutoStart(
  input: Pick<StageInput, "expActive" | "geometry"> & { instrumentModes: boolean },
  io: Pick<StageIo, "fetchSelection" | "startCapture" | "setRunning" | "log">,
): Promise<StageNotice | null> {
  try {
    if (input.expActive) return null;
    const selection = await io.fetchSelection();
    if (!overviewAutoStart(input.geometry, selection, input.instrumentModes)) return null;
    return await startCamera(io as StageIo, "overview", false);
  } catch (e) {
    const text = `${START_FAILED} (${errorText(e)})`;
    io.log(text);
    return { stage: "overview", level: "error", text: START_FAILED };
  }
}
