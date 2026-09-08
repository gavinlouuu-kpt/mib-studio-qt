//! Platform/shell services for the desktop app (BE-9, issue #279, epic #246).
//!
//! These responsibilities lived in Qt frontend utilities; they are ported to
//! the Rust/Tauri shell deliberately (never by keeping Qt helper code):
//! stable app paths, persisted shell preferences, and shell-side logging into
//! the app log directory. Native open-folder / external-URL actions go
//! through `tauri-plugin-opener`, capability-scoped in
//! `capabilities/default.json`.

use std::io::Write;
use std::path::PathBuf;

use serde::Serialize;
use tauri::path::BaseDirectory;
use tauri::Manager;

/// Stable application paths (BE-9): one authoritative answer for where data,
/// config, logs, and cache live, resolved from the app identifier
/// (`bio.yofo.mib-studio`) so locations stay consistent across shells.
#[derive(Serialize, Clone, Default)]
pub struct AppPaths {
    pub app_data: String,
    pub app_config: String,
    pub app_log: String,
    pub app_cache: String,
    pub documents: String,
}

fn resolve(app: &tauri::AppHandle, dir: BaseDirectory) -> Result<PathBuf, String> {
    app.path()
        .resolve("", dir)
        .map_err(|e| format!("resolve {dir:?}: {e}"))
}

#[tauri::command]
pub fn app_paths(app: tauri::AppHandle) -> Result<AppPaths, String> {
    Ok(AppPaths {
        app_data: resolve(&app, BaseDirectory::AppData)?.to_string_lossy().into_owned(),
        app_config: resolve(&app, BaseDirectory::AppConfig)?.to_string_lossy().into_owned(),
        app_log: resolve(&app, BaseDirectory::AppLog)?.to_string_lossy().into_owned(),
        app_cache: resolve(&app, BaseDirectory::AppCache)?.to_string_lossy().into_owned(),
        documents: resolve(&app, BaseDirectory::Document)
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default(),
    })
}

fn preferences_path(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    Ok(resolve(app, BaseDirectory::AppConfig)?.join("preferences.json"))
}

/// Persisted shell preferences (window/sidebar/user settings) as one JSON
/// document in the app-config dir. Survives webview-storage clearing and is
/// shared state for any future shell.
#[tauri::command]
pub fn get_preferences(app: tauri::AppHandle) -> Result<serde_json::Value, String> {
    let path = preferences_path(&app)?;
    match std::fs::read_to_string(&path) {
        Ok(text) => serde_json::from_str(&text)
            .map_err(|e| format!("preferences.json is malformed: {e}")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            Ok(serde_json::Value::Object(Default::default()))
        }
        Err(e) => Err(format!("read preferences: {e}")),
    }
}

/// Atomic write (temp file + rename) so a crash mid-write never corrupts the
/// preferences document.
#[tauri::command]
pub fn set_preferences(app: tauri::AppHandle, preferences: serde_json::Value) -> Result<(), String> {
    let path = preferences_path(&app)?;
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("create config dir: {e}"))?;
    }
    let tmp = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(&preferences).map_err(|e| e.to_string())?;
    std::fs::write(&tmp, text).map_err(|e| format!("write preferences: {e}"))?;
    std::fs::rename(&tmp, &path).map_err(|e| format!("commit preferences: {e}"))?;
    Ok(())
}

/// Shell-side log sink (BE-9): webview/JS log lines land in
/// `<app_log>/desktop-shell.log` next to the backend logs, so C++/Rust/
/// frontend context can be correlated from one directory. Never log tokens.
#[tauri::command]
pub fn shell_log(app: tauri::AppHandle, level: String, message: String) -> Result<(), String> {
    let dir = resolve(&app, BaseDirectory::AppLog)?;
    std::fs::create_dir_all(&dir).map_err(|e| format!("create log dir: {e}"))?;
    let path = dir.join("desktop-shell.log");
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .map_err(|e| format!("open shell log: {e}"))?;
    let epoch_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    writeln!(file, "[{epoch_ms}] [{}] {message}", level.to_uppercase())
        .map_err(|e| format!("write shell log: {e}"))?;
    Ok(())
}
