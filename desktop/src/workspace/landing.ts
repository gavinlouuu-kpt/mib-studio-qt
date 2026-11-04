// Which view the app opens in (the workspace branch, #716): the four-stage workspace (Prepare)
// by default, or the classic view when the operator last chose it. The choice is remembered per
// browser (localStorage; the Tauri webview's storage on the desktop, the browser's per origin on
// the PZ7035), and only an explicit switch records it: the menu row's "Workspace preview" and the
// workspace's "Classic view" (its onExit). Jumps that leave the workspace for a task in the
// classic view (Prepare's setup buttons, the Summary's Open in Review) do not change it.
//
// Opening in the workspace acts on nothing: the classic tab stays on Connect (it only reads) and
// the classic startup landing is skipped while the workspace is open (App), so no camera mode is
// switched and no LED lit behind the operator. Every storage access is guarded: storage can throw
// or be absent (blocked site data, private windows, tests), and the default then applies.
//
// Pure module: no React imports.

export type LandingView = "workspace" | "classic";

export const LANDING_STORAGE_KEY = "yofo.workspace.landing";
/** On this branch the app opens in the workspace until the operator chooses the classic view. */
export const DEFAULT_LANDING: LandingView = "workspace";

type Reader = Pick<Storage, "getItem">;
type Writer = Pick<Storage, "setItem">;

function localStore(): (Reader & Writer) | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/** The remembered view, or the default when none (or an unrecognized value) is stored. */
export function readLanding(storage: Reader | null = localStore()): LandingView {
  try {
    const v = storage?.getItem(LANDING_STORAGE_KEY);
    return v === "workspace" || v === "classic" ? v : DEFAULT_LANDING;
  } catch {
    return DEFAULT_LANDING;
  }
}

/** Remember the operator's explicit choice; a storage failure only means it is not remembered. */
export function writeLanding(view: LandingView, storage: Writer | null = localStore()): void {
  try {
    storage?.setItem(LANDING_STORAGE_KEY, view);
  } catch {
    /* storage unavailable: the default applies at the next start */
  }
}

/** The workspace stage the app opens on: Prepare (0), or null for the classic view. */
export const initialWorkspaceStage = (view: LandingView): number | null => (view === "workspace" ? 0 : null);
