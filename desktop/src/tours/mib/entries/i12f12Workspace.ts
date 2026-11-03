// Tour: I12 and F12, saving a workspace plan, reopening it and what a restart does (workspace Experiment ▸ Plan).
// Backend tour. A tour cannot restart the server, so the stale refusal after a restart is shown only in the chart and evidence.
import type { TourEntry } from "../../registry";
import { workspaceView } from "../views";
import { FACTS } from "../facts";
import { before, CLASS, DEMONSTRATED_LABEL, DRIVE_DATE, E2E_STEPS, E2E_TEST, EVIDENCE_DOC, GOALS_DOC, INTRODUCED, PLAN_DOC } from "../sources";

const S = {
  goals: GOALS_DOC,
  plan: PLAN_DOC,
  evidence: EVIDENCE_DOC,
  e2e: E2E_TEST,
  doc: "knowledge_map/services/WorkspaceStore.md",
  store: "src/backend/workspace/WorkspaceStore.cpp",
  design: "docs/design/ique-workflow/README.md",
  saved: "desktop/src/workspace/SavedPlan.tsx",
  nativeTest: "tests/workspace/workspace_store_test.cpp",
  tsTest: "desktop/src/workspace/SavedPlan.test.tsx",
  shell: "knowledge_map/architecture/Desktop-Shell.md",
  bPlan: before("94e50990", "desktop/src/workspace/usePlan.ts"),
  bSqlite: before("94e50990", "src/backend/database/SqliteService.cpp"),
  bContract: before("94e50990", "crates/mib-bridge/contract/bridge-contract.json"),
} as const;

export const i12f12Workspace: TourEntry = {
  id: "fold-in-i12-f12-workspace",
  title: "Saved workspace: save, reopen, restart (I12, F12)",
  summary: "A workspace plan is saved as a numbered revision by the backend. A save based on an old revision is refused. A restart keeps the history.",
  kind: "backend",
  introduced: INTRODUCED,
  source: Object.values(S),
  when: () => true,
  flows: {
    before: {
      title: "A store nobody could reach",
      cols: 2,
      nodes: [
        { id: "plan", label: "Plan in memory", sub: "usePlan, local only", col: 0, row: 0, tone: "b" },
        { id: "contract", label: "Bridge contract", sub: "no workspace command", col: 1, row: 0, tone: "warn" },
        { id: "store", label: "Store in backend", sub: "defined, not wired", col: 1, row: 1, tone: "a" },
        { id: "copies", label: "Two copies", sub: "I side and F side", col: 1, row: 2, tone: "warn" },
        { id: "abi", label: "ABI 47 twice", sub: "different contents", col: 0, row: 2, tone: "warn" },
      ],
      edges: [
        { from: "plan", to: "contract", label: "no way to save" },
        { from: "contract", to: "store", dashed: true },
        { from: "store", to: "copies", label: "diverged" },
        { from: "copies", to: "abi" },
      ],
      legend: "At 94e50990, before the I stack merged. Orange: what the sources show was missing or in conflict.",
    },
    after: {
      title: "One store, revisions by compare-and-swap",
      cols: 2,
      nodes: [
        { id: "ui", label: "Saved plan", sub: "Experiment, Plan", col: 0, row: 0, tone: "b" },
        { id: "write", label: "workspace_write", sub: "names its base rev.", col: 1, row: 0, tone: "a" },
        { id: "conflict", label: "Stale revision", sub: "conflict, no overwrite", col: 0, row: 1, tone: "warn" },
        { id: "store", label: "WorkspaceStore", sub: "SQLite, every revision", col: 1, row: 1, tone: "c" },
        { id: "history", label: "History, restore", sub: "restore appends", col: 0, row: 2, tone: "a" },
        { id: "restart", label: "Server restarts", sub: "head and history kept", col: 1, row: 2, tone: "d" },
        { id: "reopen", label: "Reopen", sub: "workspace_read", col: 0, row: 3, tone: "a" },
        { id: "stale", label: "Old tokens", sub: "stale after restart", col: 1, row: 3, tone: "warn" },
      ],
      edges: [
        { from: "ui", to: "write", label: "Save" },
        { from: "write", to: "store", label: "commit" },
        { from: "store", to: "conflict", label: "base is old" },
        { from: "store", to: "history" },
        { from: "store", to: "restart", label: "process ends", dashed: true },
        { from: "restart", to: "reopen" },
        { from: "restart", to: "stale", label: "camera token, job" },
      ],
      legend: "After the I stack (ABI 48). The old-token refusal after a restart is the camera feature token and discovery job (E01), shown here only: a tour cannot restart the server.",
    },
  },
  evidence: {
    demonstratedLabel: DEMONSTRATED_LABEL,
    demonstrated: [
      { text: `Save new workspace answers with a revision number, and the card says it is a recorded plan, not proof of acquisition (${CLASS.real}).`, date: DRIVE_DATE, source: S.saved },
      { text: `Read revision history lists the revisions saved so far (${CLASS.real}).`, date: DRIVE_DATE, source: S.saved },
    ],
    tested: [
      { text: "A workspace and a plan are saved as revisions 1 and 2, with two recipes (real backend, SQLite store).", source: S.evidence, test: `fold_in_e2e: ${E2E_STEPS.i12Save}` },
      { text: "After the server process restarts on the same data directory, the workspace head and history, the plan revisions, the recipe and its child and the pinned evaluation all survive; the old feature token and the old discovery job are refused as stale (real backend, new process, mock camera).", source: S.evidence, test: `fold_in_e2e: ${E2E_STEPS.i12Restart}` },
      { text: "A save based on a stale revision is a conflict; a duplicate create is a conflict; restore appends; a restart keeps the head and the command replay; two connections racing give one success and one conflict.", source: S.nativeTest, test: "analysis.workspace_store_test" },
      { text: "A conflict reply clears the opened snapshot, and a viewer cannot write (mocked invoke).", source: S.tsTest, test: "clears stale saved annotations on conflict and viewer cannot write" },
    ],
    uncertain: [
      { text: "The headless run never sends a stale expected revision. The conflict is proven by the native store test and the view's mocked-invoke test, not through the server.", source: S.evidence },
      { text: "F12 asks that a reopen after an incompatible change recomputes instead of reusing results. The headless run reopens after a compatible restart and gets the same counts; an incompatible change is not exercised.", source: S.goals },
      { text: "Rendered browser and Tauri qualification of the saved plan remains open, and the store's thread-sanitizer run failed to start.", source: S.design },
      { text: "After a restart the Review membership cache, the recording open in Review, the camera session token and discovery jobs are gone; they are not part of the saved workspace.", source: S.doc },
    ],
  },
  steps: [
    { target: { flow: "before", node: "store" }, title: "Before: a store nobody could reach", cite: [S.bSqlite, S.bContract, S.bPlan],
      text: "The backend could hold a workspace, but the bridge had no command for it and the plan lived in React state. Nothing could be saved or reopened." },
    { target: { flow: "before", node: "copies" }, title: "Before: two diverging copies", cite: [S.plan],
      text: "The plan's audit found the I and F branches each carried their own copy of the store, both claiming bridge ABI 47 with different contents." },
    { target: { flow: "after", node: "conflict" }, title: "After: a stale save is refused", cite: [S.doc, S.store],
      text: "A save names the revision it was based on. If another save came first, the backend answers 'conflict' and overwrites nothing. Every revision is kept." },
    { target: { flow: "after", node: "stale" }, title: "After a restart", cite: [S.evidence, S.doc],
      text: "A restart keeps the workspace head, its history and its command receipts. A camera token or discovery job from before it is refused as stale. This tour cannot restart the server, so this part is shown only here and in the evidence." },
    { target: { evidence: true }, title: "What is proven, and what is not", cite: [S.evidence, S.goals],
      text: "The headless run proves save, reopen and a restart. The stale-revision conflict is proven by the native store test, and the 'incompatible change' case is not exercised." },
    { target: { ui: "saved-plan", view: workspaceView("experiment", "plan") }, title: "Saved workspace plan", cite: [S.saved, S.shell],
      text: "This card saves the sample sheet and the selected operator to the app's database. Opening only inspects; nothing is applied or started." },
    { target: { ui: "saved-plan-save" }, title: "Save the plan", cite: [S.saved, S.store],
      text: "Save new workspace sends the plan to the backend with no base revision. After you open a workspace, the button says Save new revision and names the revision it was based on.",
      action: {
        await: FACTS.workspaceSaved,
        prompt: "Press Save new workspace.",
        success: "the backend saved a revision ({detail}).",
        recovery: "The backend did not save. The message under the card says why: a conflict, an invalid document, or another client in control. A lost reply keeps the exact command: use Retry same save.",
        timeoutMs: 120_000,
      } },
    { target: { ui: "saved-plan-message" }, title: "What the backend said", cite: [S.saved],
      text: "The message repeats the revision the backend returned. A saved plan is a record. It is not proof that anything was acquired or is compatible." },
    { target: { ui: "saved-plan-history" }, title: "Revision history", cite: [S.saved, S.doc], interactive: true,
      text: "Press Read revision history: it lists every revision of this workspace. Restore appends a new revision; it never deletes history." },
    { target: { ui: "saved-plan-library" }, title: "Workspace library", cite: [S.saved],
      text: "List workspaces reads the saved workspaces from the backend. Open one to inspect its annotations; opening it does not change the plan you are editing." },
  ],
};
