//! The desktop shell's platform commands, answered for the instrument (YOFO Studio S5): paths,
//! preferences and the UI log live under the server's data directory, shared by every client.
//! Mirrors `desktop/src-tauri/src/platform.rs`.

use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

pub fn app_paths(data: &Path) -> Value {
    let dir = |sub: &str| data.join(sub).to_string_lossy().into_owned();
    json!({
        "app_data": data.to_string_lossy(),
        "app_config": dir("config"),
        "app_log": dir("logs"),
        "app_cache": dir("cache"),
        "documents": dir("documents"),
    })
}

fn preferences_path(data: &Path) -> PathBuf {
    data.join("config").join("preferences.json")
}

pub fn get_preferences(data: &Path) -> Result<Value, String> {
    match std::fs::read_to_string(preferences_path(data)) {
        Ok(text) => serde_json::from_str(&text).map_err(|e| format!("preferences.json is malformed: {e}")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Value::Object(Default::default())),
        Err(e) => Err(format!("read preferences: {e}")),
    }
}

/// Atomic write (temp file + rename), as on the desktop.
pub fn set_preferences(data: &Path, preferences: &Value) -> Result<(), String> {
    let path = preferences_path(data);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("create config dir: {e}"))?;
    }
    let tmp = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(preferences).map_err(|e| e.to_string())?;
    std::fs::write(&tmp, text).map_err(|e| format!("write preferences: {e}"))?;
    std::fs::rename(&tmp, &path).map_err(|e| format!("commit preferences: {e}"))
}

/// UI log lines from remote clients land in `<data>/logs/remote-shell.log`. Never log tokens.
pub fn shell_log(data: &Path, client: &str, level: &str, message: &str) -> Result<(), String> {
    let dir = data.join("logs");
    std::fs::create_dir_all(&dir).map_err(|e| format!("create log dir: {e}"))?;
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(dir.join("remote-shell.log"))
        .map_err(|e| format!("open shell log: {e}"))?;
    let epoch_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    writeln!(file, "[{epoch_ms}] [{}] [{client}] {message}", level.to_uppercase()).map_err(|e| format!("write shell log: {e}"))
}
