//! Checked persistence stays in the shared backend, not a Rust config copy.
use serde::Serialize;
use tauri::State;
use crate::AppState;

#[derive(Serialize)]
pub struct ConfigDocument {
    ok: bool, path: String, revision: String, document_json: String, error: String,
}
#[derive(Serialize)]
pub struct ConfigTransactionResult {
    saved: bool, applied: bool, verified: bool, conflict: bool, revision: String, error: String,
}
#[tauri::command]
pub fn fetch_config_document(state: State<AppState>, path: String) -> Result<ConfigDocument, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let d = guard.pin_mut().fetch_config_document(&path);
    Ok(ConfigDocument { ok: d.ok, path: d.path, revision: d.revision, document_json: d.document_json, error: d.error })
}
#[tauri::command]
pub fn apply_config_document(state: State<AppState>, path: String, baseline: String, patch: String) -> Result<ConfigTransactionResult, String> {
    if patch.len() > 65_536 { return Err("Config patch exceeds 64 KiB".into()); }
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let r = guard.pin_mut().apply_config_document(&path, &baseline, &patch);
    Ok(ConfigTransactionResult { saved: r.saved, applied: r.applied, verified: r.verified, conflict: r.conflict, revision: r.revision, error: r.error })
}
