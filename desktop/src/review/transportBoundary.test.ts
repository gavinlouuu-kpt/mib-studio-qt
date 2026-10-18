import { expect, it } from "vitest";

// #512 S9: the review module reaches the backend, the dialogs and the opener
// only through desktop/src/transport (invoke / upload, transport/dialogs), so
// it runs unchanged over Tauri and the WebSocket. Tests may still mock the
// Tauri packages; the module's sources may not import them.
const FORBIDDEN = /["']@tauri-apps\/(?:api\/core|plugin-dialog|plugin-opener)(?:\/[^"']*)?["']/;

it("imports no Tauri invoke, dialog or opener package under desktop/src/review", () => {
  const sources = import.meta.glob("./**/*.{ts,tsx}", { query: "?raw", import: "default", eager: true }) as Record<string, string>;
  expect(Object.keys(sources)).toContain("./reviewBridge.ts");
  const direct = Object.entries(sources)
    .filter(([path, source]) => !path.includes(".test.") && FORBIDDEN.test(source))
    .map(([path]) => path);
  expect(direct).toEqual([]);
});

it("recognises every import form of the guarded packages", () => {
  for (const line of [
    'import { invoke } from "@tauri-apps/api/core";',
    "import { open, save } from '@tauri-apps/plugin-dialog';",
    'import { revealItemInDir } from "@tauri-apps/plugin-opener";',
    'export { ask } from "@tauri-apps/plugin-dialog";',
    'const core = await import("@tauri-apps/api/core");',
  ])
    expect(FORBIDDEN.test(line), line).toBe(true);
  for (const line of ['import { listen } from "@tauri-apps/api/event";', 'import { invoke } from "../transport";'])
    expect(FORBIDDEN.test(line), line).toBe(false);
});
