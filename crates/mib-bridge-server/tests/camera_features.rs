//! E02 (#806 E-1/E-2, bridge ABI 49): the camera's GenApi node map through the real server,
//! the real backend and the mock camera's fake node map. Evidence class: fixture/mock (never
//! hardware). No feature name is written here: every name comes from the enumerated tree.
//! enumerate -> read -> write with readback -> refused write on a read-only node -> stale
//! generation refused after a capture restart. Backend state is process-global: serial.
//!
//! Feature I/O is refused while the sort trigger is armed or running (it is bound and started
//! with every capture session in the default boot): `feature_io_refused_while_the_sort_trigger_is_armed`
//! proves that through the server; the round trip boots with the trigger service disabled
//! (`MIB_DISABLED_SERVICES=trigger`), the only state in which a node map may be touched.

use std::sync::Arc;
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use mib_app_commands::AppState;
use mib_bridge_server::{Server, ServerConfig};
use serde_json::{json, Value};
use serial_test::serial;
use tokio_tungstenite::tungstenite::Message;

type Socket =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

const TOKEN: &str = "test-token";

async fn start(tag: &str) -> (Arc<Server>, std::net::SocketAddr, std::path::PathBuf) {
    let data = std::env::temp_dir().join(format!("yofo_server_{tag}_{}", std::process::id()));
    std::fs::create_dir_all(&data).unwrap();
    let mut config = ServerConfig::new(data.to_string_lossy());
    config.token = Some(TOKEN.into());
    config.ping_interval = Duration::from_millis(200);
    config.ping_timeout = Duration::from_secs(2);
    config.client_grace = Duration::from_millis(500);
    let server = Server::new(config, Arc::new(AppState::new()));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(server.clone().serve(listener, std::future::pending()));
    (server, addr, data)
}

async fn connect(addr: std::net::SocketAddr) -> Socket {
    tokio_tungstenite::connect_async(format!("ws://{addr}/ws?token={TOKEN}"))
        .await
        .unwrap()
        .0
}

/// One request; the JSON reply (`Ok`) or the error string (`Err`), skipping events and pings.
async fn call(ws: &mut Socket, id: u64, cmd: &str, args: Value) -> Result<Value, String> {
    let text = json!({ "request_id": id, "cmd": cmd, "args": args }).to_string();
    ws.send(Message::Text(text.into())).await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let message = tokio::time::timeout_at(deadline.into(), ws.next())
            .await
            .expect("reply in time")
            .unwrap()
            .unwrap();
        if let Message::Text(text) = message {
            let value: Value = serde_json::from_str(text.as_str()).unwrap();
            if value.get("event").is_some() || value.get("session").is_some() {
                continue;
            }
            assert_eq!(value["request_id"], json!(id), "{value}");
            return match value.get("error") {
                Some(error) => Err(error.as_str().unwrap().to_string()),
                None => Ok(value["ok"].clone()),
            };
        }
    }
}

async fn ok(ws: &mut Socket, id: u64, cmd: &str, args: Value) -> Value {
    call(ws, id, cmd, args).await.unwrap_or_else(|e| panic!("{cmd}: {e}"))
}

fn mock_frames() -> String {
    std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../data/mock_frames")
        .to_string_lossy()
        .into_owned()
}

/// The tree once the capture session's node map is open (the camera is running).
async fn open_tree(ws: &mut Socket, id: u64) -> Value {
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        let tree = ok(ws, id, "camera_feature_tree", json!({"request": {"modules": ["remote"]}})).await;
        if tree["outcome"] == "read" {
            return tree;
        }
        assert!(Instant::now() < deadline, "node map never opened: {tree}");
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

fn request(session: &Value, feature: &str, value: Option<Value>) -> Value {
    let mut r = json!({"session": session, "module": "remote", "feature": feature});
    if let Some(v) = value {
        r["value"] = v;
    }
    json!({ "request": r })
}

/// Boots the backend with or without the sort trigger service (read at init).
fn trigger_service(enabled: bool) {
    if enabled {
        std::env::remove_var("MIB_DISABLED_SERVICES");
    } else {
        std::env::set_var("MIB_DISABLED_SERVICES", "trigger");
    }
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn feature_io_refused_while_the_sort_trigger_is_armed() {
    trigger_service(true);
    let (server, addr, data) = start("camera_features_trigger").await;
    let mut ws = connect(addr).await;
    ok(&mut ws, 1, "init", json!({"dataDir": ""})).await;
    ok(&mut ws, 2, "configure_mock",
       json!({"frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true})).await;
    let started = ok(&mut ws, 3, "start_capture", Value::Null).await;
    assert_eq!(started["ok"], true, "{started}");
    // The trigger binds to the capture camera; from then on every feature operation is refused
    // before the device, with the reason (not an error reply, not an empty tree).
    let deadline = Instant::now() + Duration::from_secs(15);
    let tree = loop {
        let tree = ok(&mut ws, 4, "camera_feature_tree", json!({"request": {"modules": ["remote"]}})).await;
        if tree["reason"].as_str().is_some_and(|r| r.contains("trigger")) {
            break tree;
        }
        assert!(Instant::now() < deadline, "the trigger never armed: {tree}");
        tokio::time::sleep(Duration::from_millis(20)).await;
    };
    assert_eq!(tree["outcome"], "refused", "{tree}");
    assert_eq!(tree["modules"], json!([]), "nothing enumerated: {tree}");
    let session = json!({"instance": "x", "source": "mock", "generation": "1", "capability_revision": "1"});
    let read = ok(&mut ws, 5, "camera_feature_read", request(&session, "Any", None)).await;
    assert_eq!(read["outcome"], "refused", "{read}");
    assert!(read["reason"].as_str().unwrap().contains("trigger"), "{read}");
    let write = ok(&mut ws, 6, "camera_feature_write",
                   request(&session, "Any", Some(json!({"type": "float", "value": 1.0})))).await;
    assert_eq!(write["outcome"], "refused", "{write}");
    assert!(write["reason"].as_str().unwrap().contains("trigger"), "{write}");
    assert!(write["readback"].is_null(), "the device was not touched: {write}");
    ok(&mut ws, 7, "stop_capture", Value::Null).await;
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(data);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn mock_node_map_round_trip_through_the_server() {
    trigger_service(false);
    let (server, addr, data) = start("camera_features").await;
    let mut ws = connect(addr).await;
    ok(&mut ws, 1, "init", json!({"dataDir": ""})).await;
    assert_eq!(ok(&mut ws, 2, "abi_version", Value::Null).await, json!(49));

    // No camera session yet: the tree is refused (not empty, not an error reply).
    let none = ok(&mut ws, 3, "camera_feature_tree", Value::Null).await;
    assert_eq!(none["outcome"], "refused", "{none}");
    assert!(none["session"].is_null());

    ok(&mut ws, 4, "configure_mock",
       json!({"frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true})).await;
    let started = ok(&mut ws, 5, "start_capture", Value::Null).await;
    assert_eq!(started["ok"], true, "{started}");

    // 1. Enumerate: categories and typed features from the node map.
    let tree = open_tree(&mut ws, 6).await;
    assert_eq!(tree["evidence"], "fixture/mock", "the mock is fixture evidence, never device");
    let session1 = tree["session"].clone();
    assert_eq!(session1["capability_revision"], "1");
    let remote = &tree["modules"][0];
    assert_eq!(remote["module"], "remote");
    assert_eq!(remote["enumerated"], true, "{remote}");
    let categories = remote["categories"].as_array().unwrap();
    assert!(!remote["roots"].as_array().unwrap().is_empty() && !categories.is_empty());
    // Every category's features are enumerated features with descriptors; nested categories exist.
    let names: Vec<&str> = remote["features"].as_array().unwrap().iter()
        .map(|f| f["name"].as_str().unwrap()).collect();
    for c in categories {
        for f in c["features"].as_array().unwrap() {
            assert!(names.contains(&f.as_str().unwrap()), "{f} listed and enumerated");
        }
    }
    assert!(categories.iter().any(|c| !c["categories"].as_array().unwrap().is_empty()), "nested categories");
    let kinds: std::collections::BTreeSet<&str> = remote["features"].as_array().unwrap().iter()
        .map(|f| f["descriptor"]["kind"].as_str().unwrap()).collect();
    assert!(kinds.len() >= 3, "several GenApi types: {kinds:?}");

    let pick = |pred: &dyn Fn(&Value) -> bool| -> (String, Value) {
        let f = remote["features"].as_array().unwrap().iter()
            .find(|f| pred(&f["descriptor"])).expect("fixture has the shape");
        (f["name"].as_str().unwrap().to_string(), f["descriptor"].clone())
    };
    let (writable, wd) = pick(&|d| d["kind"] == "float" && d["writable"] == true
        && d["minimum"].is_object() && d["maximum"].is_object());
    let (read_only, _) = pick(&|d| d["readable"] == true && d["writable"] == false);

    // 2. Read by name under the token.
    let read = ok(&mut ws, 7, "camera_feature_read", request(&session1, &writable, None)).await;
    assert_eq!(read["outcome"], "read", "{read}");
    assert_eq!(read["readback"]["kind"], "float");
    assert_eq!(read["session"], session1);

    // 3. Write with readback; requested and readback are separate members.
    let lo = wd["minimum"]["value"].as_f64().unwrap();
    let hi = wd["maximum"]["value"].as_f64().unwrap();
    let target = lo + (hi - lo) / 4.0;
    let value = json!({"type": "float", "value": target});
    let written = ok(&mut ws, 8, "camera_feature_write", request(&session1, &writable, Some(value.clone()))).await;
    assert_eq!(written["outcome"], "readback", "{written}");
    assert_eq!(written["requested"], value);
    assert_eq!(written["readback"]["value"]["value"].as_f64(), Some(target));
    assert_eq!(written["session"]["capability_revision"], "2", "a write advances the revision");
    let session2 = written["session"].clone();
    let again = ok(&mut ws, 9, "camera_feature_read", request(&session2, &writable, None)).await;
    assert_eq!(again["readback"]["value"]["value"].as_f64(), Some(target), "the device holds it");

    // The pre-write view is stale now: refused without reaching the port.
    let old_view = ok(&mut ws, 10, "camera_feature_write", request(&session1, &writable, Some(value.clone()))).await;
    assert_eq!(old_view["outcome"], "stale", "{old_view}");

    // 4. A read-only node: refused, with the re-read descriptor saying why.
    let refused = ok(&mut ws, 11, "camera_feature_write",
                     request(&session2, &read_only, Some(json!({"type": "string", "value": "x"})))).await;
    assert_eq!(refused["outcome"], "refused", "{refused}");
    assert_eq!(refused["readback"]["writable"], false);
    assert!(!refused["reason"].as_str().unwrap().is_empty());
    assert_eq!(refused["session"], session2, "a refusal does not advance the revision");

    // An out-of-range request is refused by the device's own bounds, distinct from read-only.
    let out_of_range = ok(&mut ws, 12, "camera_feature_write",
                          request(&session2, &writable, Some(json!({"type": "float", "value": hi * 2.0 + 1.0})))).await;
    assert_eq!(out_of_range["outcome"], "refused", "{out_of_range}");
    assert_eq!(out_of_range["readback"]["writable"], true);

    // A malformed request is an error before the backend.
    let bad = call(&mut ws, 13, "camera_feature_read", json!({"request": [1]})).await;
    assert!(bad.unwrap_err().starts_with("INVALID_ARGUMENTS"));

    // A viewer may read but not write.
    let mut viewer = connect(addr).await;
    ok(&mut viewer, 1, "init", json!({"dataDir": ""})).await;
    let seen = ok(&mut viewer, 2, "camera_feature_read", request(&session2, &writable, None)).await;
    assert_eq!(seen["outcome"], "read");
    let denied = call(&mut viewer, 3, "camera_feature_write", request(&session2, &writable, Some(value.clone()))).await;
    assert!(denied.unwrap_err().starts_with("VIEWER_ONLY"));
    viewer.close(None).await.unwrap();

    // 5. Stop and restart capture: a new generation; the old token is refused as stale.
    ok(&mut ws, 14, "stop_capture", Value::Null).await;
    let closed = ok(&mut ws, 15, "camera_feature_read", request(&session2, &writable, None)).await;
    assert_eq!(closed["outcome"], "stale", "closed session: {closed}");
    let restarted = ok(&mut ws, 16, "start_capture", Value::Null).await;
    assert_eq!(restarted["ok"], true, "{restarted}");
    let tree2 = open_tree(&mut ws, 17).await;
    let session3 = tree2["session"].clone();
    assert_ne!(session3["generation"], session2["generation"], "restart opens a new generation");
    let stale = ok(&mut ws, 18, "camera_feature_write", request(&session2, &writable, Some(value.clone()))).await;
    assert_eq!(stale["outcome"], "stale", "{stale}");
    assert_eq!(stale["session"], session3, "the reply names the current session");
    let fresh = ok(&mut ws, 19, "camera_feature_read", request(&session3, &writable, None)).await;
    assert_eq!(fresh["outcome"], "read", "{fresh}");

    ok(&mut ws, 20, "stop_capture", Value::Null).await;
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    trigger_service(true);
    let _ = std::fs::remove_dir_all(data);
}

/// One backend instance behind a fresh server: init, mock capture running, the remote tree read.
/// The trigger service is disabled: feature I/O is refused while it is armed or running.
async fn backend_with_tree(tag: &str) -> (Arc<Server>, Socket, std::path::PathBuf, Value) {
    trigger_service(false);
    let (server, addr, data) = start(tag).await;
    let mut ws = connect(addr).await;
    ok(&mut ws, 1, "init", json!({"dataDir": ""})).await;
    ok(&mut ws, 2, "configure_mock",
       json!({"frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true})).await;
    let started = ok(&mut ws, 3, "start_capture", Value::Null).await;
    assert_eq!(started["ok"], true, "{started}");
    let tree = open_tree(&mut ws, 4).await;
    (server, ws, data, tree)
}

async fn shut_down(server: Arc<Server>, mut ws: Socket, data: std::path::PathBuf) {
    ok(&mut ws, 900, "stop_capture", Value::Null).await;
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    trigger_service(true);
    let _ = std::fs::remove_dir_all(data);
}

/// Poll a camera discovery job of this backend to its end (checked by instance).
async fn finish_discovery(ws: &mut Socket, id: u64, start: &Value) -> Value {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        let snap = ok(ws, id, "fetch_device_discovery",
                      json!({"jobId": start["job_id"], "instance": start["instance"]})).await;
        assert_eq!(snap["stale"], false, "{snap}");
        assert_eq!(snap["valid"], true, "{snap}");
        if snap["state"].as_u64().unwrap() >= 2 {
            return snap;
        }
        assert!(Instant::now() < deadline, "discovery never ended: {snap}");
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

/// E01 (#806 E-4): a backend restart. The second backend instance numbers capture generations,
/// capability revisions and discovery jobs from the start again, so the first instance's
/// tokens can equal the second's member for member except `instance`. Every such token is
/// refused: the feature commands answer stale without touching the port, and the discovery
/// fetch/cancel answer stale/false without touching the new job. Evidence: fixture/mock.
#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn tokens_from_before_a_backend_restart_are_refused() {
    // Backend instance 1.
    let (server, mut ws, data, tree) = backend_with_tree("restart_before").await;
    let before = tree["session"].clone();
    let remote = &tree["modules"][0];
    let writable = remote["features"].as_array().unwrap().iter()
        .find(|f| f["descriptor"]["kind"] == "float" && f["descriptor"]["writable"] == true)
        .map(|f| f["name"].as_str().unwrap().to_string())
        .expect("fixture has a writable float");
    let discovery = ok(&mut ws, 5, "start_camera_discovery", Value::Null).await;
    assert_eq!(discovery["accepted"], true, "{discovery}");
    assert_eq!(discovery["instance"].as_str().unwrap().len(), 36, "instance is a UUID: {discovery}");
    let finished = finish_discovery(&mut ws, 6, &discovery).await;
    assert_eq!(finished["instance"], discovery["instance"]);
    assert_eq!(before["instance"].as_str().unwrap().len(), 36, "token names its instance: {before}");
    shut_down(server, ws, data).await;

    // Backend instance 2: a new process would construct exactly this.
    let (server, mut ws, data, tree) = backend_with_tree("restart_after").await;
    let after = tree["session"].clone();
    assert_ne!(after["instance"], before["instance"], "a new backend draws a new instance");
    for member in ["source", "generation", "capability_revision"] {
        assert_eq!(after[member], before[member], "the restart reuses {member}: {before} vs {after}");
    }

    // The old token is stale for read and write; the write never reaches the port.
    let read = ok(&mut ws, 10, "camera_feature_read", request(&before, &writable, None)).await;
    assert_eq!(read["outcome"], "stale", "{read}");
    assert_eq!(read["session"], after, "the stale reply names the current session");
    let value = json!({"type": "float", "value": 1.0});
    let write = ok(&mut ws, 11, "camera_feature_write", request(&before, &writable, Some(value))).await;
    assert_eq!(write["outcome"], "stale", "{write}");
    assert!(write["readback"].is_null(), "{write}");
    let fresh = ok(&mut ws, 12, "camera_feature_read", request(&after, &writable, None)).await;
    assert_eq!(fresh["outcome"], "read", "the current token reads: {fresh}");
    assert_eq!(fresh["session"], after, "a refused write did not advance the revision");

    // The old discovery job identity is stale on the new backend, even when the new backend
    // has a job with the same ID.
    let reused = ok(&mut ws, 13, "start_camera_discovery", Value::Null).await;
    assert_eq!(reused["accepted"], true, "{reused}");
    assert_ne!(reused["instance"], discovery["instance"]);
    let old = ok(&mut ws, 14, "fetch_device_discovery",
                 json!({"jobId": discovery["job_id"], "instance": discovery["instance"]})).await;
    assert_eq!(old["stale"], true, "{old}");
    assert_eq!(old["valid"], false, "{old}");
    assert_eq!(old["job_id"], "0", "nothing of a current job is reported: {old}");
    assert!(old["candidates"].as_array().unwrap().is_empty(), "{old}");
    assert_eq!(old["instance"], reused["instance"], "the stale reply names the current instance");
    let cancelled = ok(&mut ws, 15, "cancel_device_discovery",
                       json!({"jobId": discovery["job_id"], "instance": discovery["instance"]})).await;
    assert_eq!(cancelled, json!(false), "a cancel from before the restart touches nothing");
    let current = finish_discovery(&mut ws, 16, &reused).await;
    assert_ne!(current["state"], 3, "the new job was not cancelled: {current}");
    shut_down(server, ws, data).await;
}
