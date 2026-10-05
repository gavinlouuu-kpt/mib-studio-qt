//! Checked persistence stays in the shared backend, not a Rust config copy.
use serde::Serialize;
use crate::AppState;

#[derive(Serialize)]
pub struct ConfigDocument {
    ok: bool, path: String, revision: String, document_json: String, error: String,
}
#[derive(Serialize)]
pub struct ConfigTransactionResult {
    saved: bool, applied: bool, verified: bool, conflict: bool, revision: String, error: String,
}
pub fn fetch_config_document(state: &AppState, path: String) -> Result<ConfigDocument, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let d = guard.pin_mut().fetch_config_document(&path);
    Ok(ConfigDocument { ok: d.ok, path: d.path, revision: d.revision, document_json: d.document_json, error: d.error })
}
pub fn apply_config_document(state: &AppState, path: String, baseline: String, patch: String) -> Result<ConfigTransactionResult, String> {
    if patch.len() > 65_536 { return Err("Config patch exceeds 64 KiB".into()); }
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let r = guard.pin_mut().apply_config_document(&path, &baseline, &patch);
    Ok(ConfigTransactionResult { saved: r.saved, applied: r.applied, verified: r.verified, conflict: r.conflict, revision: r.revision, error: r.error })
}

pub fn profile_command(state: &AppState, base: String, request: String) -> Result<serde_json::Value, String> {
    if request.len() > 8 * 1024 * 1024 + 4096 { return Err("Profile request too large".into()); }
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().profile_command(&base, &request)).map_err(|e| e.to_string())
}

/// Blocking: fetches over the network. Hosts run it off their event loop.
pub fn profile_fetch_url(url: String) -> Result<serde_json::Value, String> {
    serde_json::from_str(&mib_bridge::ffi::profile_fetch_url(&url)).map_err(|e| e.to_string())
}

/// Blocking: may download and verify a core. `cache_dir` is the host's processing-core cache.
pub fn processing_core_command(state: &AppState, cache_dir: &str, request: String) -> Result<serde_json::Value, String> {
    if request.len() > 4 * 1024 * 1024 { return Err("Core request too large".into()); }
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().processing_core_command(cache_dir, &request)).map_err(|e| e.to_string())
}
