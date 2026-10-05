// Central profile registry view model (bridge schema v15, issue #398).
//
// Pure projection of the backend registry snapshot onto what the Central
// Methods panel renders, so the panel presents the backend truth: each
// central state is shown as itself (never collapsed into "ready"), central
// state is never presented as local validation. #398 M2b adds per-revision
// local validation on this instrument and the selected-row actions
// (materialize, mark validated / failed). Apply (M2c) goes through the
// backend config.json applier: preview the changed keys, confirm, apply.
import type {
  MethodApplyPlan,
  MethodApplyResult,
  RegistryDraft,
  RegistryJob,
  RegistryRevision,
  RegistrySnapshot,
} from "./bridge";
import {
  REGISTRY_CENTRAL_STATES,
  REGISTRY_CONNECTIVITY,
  REGISTRY_JOB_KINDS,
  REGISTRY_JOB_STATES,
  REGISTRY_LOCAL_VALIDATION,
  REGISTRY_SESSION_STATES,
} from "./bridgeContract";

export interface RegistryRow {
  revisionId: string;
  method: string;
  revision: string;
  project: string;
  state: string;
  revoked: boolean;
  hashPrefix: string;
  hash: string;
  author: string;
  /** Local validation on this instrument under the current context. */
  validation: string;
  validationFailed: boolean;
  materialized: boolean;
  /** Published or superseded: may be materialized/validated. */
  usable: boolean;
  /** #398 M3b: "r13 available" or "". */
  update: string;
  notes: string;
}

/** What the selected row allows (all false when nothing is selected). */
export interface RegistryActions {
  canMaterialize: boolean;
  canMarkValidated: boolean;
  /** Materialized published/superseded revision (M2c); `applyReason` is the tooltip. */
  canApply: boolean;
  applyReason: string;
}

export interface RegistryView {
  status: string;
  account: string;
  activity: string[];
  warnings: string[];
  showSignIn: boolean;
  canSignIn: boolean;
  canRefresh: boolean;
  canCancel: boolean;
  canSignOut: boolean;
  /** Prefill for the email field from the cached/signed-in session. */
  email: string;
  /** "Instrument: MIB-01 (1234abcd…)" or a warning when unknown. */
  instrument: string;
  rows: RegistryRow[];
}

export const APPLY_NEEDS_FILES = "Materialize the revision first; Apply loads its files exactly.";
export const APPLY_HINT = "Load this revision's config.json exactly (you confirm the changed settings first).";

export const CENTRAL_STATE_NOTE =
  "Central state is the registry's approval and publication record only. It is not local " +
  "validation on this instrument, and listing a method here does not select or apply it.";

export function connectivityText(s: RegistrySnapshot): string {
  if (!s.valid) return "Central registry: backend not ready";
  if (!s.configured) {
    return "Central registry: not configured (set MIB_PROFILE_REGISTRY_URL and MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY)";
  }
  switch (s.connectivity) {
    case REGISTRY_CONNECTIVITY.Online:
      return "Central registry: online";
    case REGISTRY_CONNECTIVITY.Offline:
      return "Central registry: offline (cached methods stay available)";
    case REGISTRY_CONNECTIVITY.AuthenticationRequired:
      return "Central registry: sign-in required";
    case REGISTRY_CONNECTIVITY.PermissionDenied:
      return "Central registry: access denied";
    case REGISTRY_CONNECTIVITY.Failed:
      return "Central registry: request failed";
    default:
      return "Central registry: not contacted yet";
  }
}

export function accountText(s: RegistrySnapshot): string {
  const who = s.email || s.subject_id;
  if (s.session === REGISTRY_SESSION_STATES.SignedIn) return `Signed in as ${who}`;
  if (s.session === REGISTRY_SESSION_STATES.CachedOffline) {
    return `Cached methods for ${who} (not signed in; sign in to refresh)`;
  }
  return "Not signed in";
}

export function centralStateText(state: number): string {
  switch (state) {
    case REGISTRY_CENTRAL_STATES.Submitted:
      return "Submitted";
    case REGISTRY_CENTRAL_STATES.Approved:
      return "Approved (not published)";
    case REGISTRY_CENTRAL_STATES.Rejected:
      return "Rejected";
    case REGISTRY_CENTRAL_STATES.Published:
      return "Published";
    case REGISTRY_CENTRAL_STATES.Superseded:
      return "Superseded";
    case REGISTRY_CENTRAL_STATES.Archived:
      return "Archived";
    case REGISTRY_CENTRAL_STATES.Revoked:
      return "REVOKED - do not use";
    default:
      return `Unknown state ${state}`;
  }
}

function nameOf(table: Record<string, number>, value: number): string {
  const entry = Object.entries(table).find(([, v]) => v === value);
  return entry ? entry[0] : String(value);
}

export function localValidationText(r: RegistryRevision): string {
  switch (r.local_validation) {
    case REGISTRY_LOCAL_VALIDATION.Passed:
      return `Validated here by ${r.validated_by} (${r.validated_at_utc} UTC)`;
    case REGISTRY_LOCAL_VALIDATION.Failed:
      return `Validation FAILED here (${r.validated_by}, ${r.validated_at_utc} UTC)`;
    default:
      return "Not validated here";
  }
}

export function instrumentText(s: RegistrySnapshot): string {
  if (!s.valid) return "";
  if (!s.instrument_id) return "Instrument identity unknown: local validation is unavailable";
  const id = `${s.instrument_id.slice(0, 8)}…`;
  return s.instrument_name ? `Instrument: ${s.instrument_name} (${id})` : `Instrument: ${id}`;
}

export function actionsFor(view: RegistryView, s: RegistrySnapshot | null, revisionId: string | null): RegistryActions {
  const none = { canMaterialize: false, canMarkValidated: false, canApply: false, applyReason: APPLY_HINT };
  const row = view.rows.find((r) => r.revisionId === revisionId);
  if (!s || !row || s.busy || !s.valid || !s.configured) return none;
  const hasCache = s.session !== REGISTRY_SESSION_STATES.SignedOut;
  return {
    canMaterialize: hasCache && row.usable,
    // The validator must be an authenticated registry user (backend rule).
    canMarkValidated: s.session === REGISTRY_SESSION_STATES.SignedIn && row.usable && !!s.instrument_id,
    canApply: hasCache && row.usable && row.materialized,
    applyReason: row.usable && !row.materialized ? APPLY_NEEDS_FILES : APPLY_HINT,
  };
}

/** Confirmation text for an Apply preview (#398 M2c). */
export function applyConfirmText(plan: MethodApplyPlan): string {
  if (!plan.ok) return `Cannot apply: ${plan.error}`;
  const shown = plan.changed_keys.slice(0, 25).map((k) => `  • ${k}`);
  if (plan.changed_keys.length > 25) shown.push(`  ... and ${plan.changed_keys.length - 25} more`);
  const summary =
    plan.changed_keys.length === 0
      ? "The applied config.json already has these values; it will be loaded byte-for-byte."
      : "These config.json settings change (instrument settings such as COM ports or the save directory travel with the method):\n" +
        shown.join("\n");
  return (
    `Apply "${plan.display_name}" r${plan.revision_number} (${plan.central_state})?\n\n${summary}\n\n` +
    `The camera script is not applied automatically: ${plan.camera_script_path}`
  );
}

/** Outcome line of an Apply (#398 M2c). */
export function applyResultText(result: MethodApplyResult): string {
  if (!result.ok) return `Apply failed: ${result.error}`;
  const skipped = result.not_applied.length > 0 ? ` Not applicable in this app: ${result.not_applied.join(", ")}.` : "";
  return `Applied (${result.applied.join(", ")}).${skipped}`;
}

export function jobText(job: RegistryJob): string {
  if (!job.job_id || job.job_id === "0") return "";
  const kind = nameOf(REGISTRY_JOB_KINDS, job.kind).replace(/([a-z])([A-Z])/g, "$1 $2").toLowerCase();
  const state = nameOf(REGISTRY_JOB_STATES, job.state).toLowerCase();
  return job.message ? `Last action: ${kind} ${state} - ${job.message}` : `Last action: ${kind} ${state}`;
}

export function toRegistryView(s: RegistrySnapshot): RegistryView {
  const signedIn = s.session === REGISTRY_SESSION_STATES.SignedIn;
  const hasUser = s.session !== REGISTRY_SESSION_STATES.SignedOut;
  const usable = s.valid && s.configured;

  const activity: string[] = [];
  if (s.busy) activity.push("Working...");
  if (s.has_last_successful_refresh) {
    activity.push(`Last refresh: ${new Date(s.last_successful_refresh_unix_ms).toISOString()}`);
  }
  const job = jobText(s.last_job);
  if (job) activity.push(job);

  const warnings: string[] = [];
  if (s.cache_error) warnings.push(s.cache_error);
  if (s.corrupt_revision_ids.length > 0) {
    warnings.push(`${s.corrupt_revision_ids.length} cached revision(s) failed integrity checks and are hidden.`);
  }
  if (Number(s.rejected_revisions) > 0) {
    warnings.push(`${s.rejected_revisions} registry revision(s) failed verification and were not cached.`);
  }
  if (usable && s.health_message && s.connectivity !== REGISTRY_CONNECTIVITY.Online) {
    warnings.push(s.health_message);
  }

  const projectNames = new Map(s.projects.map((p) => [p.project_id, p.display_name]));
  const rows = s.revisions.map((r) => ({
    revisionId: r.revision_id,
    method: r.display_name,
    revision: `r${r.revision_number}`,
    project: projectNames.get(r.project_id) ?? r.project_id,
    state: centralStateText(r.central_state),
    revoked: r.central_state === REGISTRY_CENTRAL_STATES.Revoked,
    hashPrefix: r.content_hash.slice(0, 12),
    hash: r.content_hash,
    author: r.author_id,
    validation: localValidationText(r),
    validationFailed: r.local_validation === REGISTRY_LOCAL_VALIDATION.Failed,
    materialized: r.materialized_dir !== "",
    usable:
      r.central_state === REGISTRY_CENTRAL_STATES.Published || r.central_state === REGISTRY_CENTRAL_STATES.Superseded,
    update: (() => {
      const newer = s.revisions.find((n) => n.revision_id === r.newer_revision_id);
      return newer ? `r${newer.revision_number} available` : "";
    })(),
    notes: r.release_notes,
  }));

  return {
    status: connectivityText(s),
    account: accountText(s),
    activity,
    warnings,
    showSignIn: usable && !signedIn,
    canSignIn: usable && !s.busy,
    canRefresh: usable && signedIn && !s.busy,
    canCancel: usable && (s.busy || Number(s.queued_jobs) > 0),
    canSignOut: usable && hasUser && !s.busy,
    email: hasUser ? s.email : "",
    instrument: instrumentText(s),
    rows,
  };
}

/** True when the snapshot changed in a way the panel must re-render. */
export function changed(prev: RegistrySnapshot | null, next: RegistrySnapshot): boolean {
  if (!prev || prev.generation !== next.generation || prev.busy !== next.busy || prev.valid !== next.valid) return true;
  // Local validation also depends on the method context (core build, camera
  // source), which changes without a registry generation bump.
  const local = (x: RegistrySnapshot) => x.revisions.map((r) => r.local_validation).join(",");
  return local(prev) !== local(next);
}

// ---- #398 M3b authoring: role and conflict rules ----

/** UI enablement only; the server enforces roles. */
export function hasRole(s: RegistrySnapshot, projectId: string, role: string): boolean {
  const p = s.projects.find((x) => x.project_id === projectId);
  return !!p && (p.roles.includes(role) || p.roles.includes("admin"));
}

export function revisionLabel(s: RegistrySnapshot, revisionId: string): string {
  if (!revisionId) return "";
  const r = s.revisions.find((x) => x.revision_id === revisionId);
  return r ? `r${r.revision_number}` : `${revisionId.slice(0, 8)}…`;
}

export interface ReviewActions {
  canNewDraft: boolean;
  canApprove: boolean;
  canReject: boolean;
  canPublish: boolean;
  canArchive: boolean;
  canRevoke: boolean;
  canHistory: boolean;
}

export function reviewActionsFor(s: RegistrySnapshot | null, revisionId: string | null): ReviewActions {
  const none = {
    canNewDraft: false,
    canApprove: false,
    canReject: false,
    canPublish: false,
    canArchive: false,
    canRevoke: false,
    canHistory: false,
  };
  const r = s?.revisions.find((x) => x.revision_id === revisionId);
  if (!s || !r || !s.valid || !s.configured || s.busy) return none;
  const signedIn = s.session === REGISTRY_SESSION_STATES.SignedIn;
  const hasCache = s.session !== REGISTRY_SESSION_STATES.SignedOut;
  const C = REGISTRY_CENTRAL_STATES;
  const reviewer = signedIn && hasRole(s, r.project_id, "reviewer");
  const publisher = signedIn && hasRole(s, r.project_id, "publisher");
  return {
    canNewDraft: hasCache,
    canApprove: reviewer && r.central_state === C.Submitted,
    canReject: reviewer && r.central_state === C.Submitted,
    canPublish: publisher && r.central_state === C.Approved,
    canArchive: publisher && (r.central_state === C.Published || r.central_state === C.Superseded),
    canRevoke:
      publisher &&
      (r.central_state === C.Published || r.central_state === C.Superseded || r.central_state === C.Archived),
    canHistory: signedIn,
  };
}

export interface DraftRow {
  draftId: string;
  method: string;
  base: string;
  status: string;
  notes: string;
  conflict: boolean;
  submitted: boolean;
}

export function draftRows(s: RegistrySnapshot): DraftRow[] {
  return s.drafts.map((d: RegistryDraft) => {
    const conflict = s.submit_conflict.present && s.submit_conflict.draft_id === d.draft_id;
    const submitted = d.submitted_revision_id !== "";
    return {
      draftId: d.draft_id,
      method: d.method_display_name,
      base: d.new_method ? "new method" : revisionLabel(s, d.base_revision_id),
      status: submitted
        ? `Submitted as ${revisionLabel(s, d.submitted_revision_id)}`
        : conflict
          ? `CONFLICT: head is now ${revisionLabel(s, s.submit_conflict.head_revision_id)}`
          : "Draft (not submitted)",
      notes: d.release_notes,
      conflict,
      submitted,
    };
  });
}

export interface DraftActions {
  canNewMethod: boolean;
  canEditNotes: boolean;
  canSubmit: boolean;
  canBranch: boolean;
  canFromHead: boolean;
  canDiscard: boolean;
}

export function authorProjects(s: RegistrySnapshot): { id: string; name: string }[] {
  return s.projects.filter((p) => hasRole(s, p.project_id, "author")).map((p) => ({ id: p.project_id, name: p.display_name }));
}

export function draftActionsFor(s: RegistrySnapshot | null, draftId: string | null): DraftActions {
  const usable = !!s && s.valid && s.configured && !s.busy && s.session !== REGISTRY_SESSION_STATES.SignedOut;
  const d = s?.drafts.find((x) => x.draft_id === draftId);
  const signedIn = s?.session === REGISTRY_SESSION_STATES.SignedIn;
  const open = !!d && d.submitted_revision_id === "";
  const author = !!s && !!d && hasRole(s, d.project_id, "author");
  const conflict = !!s && !!d && s.submit_conflict.present && s.submit_conflict.draft_id === d.draft_id;
  return {
    canNewMethod: usable && !!s && authorProjects(s).length > 0,
    canEditNotes: usable && open,
    canSubmit: usable && signedIn && open && author && !conflict,
    canBranch: usable && signedIn && open && author && conflict,
    canFromHead: usable && open && conflict && !!s && s.submit_conflict.head_revision_id !== "",
    canDiscard: usable && !!d,
  };
}

function changes(keys: string[]): string {
  if (keys.length === 0) return "none";
  const shown = keys.slice(0, 12).join(", ");
  return keys.length > 12 ? `${shown}, … ${keys.length - 12} more` : shown;
}

/** The conflict explanation for `draftId`, or "" when it has none. */
export function conflictText(s: RegistrySnapshot, draftId: string | null): string {
  const c = s.submit_conflict;
  if (!c.present || c.draft_id !== draftId) return "";
  const base = revisionLabel(s, c.base_revision_id);
  const head = revisionLabel(s, c.head_revision_id);
  const lines = [`This draft is based on ${base}, but ${head} has been published since. Nothing was sent.`];
  if (c.compared) {
    lines.push(`Changed upstream: ${changes(c.upstream_changes)}`);
    lines.push(`Your draft differs from the head in: ${changes(c.draft_vs_head)}`);
  } else {
    lines.push("Refresh to compare the two revisions.");
  }
  lines.push(`Choose: submit as a branch of ${base}, start a new draft from ${head}, or discard.`);
  return lines.join("\n");
}

/** Lineage, notes, update and (when fetched) history of a revision. */
export function detailLines(s: RegistrySnapshot, revisionId: string | null): string[] {
  const r = s.revisions.find((x) => x.revision_id === revisionId);
  if (!r) return [];
  const lines = [
    `${r.display_name} r${r.revision_number} · ${centralStateText(r.central_state)} · parent ${
      r.parent_revision_id ? revisionLabel(s, r.parent_revision_id) : "none"
    }`,
  ];
  const newer = s.revisions.find((n) => n.revision_id === r.newer_revision_id);
  if (newer) lines.push(`Update available: r${newer.revision_number} is published`);
  lines.push(`Release notes: ${r.release_notes || "(none)"}`);
  if (s.history_revision_id === r.revision_id) {
    for (const h of s.history.filter((x) => x.review))
      lines.push(`Review: ${h.what} by ${h.who} at ${h.created_at} - ${h.reason}`);
    for (const h of s.history.filter((x) => !x.review))
      lines.push(`Event: ${h.what} by ${h.who} at ${h.created_at}${h.reason ? ` - ${h.reason}` : ""}`);
  }
  return lines;
}
