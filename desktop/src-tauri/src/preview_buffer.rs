use tauri::{Manager, State};
use crate::AppState;
#[tauri::command]
pub fn fetch_preview_buffer(state: State<AppState>) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().fetch_preview_buffer()).map_err(|e| e.to_string())
}
#[tauri::command]
pub async fn save_preview_buffer(app: tauri::AppHandle, request: String) -> Result<serde_json::Value, String> {
    if request.len() > 16384 { return Err("buffer request exceeds limit".into()); }
    tauri::async_runtime::spawn_blocking(move || {
        let state = app.state::<AppState>();
        let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
        serde_json::from_str(&guard.pin_mut().save_preview_buffer(&request)).map_err(|e| e.to_string())
    }).await.map_err(|e| e.to_string())?
}
