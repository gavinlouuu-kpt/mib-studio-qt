//! Fold-in end-to-end scenario (E, I, F; docs/architecture/fold-in-backend-goals.md) through the
//! real `yofo-studio-server` binary, as a user would drive it: every step is a WebSocket command
//! to one real backend process, and step 7 restarts that process on the same data directory.
//!
//! Evidence classes, printed per step:
//! - `mock camera`: the mock folder camera and its fake GenApi node map (never hardware);
//! - `real backend`: the native authorities on real files (coordinator, recorder, F evaluator,
//!   recipe provider, SQLite workspace store);
//! - `synthetic data`: recordings written by the production `Hdf5Service` writer through the review
//!   fixture (`review_fixture_write_population`) when the mock recordings hold no measured cells,
//!   and generated FCS 3.0 files for the multi-unit comparison and the 4PL fit.
//!
//! Run (prints the step log with timings; exits non-zero on the first failed assertion):
//!   MIB_BRIDGE_NO_CMAKE=1 cargo test --manifest-path crates/mib-bridge-server/Cargo.toml \
//!     --test fold_in_e2e -- --ignored --nocapture
//! Ignored by default: it spawns the server twice and records two runs (about a minute).

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio_tungstenite::tungstenite::Message;

type Socket =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

// ---------------------------------------------------------------------------------------------
// Step log

struct Log {
    t0: Instant,
    rows: Vec<(String, String, String, f64)>,
    current: Option<(String, String, Instant)>,
}

impl Log {
    fn new() -> Self {
        println!("# fold-in e2e (real yofo-studio-server process)");
        Log { t0: Instant::now(), rows: Vec::new(), current: None }
    }
    fn begin(&mut self, id: &str, class: &str) {
        println!("\n== [{:>7.2}s] {id} ({class})", self.t0.elapsed().as_secs_f64());
        self.current = Some((id.into(), class.into(), Instant::now()));
    }
    fn note(&self, line: impl AsRef<str>) {
        println!("   [{:>7.2}s] {}", self.t0.elapsed().as_secs_f64(), line.as_ref());
    }
    fn end(&mut self) {
        let (id, class, t) = self.current.take().unwrap();
        let s = t.elapsed().as_secs_f64();
        println!("   PASS {id} in {s:.2}s");
        self.rows.push((id, class, "PASS".into(), s));
    }
    fn table(&self) {
        println!("\n| Step | Evidence class | Result | Time (s) |\n|---|---|---|---|");
        for (id, class, r, s) in &self.rows {
            println!("| {id} | {class} | {r} | {s:.2} |");
        }
        println!("Total {:.1}s", self.t0.elapsed().as_secs_f64());
    }
}

impl Drop for Log {
    fn drop(&mut self) {
        if std::thread::panicking() {
            if let Some((id, class, t)) = &self.current {
                println!("   FAIL {id} ({class}) after {:.2}s", t.elapsed().as_secs_f64());
            }
            self.table();
        }
    }
}

macro_rules! check {
    ($cond:expr, $($fmt:tt)+) => {
        if !$cond {
            panic!("ASSERTION FAILED: {}: {}", stringify!($cond), format!($($fmt)+));
        }
    };
}

// ---------------------------------------------------------------------------------------------
// The server process and its socket

struct Backend {
    child: Child,
    ws: Socket,
    next: u64,
}

fn free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

async fn spawn_backend(data: &Path, log: &Log) -> Backend {
    let port = free_port();
    let child = Command::new(env!("CARGO_BIN_EXE_yofo-studio-server"))
        .args(["--listen", &format!("127.0.0.1:{port}"), "--data-dir"])
        .arg(data)
        .arg("--no-token")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn yofo-studio-server");
    log.note(format!("spawned yofo-studio-server pid {} on 127.0.0.1:{port}, data {}", child.id(), data.display()));
    let deadline = Instant::now() + Duration::from_secs(30);
    let ws = loop {
        match tokio_tungstenite::connect_async(format!("ws://127.0.0.1:{port}/ws")).await {
            Ok((ws, _)) => break ws,
            Err(e) => {
                check!(Instant::now() < deadline, "server never accepted: {e}");
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
        }
    };
    Backend { child, ws, next: 0 }
}

impl Backend {
    /// One command: `Ok(reply.ok)` or `Err(reply.error)`; pushed events and session notes are skipped.
    async fn call(&mut self, cmd: &str, args: Value) -> Result<Value, String> {
        self.next += 1;
        let id = self.next;
        let text = json!({ "request_id": id, "cmd": cmd, "args": args }).to_string();
        self.ws.send(Message::Text(text.into())).await.unwrap();
        let deadline = Instant::now() + Duration::from_secs(120);
        loop {
            let message = tokio::time::timeout_at(deadline.into(), self.ws.next())
                .await
                .unwrap_or_else(|_| panic!("{cmd}: no reply in 120 s"))
                .expect("socket open")
                .expect("socket message");
            if let Message::Text(text) = message {
                let v: Value = serde_json::from_str(text.as_str()).unwrap();
                if v.get("request_id") != Some(&json!(id)) {
                    continue;
                }
                return match v.get("error") {
                    Some(e) => Err(e.as_str().unwrap_or_default().to_string()),
                    None => Ok(v["ok"].clone()),
                };
            }
        }
    }
    async fn ok(&mut self, cmd: &str, args: Value) -> Value {
        match self.call(cmd, args).await {
            Ok(v) => v,
            Err(e) => panic!("ASSERTION FAILED: {cmd} replied with an error: {e}"),
        }
    }
    /// `{request}`-wrapped JSON commands that answer {status, ...}; asserts status ok.
    async fn req_ok(&mut self, cmd: &str, request: Value) -> Value {
        let r = self.ok(cmd, json!({ "request": request })).await;
        check!(r["status"] == "ok", "{cmd}: {}", short(&r));
        r
    }
    async fn workspace(&mut self, request: Value, write: bool) -> Value {
        let cmd = if write { "workspace_write" } else { "workspace_read" };
        self.ok(cmd, json!({ "request": request.to_string() })).await
    }
    /// SIGTERM: the server finalises and saves, then releases the backend, then exits.
    async fn stop(mut self, log: &Log) {
        let _ = self.ws.close(None).await;
        unsafe {
            libc::kill(self.child.id() as i32, libc::SIGTERM);
        }
        let deadline = Instant::now() + Duration::from_secs(60);
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                log.note(format!("server exited: {status}"));
                return;
            }
            check!(Instant::now() < deadline, "server did not exit after SIGTERM");
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }
}

fn short(v: &Value) -> String {
    let s = v.to_string();
    if s.len() > 1500 { format!("{}...", &s[..1500]) } else { s }
}

fn dec(v: &Value) -> u64 {
    match v {
        Value::String(s) => s.parse().unwrap_or_else(|_| panic!("not a decimal: {v}")),
        Value::Number(n) => n.as_u64().unwrap(),
        _ => panic!("not a count: {v}"),
    }
}

fn mock_frames() -> String {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../data/mock_frames").to_string_lossy().into_owned()
}

// ---------------------------------------------------------------------------------------------
// Synthetic FCS 3.0 list-mode files (float32, little-endian) for the multi-unit analyses

fn write_fcs(path: &Path, columns: &[(&str, Vec<f32>)]) {
    let n = columns[0].1.len();
    let mut data = Vec::with_capacity(n * columns.len() * 4);
    for i in 0..n {
        for (_, c) in columns {
            data.extend_from_slice(&c[i].to_le_bytes());
        }
    }
    let text_for = |begin: usize, end: usize| {
        let mut t = format!(
            "/$BEGINANALYSIS/0/$ENDANALYSIS/0/$BEGINSTEXT/0/$ENDSTEXT/0/$BEGINDATA/{begin}/$ENDDATA/{end}\
             /$BYTEORD/1,2,3,4/$DATATYPE/F/$MODE/L/$NEXTDATA/0/$PAR/{}/$TOT/{n}",
            columns.len()
        );
        for (i, (name, _)) in columns.iter().enumerate() {
            let p = i + 1;
            t += &format!("/$P{p}N/{name}/$P{p}B/32/$P{p}E/0,0/$P{p}R/1000000");
        }
        t + "/"
    };
    let mut begin = 0usize;
    let text = loop {
        let t = text_for(begin, begin + data.len() - 1);
        let want = 58 + t.len();
        if want == begin {
            break t;
        }
        begin = want;
    };
    let header = format!(
        "FCS3.0    {:>8}{:>8}{:>8}{:>8}{:>8}{:>8}",
        58,
        58 + text.len() - 1,
        begin,
        begin + data.len() - 1,
        0,
        0
    );
    assert_eq!(header.len(), 58);
    let mut bytes = header.into_bytes();
    bytes.extend_from_slice(text.as_bytes());
    bytes.extend_from_slice(&data);
    std::fs::write(path, bytes).unwrap();
}

/// Deterministic normal variates (xorshift + Box-Muller): no RNG crate in this test.
struct Rng(u64);
impl Rng {
    fn uniform(&mut self) -> f64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        ((self.0 >> 11) as f64 + 0.5) / (1u64 << 53) as f64
    }
    fn normal(&mut self) -> f64 {
        (-2.0 * self.uniform().ln()).sqrt() * (2.0 * std::f64::consts::PI * self.uniform()).cos()
    }
}

fn fcs_unit(path: &Path, center: f64, seed: u64) {
    let mut rng = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
    let area: Vec<f32> = (0..400).map(|_| (center * (1.0 + 0.08 * rng.normal())) as f32).collect();
    let deform: Vec<f32> = (0..400).map(|_| (0.05 + 0.01 * rng.normal()).max(0.001) as f32).collect();
    write_fcs(path, &[("area", area), ("deformability", deform)]);
}

// ---------------------------------------------------------------------------------------------
// Scenario helpers

async fn open_tree(b: &mut Backend) -> Value {
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let tree = b.ok("camera_feature_tree", json!({ "request": { "modules": ["remote"] } })).await;
        if tree["outcome"] == "read" {
            return tree;
        }
        check!(Instant::now() < deadline, "node map never opened: {}", short(&tree));
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

fn feature_request(session: &Value, feature: &str, value: Option<Value>) -> Value {
    let mut r = json!({ "session": session, "module": "remote", "feature": feature });
    if let Some(v) = value {
        r["value"] = v;
    }
    json!({ "request": r })
}

async fn finish_discovery(b: &mut Backend, start: &Value) -> Value {
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let snap = b.ok("fetch_device_discovery", json!({ "jobId": start["job_id"], "instance": start["instance"] })).await;
        check!(snap["stale"] == false && snap["valid"] == true, "{}", short(&snap));
        if snap["state"].as_u64().unwrap() >= 2 {
            return snap;
        }
        check!(Instant::now() < deadline, "discovery never ended: {}", short(&snap));
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

async fn wait_terminal(b: &mut Backend) -> Value {
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let s = b.ok("fetch_experiment_status", Value::Null).await;
        if s["terminal"] == true && s["state"] == 0 {
            return s;
        }
        check!(Instant::now() < deadline, "run never finalized: {}", short(&s));
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

/// Snapshot a source and save a plan pinned to it; returns (source descriptor, saved revision).
async fn save_plan(b: &mut Backend, plan_id: &str, source: &Value, graph: &Value, expected: Value) -> (Value, String) {
    let snap = b.req_ok("analysis_read", json!({ "action": "snapshot", "source": source })).await;
    let binding: serde_json::Map<String, Value> = ["fingerprint", "sourceKey", "parameters", "scope"]
        .iter()
        .map(|k| (k.to_string(), snap["source"][*k].clone()))
        .collect();
    let mut g = graph.clone();
    g["id"] = json!(plan_id);
    let saved = b
        .req_ok(
            "analysis_write",
            json!({ "action": "save", "planId": plan_id, "expectedRevision": expected,
                    "commandId": format!("save-{plan_id}-{}", g["revision"]), "graph": g, "feature": "area",
                    "source": source, "sourceBinding": binding }),
        )
        .await;
    (snap["source"].clone(), saved["revision"].as_str().unwrap().to_string())
}

fn all_graph() -> Value {
    json!({ "id": "", "revision": "draft", "output": "all",
            "nodes": [{ "id": "all", "parent": "", "kind": "interval", "feature": "area", "minimum": null, "maximum": null }] })
}

// ---------------------------------------------------------------------------------------------

#[tokio::test(flavor = "multi_thread")]
#[ignore = "end-to-end scenario: spawns the server twice; run with --ignored --nocapture"]
async fn fold_in_end_to_end() {
    let mut log = Log::new();
    let root = std::env::temp_dir().join(format!("fold_in_e2e_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let data = root.join("data");
    let runs = root.join("runs");
    let profiles = root.join("profiles");
    for d in [&data, &runs, &profiles] {
        std::fs::create_dir_all(d).unwrap();
    }
    let base = profiles.to_string_lossy().into_owned();
    log.note(format!("scenario root {}", root.display()));

    let mut b = spawn_backend(&data, &log).await;
    check!(b.ok("init", json!({ "dataDir": "" })).await == json!(true), "init");
    let abi = b.ok("abi_version", Value::Null).await;
    check!(abi == json!(50), "bridge ABI {abi}");
    log.note("init ok, bridge abi_version 50");

    // ---- E01: discovery finds the mock source; replies carry the backend instance.
    log.begin("E01 discovery + instance", "mock camera / real backend");
    let discovery = b.ok("start_camera_discovery", Value::Null).await;
    check!(discovery["accepted"] == true, "{}", short(&discovery));
    let instance = discovery["instance"].as_str().unwrap_or_default().to_string();
    check!(instance.len() == 36, "start reply carries a UUID instance: {}", short(&discovery));
    let finished = finish_discovery(&mut b, &discovery).await;
    check!(finished["instance"] == discovery["instance"], "snapshot names the same instance");
    let mock = finished["candidates"].as_array().unwrap().iter().find(|c| c["provider_id"] == "mock").cloned();
    check!(mock.is_some(), "mock source among candidates: {}", short(&finished));
    log.note(format!("job {} state {} instance {instance}; candidates {}; mock: {}", finished["job_id"], finished["state"],
        finished["candidates"].as_array().unwrap().len(), mock.unwrap()["display_name"]));
    log.end();

    // ---- E02: node map in the default boot; read, write+readback, read-only refusal; refused while
    //           realtime processing / an experiment is active (checked in E04 below).
    log.begin("E02 feature tree / read / write / refusals", "mock camera (fake GenApi node map)");
    let none = b.ok("camera_feature_tree", json!({ "request": {} })).await;
    check!(none["outcome"] == "refused", "no session yet: {}", short(&none));
    let cfg = b.ok("configure_mock", json!({ "frameDir": mock_frames(), "frameIntervalMs": 10, "loopFiles": true })).await;
    check!(cfg["ok"] == true, "{}", short(&cfg));
    let started = b.ok("start_capture", Value::Null).await;
    check!(started["ok"] == true, "{}", short(&started));
    let trigger = b.ok("fetch_trigger_status", Value::Null).await;
    log.note(format!("default boot trigger status: {}", short(&trigger)));
    let tree = open_tree(&mut b).await;
    check!(tree["evidence"] == "fixture/mock", "evidence class: {}", tree["evidence"]);
    let remote = tree["modules"][0].clone();
    check!(remote["module"] == "remote" && remote["enumerated"] == true, "{}", short(&remote));
    let features = remote["features"].as_array().unwrap().clone();
    let categories = remote["categories"].as_array().unwrap().len();
    check!(!features.is_empty() && categories > 0, "tree has categories and features");
    let session1 = tree["session"].clone();
    check!(session1["instance"].as_str().unwrap_or_default().len() == 36, "token names its instance: {session1}");
    let writable = features.iter().find(|f| {
        let d = &f["descriptor"];
        d["kind"] == "float" && d["writable"] == true && d["minimum"].is_object() && d["maximum"].is_object()
    }).expect("a writable bounded float").clone();
    let read_only = features.iter().find(|f| f["descriptor"]["readable"] == true && f["descriptor"]["writable"] == false)
        .expect("a read-only node").clone();
    let wname = writable["name"].as_str().unwrap().to_string();
    let rname = read_only["name"].as_str().unwrap().to_string();
    log.note(format!("enumerated {} features in {categories} categories; writable float {wname}, read-only {rname} ({})",
        features.len(), read_only["descriptor"]["kind"]));
    let read = b.ok("camera_feature_read", feature_request(&session1, &wname, None)).await;
    check!(read["outcome"] == "read", "{}", short(&read));
    let lo = writable["descriptor"]["minimum"]["value"].as_f64().unwrap();
    let hi = writable["descriptor"]["maximum"]["value"].as_f64().unwrap();
    let target = lo + (hi - lo) / 3.0;
    let value = json!({ "type": "float", "value": target });
    let written = b.ok("camera_feature_write", feature_request(&session1, &wname, Some(value.clone()))).await;
    check!(written["outcome"] == "readback", "{}", short(&written));
    check!(written["requested"] == value, "requested kept apart");
    check!(written["readback"]["value"]["value"].as_f64() == Some(target), "readback {}", written["readback"]);
    let session2 = written["session"].clone();
    check!(session2["capability_revision"] == "2", "a write advances the revision: {session2}");
    let again = b.ok("camera_feature_read", feature_request(&session2, &wname, None)).await;
    check!(again["readback"]["value"]["value"].as_f64() == Some(target), "the device holds it");
    let ro_kind = read_only["descriptor"]["kind"].as_str().unwrap();
    let ro_value = match ro_kind {
        "integer" => json!({ "type": "integer", "value": "1" }),
        "float" => json!({ "type": "float", "value": 1.0 }),
        "boolean" => json!({ "type": "boolean", "value": true }),
        _ => json!({ "type": "string", "value": "x" }),
    };
    let refused = b.ok("camera_feature_write", feature_request(&session2, &rname, Some(ro_value))).await;
    check!(refused["outcome"] == "refused" && refused["readback"]["writable"] == false, "{}", short(&refused));
    check!(refused["session"] == session2, "a refusal does not advance the revision");
    log.note(format!("write {wname}={target} readback ok (revision 2); read-only write refused: {}", refused["reason"]));

    // Realtime processing on: feature I/O refused, nothing reaches the port.
    let applied = b.ok("apply_processing", json!({ "realtimeEnabled": true, "pixelToMicron": 0.25 })).await;
    check!(applied["ok"] == true, "{}", short(&applied));
    let deadline = Instant::now() + Duration::from_secs(10);
    let rt = loop {
        let r = b.ok("camera_feature_read", feature_request(&session2, &wname, None)).await;
        if r["outcome"] == "refused" || Instant::now() > deadline {
            break r;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    };
    check!(rt["outcome"] == "refused", "read refused while realtime processing runs: {}", short(&rt));
    let rtw = b.ok("camera_feature_write", feature_request(&session2, &wname, Some(value.clone()))).await;
    check!(rtw["outcome"] == "refused" && rtw["readback"].is_null(), "write refused, port untouched: {}", short(&rtw));
    log.note(format!("realtime processing on: read/write refused: {}", rt["reason"]));
    log.end();

    // ---- E04/E06: method selection, two recorded runs (sample A, sample B), reconciled accounting.
    log.begin("E04/E06 method + runs A and B + accounting", "mock camera / real backend");
    let config = b.ok("fetch_processing_config_json", Value::Null).await;
    let config_doc = match &config {
        Value::String(s) => s.clone(),
        other => other.to_string(),
    };
    let created = b.ok("profile_command", json!({ "base": base, "request": json!({ "operation": "create",
        "name": "e2e-method", "document_json": config_doc }).to_string() })).await;
    check!(created["ok"] == true, "{}", short(&created));
    let profile = b.ok("profile_command", json!({ "base": base, "request": r#"{"operation":"read","name":"e2e-method"}"# })).await;
    let method_revision = profile["profile"]["revision"].as_str().unwrap().to_string();
    // The profile authority applies only with capture and realtime processing stopped.
    check!(b.ok("apply_processing", json!({ "realtimeEnabled": false, "pixelToMicron": 0.25 })).await["ok"] == true, "realtime off");
    check!(b.ok("stop_capture", Value::Null).await["ok"] == true, "capture stopped for the method change");
    let applied = b.ok("profile_command", json!({ "base": base, "request": json!({ "operation": "apply",
        "name": "e2e-method", "baseline": method_revision }).to_string() })).await;
    log.note(format!("method profile e2e-method revision {method_revision} created from the live config; apply: ok={} {}",
        applied["ok"], applied.get("error").map(|e| e.to_string()).unwrap_or_default()));
    check!(applied["ok"] == true, "method applied through the profile authority: {}", short(&applied));
    check!(b.ok("start_capture", Value::Null).await["ok"] == true, "capture restarted");
    // A capture restart opens a new generation: the pre-restart token is stale from here on.
    let tree_run = open_tree(&mut b).await;
    let session2 = tree_run["session"].clone();
    // Applying the method leaves realtime processing off; turning it on for the runs is a
    // configuration change after the method, which the recipe diff below reports (method: different).
    let reapplied = b.ok("apply_processing", json!({ "realtimeEnabled": true, "pixelToMicron": 0.25 })).await;
    check!(reapplied["ok"] == true, "{}", short(&reapplied));

    let experiment_id = "5f0c1d2e-3a4b-4c5d-8e6f-7a8b9c0d1e2f";
    let mut recordings: Vec<(String, PathBuf, Value)> = Vec::new();
    for (index, sample) in ["A", "B"].iter().enumerate() {
        let out = runs.join(format!("sample_{sample}.h5"));
        let request = json!({ "experiment_id": experiment_id, "run_index": index, "sample_ref": sample,
            "operator": { "name": "fold-in e2e" },
            "annotation": { "sample": format!("sample {sample}"), "condition": if index == 0 { "control" } else { "treated" },
                            "experimental_unit": format!("unit-{sample}"), "notes": "mock camera e2e; not biological evidence" } })
            .to_string();
        let deadline = Instant::now() + Duration::from_secs(30);
        let start = loop {
            let r = b.ok("experiment_start", json!({ "outputPath": out.to_string_lossy(), "startRequestJson": request })).await;
            if r["ok"] == true || Instant::now() > deadline {
                break r;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        };
        check!(start["ok"] == true && start["start_outcome"] == 0, "start {sample}: {}", short(&start));
        let status = b.ok("fetch_experiment_status", Value::Null).await;
        check!(status["state"] == 2, "Active: {}", short(&status));
        let generation = status["start_generation"].as_str().unwrap().to_string();
        let during = b.ok("camera_feature_read", feature_request(&session2, &wname, None)).await;
        check!(during["outcome"] == "refused", "feature I/O refused while the experiment is active: {}", short(&during));
        tokio::time::sleep(Duration::from_millis(2500)).await;
        let stop = b.ok("experiment_stop", json!({ "expectedStartGeneration": generation })).await;
        check!(stop["ok"] == true && stop["stop_outcome"] == 0, "stop {sample}: {}", short(&stop));
        let terminal = wait_terminal(&mut b).await;
        let acc = b.ok("fetch_run_accounting", json!({ "source": "last_run" })).await;
        check!(acc["available"] == true, "{}", short(&acc));
        check!(dec(&acc["start_generation"]) == generation.parse::<u64>().unwrap(), "accounting names the run: {}", short(&acc));
        check!(acc["reconciled"] == true, "run {sample} accounting reconciles: {}", short(&acc));
        let p = |k: &str| dec(&acc[k]);
        check!(p("persistence_admitted") == p("persistence_committed") + p("persistence_failed") + p("persistence_pending_at_stop"),
            "persistence conserved: {}", short(&acc));
        check!(out.exists(), "file written: {}", out.display());
        log.note(format!(
            "run {sample}: gen {generation} completion {} ({}), admitted {} processed {} empty {} objects {} committed {} failed {}; file {} B; stop: {}",
            acc["completion_name"], acc["completion_reason"], acc["admitted"], acc["processed"], acc["empty"],
            acc["objects_detected"], acc["persistence_committed"], acc["persistence_failed"],
            std::fs::metadata(&out).map(|m| m.len()).unwrap_or(0), terminal["completion"]));
        recordings.push((sample.to_string(), out, acc));
    }
    // The recorded files reopen in review with their own accounting.
    let mut analysis_paths: Vec<PathBuf> = Vec::new();
    let mut synthetic = false;
    for (sample, path, _) in &recordings {
        let opened = b.ok("load_recording", json!({ "filePath": path.to_string_lossy() })).await;
        check!(opened["ok"] == true, "reopen {sample}: {}", short(&opened));
        let review = b.ok("fetch_run_accounting", json!({ "source": "review" })).await;
        let meta = b.ok("fetch_review_metadata", Value::Null).await;
        let pop = b.ok("analyze_population", json!({ "request": { "expectedPath": path.to_string_lossy(), "feature": "area",
            "minimum": null, "maximum": null } })).await;
        let measured = pop["measured"].as_str().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0);
        let _ = meta;
        log.note(format!("reopened {sample}: review accounting recorded={} reconciled={}; measured area events {measured}{}",
            review["recorded"], review["reconciled"],
            if pop["ok"] == false { format!(" (population refused: {})", pop["message"]) } else { String::new() }));
        if measured < 50 {
            synthetic = true;
        }
    }
    let _ = b.ok("close_review", Value::Null).await;
    if synthetic {
        log.note("FALLBACK: the mock-camera recordings hold no (or too few) measured cells: the mock folder is one 512x96 \
                  frame looped, so the analysis steps below use SYNTHETIC recordings written by the production Hdf5Service \
                  writer (review_fixture_write_population, 6000 cells, seeds 11 / 12)");
        for (sample, seed) in [("A", 11u64), ("B", 12u64)] {
            let p = runs.join(format!("synthetic_{sample}.h5"));
            check!(mib_bridge::review_ffi::review_fixture_write_population(&p.to_string_lossy(), 6000, seed), "write synthetic {sample}");
            analysis_paths.push(p);
        }
    } else {
        analysis_paths = recordings.iter().map(|(_, p, _)| p.clone()).collect();
    }
    log.end();
    let analysis_class = if synthetic { "synthetic data (production writer) / real backend" } else { "mock camera / real backend" };
    let path_a = analysis_paths[0].to_string_lossy().into_owned();
    let path_b = analysis_paths[1].to_string_lossy().into_owned();
    let source_a = json!({ "kind": "stored", "path": path_a });
    let source_b = json!({ "kind": "stored", "path": path_b });

    // Data-driven gates: quartiles from the native evaluator, never computed here.
    check!(b.ok("load_recording", json!({ "filePath": path_a })).await["ok"] == true, "open A");
    let quart = |r: &Value| {
        let f = |k: &str| r[k].as_f64().unwrap_or_else(|| panic!("ASSERTION FAILED: statistics.{k}: {}", short(r)));
        (f("p25"), f("median"), f("p95"))
    };
    let ra = b.ok("analyze_population", json!({ "request": { "expectedPath": path_a, "feature": "area", "minimum": null, "maximum": null } })).await;
    let rd = b.ok("analyze_population", json!({ "request": { "expectedPath": path_a, "feature": "deformability", "minimum": null, "maximum": null } })).await;
    let (a25, a50, a95) = quart(&ra);
    let (d25, _d50, d95) = quart(&rd);
    let graph = json!({ "id": "", "revision": "draft1", "output": "target", "nodes": [
        { "id": "sized", "parent": "", "kind": "interval", "feature": "area", "minimum": a25, "maximum": null },
        { "id": "shape", "parent": "", "kind": "polygon", "x": "area", "y": "deformability",
          "vertices": [[a50 * 0.5, d25], [a95 * 1.5, d25], [a95 * 1.5, d95 * 2.0], [a50 * 0.5, d95 * 2.0]] },
        { "id": "target", "parent": "", "kind": "and", "operands": ["sized", "shape"] } ] });

    // ---- I02: recipe pairing the method with an analysis plan; derive; diff; apply preview.
    log.begin("I02 recipe create / derive / diff / apply preview", "real backend (recipe provider)");
    let (_, plan_rev) = save_plan(&mut b, "e2e-plan", &source_a, &graph, Value::Null).await;
    check!(plan_rev == "1", "plan revision 1");
    let create = json!({ "command_id": "e2e-create", "recipe_id": "e2e-recipe", "title": "Fold-in e2e recipe",
        "note": "method used for runs A/B + population plan",
        "method": { "source": "local", "id": "e2e-method", "revision": method_revision },
        "analysis_plan": { "id": "e2e-plan", "revision": "1" }, "profile_base": base });
    let original = b.req_ok("recipe_create", create.clone()).await["recipe"].clone();
    check!(original["revision"] == "1" && original["lineage"]["recipe"].is_null(), "{}", short(&original));
    let derived = b.req_ok("recipe_derive", json!({ "command_id": "e2e-derive", "recipe_id": "e2e-recipe-child",
        "parent": { "id": "e2e-recipe", "revision": "1" }, "note": "next run", "profile_base": base })).await;
    let child = derived["recipe"].clone();
    check!(child["lineage"]["recipe"] == json!({ "id": "e2e-recipe", "revision": "1" }), "lineage: {}", short(&child));
    let diff = b.req_ok("recipe_diff", json!({ "recipe_id": "e2e-recipe-child", "revision": "1", "against": "instrument",
        "profile_base": base })).await;
    let items = diff["items"].as_array().unwrap();
    let dims: Vec<&str> = items.iter().map(|i| i["dimension"].as_str().unwrap()).collect();
    check!(dims == ["method", "profile", "core", "calibration"], "diff dimensions {dims:?}");
    for i in items {
        check!(!i["reason"].as_str().unwrap_or_default().is_empty(), "every item has a reason: {i}");
    }
    check!(diff["applied"] == false, "diff applies nothing");
    log.note(format!("diff vs instrument: {} (overall {})", items.iter().map(|i| format!("{}={}", i["dimension"].as_str().unwrap(),
        i["status"].as_str().unwrap())).collect::<Vec<_>>().join(", "), diff["overall"]));
    for i in items {
        log.note(format!("  {}: {} - {}", i["dimension"].as_str().unwrap(), i["status"].as_str().unwrap(), i["reason"].as_str().unwrap()));
    }
    let preview = b.req_ok("recipe_apply_preview", json!({ "recipe_id": "e2e-recipe-child", "revision": "1", "profile_base": base })).await;
    check!(preview["applied"] == false && preview["hardware_actuated"] == false, "{}", short(&preview));
    check!(preview["requires_explicit_acceptance"] == true, "explicit acceptance required");
    let diff_after = b.req_ok("recipe_diff", json!({ "recipe_id": "e2e-recipe-child", "revision": "1", "against": "instrument",
        "profile_base": base })).await;
    check!(diff_after["items"] == diff["items"], "the preview changed no instrument state");
    let reread = b.req_ok("recipe_get", json!({ "recipe_id": "e2e-recipe", "revision": "1" })).await;
    check!(reread["recipe"] == original, "original unchanged");
    log.note(format!("apply preview: applied=false hardware_actuated=false; changes {}; method_selection {}",
        preview["changes"].as_array().map(|c| c.len()).unwrap_or(0), short(&preview["method_selection"])));
    log.end();

    // ---- F04/F05/F08: open A in review; population graph; exact counts, statistics, histogram, gallery.
    log.begin("F04/F05/F08 population graph, statistics, histogram, gallery", analysis_class);
    let eval = b.req_ok("analysis_read", json!({ "action": "evaluate", "planId": "e2e-plan", "revision": "1", "source": source_a })).await;
    let e = &eval["evaluation"];
    let pop = b.ok("analyze_population", json!({ "request": { "expectedPath": path_a, "feature": "area", "minimum": null,
        "maximum": null, "graph": { "id": "e2e-plan", "revision": "1", "output": "target", "nodes": graph["nodes"] },
        "histogram": { "minimum": 0.0, "maximum": a95 * 2.0, "bins": 64 }, "galleryOffset": 0 } })).await;
    check!(pop["ok"] == true, "{}", short(&pop));
    let pops = pop["populations"].as_array().unwrap();
    let by = |id: &str| pops.iter().find(|p| p["id"] == id).unwrap().clone();
    let (sized, shape, target) = (by("sized"), by("shape"), by("target"));
    let events = dec(&pop["evaluation"]["events"]);
    for p in [&sized, &shape, &target] {
        check!(dec(&p["selected"]) + dec(&p["excluded"]) + dec(&p["unknown"]) == events, "tri-state partition of {events}: {p}");
    }
    check!(dec(&target["selected"]) <= dec(&sized["selected"]).min(dec(&shape["selected"])), "AND within operands");
    check!(dec(&target["selected"]) > 0, "non-empty target");
    let st = &pop;
    check!(dec(&st["selected"]) == dec(&target["selected"]), "statistics over the output population");
    let h = &pop["histogram"];
    let hist_total: u64 = h["counts"].as_array().unwrap().iter().map(dec).sum::<u64>() + dec(&h["underflow"]) + dec(&h["overflow"]);
    check!(hist_total == dec(&h["measured"]), "histogram sum + under + overflow = measured: {hist_total} vs {}", h["measured"]);
    check!(dec(&h["measured"]) + dec(&h["missing"]) == dec(&target["selected"]), "histogram covers n");
    check!(hist_total == dec(&st["measured"]), "histogram total = n measured");
    // The pinned-plan evaluation (analysis_read) and the review population path agree exactly.
    check!(e["measured"] == target["selected"] && e["denominator"] == json!(events.to_string()),
        "plan evaluation n {} / denominator {} vs population {} of {events}", e["measured"], e["denominator"], target["selected"]);
    for k in ["mean", "median", "p05", "p95", "cvPercent"] {
        check!(e[k] == pop[k], "{k}: evaluate {} vs analyze_population {}", e[k], pop[k]);
    }
    check!(e["populationComplete"] == true, "complete population");
    let gallery = b.req_ok("analysis_read", json!({ "action": "gallery", "planId": "e2e-plan", "revision": "1", "source": source_a, "offset": 0 })).await;
    let images = gallery["images"].as_array().unwrap();
    check!(!images.is_empty() && images.len() <= 12, "gallery page");
    check!(dec(&gallery["selected"]) == dec(&target["selected"]), "gallery denominator = n");
    let with_pixels = images.iter().filter(|i| i["pixels"].is_array()).count();
    log.note(format!("events {events}; sized {} / shape {} / target {} selected; n {} mean {:.1} median {:.1} cv {:.1}% p05 {:.1} p95 {:.1} px2",
        sized["selected"], shape["selected"], target["selected"], st["selected"], st["mean"].as_f64().unwrap_or(f64::NAN),
        st["median"].as_f64().unwrap_or(f64::NAN), st["cvPercent"].as_f64().unwrap_or(f64::NAN),
        st["p05"].as_f64().unwrap_or(f64::NAN), st["p95"].as_f64().unwrap_or(f64::NAN)));
    log.note(format!("histogram 64 bins: sum {} + under {} + over {} = {hist_total} = measured; gallery {} tiles ({with_pixels} with pixels), selected {}",
        h["counts"].as_array().unwrap().iter().map(dec).sum::<u64>(), h["underflow"], h["overflow"], images.len(), gallery["selected"]));
    log.end();

    // ---- F09: ComparisonSpec over A and B (1 unit per group: insufficient), then >= 3 units and a 4PL fit.
    log.begin("F09 comparison A vs B (insufficient)", analysis_class);
    let retained_a = b.req_ok("analysis_write", json!({ "action": "retainSource", "source": source_a, "commandId": "retain-a" })).await;
    let snap_a = b.req_ok("analysis_read", json!({ "action": "snapshot", "source": source_a })).await;
    let area_unit = snap_a["source"]["parameters"]["area"].clone();
    check!(area_unit.is_string(), "declared area unit: {}", short(&snap_a["source"]["parameters"]));
    check!(b.ok("load_recording", json!({ "filePath": path_b })).await["ok"] == true, "open B");
    let (_, plan_b_rev) = save_plan(&mut b, "e2e-plan-b", &source_b, &graph, Value::Null).await;
    let retained_b = b.req_ok("analysis_write", json!({ "action": "retainSource", "source": source_b, "commandId": "retain-b" })).await;
    let metric = json!({ "population": "target", "parameter": "area", "statistic": "median", "unit": area_unit });
    let spec = json!({ "schema": "F-comparison-spec/1", "metric": metric, "design": "unpaired", "groups": ["control", "treated"],
        "inference": "both", "samples": [
            { "sampleId": "A", "group": "control", "experimentalUnit": "unit-A", "planId": "e2e-plan", "revision": "1", "source": retained_a["descriptor"] },
            { "sampleId": "B", "group": "treated", "experimentalUnit": "unit-B", "planId": "e2e-plan-b", "revision": plan_b_rev, "source": retained_b["descriptor"] } ] });
    let cmp = b.ok("analysis_read", json!({ "request": { "action": "comparisonSpec", "spec": spec } })).await;
    let c = &cmp["comparison"];
    let insufficient = cmp["status"] == "insufficient" || c["status"] == "insufficient" || c["inference"]["status"] == "insufficient";
    check!(insufficient, "1 unit per group is insufficient: {}", short(&cmp));
    check!(!c["inference"].to_string().contains("pValue\":0") && c["inference"]["tests"].as_array().map(|t| t.is_empty()).unwrap_or(true),
        "no test or p-value: {}", short(&c["inference"]));
    log.note(format!("status {} / inference {}; groups {}; reasons {}", cmp["status"], c["inference"]["status"],
        c["groups"].as_array().map(|g| g.iter().map(|x| format!("{}: n={} median-of-units {}", x["group"], x["units"], x["mean"])).collect::<Vec<_>>().join("; ")).unwrap_or_default(),
        short(&c["inference"]["reasons"])));
    log.end();

    log.begin("F09 comparison >= 3 units/group + 4PL fit (generated FCS units)", "synthetic data (generated FCS 3.0) / real backend");
    let fcs = root.join("fcs");
    std::fs::create_dir_all(&fcs).unwrap();
    let mut samples = Vec::new();
    for (g, center, group) in [(0u64, 1000.0, "control"), (1, 1300.0, "treated")] {
        for u in 0..4u64 {
            let p = fcs.join(format!("{group}_{u}.fcs"));
            fcs_unit(&p, center * (1.0 + 0.03 * (u as f64 - 1.5)), 100 + g * 10 + u);
            let source = json!({ "kind": "fcs", "path": p.to_string_lossy() });
            let (desc, rev) = save_plan(&mut b, &format!("fcs-{group}-{u}"), &source, &all_graph(), Value::Null).await;
            samples.push(json!({ "sampleId": format!("{group}-{u}"), "group": group, "experimentalUnit": format!("{group}-unit-{u}"),
                "planId": format!("fcs-{group}-{u}"), "revision": rev, "source": source, "_unit": desc["parameters"]["area"] }));
        }
    }
    let fcs_unit_label = samples[0]["_unit"].clone();
    for s in samples.iter_mut() {
        s.as_object_mut().unwrap().remove("_unit");
    }
    let fmetric = json!({ "population": "all", "parameter": "area", "statistic": "median", "unit": fcs_unit_label });
    let spec = json!({ "schema": "F-comparison-spec/1", "metric": fmetric, "design": "unpaired", "groups": ["control", "treated"],
        "inference": "both", "samples": samples });
    let cmp = b.req_ok("analysis_read", json!({ "action": "comparisonSpec", "spec": spec })).await;
    let c = &cmp["comparison"];
    check!(c["inference"]["status"] == "computed", "4 units per group: inference computed: {}", short(c));
    let groups = c["groups"].as_array().unwrap();
    check!(groups.iter().all(|g| g["units"] == 4), "n counts units, not events: {}", short(&c["groups"]));
    let welch = c["inference"]["tests"].as_array().unwrap().iter().find(|t| t["test"] == "welch-t").cloned().expect("welch-t");
    check!(welch["status"] == "computed" && welch["pValue"].as_f64().unwrap() < 0.01, "shift detected: {welch}");
    log.note(format!("4 vs 4 units: Welch t {:.2} p {:.2e}; tests {}", welch["statistic"].as_f64().unwrap(), welch["pValue"].as_f64().unwrap(),
        c["inference"]["tests"].as_array().unwrap().iter().map(|t| format!("{}:{}", t["test"].as_str().unwrap(), t["status"].as_str().unwrap())).collect::<Vec<_>>().join(", ")));

    // 4PL: 7 concentrations x 3 units; response = bottom + (top - bottom) / (1 + (ec50 / c)^hill).
    let (bottom, top, ec50, hill) = (800.0f64, 2400.0f64, 1.0f64, 1.2f64);
    let mut fit_samples = Vec::new();
    for (k, conc) in [0.01, 0.04, 0.16, 0.63, 2.5, 10.0, 40.0].iter().enumerate() {
        let response = bottom + (top - bottom) / (1.0 + (ec50 / conc).powf(hill));
        for r in 0..3u64 {
            let id = format!("fit-{k}-{r}");
            let p = fcs.join(format!("{id}.fcs"));
            fcs_unit(&p, response * (1.0 + 0.02 * (r as f64 - 1.0)), 1000 + k as u64 * 10 + r);
            let source = json!({ "kind": "fcs", "path": p.to_string_lossy() });
            let (_, rev) = save_plan(&mut b, &id, &source, &all_graph(), Value::Null).await;
            fit_samples.push(json!({ "sampleId": id, "experimentalUnit": format!("unit-{k}-{r}"),
                "concentration": { "value": conc, "unit": "uM" }, "planId": id, "revision": rev, "source": source }));
        }
    }
    let fit_spec = json!({ "schema": "F-fit-spec/1", "metric": fmetric, "model": "4PL", "concentrationUnit": "uM",
        "concentrationScale": "log10", "weighting": "none", "design": "independent-units", "samples": fit_samples });
    let fit = b.req_ok("analysis_read", json!({ "action": "concentrationFit", "spec": fit_spec })).await;
    let f = &fit["fit"];
    check!(f["status"] == "fit" && f["model"] == "4PL", "a 4PL fit: {}", short(&fit));
    check!(f["distinctConcentrations"] == 7 && f["unitPoints"] == 21, "7 concentrations x 3 units: {} / {}", f["distinctConcentrations"], f["unitPoints"]);
    let fit_text = f.to_string();
    let ec50_fit = f["ec50"]["value"].as_f64().or_else(|| f["ec50"].as_f64());
    check!(ec50_fit.is_some(), "EC50 reported: {}", short(f));
    let ec50_fit = ec50_fit.unwrap();
    check!(ec50_fit > 0.5 && ec50_fit < 2.0, "EC50 {ec50_fit} near the generating 1.0 uM");
    check!(f["converged"] == true, "converged: {}", short(f));
    log.note(format!("4PL over 21 units: EC50 {ec50_fit:.3} uM (true 1.0), R2 {}, termination {}, suspect {} {}",
        f["rSquared"], f["termination"], f["suspect"], short(&f["suspectReasons"])));
    let _ = fit_text;
    // Insufficient fit input yields no parameters.
    let few = json!({ "schema": "F-fit-spec/1", "metric": fmetric, "model": "4PL", "concentrationUnit": "uM",
        "concentrationScale": "log10", "weighting": "none", "design": "independent-units", "samples": fit_samples[..3].to_vec() });
    let none = b.ok("analysis_read", json!({ "request": { "action": "concentrationFit", "spec": few } })).await;
    check!(none["fit"]["status"] == "insufficient" || none["status"] == "insufficient", "3 units at one concentration: {}", short(&none));
    log.note(format!("one-concentration fit: {} ({})", none["fit"]["status"], short(&none["fit"]["reasons"])));
    log.end();

    // ---- I12/F12 part 1: save the workspace and a second plan revision; remember the old tokens.
    log.begin("I12/F12 save workspace", "real backend (SQLite workspace store)");
    check!(b.ok("load_recording", json!({ "filePath": path_a })).await["ok"] == true, "open A");
    let mut graph2 = graph.clone();
    graph2["revision"] = json!("draft2");
    graph2["nodes"][0]["minimum"] = json!(a50);
    let (_, plan_rev2) = save_plan(&mut b, "e2e-plan", &source_a, &graph2, json!("1")).await;
    check!(plan_rev2 == "2", "second plan revision");
    let plan_history = b.req_ok("analysis_read", json!({ "action": "history", "planId": "e2e-plan" })).await;
    let plan_v1 = b.req_ok("analysis_read", json!({ "action": "read", "planId": "e2e-plan", "revision": "1" })).await;
    let recipe_ref = json!({ "schema_version": 1, "id": child["id"], "revision": "1", "title": child["title"], "question": "Fold-in e2e",
        "method": child["method"], "analysis_plan": child["analysis_plan"], "lineage": child["lineage"] });
    let rows: Vec<Value> = ["A", "B"].iter().map(|s| json!({ "ref": s, "sample": format!("sample {s}"),
            "condition": if *s == "A" { "control" } else { "treated" }, "experimentalUnit": format!("unit-{s}"), "concentrationValue": "",
            "concentrationUnit": "", "channel": "", "medium": "", "temperatureC": "", "notes": "", "free": [] })).collect();
    let document = json!({ "schema_version": 1, "title": "Fold-in e2e workspace", "plan": { "experimentId": experiment_id, "format": "single",
        "operator": { "name": "fold-in e2e", "id": "" }, "rows": rows },
        "recipe": recipe_ref, "findings": [] });
    let ws1 = b.workspace(json!({ "action": "commit", "workspace_id": "e2e-workspace", "expected_revision": null,
        "command_id": "e2e-ws-1", "document": document }), true).await;
    check!(ws1["status"] == "ok" && ws1["revision"] == "1", "{}", short(&ws1));
    let mut doc2 = document.clone();
    doc2["title"] = json!("Fold-in e2e workspace (after runs A/B)");
    let ws2 = b.workspace(json!({ "action": "commit", "workspace_id": "e2e-workspace", "expected_revision": "1",
        "command_id": "e2e-ws-2", "document": doc2 }), true).await;
    check!(ws2["status"] == "ok" && ws2["revision"] == "2", "{}", short(&ws2));
    let eval_before = b.req_ok("analysis_read", json!({ "action": "evaluate", "planId": "e2e-plan", "revision": "1", "source": source_a })).await;
    let recipes_before = b.req_ok("recipe_list", json!({})).await;
    log.note(format!("workspace e2e-workspace revisions 1,2; plan e2e-plan revisions {}; recipes {}",
        plan_history["revisions"], recipes_before["recipes"].as_array().unwrap().len()));
    let old_session = b.ok("camera_feature_tree", json!({ "request": { "modules": ["remote"] } })).await["session"].clone();
    let old_session = if old_session.is_null() { session2.clone() } else { old_session };
    let old_discovery = discovery.clone();
    let _ = b.ok("close_review", Value::Null).await;
    let _ = b.ok("stop_capture", Value::Null).await;
    log.end();

    // ---- I12/F12 part 2 + E01: restart the backend process on the same data dir.
    log.begin("I12/F12 + E01 restart: reopen and stale refusal", "real backend (new process) / mock camera");
    b.stop(&log).await;
    let mut b = spawn_backend(&data, &log).await;
    check!(b.ok("init", json!({ "dataDir": "" })).await == json!(true), "init after restart");
    let ws = b.workspace(json!({ "action": "read", "workspace_id": "e2e-workspace" }), false).await;
    check!(ws["status"] == "ok" && ws["revision"] == "2" && ws["document"] == doc2, "workspace head survives: {}", short(&ws));
    let hist = b.workspace(json!({ "action": "history", "workspace_id": "e2e-workspace" }), false).await;
    check!(hist["revisions"] == json!(["1", "2"]), "workspace revisions survive: {}", short(&hist));
    let r1 = b.workspace(json!({ "action": "read", "workspace_id": "e2e-workspace", "revision": "1" }), false).await;
    check!(r1["document"] == document, "revision 1 is immutable");
    let ph = b.req_ok("analysis_read", json!({ "action": "history", "planId": "e2e-plan" })).await;
    check!(ph["revisions"] == plan_history["revisions"], "plan revisions survive: {} vs {}", ph["revisions"], plan_history["revisions"]);
    let pv1 = b.req_ok("analysis_read", json!({ "action": "read", "planId": "e2e-plan", "revision": "1" })).await;
    check!(pv1["document"] == plan_v1["document"], "plan definition revision 1 survives");
    let child_after = b.req_ok("recipe_get", json!({ "recipe_id": "e2e-recipe-child", "revision": "1" })).await;
    check!(child_after["recipe"] == child, "derived recipe survives byte for byte");
    check!(child_after["recipe"]["lineage"]["recipe"] == json!({ "id": "e2e-recipe", "revision": "1" }), "lineage survives");
    let parent_after = b.req_ok("recipe_get", json!({ "recipe_id": "e2e-recipe", "revision": "1" })).await;
    check!(parent_after["recipe"] == original, "parent recipe survives");
    let recipes_after = b.req_ok("recipe_list", json!({})).await;
    check!(recipes_after["recipes"] == recipes_before["recipes"], "recipe list survives");
    check!(b.ok("load_recording", json!({ "filePath": path_a })).await["ok"] == true, "reopen A");
    let eval_after = b.req_ok("analysis_read", json!({ "action": "evaluate", "planId": "e2e-plan", "revision": "1", "source": source_a })).await;
    let strip = |v: &Value| { let mut v = v["evaluation"].clone(); if let Some(o) = v.as_object_mut() { o.remove("membershipKey"); o.remove("evaluatedAt"); o.remove("cached"); } v };
    let (eb, ea) = (strip(&eval_before), strip(&eval_after));
    let same_counts = ["selected", "excluded", "unknown", "denominator", "graphFingerprint", "mean", "median"].iter().all(|k| eb[k] == ea[k]);
    check!(same_counts, "pinned plan re-evaluates to the same exact answer after restart: {} vs {}", short(&eb), short(&ea));
    log.note("workspace head/history, plan revisions 1-2, recipe + child lineage, recipe list and the pinned evaluation all survive the process restart");

    // E01: the old backend's feature token and discovery job are stale here.
    let cfg = b.ok("configure_mock", json!({ "frameDir": mock_frames(), "frameIntervalMs": 10, "loopFiles": true })).await;
    check!(cfg["ok"] == true, "{}", short(&cfg));
    check!(b.ok("start_capture", Value::Null).await["ok"] == true, "capture after restart");
    let tree2 = open_tree(&mut b).await;
    let fresh = tree2["session"].clone();
    check!(fresh["instance"] != old_session["instance"], "a new process draws a new instance");
    let stale_read = b.ok("camera_feature_read", feature_request(&old_session, &wname, None)).await;
    check!(stale_read["outcome"] == "stale" && stale_read["session"] == fresh, "old token stale on read: {}", short(&stale_read));
    let stale_write = b.ok("camera_feature_write", feature_request(&old_session, &wname, Some(value.clone()))).await;
    check!(stale_write["outcome"] == "stale" && stale_write["readback"].is_null(), "old token stale on write: {}", short(&stale_write));
    let new_job = b.ok("start_camera_discovery", Value::Null).await;
    check!(new_job["instance"] != old_discovery["instance"], "new discovery instance");
    let old = b.ok("fetch_device_discovery", json!({ "jobId": old_discovery["job_id"], "instance": old_discovery["instance"] })).await;
    check!(old["stale"] == true && old["valid"] == false && old["job_id"] == "0", "old discovery job stale: {}", short(&old));
    let cancelled = b.ok("cancel_device_discovery", json!({ "jobId": old_discovery["job_id"], "instance": old_discovery["instance"] })).await;
    check!(cancelled == json!(false), "old cancel touches nothing");
    let current = finish_discovery(&mut b, &new_job).await;
    check!(current["state"] != 3, "the new job was not cancelled");
    log.note(format!("old token (instance {}) -> stale for read and write; old discovery job {} (instance {}) -> stale, cancel false; new instance {}",
        old_session["instance"], old_discovery["job_id"], old_discovery["instance"], fresh["instance"]));
    let _ = b.ok("stop_capture", Value::Null).await;
    log.end();

    b.stop(&log).await;
    log.table();
    let _ = std::fs::remove_dir_all(&root);
}
