import * as dialog from "@tauri-apps/plugin-dialog";
import * as opener from "@tauri-apps/plugin-opener";
import {isRemote} from "./index";

// Native dialogs and the opener in the desktop shell; browser stand-ins against the
// instrument, where every path is a path on the instrument's filesystem (the server opens it).

function promptPath(title: string | undefined, defaultPath: string | undefined): string | null {
  const value = window.prompt(`${title ?? "Path on the instrument"}`, defaultPath ?? "");
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

export const confirm = (async (message: string, options?: string | dialog.ConfirmDialogOptions) => {
  if (!isRemote) return dialog.confirm(message, options);
  return window.confirm(message);
}) as typeof dialog.confirm;

export const openUrl = (async (url: string | URL, openWith?: string) => {
  if (!isRemote) return opener.openUrl(url, openWith);
  window.open(String(url), "_blank", "noopener");
}) as typeof opener.openUrl;

export const revealItemInDir = (async (path: string | string[]) => {
  if (!isRemote) return opener.revealItemInDir(path);
  throw new Error(`Open the instrument's folder directly: ${Array.isArray(path) ? path.join(", ") : path}`);
}) as typeof opener.revealItemInDir;
