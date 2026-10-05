//! Central profile registry commands (#398): desktop-only. The registry
//! needs the shell's HTTPS transport (`registry_transport`, installed on the
//! bridge before `init`), so these stay out of `mib-app-commands` and are not
//! offered over the YOFO Studio WebSocket server; a registry sign-in takes a
//! password, which must not travel over that remote transport.

use mib_app_commands as cmds;
use cmds::AppState;
use mib_bridge::ffi;
use serde::Serialize;
use tauri::State;

/// Decimal job IDs from JS: exact canonical u64, like the frame indexes.
fn parse_job_id(value: &str) -> Result<u64, String> {
    let n = value.parse::<u64>().map_err(|_| "INVALID_U64".to_string())?;
    if n.to_string() != value {
        return Err("INVALID_U64".into());
    }
    Ok(n)
}

/// Registry job status for the webview; `kind`/`state` are contract
/// `registry_job_kinds` / `registry_job_states` values.
#[derive(Serialize, Clone, Default)]
pub struct RegistryJob {
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    job_id: u64,
    kind: u32,
    state: u32,
    message: String,
}

#[derive(Serialize, Clone, Default)]
pub struct RegistryProject {
    project_id: String,
    display_name: String,
    roles: Vec<String>,
}

#[derive(Serialize, Clone, Default)]
pub struct RegistryRevision {
    revision_id: String,
    method_id: String,
    project_id: String,
    display_name: String,
    author_id: String,
    content_hash: String,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    revision_number: u64,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    metadata_version: u64,
    central_state: u32,
    /// #398 M2b: "" = not materialized; `local_validation` is a contract
    /// `registry_local_validation` value for this instrument + context.
    materialized_dir: String,
    local_validation: u32,
    validated_by: String,
    validated_at_utc: String,
    /// #398 M3b
    parent_revision_id: String,
    release_notes: String,
    newer_revision_id: String,
}

#[derive(Serialize, Clone, Default)]
pub struct RegistryDraft {
    draft_id: String,
    project_id: String,
    method_id: String,
    new_method: bool,
    method_display_name: String,
    base_revision_id: String,
    release_notes: String,
    submitted_revision_id: String,
    updated_at_utc: String,
}

#[derive(Serialize, Clone, Default)]
pub struct RegistryMethod {
    method_id: String,
    project_id: String,
    display_name: String,
    head_revision_id: String,
}

#[derive(Serialize, Clone, Default)]
pub struct RegistryHistoryEntry {
    who: String,
    what: String,
    reason: String,
    created_at: String,
    review: bool,
}

#[derive(Serialize, Clone, Default)]
pub struct RegistryConflict {
    present: bool,
    draft_id: String,
    base_revision_id: String,
    head_revision_id: String,
    compared: bool,
    upstream_changes: Vec<String>,
    draft_vs_head: Vec<String>,
}

/// #398 M2c Apply preview / outcome.
#[derive(Serialize, Clone, Default)]
pub struct MethodApplyPlan {
    ok: bool,
    error: String,
    revision_id: String,
    display_name: String,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    revision_number: u64,
    central_state: String,
    changed_keys: Vec<String>,
    camera_script_path: String,
}

#[derive(Serialize, Clone, Default)]
pub struct MethodApplyResult {
    ok: bool,
    error: String,
    applied: Vec<String>,
    not_applied: Vec<String>,
}

/// Authoring command outcome (#398 M3b): `job_id` "0" = refused.
#[derive(Serialize, Clone, Default)]
pub struct RegistryCommand {
    job_id: String,
    error: String,
}

impl From<ffi::BridgeRegistryCommand> for RegistryCommand {
    fn from(c: ffi::BridgeRegistryCommand) -> Self {
        RegistryCommand { job_id: job_id_string(c.job_id), error: c.error }
    }
}

/// "Mark validated" outcome (#398 M2b): `job_id` "0" = refused, `error` why.
#[derive(Serialize, Clone, Default)]
pub struct RegistryValidationRequest {
    job_id: String,
    error: String,
}

/// Registry worker snapshot (schema v24). No token or password, ever.
#[derive(Serialize, Clone, Default)]
pub struct RegistrySnapshot {
    valid: bool,
    configured: bool,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    generation: u64,
    origin: String,
    session: u32,
    subject_id: String,
    email: String,
    connectivity: u32,
    health_message: String,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    successful_requests: u64,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    failed_requests: u64,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    rejected_revisions: u64,
    projects: Vec<RegistryProject>,
    revisions: Vec<RegistryRevision>,
    corrupt_revision_ids: Vec<String>,
    cache_error: String,
    has_last_successful_refresh: bool,
    last_successful_refresh_unix_ms: i64,
    last_job: RegistryJob,
    #[serde(serialize_with = "cmds::event_transport::serialize_u64")]
    queued_jobs: u64,
    busy: bool,
    instrument_id: String,
    instrument_name: String,
    drafts: Vec<RegistryDraft>,
    methods: Vec<RegistryMethod>,
    history_revision_id: String,
    history: Vec<RegistryHistoryEntry>,
    submit_conflict: RegistryConflict,
}

impl From<ffi::BridgeRegistryJob> for RegistryJob {
    fn from(j: ffi::BridgeRegistryJob) -> Self {
        RegistryJob { job_id: j.job_id, kind: j.kind, state: j.state, message: j.message }
    }
}

impl From<ffi::BridgeRegistrySnapshot> for RegistrySnapshot {
    fn from(s: ffi::BridgeRegistrySnapshot) -> Self {
        RegistrySnapshot {
            valid: s.valid,
            configured: s.configured,
            generation: s.generation,
            origin: s.origin,
            session: s.session,
            subject_id: s.subject_id,
            email: s.email,
            connectivity: s.connectivity,
            health_message: s.health_message,
            successful_requests: s.successful_requests,
            failed_requests: s.failed_requests,
            rejected_revisions: s.rejected_revisions,
            projects: s
                .projects
                .into_iter()
                .map(|p| RegistryProject { project_id: p.project_id, display_name: p.display_name, roles: p.roles })
                .collect(),
            revisions: s
                .revisions
                .into_iter()
                .map(|r| RegistryRevision {
                    revision_id: r.revision_id,
                    method_id: r.method_id,
                    project_id: r.project_id,
                    display_name: r.display_name,
                    author_id: r.author_id,
                    content_hash: r.content_hash,
                    revision_number: r.revision_number,
                    metadata_version: r.metadata_version,
                    central_state: r.central_state,
                    materialized_dir: r.materialized_dir,
                    local_validation: r.local_validation,
                    validated_by: r.validated_by,
                    validated_at_utc: r.validated_at_utc,
                    parent_revision_id: r.parent_revision_id,
                    release_notes: r.release_notes,
                    newer_revision_id: r.newer_revision_id,
                })
                .collect(),
            corrupt_revision_ids: s.corrupt_revision_ids,
            cache_error: s.cache_error,
            has_last_successful_refresh: s.has_last_successful_refresh,
            last_successful_refresh_unix_ms: s.last_successful_refresh_unix_ms,
            last_job: s.last_job.into(),
            queued_jobs: s.queued_jobs,
            busy: s.busy,
            instrument_id: s.instrument_id,
            instrument_name: s.instrument_name,
            drafts: s
                .drafts
                .into_iter()
                .map(|d| RegistryDraft {
                    draft_id: d.draft_id,
                    project_id: d.project_id,
                    method_id: d.method_id,
                    new_method: d.new_method,
                    method_display_name: d.method_display_name,
                    base_revision_id: d.base_revision_id,
                    release_notes: d.release_notes,
                    submitted_revision_id: d.submitted_revision_id,
                    updated_at_utc: d.updated_at_utc,
                })
                .collect(),
            methods: s
                .methods
                .into_iter()
                .map(|m| RegistryMethod {
                    method_id: m.method_id,
                    project_id: m.project_id,
                    display_name: m.display_name,
                    head_revision_id: m.head_revision_id,
                })
                .collect(),
            history_revision_id: s.history_revision_id,
            history: s
                .history
                .into_iter()
                .map(|h| RegistryHistoryEntry {
                    who: h.who,
                    what: h.what,
                    reason: h.reason,
                    created_at: h.created_at,
                    review: h.review,
                })
                .collect(),
            submit_conflict: RegistryConflict {
                present: s.submit_conflict.present,
                draft_id: s.submit_conflict.draft_id,
                base_revision_id: s.submit_conflict.base_revision_id,
                head_revision_id: s.submit_conflict.head_revision_id,
                compared: s.submit_conflict.compared,
                upstream_changes: s.submit_conflict.upstream_changes,
                draft_vs_head: s.submit_conflict.draft_vs_head,
            },
        }
    }
}

/// Job IDs go to JS as decimal strings (exact u64); "0" means refused.
fn job_id_string(id: u64) -> String {
    id.to_string()
}

/// Queue a registry sign-in. The password is handed straight to the backend
/// worker and is not stored, logged or echoed by the shell.
#[tauri::command]
pub fn registry_sign_in(state: State<AppState>, email: String, password: String) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(job_id_string(guard.pin_mut().registry_sign_in(&email, &password)))
}

#[tauri::command]
pub fn registry_sign_out(state: State<AppState>) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(job_id_string(guard.pin_mut().registry_sign_out()))
}

#[tauri::command]
pub fn registry_refresh(state: State<AppState>) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(job_id_string(guard.pin_mut().registry_refresh()))
}

#[tauri::command]
pub fn registry_download(state: State<AppState>, revision_id: String) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(job_id_string(guard.pin_mut().registry_download(&revision_id)))
}

#[tauri::command]
pub fn registry_cancel_all(state: State<AppState>) -> Result<bool, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_cancel_all())
}

/// Write a cached revision's files read-only (#398 M2b).
#[tauri::command]
pub fn registry_materialize(state: State<AppState>, revision_id: String) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(job_id_string(guard.pin_mut().registry_materialize(&revision_id)))
}

/// "Mark validated" (#398 M2b): the backend checks that `evidence_file` was
/// recorded with this revision applied on this instrument before queueing.
#[tauri::command]
pub fn registry_record_validation(
    state: State<AppState>,
    revision_id: String,
    evidence_file: String,
    passed: bool,
) -> Result<RegistryValidationRequest, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let r = guard.pin_mut().registry_record_validation(&revision_id, &evidence_file, passed);
    Ok(RegistryValidationRequest { job_id: job_id_string(r.job_id), error: r.error })
}

// ---- #398 M3b authoring commands (drafts stay local until submitted) ----

#[tauri::command]
pub fn registry_new_draft_from_revision(
    state: State<AppState>,
    revision_id: String,
    use_current_config: bool,
) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_new_draft_from_revision(&revision_id, use_current_config).into())
}

#[tauri::command]
pub fn registry_new_method_draft(
    state: State<AppState>,
    project_id: String,
    name: String,
    release_notes: String,
) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_new_method_draft(&project_id, &name, &release_notes).into())
}

#[tauri::command]
pub fn registry_set_draft_notes(state: State<AppState>, draft_id: String, notes: String) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_set_draft_notes(&draft_id, &notes).into())
}

#[tauri::command]
pub fn registry_draft_from_head(
    state: State<AppState>,
    draft_id: String,
    keep_draft_config: bool,
) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_draft_from_head(&draft_id, keep_draft_config).into())
}

#[tauri::command]
pub fn registry_submit_draft(state: State<AppState>, draft_id: String, as_branch: bool) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_submit_draft(&draft_id, as_branch).into())
}

#[tauri::command]
pub fn registry_delete_draft(state: State<AppState>, draft_id: String) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_delete_draft(&draft_id).into())
}

/// `target` is a contract `registry_central_states` value; reason required.
#[tauri::command]
pub fn registry_transition(
    state: State<AppState>,
    revision_id: String,
    target: u32,
    reason: String,
) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_transition(&revision_id, target, &reason).into())
}

#[tauri::command]
pub fn registry_fetch_history(state: State<AppState>, revision_id: String) -> Result<RegistryCommand, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().registry_fetch_history(&revision_id).into())
}

/// #398 M2c: what applying `revision_id` would change.
#[tauri::command]
pub fn registry_plan_apply(state: State<AppState>, revision_id: String) -> Result<MethodApplyPlan, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let p = guard.pin_mut().registry_plan_apply(&revision_id);
    Ok(MethodApplyPlan {
        ok: p.ok,
        error: p.error,
        revision_id: p.revision_id,
        display_name: p.display_name,
        revision_number: p.revision_number,
        central_state: p.central_state,
        changed_keys: p.changed_keys,
        camera_script_path: p.camera_script_path,
    })
}

/// #398 M2c: apply `revision_id`'s config.json exactly (backend applier).
#[tauri::command]
pub fn registry_apply_method(state: State<AppState>, revision_id: String) -> Result<MethodApplyResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let r = guard.pin_mut().registry_apply_method(&revision_id);
    Ok(MethodApplyResult { ok: r.ok, error: r.error, applied: r.applied, not_applied: r.not_applied })
}

/// Registry worker snapshot; never waits on a registry request.
#[tauri::command]
pub fn fetch_registry_snapshot(state: State<AppState>) -> Result<RegistrySnapshot, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_registry_snapshot().into())
}

#[tauri::command]
pub fn fetch_registry_job(state: State<AppState>, job_id: String) -> Result<RegistryJob, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_registry_job(parse_job_id(&job_id)?).into())
}
