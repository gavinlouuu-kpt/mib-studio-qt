import * as dialog from "@tauri-apps/plugin-dialog";
import * as opener from "@tauri-apps/plugin-opener";
import {invoke, isRemote} from "./index";

// Native dialogs and the opener in the desktop shell; browser stand-ins against the
// instrument, where every path is a path on the instrument's filesystem (the server opens it).

// The instrument's data directory, from fetch_instrument_status (storage.path): the storage root the backend
// resolves a relative output path under (#762). A relative default such as "experiment.h5" used to land in the
// server's working directory; the browser prompts start under the data directory instead (#651 G8).
let remoteDefaultDir = "";
export const setRemoteDefaultDir = (dir: string): void => { remoteDefaultDir = dir; };

/** `defaultPath` made absolute under `dir` when it is relative and `dir` is known; unchanged otherwise. */
export function resolveRemoteDefault(dir: string, defaultPath: string | undefined): string {
  const p = defaultPath ?? "";
  if (!dir || !p || p.startsWith("/") || /^[A-Za-z]:[\\/]/.test(p)) return p;
  return `${dir.replace(/[\\/]+$/, "")}/${p}`;
}

/** The text of a browser save prompt: the title, then where the file goes (#762). */
export function remoteSavePromptText(title: string, dir: string): string {
  return dir
    ? `${title}\nThe file is saved on the instrument, in ${dir}. A relative path is saved there too.`
    : `${title}\nThe file is saved on the instrument. A relative path is saved in its data directory.`;
}

// storage.path at the moment a prompt opens (#762): fetch_instrument_status answers on every server, the
// PZ7035 or not, so the default does not depend on the PZ7035 status poll having run. Falls back to the last
// known directory when the fetch fails.
async function remoteStorageDir(): Promise<string> {
  try {
    const path = (await invoke<{storage?: {path?: string}}>("fetch_instrument_status"))?.storage?.path;
    if (typeof path === "string" && path) remoteDefaultDir = path;
  } catch { /* keep the last known directory */ }
  return remoteDefaultDir;
}

function promptPath(message: string, defaultValue: string): string | null {
  const value = window.prompt(message, defaultValue);
  return value && value.trim() ? value.trim() : null;
}

export const open = (async (options?: dialog.OpenDialogOptions) => {
  if (!isRemote) return dialog.open(options);
  const path = promptPath(options?.title ?? (options?.directory ? "Folder on the instrument" : "File on the instrument"),
    resolveRemoteDefault(remoteDefaultDir, options?.defaultPath));
  if (path === null) return null;
  return options?.multiple ? [path] : path;
}) as typeof dialog.open;

// The default is <storage.path>/<suggested name>, and the prompt says where the file goes (#762).
export const save = (async (options?: dialog.SaveDialogOptions) => {
  if (!isRemote) return dialog.save(options);
  const dir = await remoteStorageDir();
  return promptPath(remoteSavePromptText(options?.title ?? "Save to (path on the instrument)", dir),
    resolveRemoteDefault(dir, options?.defaultPath));
}) as typeof dialog.save;

// The plugin replaces window.confirm with an async shim in Tauri. Use its
// public API (the permitted message command), and fail closed on dialog errors.
export const confirm = async (message: string, options?: string | dialog.ConfirmDialogOptions): Promise<boolean> => {
  try {
    const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
    return (await (inTauri ? dialog.confirm(message, options) : window.confirm(message))) === true;
  } catch {
    return false;
  }
};

// A Yes / No question (the plugin's ask, as the review module's core-contour prompts use).
// Errors propagate, as with the plugin; a browser answers with window.confirm.
export const ask = (async (message: string, options?: string | dialog.ConfirmDialogOptions) => {
  if (!isRemote) return dialog.ask(message, options);
  return window.confirm(message);
}) as typeof dialog.ask;

export const openUrl = (async (url: string | URL, openWith?: string) => {
  if (!isRemote) return opener.openUrl(url, openWith);
  window.open(String(url), "_blank", "noopener");
}) as typeof opener.openUrl;

export const revealItemInDir = (async (path: string | string[]) => {
  if (!isRemote) return opener.revealItemInDir(path);
  throw new Error(`Open the instrument's folder directly: ${Array.isArray(path) ? path.join(", ") : path}`);
}) as typeof opener.revealItemInDir;
