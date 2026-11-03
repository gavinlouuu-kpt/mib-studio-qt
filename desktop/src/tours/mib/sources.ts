// MIB Studio tours: where the tour text comes from. An entry's `source` lists repository paths (relative to the repository root,
// checked to exist by entries.test.ts) and paths in another root, written `<root>:<path>` (registry.ts):
//   goals@845ca15c   a document that lives on another branch, at commit 845ca15c. Only the two documents below.
//   before@<sha>     a repository path as it was at that commit, for the "before" flowcharts. The roots below are the commits
//                    just before each fold-in change (see BEFORE_COMMITS); a test checks the path exists there when git can.
export const GOALS_ROOT = "goals@845ca15c";
export const GOALS_DOC = `${GOALS_ROOT}:docs/architecture/fold-in-backend-goals.md`;
export const PLAN_DOC = `${GOALS_ROOT}:docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md`;
/** The documents under goals@845ca15c a source may name. */
export const GOALS_ALLOWLIST: readonly string[] = [GOALS_DOC, PLAN_DOC];

/** What each `before@<sha>` root is: the commit before the change the tour's "before" chart describes. */
export const BEFORE_COMMITS = {
  /** The workspace branch with develop merged in (fold-in slice U-1), before the E stack (#806) and the F and I merges. */
  e47efe93: "baseline before the E stack: the workspace branch after the U-1 merge",
  /** Parent of 70dc3094 (native recipe provider, I02). */
  d3b8fded: "before the native recipe provider (I02)",
  /** Parent of 4aeb876e (the first SQLite workspace revision store of the I stack, I12). */
  "94e50990": "before the I stack's workspace store",
  /** Parent of 90bb1abf (F-1 full-recording population analysis, F04). */
  "66906d3e": "before F-1 (full-recording native population analysis)",
} as const;
export type BeforeRoot = keyof typeof BEFORE_COMMITS;
export const before = (sha: BeforeRoot, path: string) => `before@${sha}:${path}`;

/** `<root>@<…>:<path>` → its parts; null for a plain repository path. */
export function splitSource(source: string): { root: string; path: string } | null {
  const m = /^([a-z]+@[0-9a-f]+):(.+)$/.exec(source);
  return m ? { root: m[1], path: m[2] } : null;
}

export const EVIDENCE_DOC = "docs/evidence/fold-in-e2e-2026-11-02.md";
export const E2E_TEST = "crates/mib-bridge-server/tests/fold_in_e2e.rs";
export const DESKTOP_SHELL_DOC = "knowledge_map/architecture/Desktop-Shell.md";

/** The steps of the headless proof, by the exact names in the evidence document's step table. */
export const E2E_STEPS = {
  e01: "E01 discovery + instance",
  e02: "E02 feature tree / read / write / refusals",
  e04e06: "E04/E06 method + runs A and B + accounting",
  i02: "I02 recipe create / derive / diff / apply preview",
  f04: "F04/F05/F08 population graph, statistics, histogram, gallery",
  i12Save: "I12/F12 save workspace",
  i12Restart: "I12/F12 + E01 restart: reopen and stale refusal",
} as const;

/** Evidence class wording, so every demonstrated item names its class the same way. */
export const CLASS = {
  mock: "mock camera",
  real: "real backend",
  fixture: "synthetic data (production-writer fixture)",
} as const;
export const DEMONSTRATED_LABEL = "Demonstrated (browser drive: mock camera and fixtures, never hardware)";
export const INTRODUCED = { date: "2026-11-03", by: "fold-in tours (claude/fold-in-tours)" } as const;
/** The day the browser drive runs against the real yofo-studio-server. */
export const DRIVE_DATE = "2026-11-03";
