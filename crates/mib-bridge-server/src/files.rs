//! Read-only file listing and download (#651 G3): `GET /files` and `GET /files/download`.
//!
//! One root, the data dir. Every request path is canonicalised and must stay under the
//! canonical root, so `..`, absolute paths elsewhere and symlinks that leave the root are all
//! refused (404, so existence outside the root is not revealed). Names starting with `.` are
//! neither listed nor served. The canonical path is then **opened under the root descriptor**
//! (`openat2` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`, `O_NONBLOCK`), `fstat` must say
//! regular file, and that same descriptor is streamed with this module's own Range handling:
//! swapping a path component for a symlink or a FIFO after the check cannot redirect or block it.
//! Files an experiment is still writing are `in_progress` and refused (409). Same token rule as
//! `/ws`; a browser request from another origin is refused (`Origin` must match `Host`, or be
//! absent, or be allow-listed); `Sec-Fetch-Site`, when sent, must be same-origin or none.

use std::fs::File;
use std::io::{Error, ErrorKind, Read, Seek, SeekFrom};
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;

use axum::body::Body;
use axum::extract::{Query, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use serde::Deserialize;
use serde_json::{json, Value};

use super::{AuthQuery, Server};

/// At most this many downloads at once (a 2-core A9 on a RAM root, next to a 5 kHz run).
pub(crate) const MAX_DOWNLOADS: usize = 2;
/// Default and maximum entries in one listing page, and the most directory entries scanned.
pub(crate) const PAGE_ENTRIES: usize = 2000;
const SCAN_ENTRIES: usize = 20_000;
/// A Range header longer than this is refused before it is parsed.
const MAX_RANGE_HEADER: usize = 64;
const CHUNK: usize = 64 * 1024;

#[derive(Deserialize)]
pub(crate) struct FilesQuery {
    path: Option<String>,
    token: Option<String>,
    offset: Option<usize>,
    limit: Option<usize>,
}

fn not_found() -> Response {
    (StatusCode::NOT_FOUND, "not found").into_response()
}

/// `Origin`, when a browser sends it, must be this server's own origin (its `Host`) or allow-listed
/// (`--allow-origin`, for a UI served from elsewhere in development). An absent `Origin` (a typed URL,
/// curl, a top-level navigation) is accepted; the opaque origin `null` is refused. Browsers always send
/// `Origin` on a WebSocket handshake, so this keeps another site's page from driving the instrument
/// through the visitor's browser even with `--no-token`.
pub(crate) fn origin_allowed(server: &Server, headers: &HeaderMap) -> bool {
    let Some(origin) = headers.get(header::ORIGIN).and_then(|v| v.to_str().ok()) else {
        return !headers.contains_key(header::ORIGIN);
    };
    if server.config().allowed_origins.iter().any(|o| o.eq_ignore_ascii_case(origin)) {
        return true;
    }
    let Some((_scheme, authority)) = origin.split_once("://") else { return false };
    let host = headers.get(header::HOST).and_then(|v| v.to_str().ok()).unwrap_or("");
    !authority.is_empty() && authority.eq_ignore_ascii_case(host)
}

/// The token rule of `/ws`, then the browser rules (Origin, then Sec-Fetch-Site when sent).
pub(crate) fn gate(server: &Server, token: &Option<String>, headers: &HeaderMap) -> Option<Response> {
    if !server.authorized(&AuthQuery { token: token.clone() }, headers) {
        return Some((StatusCode::UNAUTHORIZED, "token required").into_response());
    }
    if !origin_allowed(server, headers) {
        return Some((StatusCode::FORBIDDEN, "requests from another origin are refused").into_response());
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

/// Open `canon` (already canonical and under `root`) beneath the root descriptor without following any
/// symlink and without blocking on a FIFO or device; the caller checks the type with `fstat`.
#[cfg(target_os = "linux")]
pub(crate) fn open_beneath(root: &Path, canon: &Path, directory: bool) -> std::io::Result<File> {
    use std::ffi::CString;
    use std::os::fd::{AsRawFd, FromRawFd};
    use std::os::unix::ffi::OsStrExt;
    use std::os::unix::fs::OpenOptionsExt;

    #[repr(C)]
    struct OpenHow {
        flags: u64,
        mode: u64,
        resolve: u64,
    }
    const RESOLVE_NO_MAGICLINKS: u64 = 0x02;
    const RESOLVE_NO_SYMLINKS: u64 = 0x04;
    const RESOLVE_BENEATH: u64 = 0x08;

    let rel = canon.strip_prefix(root).map_err(|_| Error::from(ErrorKind::PermissionDenied))?;
    let mut flags = libc::O_RDONLY | libc::O_NONBLOCK | libc::O_CLOEXEC | libc::O_NOCTTY;
    if directory {
        flags |= libc::O_DIRECTORY;
    }
    let root_dir = File::open(root)?;
    let rel_c = CString::new(if rel.as_os_str().is_empty() { b".".to_vec() } else { rel.as_os_str().as_bytes().to_vec() })
        .map_err(|_| Error::from(ErrorKind::InvalidInput))?;
    let how = OpenHow { flags: flags as u64, mode: 0, resolve: RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS };
    // SAFETY: plain syscall with a valid directory fd, a NUL-terminated path and a correctly sized open_how.
    let fd = unsafe { libc::syscall(libc::SYS_openat2, root_dir.as_raw_fd(), rel_c.as_ptr(), &how as *const OpenHow, std::mem::size_of::<OpenHow>()) };
    if fd >= 0 {
        // SAFETY: the kernel returned a new descriptor we own.
        return Ok(unsafe { File::from_raw_fd(fd as i32) });
    }
    let err = Error::last_os_error();
    if err.raw_os_error() == Some(libc::ENOSYS) {
        // A kernel before 5.6: at least never follow a symlink in the final component, and never block.
        return std::fs::OpenOptions::new()
            .read(true)
            .custom_flags(flags & !libc::O_RDONLY | libc::O_NOFOLLOW)
            .open(canon);
    }
    Err(err)
}

#[cfg(not(target_os = "linux"))]
pub(crate) fn open_beneath(_root: &Path, canon: &Path, _directory: bool) -> std::io::Result<File> {
    File::open(canon)
}

/// Where the writer puts a run's output: the coordinator stores the path as given (it appends `.h5`
/// itself) and the HDF5 writer opens a relative one against the process working directory, which is not
/// necessarily the data dir. `cwd` is passed so the rule can be tested.
pub(crate) fn output_file_path(output: &str, cwd: &Path) -> PathBuf {
    let path = Path::new(output);
    let absolute = if path.is_absolute() { path.to_path_buf() } else { cwd.join(path) };
    std::fs::canonicalize(&absolute).unwrap_or(absolute)
}

/// The output file of a run that is not finished (experiment Starting/Active/Stopping), if any.
fn in_progress_path(server: &Server) -> Option<PathBuf> {
    let status = mib_app_commands::fetch_experiment_status(&server.state).ok()?;
    let status = serde_json::to_value(status).ok()?;
    if status["valid"] != json!(true) || status["terminal"] == json!(true) || status["state"].as_u64().unwrap_or(0) == 0 {
        return None;
    }
    let output = status["output_path"].as_str().filter(|p| !p.is_empty())?;
    Some(output_file_path(output, &std::env::current_dir().ok()?))
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
    // One scan at a time per permit: a big directory cannot be listed over and over in parallel.
    let Ok(permit) = server.listings.clone().try_acquire_owned() else {
        return (StatusCode::TOO_MANY_REQUESTS, "too many listings in progress").into_response();
    };
    let offset = query.offset.unwrap_or(0);
    let limit = query.limit.unwrap_or(PAGE_ENTRIES).clamp(1, PAGE_ENTRIES);
    let listed = tokio::task::spawn_blocking(move || -> Option<Value> {
        let _permit = permit; // held until the scan is done, even when the request was cancelled
        let root = root_of(&server)?;
        let dir = resolve(&root, query.path.as_deref())?;
        // The directory is opened beneath the root and read through that descriptor.
        let handle = open_beneath(&root, &dir, true).ok()?;
        if !handle.metadata().ok()?.is_dir() {
            return None;
        }
        let busy = in_progress_path(&server);
        #[cfg(target_os = "linux")]
        let reader = {
            use std::os::fd::AsRawFd;
            std::fs::read_dir(format!("/proc/self/fd/{}", handle.as_raw_fd())).ok()?
        };
        #[cfg(not(target_os = "linux"))]
        let reader = std::fs::read_dir(&dir).ok()?;
        let mut entries = Vec::new();
        let mut scanned = 0usize;
        let mut scan_truncated = false;
        for entry in reader.flatten() {
            scanned += 1;
            if scanned > SCAN_ENTRIES {
                scan_truncated = true;
                break;
            }
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') {
                continue;
            }
            let Ok(file_type) = entry.file_type() else { continue };
            // Regular files and directories are described from the entry itself; a symlink only when
            // its target stays inside the root (resolved, informational: downloads re-check on open).
            let (meta, target) = if file_type.is_symlink() {
                let Some(target) = resolve(&root, Some(&dir.join(&name).to_string_lossy())) else { continue };
                let Ok(meta) = std::fs::metadata(&target) else { continue };
                (meta, target)
            } else if file_type.is_file() || file_type.is_dir() {
                let Ok(meta) = entry.metadata() else { continue };
                (meta, dir.join(&name))
            } else {
                continue; // FIFOs, sockets, devices
            };
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
        let total = entries.len();
        let page: Vec<Value> = entries.into_iter().skip(offset).take(limit).collect();
        let parent = if dir == root { None } else { dir.parent().map(|p| p.to_string_lossy().into_owned()) };
        Some(json!({
            "root": root.to_string_lossy(),
            "path": dir.to_string_lossy(),
            "parent": parent,
            "entries": page,
            "total": total,
            "offset": offset,
            "limit": limit,
            "truncated": offset + limit < total || scan_truncated,
            "scan_truncated": scan_truncated,
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

/// One byte range of a file of `len` bytes.
#[derive(Debug, PartialEq)]
pub(crate) enum RangeRequest {
    Whole,
    Part(u64, u64), // inclusive start and end
    Unsatisfiable,
}

/// A single `bytes=a-b`, `bytes=a-` or `bytes=-n`. A header with a comma (several ranges) or longer than
/// 64 bytes is refused outright, before any parsing: multi-range parsers can be made quadratic. Anything
/// else that is not a valid single range is ignored (the whole file), as the RFC allows.
pub(crate) fn parse_range(value: Option<&str>, len: u64) -> RangeRequest {
    let Some(value) = value else { return RangeRequest::Whole };
    if value.len() > MAX_RANGE_HEADER || value.contains(',') {
        return RangeRequest::Unsatisfiable;
    }
    let Some(spec) = value.strip_prefix("bytes=") else { return RangeRequest::Whole };
    let Some((first, last)) = spec.split_once('-') else { return RangeRequest::Whole };
    let number = |s: &str| -> Option<u64> { if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) { None } else { s.parse().ok() } };
    match (number(first), number(last)) {
        (Some(a), Some(b)) if a <= b => {
            if a >= len { RangeRequest::Unsatisfiable } else { RangeRequest::Part(a, b.min(len - 1)) }
        }
        (Some(a), None) if last.is_empty() => {
            if a >= len { RangeRequest::Unsatisfiable } else { RangeRequest::Part(a, len - 1) }
        }
        (None, Some(n)) if first.is_empty() && n > 0 => {
            if len == 0 { RangeRequest::Unsatisfiable } else { RangeRequest::Part(len.saturating_sub(n), len - 1) }
        }
        _ => RangeRequest::Whole,
    }
}

pub(crate) async fn download(State(server): State<Arc<Server>>, Query(query): Query<FilesQuery>, headers: HeaderMap) -> Response {
    if let Some(refused) = gate(&server, &query.token, &headers) {
        return refused;
    }
    // The Range header is judged before any file work, so a hostile header costs nothing.
    let range_header = headers.get(header::RANGE).and_then(|v| v.to_str().ok()).map(str::to_string);
    if range_header.as_deref().is_some_and(|r| r.len() > MAX_RANGE_HEADER || r.contains(',')) {
        return (StatusCode::RANGE_NOT_SATISFIABLE, "a single short byte range only").into_response();
    }
    let opened = {
        let server = server.clone();
        tokio::task::spawn_blocking(move || -> Result<(File, String, u64, u64), Response> {
            let root = root_of(&server).ok_or_else(not_found)?;
            let canon = resolve(&root, query.path.as_deref()).ok_or_else(not_found)?;
            let file = open_beneath(&root, &canon, false).map_err(|_| not_found())?;
            let meta = file.metadata().map_err(|_| not_found())?;
            if !meta.is_file() {
                return Err(not_found()); // a FIFO, device or directory: refused, never read
            }
            if in_progress_path(&server).as_deref() == Some(canon.as_path()) {
                return Err((StatusCode::CONFLICT, "the file is still being written").into_response());
            }
            let name: String = canon
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| "download".into())
                .chars()
                .map(|c| if c.is_ascii_graphic() && c != '"' && c != '\\' { c } else { '_' })
                .collect();
            Ok((file, name, meta.len(), mtime_ns(&meta)))
        })
        .await
    };
    let (file, name, len, _mtime) = match opened {
        Ok(Ok(v)) => v,
        Ok(Err(refused)) => return refused,
        Err(_) => return not_found(),
    };
    let (status, start, end) = match parse_range(range_header.as_deref(), len) {
        RangeRequest::Whole => (StatusCode::OK, 0, len.saturating_sub(1)),
        RangeRequest::Part(a, b) => (StatusCode::PARTIAL_CONTENT, a, b),
        RangeRequest::Unsatisfiable => {
            return (StatusCode::RANGE_NOT_SATISFIABLE, [(header::CONTENT_RANGE, format!("bytes */{len}"))], "range not satisfiable")
                .into_response();
        }
    };
    let Ok(permit) = server.downloads.clone().try_acquire_owned() else {
        return (StatusCode::TOO_MANY_REQUESTS, "too many downloads in progress").into_response();
    };
    let to_send = if len == 0 { 0 } else { end - start + 1 };
    // Stream the descriptor that was checked, in chunks read on the blocking pool; the permit lives as long as the stream.
    let stream = futures_util::stream::unfold(Some((file, start, to_send, permit)), |state| async move {
        let (file, pos, left, permit) = state?;
        if left == 0 {
            return None;
        }
        let want = left.min(CHUNK as u64) as usize;
        let read = tokio::task::spawn_blocking(move || -> std::io::Result<(File, Vec<u8>)> {
            let mut file = file;
            file.seek(SeekFrom::Start(pos))?;
            let mut buf = vec![0u8; want];
            let mut got = 0;
            while got < want {
                let n = file.read(&mut buf[got..])?;
                if n == 0 {
                    break;
                }
                got += n;
            }
            buf.truncate(got);
            Ok((file, buf))
        })
        .await;
        match read {
            Ok(Ok((file, buf))) if !buf.is_empty() => {
                let n = buf.len() as u64;
                Some((Ok::<Vec<u8>, Error>(buf), Some((file, pos + n, left - n, permit))))
            }
            Ok(Ok(_)) => None, // the file got shorter than it was: end the body
            Ok(Err(e)) => Some((Err(e), None)),
            Err(_) => None,
        }
    });
    let mut response = Response::new(Body::from_stream(stream));
    *response.status_mut() = status;
    let h = response.headers_mut();
    h.insert(header::CONTENT_TYPE, HeaderValue::from_static("application/octet-stream"));
    h.insert(header::ACCEPT_RANGES, HeaderValue::from_static("bytes"));
    h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    if let Ok(v) = HeaderValue::from_str(&to_send.to_string()) {
        h.insert(header::CONTENT_LENGTH, v);
    }
    if status == StatusCode::PARTIAL_CONTENT {
        if let Ok(v) = HeaderValue::from_str(&format!("bytes {start}-{end}/{len}")) {
            h.insert(header::CONTENT_RANGE, v);
        }
    }
    if let Ok(v) = HeaderValue::from_str(&format!("attachment; filename=\"{name}\"")) {
        h.insert(header::CONTENT_DISPOSITION, v);
    }
    response
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn range_rules() {
        assert_eq!(parse_range(None, 10), RangeRequest::Whole);
        assert_eq!(parse_range(Some("bytes=3-6"), 10), RangeRequest::Part(3, 6));
        assert_eq!(parse_range(Some("bytes=3-"), 10), RangeRequest::Part(3, 9));
        assert_eq!(parse_range(Some("bytes=-4"), 10), RangeRequest::Part(6, 9));
        assert_eq!(parse_range(Some("bytes=3-99"), 10), RangeRequest::Part(3, 9));
        assert_eq!(parse_range(Some("bytes=10-12"), 10), RangeRequest::Unsatisfiable);
        assert_eq!(parse_range(Some("bytes=0-0,2-2"), 10), RangeRequest::Unsatisfiable);
        assert_eq!(parse_range(Some(&format!("bytes={}", "0-0,".repeat(5000))), 10), RangeRequest::Unsatisfiable);
        assert_eq!(parse_range(Some("bytes=5-3"), 10), RangeRequest::Whole);
        assert_eq!(parse_range(Some("items=1-2"), 10), RangeRequest::Whole);
        assert_eq!(parse_range(Some("bytes=a-b"), 10), RangeRequest::Whole);
        assert_eq!(parse_range(Some("bytes=0-0"), 0), RangeRequest::Unsatisfiable);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_component_swapped_for_a_symlink_after_the_check_is_not_followed() {
        let base = std::env::temp_dir().join(format!("yofo_beneath_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let root = base.join("root");
        let outside = base.join("outside");
        std::fs::create_dir_all(root.join("sub")).unwrap();
        std::fs::create_dir_all(&outside).unwrap();
        std::fs::write(root.join("sub").join("f.txt"), b"inside").unwrap();
        std::fs::write(outside.join("f.txt"), b"outside").unwrap();
        let root = std::fs::canonicalize(&root).unwrap();
        let canon = resolve(&root, Some("sub/f.txt")).unwrap(); // checked while it is a plain file
        let mut inside = String::new();
        open_beneath(&root, &canon, false).unwrap().read_to_string(&mut inside).unwrap();
        assert_eq!(inside, "inside");
        // after the check: the directory is replaced by a symlink to somewhere else
        std::fs::remove_dir_all(root.join("sub")).unwrap();
        std::os::unix::fs::symlink(&outside, root.join("sub")).unwrap();
        assert!(open_beneath(&root, &canon, false).is_err(), "the swapped component is refused, not followed");
        // a swapped final component too
        std::fs::remove_file(root.join("sub")).unwrap();
        std::fs::create_dir_all(root.join("sub")).unwrap();
        std::os::unix::fs::symlink(outside.join("f.txt"), root.join("sub").join("f.txt")).unwrap();
        assert!(open_beneath(&root, &canon, false).is_err(), "a swapped final component is refused");
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn output_paths_resolve_against_the_working_directory() {
        let cwd = Path::new("/var/lib/yofo-studio");
        assert_eq!(output_file_path("/abs/run.h5", cwd), PathBuf::from("/abs/run.h5"));
        assert_eq!(output_file_path("run.h5", cwd), PathBuf::from("/var/lib/yofo-studio/run.h5"));
        // data/run.h5 with the data dir "data" is cwd/data/run.h5, not cwd/data/data/run.h5
        assert_eq!(output_file_path("data/run.h5", cwd), PathBuf::from("/var/lib/yofo-studio/data/run.h5"));
    }
}
