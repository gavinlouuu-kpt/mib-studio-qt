import * as dialog from "@tauri-apps/plugin-dialog";
import * as opener from "@tauri-apps/plugin-opener";
import {isRemote} from "./index";

// Native dialogs and the opener in the desktop shell; browser stand-ins against the
// instrument, where every path is a path on the instrument's filesystem (the server opens it).

// The instrument's data directory, from fetch_instrument_status (storage.path). A relative default such as
// "experiment.h5" would land in the server's working directory; against the instrument it starts under the
// data directory instead (#651 G8).
let remoteDefaultDir = "";
export const setRemoteDefaultDir = (dir: string): void => { remoteDefaultDir = dir; };

/** `defaultPath` made absolute under `dir` when it is relative and `dir` is known; unchanged otherwise. */
export function resolveRemoteDefault(dir: string, defaultPath: string | undefined): string {
  const p = defaultPath ?? "";
  if (!dir || !p || p.startsWith("/") || /^[A-Za-z]:[\\/]/.test(p)) return p;
  return `${dir.replace(/[\\/]+$/, "")}/${p}`;
}

function promptPath(title: string | undefined, defaultPath: string | undefined): string | null {
  const value = window.prompt(`${title ?? "Path on the instrument"}`, resolveRemoteDefault(remoteDefaultDir, defaultPath));
  return value && value.trim() ? value.trim() : null;
}

export const open = (async (options?: dialog.OpenDialogOptions) => {
  if (!isRemote) return dialog.open(options);
  const path = promptPath(options?.title ?? (options?.directory ? "Folder on the instrument" : "File on the instrument"),
    options?.defaultPath);
  if (path === null) return null;
  return options?.multiple ? [path] : path;
}) as typeof dialog.open;

export const save = (async (options?: dialog.SaveDialogOptions) => {
  if (!isRemote) return dialog.save(options);
  return promptPath(options?.title ?? "Save to (path on the instrument)", options?.defaultPath);
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

export const openUrl = (async (url: string | URL, openWith?: string) => {
  if (!isRemote) return opener.openUrl(url, openWith);
  window.open(String(url), "_blank", "noopener");
}) as typeof opener.openUrl;

export const revealItemInDir = (async (path: string | string[]) => {
  if (!isRemote) return opener.revealItemInDir(path);
  throw new Error(`Open the instrument's folder directly: ${Array.isArray(path) ? path.join(", ") : path}`);
}) as typeof opener.revealItemInDir;
