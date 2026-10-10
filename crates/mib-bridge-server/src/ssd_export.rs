//! `GET /ssd/runs/{id}/records` (#667): the raw records of one closed SSD run, exactly as on the disk, idle only.
//!
//! The body is the stdout of `pzrec read RUN IMG ARGS --summary [--from F] [--count N]` (the board owner's reader, same IMG and options as start/stop), piped to the
//! response: `records x 59,392` bytes, whole records, in order, with `Content-Length` known up front. Headers `X-Run-Id`, `X-Record-Count`, `X-Record-Bytes` and
//! `X-First-Record` let the PC side check what it got. Same gate as `/files/download` (token, Origin, Sec-Fetch-Site), same listener, nothing new exposed.
//!
//! One lease covers the whole download (`ssd_export_begin` .. `ssd_export_end`): taken in C++ before the reader is spawned (the window must show IDLE, nothing recording,
//! no Start in progress), released only after the reader has exited. While it is held Studio refuses to start a run and does not poll pzrec. A completed download is
//! verified, not assumed: pzrec's own byte count (`--summary` on stderr), the bytes sent and `records x 59,392` must agree and the exit code must be 0, otherwise the
//! response ends with an error (the body is shorter than its `Content-Length`, so the client sees an aborted transfer, never a 200 that looks complete) and the route
//! logs why. A client that goes away, or a reader that goes quiet, gets SIGTERM, then SIGKILL; pzrec stops between two disk commands, so on the bounce path the
//! lease is held up to one block command (about 25 s) after a cancel (a pzblk-backed IMG could wait up to the driver's 30 min admit timeout: this bundle uses the
//! bounce path). The route takes no recovery action: what the reader's open does (it may close an interrupted run) is pzrec's.

use std::process::Stdio;
use std::sync::Arc;
use std::time::{Duration, Instant};

use axum::body::{Body, Bytes};
use axum::extract::{Path, Query, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::Response;
use mib_app_commands::AppState;
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::io::AsyncReadExt;
use tokio::process::{Child, ChildStdout, Command};

use super::{files, Server};

const RECORD_BYTES: u64 = 59_392;
const CHUNK: usize = 256 * 1024;
/// No byte from a running reader for this long: it is killed. The open phase and one block command are each bounded by about 25 s.
const IDLE_TIMEOUT_MS: u64 = 90_000;
/// After SIGTERM a reader can still be inside one block command (about 25 s on the bounce path).
const TERM_GRACE_MS: u64 = 35_000;
const KILL_WAIT: Duration = Duration::from_secs(10);
/// After stdout's EOF the reader must exit this soon.
const EXIT_WAIT: Duration = Duration::from_secs(15);
const STDERR_CAP: usize = 64 * 1024;

/// A time bound in ms, shortened only by the tests (`MIB_SSD_EXPORT_IDLE_MS`, `MIB_SSD_EXPORT_TERM_GRACE_MS`).
fn bound(env: &str, default_ms: u64) -> Duration {
    Duration::from_millis(std::env::var(env).ok().and_then(|v| v.parse().ok()).unwrap_or(default_ms))
}

#[derive(Deserialize)]
pub(crate) struct ExportQuery {
    token: Option<String>,
    from: Option<u64>,
    count: Option<u64>,
}

fn error_response(status: StatusCode, body: Value) -> Response {
    let mut response = Response::new(Body::from(body.to_string()));
    *response.status_mut() = status;
    response.headers_mut().insert(header::CONTENT_TYPE, HeaderValue::from_static("application/json"));
    response.headers_mut().insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    response
}

/// What pzrec said on stderr: its error object, the interruption, the clean-finish summary (JSON objects, one per line).
#[derive(Default, Debug, Clone, PartialEq)]
pub(crate) struct ReaderReport {
    pub error: Option<String>,
    pub name: Option<String>,
    pub code: Option<i64>,
    pub summary_bytes: Option<u64>,
    pub reported_bytes: Option<u64>,
    pub interrupted: Option<String>,
    pub raw: String,
}

pub(crate) fn parse_stderr(text: &str) -> ReaderReport {
    let mut report = ReaderReport { raw: text.trim().chars().take(600).collect(), ..Default::default() };
    for line in text.lines() {
        let Ok(Value::Object(obj)) = serde_json::from_str::<Value>(line.trim()) else { continue };
        if let Some(e) = obj.get("error").and_then(Value::as_str) {
            report.error = Some(e.to_string());
            report.name = obj.get("name").and_then(Value::as_str).map(str::to_string);
            report.code = obj.get("code").and_then(Value::as_i64);
        }
        if let Some(i) = obj.get("interrupted").and_then(Value::as_str) {
            report.interrupted = Some(i.to_string());
        }
        if let Some(b) = obj.get("bytes_written").and_then(Value::as_u64) {
            report.reported_bytes = Some(b);
        }
        // the clean finish: {"run_id", "records", "bytes", "retries", "seconds", "MBps"}
        if obj.contains_key("records") && obj.contains_key("seconds") {
            report.summary_bytes = obj.get("bytes").and_then(Value::as_u64);
        }
    }
    report
}

/// HTTP status of a reader that ended before any byte, from pzrec's error name.
fn status_for_reader(report: &ReaderReport) -> StatusCode {
    match report.name.as_deref() {
        Some("ACTIVE") | Some("HW") | Some("BAD_STATE") | Some("WEDGED") => StatusCode::SERVICE_UNAVAILABLE,
        Some("NO_SUCH_RUN") => StatusCode::NOT_FOUND,
        Some("RUN_DELETED") => StatusCode::GONE,
        Some("ARGS") => StatusCode::BAD_REQUEST,
        _ => StatusCode::BAD_GATEWAY,
    }
}

/// The verdict on a finished reader: Ok only when pzrec exited 0, its own byte count, the bytes sent and `records x 59,392` agree.
pub(crate) fn verdict(exit_code: Option<i32>, report: &ReaderReport, sent: u64, expected: u64) -> Result<(), String> {
    if exit_code != Some(0) {
        let what = report
            .name
            .clone()
            .or_else(|| report.interrupted.as_ref().map(|i| format!("interrupted ({i})")))
            .unwrap_or_else(|| "failed".to_string());
        return Err(format!("pzrec read ended with exit {:?} ({what}) after {sent} of {expected} bytes: {}", exit_code, report.raw));
    }
    if sent != expected {
        return Err(format!("the stream is {sent} bytes, the run holds {expected}"));
    }
    match report.summary_bytes {
        Some(b) if b == sent => Ok(()),
        Some(b) => Err(format!("pzrec reports {b} bytes written, {sent} were sent")),
        None => Err("pzrec gave no summary of a clean finish: the download is not verified".to_string()),
    }
}

/// The reader process and the lease it holds. Dropped without `finish()` (a client that went away) it is terminated and the lease released in a task.
struct Reader {
    child: Child,
    stdout: ChildStdout,
    stderr: Option<tokio::task::JoinHandle<Vec<u8>>>,
    lease: u64,
    state: Arc<AppState>,
    run: u32,
    expected: u64,
    sent: u64,
    deadline: Instant,
    _permit: tokio::sync::OwnedSemaphorePermit,
}

impl Reader {
    async fn stderr_text(&mut self) -> String {
        match self.stderr.take() {
            Some(h) => String::from_utf8_lossy(&tokio::time::timeout(Duration::from_secs(5), h).await.ok().and_then(Result::ok).unwrap_or_default()).into_owned(),
            None => String::new(),
        }
    }

    async fn release(&self, outcome: String) {
        let (state, lease, sent) = (self.state.clone(), self.lease, self.sent);
        let run = self.run;
        let r = tokio::task::spawn_blocking(move || mib_app_commands::ssd_export_end(&state, lease, sent, &outcome)).await;
        if !matches!(r, Ok(Ok(()))) {
            eprintln!("ssd export: lease {lease} of run {run} could not be released: {r:?}");
        }
    }

    /// SIGTERM, then SIGKILL; the lease is released after the process has exited.
    async fn terminate(mut self, why: &str) -> ReaderReport {
        if let Some(pid) = self.child.id() {
            // pzrec stops between two disk commands on SIGTERM (exit 3)
            unsafe { libc::kill(pid as i32, libc::SIGTERM) };
        }
        let waited = tokio::time::timeout(bound("MIB_SSD_EXPORT_TERM_GRACE_MS", TERM_GRACE_MS), self.child.wait()).await;
        if waited.is_err() {
            let _ = self.child.start_kill();
            let _ = tokio::time::timeout(KILL_WAIT, self.child.wait()).await;
        }
        let report = parse_stderr(&self.stderr_text().await);
        eprintln!(
            "ssd export: run {} reader terminated ({why}) after {} of {} bytes; pzrec said: {}",
            self.run, self.sent, self.expected, report.raw
        );
        self.release(format!("terminated: {why}")).await;
        report
    }

    /// stdout has ended: wait for the exit, read pzrec's last words, judge, release.
    async fn finish(mut self) -> Result<(), String> {
        let status = match tokio::time::timeout(EXIT_WAIT, self.child.wait()).await {
            Ok(Ok(s)) => Some(s),
            _ => {
                let _ = self.child.start_kill();
                let _ = tokio::time::timeout(KILL_WAIT, self.child.wait()).await;
                None
            }
        };
        let code = status.and_then(|s| s.code());
        let report = parse_stderr(&self.stderr_text().await);
        let result = verdict(code, &report, self.sent, self.expected);
        match &result {
            Ok(()) => eprintln!("ssd export: run {} sent {} bytes, verified against pzrec's summary", self.run, self.sent),
            Err(e) => eprintln!("ssd export: run {} FAILED: {e}", self.run),
        }
        self.release(match &result {
            Ok(()) => "complete".to_string(),
            Err(e) => format!("failed: {e}"),
        })
        .await;
        result
    }

    /// The next piece of stdout, bounded by the idle timeout and the lease bound. `Ok(None)` is the end of the stream.
    async fn read_chunk(&mut self) -> Result<Option<Bytes>, String> {
        let left = self.deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return Err("the download exceeded its time bound".to_string());
        }
        let mut buf = vec![0u8; CHUNK];
        let idle = bound("MIB_SSD_EXPORT_IDLE_MS", IDLE_TIMEOUT_MS);
        match tokio::time::timeout(idle.min(left), self.stdout.read(&mut buf)).await {
            Err(_) => Err(if left < idle { "the download exceeded its time bound".to_string() } else { "the reader went quiet".to_string() }),
            Ok(Err(e)) => Err(format!("reading the reader's output failed: {e}")),
            Ok(Ok(0)) => Ok(None),
            Ok(Ok(n)) => {
                buf.truncate(n);
                Ok(Some(Bytes::from(buf)))
            }
        }
    }
}

/// The stream's owner: dropped while the reader is still live (the client went away) it terminates the reader and releases the lease in a task.
struct Guard(Option<Reader>);

impl Drop for Guard {
    fn drop(&mut self) {
        if let Some(reader) = self.0.take() {
            if let Ok(handle) = tokio::runtime::Handle::try_current() {
                handle.spawn(async move {
                    reader.terminate("client disconnected").await;
                });
            }
        }
    }
}

struct Flow {
    guard: Guard,
    pending: Option<Bytes>,
    held: Option<Bytes>,
}

/// Split the last byte of the body off the chunk that reaches `expected`: `(what can go out now, the held byte)`.
fn withhold(mut chunk: Bytes, sent: u64, expected: u64) -> (Bytes, Option<Bytes>) {
    if sent == expected && !chunk.is_empty() {
        let last = chunk.split_off(chunk.len() - 1);
        (chunk, Some(last))
    } else {
        (chunk, None)
    }
}

fn io_error(message: String) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::Other, message)
}

pub(crate) async fn records(
    State(server): State<Arc<Server>>,
    Path(id): Path<u32>,
    Query(query): Query<ExportQuery>,
    headers: HeaderMap,
) -> Response {
    if let Some(refused) = files::gate(&server, &query.token, &headers) {
        return refused;
    }
    // One download at a time (a 2-core A9 next to a 5 kHz run; the SSD has one bounce buffer). A second request is refused at once.
    let Ok(permit) = server.ssd_export.clone().try_acquire_owned() else {
        return error_response(StatusCode::CONFLICT, json!({"error": "another export is in progress", "code": "BUSY"}));
    };
    let state = server.state().clone();
    let (from, count) = (query.from.unwrap_or(0), query.count.unwrap_or(0));
    let begin = {
        let state = state.clone();
        tokio::task::spawn_blocking(move || mib_app_commands::ssd_export_begin(&state, id, from, count)).await
    };
    let begin = match begin {
        Ok(Ok(v)) => v,
        Ok(Err(e)) => return error_response(StatusCode::INTERNAL_SERVER_ERROR, json!({"error": format!("the export could not be started: {e}")})),
        Err(_) => return error_response(StatusCode::INTERNAL_SERVER_ERROR, json!({"error": "the export could not be started"})),
    };
    if !begin.get("ok").and_then(Value::as_bool).unwrap_or(false) {
        let reason = begin.get("reason").and_then(Value::as_str).unwrap_or("refused").to_string();
        let code = begin.get("code").and_then(Value::as_str).unwrap_or("UNAVAILABLE").to_string();
        let status = match code.as_str() {
            "NO_SUCH_RUN" => StatusCode::NOT_FOUND,
            "RUN_DELETED" => StatusCode::GONE,
            "NOT_OFFERED" => StatusCode::CONFLICT,
            "BAD_RANGE" => StatusCode::RANGE_NOT_SATISFIABLE,
            _ => StatusCode::SERVICE_UNAVAILABLE, // BUSY (a recording, STOPPING, a Start, a drain not IDLE), UNAVAILABLE
        };
        eprintln!("ssd export: run {id} refused ({code}): {reason}");
        return error_response(status, json!({"error": reason, "code": code}));
    }
    let lease = begin.get("lease").and_then(Value::as_u64).unwrap_or(0);
    let records = begin.get("records").and_then(Value::as_u64).unwrap_or(0);
    let bytes = begin.get("bytes").and_then(Value::as_u64).unwrap_or(0);
    let max_seconds = begin.get("max_seconds").and_then(Value::as_u64).unwrap_or(120);
    let argv: Vec<String> = begin.get("argv").and_then(Value::as_array).map(|a| a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect()).unwrap_or_default();
    // Release helper for failures before a Reader exists.
    let release_now = |outcome: String| {
        let state = state.clone();
        async move {
            let _ = tokio::task::spawn_blocking(move || mib_app_commands::ssd_export_end(&state, lease, 0, &outcome)).await;
        }
    };
    if lease == 0 || argv.len() < 2 || bytes != records.saturating_mul(RECORD_BYTES) || records == 0 {
        release_now("bad begin answer".to_string()).await;
        return error_response(StatusCode::INTERNAL_SERVER_ERROR, json!({"error": "the SSD store gave an unusable export plan"}));
    }
    let mut command = Command::new(&argv[0]);
    command.args(&argv[1..]).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped()).kill_on_drop(true);
    let mut child = match command.spawn() {
        Ok(c) => c,
        Err(e) => {
            release_now(format!("spawn failed: {e}")).await;
            return error_response(StatusCode::BAD_GATEWAY, json!({"error": format!("pzrec could not be started: {e}")}));
        }
    };
    let (Some(stdout), Some(mut stderr)) = (child.stdout.take(), child.stderr.take()) else {
        let _ = child.start_kill();
        let _ = child.wait().await;
        release_now("no pipes".to_string()).await;
        return error_response(StatusCode::BAD_GATEWAY, json!({"error": "pzrec's pipes could not be opened"}));
    };
    let stderr_task = tokio::spawn(async move {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        loop {
            match stderr.read(&mut chunk).await {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    if buf.len() < STDERR_CAP {
                        buf.extend_from_slice(&chunk[..n.min(STDERR_CAP - buf.len())]);
                    }
                }
            }
        }
        buf
    });
    let mut reader = Reader {
        child,
        stdout,
        stderr: Some(stderr_task),
        lease,
        state: state.clone(),
        run: id,
        expected: bytes,
        sent: 0,
        deadline: Instant::now() + Duration::from_secs(max_seconds),
        _permit: permit,
    };
    // The first chunk before any status: a reader that is refused (a run became active, no such run, an IO error in the open) ends before one byte, and
    // then the response can carry pzrec's own error with the right status instead of a 200 and a broken body.
    let first = reader.read_chunk().await;
    let first = match first {
        Ok(Some(chunk)) => chunk,
        Ok(None) => {
            // EOF before a byte: pzrec's last words say why
            let status = reader.child.wait().await.ok().and_then(|s| s.code());
            let report = parse_stderr(&reader.stderr_text().await);
            let http = if status == Some(0) { StatusCode::BAD_GATEWAY } else { status_for_reader(&report) };
            let message = report.error.clone().unwrap_or_else(|| "pzrec read produced no data".to_string());
            eprintln!("ssd export: run {id} reader ended before any data (exit {status:?}): {}", report.raw);
            reader.release(format!("no data: {message}")).await;
            return error_response(
                http,
                json!({"error": message, "pzrec": {"name": report.name, "code": report.code, "exit_code": status, "stderr": report.raw}, "bytes_sent": 0}),
            );
        }
        Err(why) => {
            let report = reader.terminate(&why).await;
            return error_response(
                StatusCode::GATEWAY_TIMEOUT,
                json!({"error": why, "pzrec": {"name": report.name, "code": report.code, "stderr": report.raw}, "bytes_sent": 0}),
            );
        }
    };
    reader.sent = first.len() as u64;
    if reader.sent > reader.expected {
        let report = reader.terminate("more data than the run holds").await;
        return error_response(StatusCode::BAD_GATEWAY, json!({"error": "the reader sent more than the run holds", "pzrec": {"stderr": report.raw}, "bytes_sent": 0}));
    }
    // The last byte of the body is held back until the reader has exited and the counts agree: a download whose end is bad (pzrec exits 1 after its last
    // record, its summary disagrees) is then short of its Content-Length, an aborted transfer, and cannot pass for a whole one.
    let (first, held) = withhold(first, reader.sent, reader.expected);
    let flow = Flow { guard: Guard(Some(reader)), pending: Some(first), held };
    let stream = futures_util::stream::unfold(Some(flow), |flow| async move {
        let mut flow = flow?;
        if let Some(chunk) = flow.pending.take() {
            return Some((Ok::<Bytes, std::io::Error>(chunk), Some(flow)));
        }
        loop {
            let reader = flow.guard.0.as_mut()?;
            match reader.read_chunk().await {
                Ok(Some(chunk)) => {
                    reader.sent += chunk.len() as u64;
                    if reader.sent > reader.expected {
                        let reader = flow.guard.0.take()?;
                        let report = reader.terminate("more data than the run holds").await;
                        return Some((Err(io_error(format!("the reader sent more than the run holds: {}", report.raw))), None));
                    }
                    let (send, held) = withhold(chunk, reader.sent, reader.expected);
                    if held.is_some() {
                        flow.held = held;
                    }
                    if !send.is_empty() {
                        return Some((Ok(send), Some(flow)));
                    }
                }
                Ok(None) => {
                    let reader = flow.guard.0.take()?;
                    return match reader.finish().await {
                        Ok(()) => flow.held.take().map(|last| (Ok(last), None)),
                        Err(e) => Some((Err(io_error(e)), None)),
                    };
                }
                Err(why) => {
                    let reader = flow.guard.0.take()?;
                    let report = reader.terminate(&why).await;
                    return Some((Err(io_error(format!("{why}: {}", report.raw))), None));
                }
            }
        }
    });
    let mut response = Response::new(Body::from_stream(stream));
    let h = response.headers_mut();
    h.insert(header::CONTENT_TYPE, HeaderValue::from_static("application/octet-stream"));
    h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    if let Ok(v) = HeaderValue::from_str(&bytes.to_string()) {
        h.insert(header::CONTENT_LENGTH, v);
    }
    for (name, value) in [("x-run-id", id.to_string()), ("x-record-count", records.to_string()), ("x-record-bytes", RECORD_BYTES.to_string()), ("x-first-record", from.to_string())] {
        if let Ok(v) = HeaderValue::from_str(&value) {
            h.insert(header::HeaderName::from_static(name), v);
        }
    }
    if let Ok(v) = HeaderValue::from_str(&format!("attachment; filename=\"run-{id}-records.bin\"")) {
        h.insert(header::CONTENT_DISPOSITION, v);
    }
    response
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_last_byte_of_the_body_is_held_back() {
        let (send, held) = withhold(Bytes::from_static(b"abcd"), 4, 4);
        assert_eq!((send.as_ref(), held.as_deref()), (&b"abc"[..], Some(&b"d"[..])));
        let (send, held) = withhold(Bytes::from_static(b"abcd"), 4, 10);
        assert_eq!((send.len(), held.is_none()), (4, true));
        let (send, held) = withhold(Bytes::from_static(b"d"), 10, 10);
        assert!(send.is_empty() && held.is_some());
    }

    #[test]
    fn stderr_of_a_clean_finish_gives_the_byte_count() {
        let r = parse_stderr("{\"run_id\":5,\"records\":8,\"bytes\":475136,\"retries\":0,\"seconds\":0.010,\"MBps\":47.5}\n");
        assert_eq!(r.summary_bytes, Some(475136));
        assert!(r.error.is_none());
    }

    #[test]
    fn stderr_errors_carry_name_and_code() {
        let r = parse_stderr("{\"error\":\"a run is active: download only when idle\",\"code\":10,\"name\":\"ACTIVE\"}\n");
        assert_eq!((r.name.as_deref(), r.code), (Some("ACTIVE"), Some(10)));
        assert_eq!(status_for_reader(&r), StatusCode::SERVICE_UNAVAILABLE);
        let io = parse_stderr("{\"error\":\"read failed after retries\",\"code\":1,\"name\":\"IO\"}\n{\"run_id\":5,\"bytes_written\":1000,\"records_written\":0,\"fail_lba\":9,\"retries\":3}\n");
        assert_eq!((io.name.as_deref(), io.reported_bytes), (Some("IO"), Some(1000)));
        assert_eq!(status_for_reader(&io), StatusCode::BAD_GATEWAY);
        assert_eq!(status_for_reader(&parse_stderr("{\"error\":\"x\",\"code\":6,\"name\":\"NO_SUCH_RUN\"}")), StatusCode::NOT_FOUND);
        assert_eq!(status_for_reader(&parse_stderr("{\"error\":\"x\",\"code\":12,\"name\":\"RUN_DELETED\"}")), StatusCode::GONE);
        assert_eq!(status_for_reader(&parse_stderr("garbage that is not json")), StatusCode::BAD_GATEWAY);
    }

    #[test]
    fn stderr_that_is_not_json_is_kept_but_not_trusted() {
        let r = parse_stderr("pzrec: the drain did not stop cleanly\n{\"interrupted\":\"signal\",\"run_id\":5,\"bytes_written\":4096,\"records_written\":0}\n");
        assert_eq!(r.interrupted.as_deref(), Some("signal"));
        assert_eq!(r.reported_bytes, Some(4096));
        assert!(r.summary_bytes.is_none());
    }

    #[test]
    fn a_download_is_complete_only_when_all_three_counts_agree_and_pzrec_exited_clean() {
        let ok = parse_stderr("{\"run_id\":5,\"records\":8,\"bytes\":475136,\"retries\":0,\"seconds\":0.1,\"MBps\":4.7}");
        assert!(verdict(Some(0), &ok, 475136, 475136).is_ok());
        assert!(verdict(Some(0), &ok, 475135, 475136).is_err(), "bytes sent short of records x 59,392");
        assert!(verdict(Some(0), &ok, 475136, 475136 + 59392).is_err(), "pzrec's count and the plan disagree");
        let other = parse_stderr("{\"run_id\":5,\"records\":8,\"bytes\":400000,\"retries\":0,\"seconds\":0.1,\"MBps\":4.7}");
        assert!(verdict(Some(0), &other, 475136, 475136).is_err(), "pzrec's count differs from the bytes sent");
        assert!(verdict(Some(0), &ReaderReport::default(), 475136, 475136).is_err(), "no summary: not verified");
        let io = parse_stderr("{\"error\":\"read failed\",\"code\":1,\"name\":\"IO\"}");
        let e = verdict(Some(1), &io, 1000, 475136).unwrap_err();
        assert!(e.contains("IO") && e.contains("1000 of 475136"), "{e}");
        let cut = parse_stderr("{\"interrupted\":\"signal\",\"run_id\":5,\"bytes_written\":4096,\"records_written\":0}");
        assert!(verdict(Some(3), &cut, 4096, 475136).unwrap_err().contains("interrupted"));
        assert!(verdict(None, &ok, 475136, 475136).is_err(), "a reader that had to be killed is not a clean finish");
    }
}
