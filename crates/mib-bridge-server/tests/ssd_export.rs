//! `GET /ssd/runs/{id}/records` (#667): the raw records of a closed SSD run, verified, idle only, one lease for the whole download.
//!
//! The reader is a fake `pzrec` (a shell script driven by a mode file) behind the real SsdStore, lease and route: streaming and verification,
//! refusals (a recording, an armed drain, no such run, a deleted run, a bad range), pzrec's error codes in the body, a failure inside a record, a client that
//! goes away (SIGTERM seen, lease released), a reader that ignores SIGTERM (SIGKILL), a second download, and the two orders of the exclusion.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use mib_app_commands::AppState;
use mib_bridge_server::{Server, ServerConfig};
use serde_json::Value;
use serial_test::serial;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

const TOKEN: &str = "export-token";
const REC: usize = 59_392;

const FAKE: &str = r#"#!/bin/sh
D="__DIR__"
verb=$1
echo "$@" >> "$D/calls"
case "$verb" in
snapshot) echo "{\"state\":\"$(cat $D/window)\",\"state_code\":0}"; exit 0;;
status) cat "$D/status.json"; exit 0;;
runs) cat "$D/runs.json"; exit 0;;
read) ;;
*) echo "unexpected verb $verb" >&2; exit 2;;
esac
echo $$ > "$D/pid"
COUNT=8; [ -f "$D/count" ] && COUNT=$(cat "$D/count"); prev=
for a in "$@"; do [ "$prev" = --count ] && COUNT=$a; prev=$a; done
N=$((COUNT*59392))
emit() { head -c $1 /dev/zero | tr '\0' 'A'; }
summary() { echo "{\"run_id\":5,\"records\":$1,\"bytes\":$2,\"retries\":0,\"seconds\":0.1,\"MBps\":4.7}" >&2; }
set -- $(cat "$D/mode")
case "$1" in
ok) emit $N; summary $COUNT $N; exit 0;;
short) emit $((N-59392)); summary $((COUNT-1)) $((N-59392)); exit 0;;
liar) emit $N; summary $COUNT 1; exit 0;;
nosummary) emit $N; exit 0;;
iofail) emit 100000; echo '{"error":"read failed after retries","code":1,"name":"IO"}' >&2; echo '{"run_id":5,"bytes_written":98304,"records_written":1,"fail_lba":900,"retries":3}' >&2; exit 1;;
failafter) emit $N; echo '{"error":"read failed after retries","code":1,"name":"IO"}' >&2; exit 1;;
err) echo "{\"error\":\"fake $2\",\"code\":$3,\"name\":\"$2\"}" >&2; exit 1;;
slow) trap 'echo "{\"interrupted\":\"signal\",\"run_id\":5,\"bytes_written\":100000,\"records_written\":1}" >&2; touch "$D/terminated"; exit 3' TERM; emit 100000; while :; do sleep 0.1; done;;
hang) trap '' TERM; emit 100000; while :; do sleep 0.1; done;;
quiet) trap 'touch "$D/terminated"; exit 3' TERM; while :; do sleep 0.1; done;;
steady) trap 'touch "$D/terminated"; exit 3' TERM; while :; do head -c 20000 /dev/zero | tr '\0' 'A'; sleep 0.1; done;;
flood) exec head -c $N /dev/zero;;
deaf) trap '' TERM; while :; do sleep 0.1; done;;
esac
"#;

fn run_row(id: u32, written: u64, open: bool, deleted: bool) -> String {
    format!(
        r#"{{"run_id":{id},"flags":0,"open":{},"deleted":{},"incomplete":0,"start_lba":2048,"end_lba":{},"start_unix_ms":1791530000000,"wall_source":1,"client_tag":1,"filter":0,"sampler_n":0,"rec_sectors":116,"record_sectors":116,"first_frame_id":1,"last_frame_id":{written},"first_ticks":0,"last_ticks":1,"tick_hz":100000000,"seen":{written},"empty_filtered":0,"invalid_not_sampled":0,"passed":{written},"written":{written},"dropped":0,"failed":0,"recoveries":0,"reason":0,"counts_unknown":false}}"#,
        open as u8,
        deleted as u8,
        2048 + written * 116 + 1
    )
}

fn status_json(open: bool) -> String {
    format!(
        r#"{{"state":"{}","state_code":3,"last_error":"OK","raw_sectors":417792,"head_lba":2048,"tail_lba":2048,"free_sectors":415744,"min_run_sectors":131072,"runs":5,"next_run_id":6,"table_entries":256,"open_run":{open},"open_run_id":{},"open_start_unix_ms":1791530000000,"open_client_tag":7,"open_wall_source":1,"recovered_runs":0,"skipped_bad_entries":0,"recovered_ids":[0,0,0,0,0,0,0,0],"counters":{{"seen":0,"empty_filtered":0,"invalid_not_sampled":0,"passed":0,"written":0,"dropped":0,"failed":0,"bytes_written":0,"drain_kbps":0,"first_frame_id":0,"last_frame_id":0}}}}"#,
        if open { "RECORDING" } else { "READY" },
        if open { 6 } else { 0 }
    )
}

struct Fixture {
    addr: std::net::SocketAddr,
    dir: PathBuf,
    state: Arc<AppState>,
}

impl Fixture {
    fn set(&self, file: &str, text: &str) {
        std::fs::write(self.dir.join(file), text).unwrap();
    }
    fn mode(&self, text: &str) {
        self.set("mode", text);
    }
    fn calls(&self) -> String {
        std::fs::read_to_string(self.dir.join("calls")).unwrap_or_default()
    }
    fn reads(&self) -> usize {
        self.calls().lines().filter(|l| l.starts_with("read ")).count()
    }
    /// The lease is free: a fresh begin works (and is ended again).
    fn lease_free(&self) -> bool {
        let begin = mib_app_commands::ssd_export_begin(&self.state, 5, 0, 0).unwrap();
        if begin["ok"] == true {
            mib_app_commands::ssd_export_end(&self.state, begin["lease"].as_u64().unwrap(), 0, "probe").unwrap();
            true
        } else {
            false
        }
    }
    async fn wait_lease_free(&self) {
        for _ in 0..200 {
            if self.lease_free() {
                return;
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        panic!("the lease was not released");
    }
}

async fn start(tag: &str) -> Fixture {
    let dir = std::env::temp_dir().join(format!("yofo_ssd_export_{tag}_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let data = dir.join("data");
    std::fs::create_dir_all(&data).unwrap();
    let script = dir.join("pzrec");
    std::fs::write(&script, FAKE.replace("__DIR__", &dir.to_string_lossy())).unwrap();
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    std::env::set_var("MIB_PZREC", &script);
    std::env::set_var("MIB_SSD_IMAGE", "disk");
    std::env::set_var("MIB_PZREC_ARGS", "--hw pl");
    std::env::set_var("MIB_SSD_EXPORT_IDLE_MS", "1500");
    std::env::set_var("MIB_SSD_EXPORT_TERM_GRACE_MS", "800");
    std::env::remove_var("MIB_SSD_EXPORT_MAX_MS");
    let fixture_files = [
        ("window", "IDLE".to_string()),
        ("status.json", status_json(false)),
        ("runs.json", format!("[{},{}]", run_row(5, 8, false, false), run_row(4, 8, false, true))),
        ("mode", "ok".to_string()),
    ];
    for (name, text) in fixture_files {
        std::fs::write(dir.join(name), text).unwrap();
    }
    let state = Arc::new(AppState::new());
    assert!(mib_app_commands::init(&state, &data.to_string_lossy(), "").unwrap(), "backend initialises");
    let mut config = ServerConfig::new(data.to_string_lossy());
    config.token = Some(TOKEN.into());
    let server = Server::new(config, state.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(server.clone().serve(listener, std::future::pending()));
    Fixture { addr, dir, state }
}

struct Reply {
    status: u16,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

impl Reply {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    fn json(&self) -> Value {
        serde_json::from_slice(&self.body).unwrap_or(Value::Null)
    }
}

fn parse_head(raw: &[u8]) -> Option<(u16, Vec<(String, String)>, usize)> {
    let split = raw.windows(4).position(|w| w == b"\r\n\r\n")?;
    let header = String::from_utf8_lossy(&raw[..split]).into_owned();
    let mut lines = header.lines();
    let status = lines.next()?.split_whitespace().nth(1)?.parse().ok()?;
    let headers = lines.filter_map(|l| l.split_once(':').map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))).collect();
    Some((status, headers, split + 4))
}

fn request_text(target: &str, extra: &[(&str, &str)]) -> String {
    let mut text = format!("GET {target} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
    for (k, v) in extra {
        text.push_str(&format!("{k}: {v}\r\n"));
    }
    text.push_str("\r\n");
    text
}

/// Read the whole reply until the server closes (a truncated body simply ends early).
async fn get_with(addr: std::net::SocketAddr, target: &str, extra: &[(&str, &str)]) -> Reply {
    let mut stream = tokio::net::TcpStream::connect(addr).await.unwrap();
    stream.write_all(request_text(target, extra).as_bytes()).await.unwrap();
    let mut raw = Vec::new();
    let _ = tokio::time::timeout(Duration::from_secs(20), stream.read_to_end(&mut raw)).await.expect("reply in time");
    let (status, headers, at) = parse_head(&raw).expect("a full header");
    let mut body = raw[at..].to_vec();
    if headers.iter().any(|(k, v)| k.eq_ignore_ascii_case("transfer-encoding") && v.contains("chunked")) {
        let mut out = Vec::new();
        let mut rest = &body[..];
        while let Some(eol) = rest.windows(2).position(|w| w == b"\r\n") {
            let size = usize::from_str_radix(std::str::from_utf8(&rest[..eol]).unwrap().trim(), 16).unwrap_or(0);
            if size == 0 || rest.len() < eol + 2 + size {
                break;
            }
            out.extend_from_slice(&rest[eol + 2..eol + 2 + size]);
            rest = &rest[(eol + 2 + size + 2).min(rest.len())..];
        }
        body = out;
    }
    Reply { status, headers, body }
}

async fn get(f: &Fixture, target: &str) -> Reply {
    let sep = if target.contains('?') { '&' } else { '?' };
    get_with(f.addr, &format!("{target}{sep}token={TOKEN}"), &[]).await
}

async fn pid_gone(dir: &Path) -> bool {
    let Ok(text) = std::fs::read_to_string(dir.join("pid")) else { return false };
    let pid: i32 = text.trim().parse().unwrap();
    for _ in 0..100 {
        if unsafe { libc::kill(pid, 0) } != 0 {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    false
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn streams_the_records_of_a_closed_run_verified() {
    let f = start("ok").await;
    let reply = get(&f, "/ssd/runs/5/records").await;
    assert_eq!(reply.status, 200);
    assert_eq!(reply.header("content-length"), Some((8 * REC).to_string().as_str()));
    assert_eq!(reply.header("x-run-id"), Some("5"));
    assert_eq!(reply.header("x-record-count"), Some("8"));
    assert_eq!(reply.header("x-record-bytes"), Some("59392"));
    assert_eq!(reply.header("cache-control"), Some("no-store"));
    assert_eq!(reply.body.len(), 8 * REC);
    assert!(reply.body.iter().all(|b| *b == b'A'));
    f.wait_lease_free().await;
    assert!(f.calls().contains("read 5 disk --hw pl --summary"), "the reader gets the run, the image and the board's options: {}", f.calls());
    // a window of the run
    let part = get(&f, "/ssd/runs/5/records?from=2&count=3").await;
    assert_eq!(part.status, 200);
    assert_eq!((part.header("x-record-count"), part.header("x-first-record")), (Some("3"), Some("2")));
    assert_eq!(part.body.len(), 3 * REC);
    assert!(f.calls().contains("--summary --from 2 --count 3"));
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn the_gate_is_the_one_of_files_download() {
    let f = start("gate").await;
    assert_eq!(get_with(f.addr, "/ssd/runs/5/records", &[]).await.status, 401);
    assert_eq!(get_with(f.addr, &format!("/ssd/runs/5/records?token={TOKEN}"), &[("Origin", "http://evil.example")]).await.status, 403);
    assert_eq!(get_with(f.addr, &format!("/ssd/runs/5/records?token={TOKEN}"), &[("Sec-Fetch-Site", "cross-site")]).await.status, 403);
    assert_eq!(f.reads(), 0, "a refused request never reaches the disk");
    assert!(f.lease_free());
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn refuses_with_503_while_anything_records_or_is_armed() {
    let f = start("busy").await;
    for window in ["ARMED", "DRAINING", "STOPPING", "FAULT", "WEDGED"] {
        f.set("window", window);
        let reply = get(&f, "/ssd/runs/5/records").await;
        assert_eq!(reply.status, 503, "{window}");
        assert_eq!(reply.json()["code"], "BUSY");
        assert!(reply.json()["error"].as_str().unwrap().contains(window));
    }
    f.set("window", "IDLE");
    f.set("status.json", &status_json(true));
    assert_eq!(get(&f, "/ssd/runs/5/records").await.status, 503, "an open run");
    assert_eq!(f.reads(), 0, "pzrec read was never started");
    f.set("status.json", &status_json(false));
    assert_eq!(get(&f, "/ssd/runs/5/records").await.status, 200, "idle again");
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn refuses_what_the_run_table_does_not_offer() {
    let f = start("table").await;
    let none = get(&f, "/ssd/runs/9/records").await;
    assert_eq!((none.status, none.json()["code"].as_str()), (404, Some("NO_SUCH_RUN")));
    let deleted = get(&f, "/ssd/runs/4/records").await;
    assert_eq!((deleted.status, deleted.json()["code"].as_str()), (410, Some("RUN_DELETED")));
    let range = get(&f, "/ssd/runs/5/records?from=8").await;
    assert_eq!((range.status, range.json()["code"].as_str()), (416, Some("BAD_RANGE")));
    let past = get(&f, "/ssd/runs/5/records?from=6&count=5").await;
    assert_eq!(past.status, 416, "never clipped");
    f.set("runs.json", &format!("[{}]", run_row(5, 8, true, false)));
    assert_eq!(get(&f, "/ssd/runs/5/records").await.status, 409, "an open run is not offered");
    assert_eq!(f.reads(), 0);
    f.set("runs.json", &format!("[{}]", run_row(5, 8, false, false)));
    assert!(f.lease_free(), "a refusal leaves no lease behind");
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn pzrec_errors_before_the_first_byte_become_a_status_with_its_code_and_name() {
    let f = start("codes").await;
    for (name, code, status) in [
        ("ACTIVE", 10, 503),
        ("HW", 4, 503),
        ("NO_SUCH_RUN", 6, 404),
        ("RUN_DELETED", 12, 410),
        ("ARGS", 2, 400),
        ("IO", 1, 502),
        ("BAD_STATE", 7, 503),
        ("WEDGED", 9, 503),
    ] {
        f.mode(&format!("err {name} {code}"));
        let reply = get(&f, "/ssd/runs/5/records").await;
        assert_eq!(reply.status, status, "{name}");
        let body = reply.json();
        assert_eq!(body["pzrec"]["name"], name);
        assert_eq!(body["pzrec"]["code"], code);
        assert_eq!(body["pzrec"]["exit_code"], 1);
        assert!(body["error"].as_str().unwrap().contains(name), "{body}");
        assert_eq!(body["bytes_sent"], 0);
        f.wait_lease_free().await;
    }
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_failure_inside_a_record_ends_the_response_short_never_whole() {
    let f = start("iofail").await;
    f.mode("iofail");
    let reply = get(&f, "/ssd/runs/5/records").await;
    assert_eq!(reply.status, 200, "the status was sent with the first bytes");
    assert_eq!(reply.header("content-length"), Some((8 * REC).to_string().as_str()));
    assert!(reply.body.len() < 8 * REC, "the body is cut: {} bytes", reply.body.len());
    f.wait_lease_free().await;
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_download_whose_counts_disagree_is_never_complete() {
    let f = start("counts").await;
    for mode in ["short", "liar", "nosummary", "failafter"] {
        f.mode(mode);
        let reply = get(&f, "/ssd/runs/5/records").await;
        assert_eq!(reply.status, 200, "{mode}");
        assert!(reply.body.len() < 8 * REC, "{mode}: {} of {} bytes, the last byte is held until the reader is verified", reply.body.len(), 8 * REC);
        f.wait_lease_free().await;
    }
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_client_that_goes_away_terminates_the_reader_and_releases_the_lease() {
    let f = start("cancel").await;
    f.mode("slow");
    let mut stream = tokio::net::TcpStream::connect(f.addr).await.unwrap();
    stream.write_all(request_text(&format!("/ssd/runs/5/records?token={TOKEN}"), &[]).as_bytes()).await.unwrap();
    let mut buf = vec![0u8; 4096];
    let n = tokio::time::timeout(Duration::from_secs(10), stream.read(&mut buf)).await.expect("first bytes").unwrap();
    assert!(n > 0);
    assert!(!f.lease_free(), "the lease is held during the download");
    drop(stream);
    f.wait_lease_free().await;
    assert!(f.dir.join("terminated").exists(), "the reader got SIGTERM");
    assert!(pid_gone(&f.dir).await, "and is gone");
    // a new download works at once
    f.mode("ok");
    assert_eq!(get(&f, "/ssd/runs/5/records").await.status, 200);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_reader_that_ignores_sigterm_is_killed_and_a_quiet_one_is_cut_off() {
    let f = start("kill").await;
    f.mode("hang");
    let mut stream = tokio::net::TcpStream::connect(f.addr).await.unwrap();
    stream.write_all(request_text(&format!("/ssd/runs/5/records?token={TOKEN}"), &[]).as_bytes()).await.unwrap();
    let mut buf = vec![0u8; 4096];
    let _ = tokio::time::timeout(Duration::from_secs(10), stream.read(&mut buf)).await.expect("first bytes").unwrap();
    drop(stream);
    f.wait_lease_free().await;
    assert!(pid_gone(&f.dir).await, "SIGKILL after the grace");
    // a reader that never writes: the idle bound ends it (504) with the lease released
    let _ = std::fs::remove_file(f.dir.join("terminated"));
    f.mode("quiet");
    let reply = get(&f, "/ssd/runs/5/records").await;
    assert_eq!(reply.status, 504);
    f.wait_lease_free().await;
    assert!(f.dir.join("terminated").exists());
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_second_download_is_refused_while_one_runs() {
    let f = start("second").await;
    f.mode("slow");
    let mut first = tokio::net::TcpStream::connect(f.addr).await.unwrap();
    first.write_all(request_text(&format!("/ssd/runs/5/records?token={TOKEN}"), &[]).as_bytes()).await.unwrap();
    let mut buf = vec![0u8; 4096];
    let _ = tokio::time::timeout(Duration::from_secs(10), first.read(&mut buf)).await.expect("first bytes").unwrap();
    let second = get(&f, "/ssd/runs/5/records").await;
    assert_eq!(second.status, 409);
    assert_eq!(second.json()["code"], "BUSY");
    assert_eq!(f.reads(), 1, "only one reader was started");
    drop(first);
    f.wait_lease_free().await;
    f.mode("ok");
    assert_eq!(get(&f, "/ssd/runs/5/records").await.status, 200);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn the_lease_and_the_route_exclude_each_other_both_ways() {
    let f = start("lease").await;
    // a lease taken elsewhere (the bridge command) refuses the route
    let held = mib_app_commands::ssd_export_begin(&f.state, 5, 0, 0).unwrap();
    assert_eq!(held["ok"], true, "{held}");
    assert_eq!(held["records"], 8);
    assert_eq!(held["bytes"], 8 * REC);
    assert_eq!(held["argv"][1], "read");
    let route = get(&f, "/ssd/runs/5/records").await;
    assert_eq!((route.status, route.json()["code"].as_str()), (503, Some("BUSY")));
    assert_eq!(f.reads(), 0);
    // ending is idempotent
    let lease = held["lease"].as_u64().unwrap();
    mib_app_commands::ssd_export_end(&f.state, lease, 0, "test").unwrap();
    mib_app_commands::ssd_export_end(&f.state, lease, 0, "again").unwrap();
    assert_eq!(get(&f, "/ssd/runs/5/records").await.status, 200);
    // a window that shows a run active at the moment of the begin refuses it
    f.set("window", "DRAINING");
    let refused = mib_app_commands::ssd_export_begin(&f.state, 5, 0, 0).unwrap();
    assert_eq!((refused["ok"].clone(), refused["code"].clone()), (Value::Bool(false), Value::String("BUSY".into())));
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_client_that_stops_reading_cannot_hold_the_lease_past_the_bound() {
    let f = start("stall").await;
    f.mode("flood");
    f.set("count", "3000");                                  // 178 MB: far more than the socket and the pipe hold
    f.set("runs.json", &format!("[{}]", run_row(5, 3000, false, false)));
    std::env::set_var("MIB_SSD_EXPORT_MAX_MS", "1500");
    let mut stream = tokio::net::TcpStream::connect(f.addr).await.unwrap();
    stream.write_all(request_text(&format!("/ssd/runs/5/records?token={TOKEN}"), &[]).as_bytes()).await.unwrap();
    // the socket stays open and is never read: the pipe fills, nobody polls the stream
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert!(!f.lease_free(), "held while the download runs");
    f.wait_lease_free().await;
    assert!(pid_gone(&f.dir).await, "the reader was terminated by the supervisor with the client still connected");
    drop(stream);
    std::env::remove_var("MIB_SSD_EXPORT_MAX_MS");
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_client_that_drops_a_steady_download_is_noticed_without_the_idle_timeout() {
    let f = start("steady").await;
    f.mode("steady");
    // output every 100 ms keeps the idle timeout (1.5 s) from ever firing: only the dropped stream can end this
    let mut stream = tokio::net::TcpStream::connect(f.addr).await.unwrap();
    stream.write_all(request_text(&format!("/ssd/runs/5/records?token={TOKEN}"), &[]).as_bytes()).await.unwrap();
    let mut buf = vec![0u8; 8192];
    for _ in 0..3 {
        let n = tokio::time::timeout(Duration::from_secs(5), stream.read(&mut buf)).await.expect("data").unwrap();
        assert!(n > 0);
    }
    let dropped = std::time::Instant::now();
    drop(stream);
    f.wait_lease_free().await;
    assert!(dropped.elapsed() < Duration::from_millis(1400), "released in {:?}: that is the drop path, not the idle timeout", dropped.elapsed());
    assert!(f.dir.join("terminated").exists() && pid_gone(&f.dir).await);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_future_dropped_while_the_reader_is_being_terminated_still_releases_the_lease() {
    let f = start("midterm").await;
    f.mode("deaf");                                          // ignores SIGTERM and writes nothing
    std::env::set_var("MIB_SSD_EXPORT_IDLE_MS", "500");
    std::env::set_var("MIB_SSD_EXPORT_TERM_GRACE_MS", "3000");
    let mut stream = tokio::net::TcpStream::connect(f.addr).await.unwrap();
    stream.write_all(request_text(&format!("/ssd/runs/5/records?token={TOKEN}"), &[]).as_bytes()).await.unwrap();
    // 0.5 s idle -> the route terminates the reader and waits out the 3 s grace; the client leaves in the middle of that
    tokio::time::sleep(Duration::from_millis(1200)).await;
    assert!(!f.lease_free(), "still held during the grace");
    drop(stream);
    f.wait_lease_free().await;
    assert!(pid_gone(&f.dir).await, "SIGKILL after the grace, though nobody was waiting for the answer");
}
