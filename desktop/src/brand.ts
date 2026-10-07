// The product name shown to the operator (#550 m13). The PZ7035 version is YOFO Studio (rebrand
// decided 2026-09-30, user-visible identity only: ABI names, MIB_* macros and repo names stay).
// The browser UI is served only by the instrument's yofo-studio-server, so it is YOFO Studio from
// the first paint, before the capability report arrives.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

export const YOFO_STUDIO = "YOFO Studio";
export const MIB_STUDIO = "MIB Studio";

export function productName(opts: { pz7035: boolean; remote: boolean }): string {
  return opts.pz7035 || opts.remote ? YOFO_STUDIO : MIB_STUDIO;
}
