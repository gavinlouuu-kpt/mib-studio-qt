//! `GET /diagnostics` (#651 G12): what an operator needs to attach to an issue without a shell. Read-only,
//! the token and cross-site rules of `/files`. Versions and the installed bundle line, the process
//! uptime, the data dir, the tail of `<data>/logs/app.log` (size-capped), and the start-up lines that
//! answer the usual questions (E-modulus LUT, PL identity, RXH1, camera mode, execution provider).

use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::sync::Arc;

use axum::extract::{Query, State};
use axum::http::HeaderMap;
use axum::response::{IntoResponse, Response};
use serde::Deserialize;
use serde_json::{json, Value};

use super::Server;

/// The log tail is read from at most this many bytes at the end of the file.
const TAIL_BYTES: u64 = 256 * 1024;
/// The "key lines" are searched in at most this many bytes at the end of the file.
const SCAN_BYTES: u64 = 2 * 1024 * 1024;
const MAX_LINES: usize = 500;
const MAX_KEY_LINES: usize = 40;
/// Start-up and mode lines that answer the usual field questions.
const KEY_WORDS: &[&str] = &[
    "Young's modulus LUT",
    "RXH1",
    "PL results from",
    "execution provider",
    "AppBackend: instrument mode",
    "Align preview",
    "PL science",
    "profile",
];

#[derive(Deserialize)]
pub(crate) struct DiagnosticsQuery {
    lines: Option<usize>,
    token: Option<String>,
}

/// The last `max_bytes` of a file as lossy text, whole lines only (a cut first line is dropped).
fn tail_text(path: &Path, max_bytes: u64) -> Option<(String, u64, bool)> {
    let mut file = std::fs::File::open(path).ok()?;
    let len = file.metadata().ok()?.len();
    let start = len.saturating_sub(max_bytes);
    file.seek(SeekFrom::Start(start)).ok()?;
    let mut bytes = Vec::new();
    file.take(max_bytes).read_to_end(&mut bytes).ok()?;
    let text = String::from_utf8_lossy(&bytes).into_owned();
    let truncated = start > 0;
    let text = if truncated { text.split_once('\n').map(|(_, rest)| rest.to_string()).unwrap_or_default() } else { text };
    Some((text, len, truncated))
}

pub(crate) fn collect(server: &Server, lines: usize) -> Value {
    let config = server.config();
    let lines = lines.clamp(1, MAX_LINES);
    let log_path = Path::new(&config.data_dir).join("logs").join("app.log");
    let log = tail_text(&log_path, TAIL_BYTES).map(|(text, size, truncated)| {
        let all: Vec<&str> = text.lines().collect();
        let tail: Vec<&str> = all[all.len().saturating_sub(lines)..].to_vec();
        json!({
            "path": log_path.to_string_lossy(),
            "size": size,
            "tail": tail,
            "tail_truncated": truncated || all.len() > lines,
        })
    });
    let key_lines = tail_text(&log_path, SCAN_BYTES)
        .map(|(text, _, _)| {
            let mut found: Vec<String> = text
                .lines()
                .filter(|l| KEY_WORDS.iter().any(|w| l.contains(w)))
                .map(|l| l.chars().take(400).collect())
                .collect();
            let skip = found.len().saturating_sub(MAX_KEY_LINES);
            found.drain(..skip);
            found
        })
        .unwrap_or_default();
    // The bundle line the installer left next to the UI (`<dist>/../BUILD_INFO`).
    let bundle = config
        .dist_dir
        .as_ref()
        .and_then(|d| d.parent().map(|p| p.join("BUILD_INFO")))
        .and_then(|p| std::fs::read_to_string(p).ok())
        .map(|t| t.lines().take(20).collect::<Vec<_>>().join("\n"));
    json!({
        "server": {
            "version": env!("CARGO_PKG_VERSION"),
            "boot_id": server.boot_id,
            "uptime_s": server.started.elapsed().as_secs(),
        },
        "bundle": bundle,
        "data_dir": config.data_dir,
        "log": log,
        "key_lines": key_lines,
    })
}

pub(crate) async fn diagnostics(
    State(server): State<Arc<Server>>,
    Query(query): Query<DiagnosticsQuery>,
    headers: HeaderMap,
) -> Response {
    if let Some(refused) = super::files::gate(&server, &query.token, &headers) {
        return refused;
    }
    let lines = query.lines.unwrap_or(200);
    match tokio::task::spawn_blocking(move || collect(&server, lines)).await {
        Ok(value) => axum::Json(value).into_response(),
        Err(_) => (axum::http::StatusCode::INTERNAL_SERVER_ERROR, "diagnostics failed").into_response(),
    }
}
