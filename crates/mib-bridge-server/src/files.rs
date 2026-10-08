//! Read-only file listing and download (#651 G3): `GET /files` and `GET /files/download`.
//!
//! One root, the data dir. Every request path is canonicalised and must stay under the
//! canonical root, so `..`, absolute paths elsewhere and symlinks that leave the root are all
//! refused (404, so existence outside the root is not revealed). Names starting with `.` are
//! neither listed nor served. Files an experiment is still writing are listed as
//! `in_progress` and refused for download until the run is finished. Same token rule as `/ws`;
//! a cross-site browser request (`Sec-Fetch-Site` other than same-origin/none) is refused.

use std::path::{Component, Path, PathBuf};
use std::sync::Arc;

use axum::body::Body;
use axum::extract::{Query, Request, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use futures_util::StreamExt;
use serde::Deserialize;
use serde_json::{json, Value};
use tower::ServiceExt;
use tower_http::services::ServeFile;

use super::{AuthQuery, Server};

/// At most this many downloads at once (a 2-core A9 on a RAM root, next to a 5 kHz run).
pub(crate) const MAX_DOWNLOADS: usize = 2;

#[derive(Deserialize)]
pub(crate) struct FilesQuery {
    path: Option<String>,
    token: Option<String>,
}

fn not_found() -> Response {
    (StatusCode::NOT_FOUND, "not found").into_response()
}

/// The token rule of `/ws`, then the cross-site rule.
fn gate(server: &Server, token: &Option<String>, headers: &HeaderMap) -> Option<Response> {
    if !server.authorized(&AuthQuery { token: token.clone() }, headers) {
        return Some((StatusCode::UNAUTHORIZED, "token required").into_response());
    }
    match headers.get("sec-fetch-site").and_then(|v| v.to_str().ok()) {
        None | Some("same-origin") | Some("none") => None,
        Some(_) => Some((StatusCode::FORBIDDEN, "cross-site requests are refused").into_response()),
    }
}

fn root_of(server: &Server) -> Option<PathBuf> {
    std::fs::canonicalize(&server.config().data_dir).ok()
}

/// Resolve a request path under `root` (relative paths are joined to it), or `None` when it does
/// not exist, leaves the root (including through a symlink) or passes a dot-name.
pub(crate) fn resolve(root: &Path, requested: Option<&str>) -> Option<PathBuf> {
    let joined = match requested.map(str::trim).filter(|p| !p.is_empty()) {
        None => root.to_path_buf(),
        Some(p) => {
            let p = Path::new(p);
            if p.is_absolute() {
                p.to_path_buf()
            } else {
                root.join(p)
            }
        }
    };
    let canon = std::fs::canonicalize(joined).ok()?;
    let rel = canon.strip_prefix(root).ok()?;
    for component in rel.components() {
        if let Component::Normal(name) = component {
            if name.to_string_lossy().starts_with('.') {
                return None;
            }
        }
    }
    Some(canon)
}

/// The output file of a run that is not finished (experiment Starting/Active/Stopping), if any.
fn in_progress_path(server: &Server, root: &Path) -> Option<PathBuf> {
    let status = mib_app_commands::fetch_experiment_status(&server.state).ok()?;
    let status = serde_json::to_value(status).ok()?;
    if status["valid"] != json!(true) || status["terminal"] == json!(true) || status["state"].as_u64().unwrap_or(0) == 0 {
        return None;
    }
    let output = status["output_path"].as_str().filter(|p| !p.is_empty())?;
    let path = Path::new(output);
    let joined = if path.is_absolute() { path.to_path_buf() } else { root.join(path) };
    Some(std::fs::canonicalize(&joined).unwrap_or(joined))
}

fn mtime_ns(meta: &std::fs::Metadata) -> u64 {
    meta.modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0)
}

pub(crate) async fn list(State(server): State<Arc<Server>>, Query(query): Query<FilesQuery>, headers: HeaderMap) -> Response {
    if let Some(refused) = gate(&server, &query.token, &headers) {
        return refused;
    }
    let listed = tokio::task::spawn_blocking(move || -> Option<Value> {
        let root = root_of(&server)?;
        let dir = resolve(&root, query.path.as_deref())?;
        if !std::fs::metadata(&dir).ok()?.is_dir() {
            return None;
        }
        let busy = in_progress_path(&server, &root);
        let mut entries = Vec::new();
        for entry in std::fs::read_dir(&dir).ok()?.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') {
                continue;
            }
            // A symlink counts only when its target stays inside the root.
            let Some(target) = resolve(&root, Some(&entry.path().to_string_lossy())) else { continue };
            let Ok(meta) = std::fs::metadata(&target) else { continue };
            let kind = if meta.is_dir() {
                "dir"
            } else if meta.is_file() {
                "file"
            } else {
                continue;
            };
            let in_progress = kind == "file" && busy.as_deref() == Some(target.as_path());
            entries.push(json!({
                "name": name,
                "kind": kind,
                "size": if kind == "file" { meta.len() } else { 0 },
                "mtime_ns": mtime_ns(&meta),
                "in_progress": in_progress,
            }));
        }
        entries.sort_by(|a, b| {
            let dir_first = (b["kind"] == json!("dir")).cmp(&(a["kind"] == json!("dir")));
            dir_first.then_with(|| a["name"].as_str().cmp(&b["name"].as_str()))
        });
        let parent = if dir == root { None } else { dir.parent().map(|p| p.to_string_lossy().into_owned()) };
        Some(json!({
            "root": root.to_string_lossy(),
            "path": dir.to_string_lossy(),
            "parent": parent,
            "entries": entries,
        }))
    })
    .await
    .ok()
    .flatten();
    match listed {
        Some(value) => axum::Json(value).into_response(),
        None => not_found(),
    }
}

pub(crate) async fn download(
    State(server): State<Arc<Server>>,
    Query(query): Query<FilesQuery>,
    headers: HeaderMap,
    request: Request,
) -> Response {
    if let Some(refused) = gate(&server, &query.token, &headers) {
        return refused;
    }
    let checked = {
        let server = server.clone();
        tokio::task::spawn_blocking(move || -> Result<PathBuf, Response> {
            let root = root_of(&server).ok_or_else(not_found)?;
            let file = resolve(&root, query.path.as_deref()).ok_or_else(not_found)?;
            if !std::fs::metadata(&file).map(|m| m.is_file()).unwrap_or(false) {
                return Err(not_found());
            }
            if in_progress_path(&server, &root).as_deref() == Some(file.as_path()) {
                return Err((StatusCode::CONFLICT, "the file is still being written").into_response());
            }
            Ok(file)
        })
        .await
    };
    let file = match checked {
        Ok(Ok(file)) => file,
        Ok(Err(refused)) => return refused,
        Err(_) => return not_found(),
    };
    let Ok(permit) = server.downloads.clone().try_acquire_owned() else {
        return (StatusCode::TOO_MANY_REQUESTS, "too many downloads in progress").into_response();
    };
    // ServeFile gives Content-Length, Last-Modified and Range (resume over the tunnel).
    let Ok(response) = ServeFile::new(&file).oneshot(request).await;
    let (mut parts, body) = response.into_parts();
    let name: String = file
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "download".into())
        .chars()
        .map(|c| if c.is_ascii_graphic() && c != '"' && c != '\\' { c } else { '_' })
        .collect();
    if let Ok(value) = HeaderValue::from_str(&format!("attachment; filename=\"{name}\"")) {
        parts.headers.insert(header::CONTENT_DISPOSITION, value);
    }
    // The permit lives as long as the body stream does.
    let stream = Body::new(body).into_data_stream().map(move |chunk| {
        let _held = &permit;
        chunk
    });
    Response::from_parts(parts, Body::from_stream(stream))
}
