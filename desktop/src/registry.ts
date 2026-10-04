// Central profile registry view model (bridge schema v15, issue #398).
//
// Pure projection of the backend registry snapshot onto what the Central
// Methods panel renders. It mirrors the Qt CentralMethodsDialog wording and
// enablement rules so both shells present the same backend truth: each
// central state is shown as itself (never collapsed into "ready"), central
// state is never presented as local validation. #398 M2b adds per-revision
// local validation on this instrument and the selected-row actions
// (materialize, mark validated / failed). Apply stays a Qt-shell action: the
// React shell has no config.json applier yet, so it is shown disabled with
// the reason rather than simulated.
import type { RegistryJob, RegistryRevision, RegistrySnapshot } from "./bridge";
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
}

/** What the selected row allows (all false when nothing is selected). */
export interface RegistryActions {
  canMaterialize: boolean;
  canMarkValidated: boolean;
  /** Always false in the React shell; `applyReason` says why. */
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

export const APPLY_UNAVAILABLE =
  "Apply is available in the Qt app. The React shell has no config.json applier yet (#398 follow-up), " +
  "so applying here would not load the method.";

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
  const none = { canMaterialize: false, canMarkValidated: false, canApply: false, applyReason: APPLY_UNAVAILABLE };
  const row = view.rows.find((r) => r.revisionId === revisionId);
  if (!s || !row || s.busy || !s.valid || !s.configured) return none;
  const hasCache = s.session !== REGISTRY_SESSION_STATES.SignedOut;
  return {
    canMaterialize: hasCache && row.usable,
    // The validator must be an authenticated registry user (backend rule).
    canMarkValidated: s.session === REGISTRY_SESSION_STATES.SignedIn && row.usable && !!s.instrument_id,
    canApply: false,
    applyReason: APPLY_UNAVAILABLE,
  };
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
