//! E05/E06 buffer acceptance against the real backend with synthetic camera input.
//! Backend state is process-global, so the tests run serially.

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

/// One request; returns the reply (text JSON or binary payload after the id), skipping events
/// and pings but recording events in `events`.
async fn call(
    ws: &mut Socket,
    id: u64,
    cmd: &str,
    args: Value,
    events: &mut Vec<Value>,
) -> Result<Reply, String> {
    call_text(
        ws,
        id,
        json!({ "request_id": id, "cmd": cmd, "args": args }).to_string(),
        events,
    )
    .await
}

/// The same with a request frame written by hand (the exact bytes a client sends).
async fn call_text(
    ws: &mut Socket,
    id: u64,
    text: String,
    events: &mut Vec<Value>,
) -> Result<Reply, String> {
    ws.send(Message::Text(text.into())).await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let message = tokio::time::timeout_at(deadline.into(), ws.next())
            .await
            .expect("reply in time")
            .unwrap()
            .unwrap();
        match message {
            Message::Text(text) => {
                let value: Value = serde_json::from_str(text.as_str()).unwrap();
                if value.get("event").is_some() {
                    events.push(value["event"].clone());
                    continue;
                }
                if value.get("session").is_some() {
                    continue; // control changes; the tests read server.controller()
                }
                assert_eq!(value["request_id"], json!(id), "{value}");
                return match value.get("error") {
                    Some(error) => Err(error.as_str().unwrap().to_string()),
                    None => Ok(Reply::Json(value["ok"].clone())),
                };
            }
            Message::Binary(bytes) => {
                assert_eq!(u64::from_le_bytes(bytes[..8].try_into().unwrap()), id);
                return Ok(Reply::Binary(bytes[8..].to_vec()));
            }
            _ => continue,
        }
    }
}

#[derive(Debug)]
enum Reply {
    Json(Value),
    Binary(#[allow(dead_code)] Vec<u8>),
}

fn mock_frames() -> String {
    std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../data/mock_frames")
        .to_string_lossy()
        .into_owned()
}

async fn json_call(ws: &mut Socket, id: u64, command: &str, args: Value) -> Value {
    match call(ws, id, command, args, &mut Vec::new()).await.unwrap() {
        Reply::Json(value) => value,
        other => panic!("expected JSON, got {other:?}"),
    }
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn stopped_retention_resize_export_never_restarts_capture() {
    let (server, addr, data) = start("egrabber_buffer").await;
    let mut ws = connect(addr).await;
    json_call(&mut ws, 1, "init", json!({"dataDir": ""})).await;
    json_call(
        &mut ws,
        2,
        "configure_mock",
        json!({"frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true}),
    )
    .await;
    let started = json_call(&mut ws, 3, "start_capture", Value::Null).await;
    assert_eq!(started["ok"], true, "{started}");
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let range = json_call(&mut ws, 30, "fetch_preview_buffer", Value::Null).await;
        if range["count"]
            .as_str()
            .and_then(|n| n.parse::<u64>().ok())
            .unwrap_or(0)
            > 8
        {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "mock capture did not retain frames: {range}"
        );
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    let busy = json_call(
        &mut ws,
        4,
        "save_preview_buffer",
        json!({"request": json!({"action":"resize", "capacity":"8"}).to_string()}),
    )
    .await;
    assert_eq!(
        busy["ok"], false,
        "resize while capturing must fail: {busy}"
    );
    json_call(&mut ws, 5, "stop_capture", Value::Null).await;
    let before = json_call(&mut ws, 6, "fetch_preview_buffer", Value::Null).await;
    assert_eq!(before["capture_running"], false);
    assert_eq!(before["available"], true, "{before}");
    let generation = before["generation"].as_str().unwrap();
    let export = json_call(
        &mut ws,
        7,
        "save_preview_buffer",
        json!({"request": json!({
        "output_root": data.to_string_lossy(), "first":before["first"], "last":before["first"],
        "generation": generation, "format":"tiff", "range_mode":"index"
    }).to_string()}),
    )
    .await;
    assert_eq!(export["ok"], true, "{export}");
    let output = std::path::Path::new(export["output_path"].as_str().unwrap());
    assert!(output.exists(), "export output exists");
    let exported_files: Vec<_> = std::fs::read_dir(output)
        .unwrap()
        .map(|e| e.unwrap().path())
        .collect();
    assert!(!exported_files.is_empty(), "range export produced files");
    let refused = json_call(
        &mut ws,
        70,
        "save_preview_buffer",
        json!({"request":json!({
        "action":"resize", "capacity":"8", "generation":generation
    }).to_string()}),
    )
    .await;
    assert_eq!(
        refused["ok"], false,
        "destructive shrink requires confirmation: {refused}"
    );
    let unchanged = json_call(&mut ws, 71, "fetch_preview_buffer", Value::Null).await;
    assert_eq!(
        unchanged["count"], before["count"],
        "refusal preserves retained frames"
    );
    assert_eq!(unchanged["generation"], before["generation"]);
    let resized = json_call(
        &mut ws,
        8,
        "save_preview_buffer",
        json!({"request":json!({
        "action":"resize", "capacity":"8", "confirm_clear":true, "generation":generation
    }).to_string()}),
    )
    .await;
    assert_eq!(resized["ok"], true, "{resized}");
    let after = json_call(&mut ws, 9, "fetch_preview_buffer", Value::Null).await;
    assert_eq!(
        after["capture_running"], false,
        "resize must not auto-resume"
    );
    assert_eq!(after["capacity"], "8");
    let stale = json_call(
        &mut ws,
        10,
        "save_preview_buffer",
        json!({"request":json!({
        "action":"resize", "capacity":"9", "generation":generation
    }).to_string()}),
    )
    .await;
    assert_eq!(stale["ok"], false, "old identity must be rejected: {stale}");
    assert!(stale["error"]
        .as_str()
        .unwrap()
        .contains("identity changed"));
    let final_state = json_call(&mut ws, 11, "fetch_preview_buffer", Value::Null).await;
    assert_eq!(final_state["capacity"], "8");
    assert_eq!(final_state["capture_running"], false);
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(data);
}
