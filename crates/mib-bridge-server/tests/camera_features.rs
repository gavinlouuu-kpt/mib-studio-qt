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
    let session = json!({"source": "mock", "generation": "1", "capability_revision": "1"});
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
