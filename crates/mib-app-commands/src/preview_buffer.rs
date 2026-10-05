use crate::AppState;

pub fn fetch_preview_buffer(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().fetch_preview_buffer()).map_err(|e| e.to_string())
}

/// Blocking: writes the buffered frames.
pub fn save_preview_buffer(state: &AppState, request: String) -> Result<serde_json::Value, String> {
    if request.len() > 16384 { return Err("buffer request exceeds limit".into()); }
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().save_preview_buffer(&request)).map_err(|e| e.to_string())
}
