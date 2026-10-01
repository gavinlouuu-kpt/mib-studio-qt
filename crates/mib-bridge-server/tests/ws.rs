//! The WebSocket protocol end to end against the real backend with the mock camera.
//! Backend state is process-global, so the tests run serially.

use std::sync::Arc;
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use mib_app_commands::AppState;
use mib_bridge_server::{Server, ServerConfig};
use serde_json::{json, Value};
use serial_test::serial;
use tokio_tungstenite::tungstenite::Message;

type Socket = tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

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
    tokio_tungstenite::connect_async(format!("ws://{addr}/ws?token={TOKEN}")).await.unwrap().0
}

/// One request; returns the reply (text JSON or binary payload after the id), skipping events
/// and pings but recording events in `events`.
async fn call(ws: &mut Socket, id: u64, cmd: &str, args: Value, events: &mut Vec<Value>) -> Result<Reply, String> {
    ws.send(Message::Text(json!({ "request_id": id, "cmd": cmd, "args": args }).to_string().into())).await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let message = tokio::time::timeout_at(deadline.into(), ws.next()).await.expect("reply in time").unwrap().unwrap();
        match message {
            Message::Text(text) => {
                let value: Value = serde_json::from_str(text.as_str()).unwrap();
                if value.get("event").is_some() {
                    events.push(value["event"].clone());
                    continue;
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
    Binary(Vec<u8>),
}

fn mock_frames() -> String {
    std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../data/mock_frames").to_string_lossy().into_owned()
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn mock_capture_over_websocket() {
    let (server, addr, data) = start("capture").await;

    let refused = tokio_tungstenite::connect_async(format!("ws://{addr}/ws?token=wrong")).await;
    assert!(refused.is_err(), "a wrong token is refused at the upgrade");

    let mut ws = connect(addr).await;
    let mut events = Vec::new();
    let Ok(Reply::Json(ok)) = call(&mut ws, 1, "init", json!({ "dataDir": "" }), &mut events).await else { panic!() };
    assert_eq!(ok, json!(true));
    let Ok(Reply::Json(r)) = call(&mut ws, 2, "configure_mock",
        json!({ "frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true }), &mut events).await else { panic!() };
    assert_eq!(r["ok"], json!(true), "{r}");
    let Ok(Reply::Json(r)) = call(&mut ws, 3, "start_capture", Value::Null, &mut events).await else { panic!() };
    assert_eq!(r["ok"], json!(true), "{r}");

    // Pull frames the way the UI does: one request per drawn frame.
    let mut frames = 0;
    let mut id = 10;
    let deadline = Instant::now() + Duration::from_secs(10);
    while frames < 5 && Instant::now() < deadline {
        id += 1;
        let Ok(Reply::Binary(packet)) = call(&mut ws, id, "fetch_frame_packet", Value::Null, &mut events).await else {
            panic!("frame packets are binary")
        };
        if packet.len() > 96 {
            assert_eq!(&packet[..4], b"MIBF");
            frames += 1;
        } else {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }
    assert_eq!(frames, 5, "five frames pulled");

    // Events are pushed by the server; a client may not drain them itself.
    let deadline = Instant::now() + Duration::from_secs(5);
    while !events.iter().any(|e| e["events"].as_array().is_some_and(|a| a.iter().any(|x| x["kind"] == "CameraStatus")))
        && Instant::now() < deadline
    {
        id += 1;
        let _ = call(&mut ws, id, "is_initialized", Value::Null, &mut events).await;
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert!(events.iter().all(|e| e["transport_version"] == json!(1)));
    assert!(
        events.iter().any(|e| e["events"].as_array().is_some_and(|a| a.iter().any(|x| x["kind"] == "CameraStatus"))),
        "camera status events are pushed (live frames are pulled, not announced)"
    );
    assert_eq!(call(&mut ws, 90, "poll_events_exact", Value::Null, &mut events).await.unwrap_err(), "SERVER_OWNS_EVENTS");
    assert!(call(&mut ws, 91, "no_such_command", Value::Null, &mut events).await.unwrap_err().starts_with("UNKNOWN_COMMAND"));
    // init is idempotent for later clients.
    let Ok(Reply::Json(ok)) = call(&mut ws, 92, "init", json!({ "dataDir": "/nonexistent" }), &mut events).await else { panic!() };
    assert_eq!(ok, json!(true));

    // Desktop platform commands are answered from the instrument's data directory.
    let Ok(Reply::Json(paths)) = call(&mut ws, 94, "app_paths", Value::Null, &mut events).await else { panic!() };
    assert_eq!(paths["app_data"], json!(data.to_string_lossy()));
    call(&mut ws, 95, "set_preferences", json!({ "preferences": { "sidebar": "collapsed" } }), &mut events).await.unwrap();
    let Ok(Reply::Json(preferences)) = call(&mut ws, 96, "get_preferences", Value::Null, &mut events).await else { panic!() };
    assert_eq!(preferences, json!({ "sidebar": "collapsed" }));
    call(&mut ws, 97, "shell_log", json!({ "level": "info", "message": "hello" }), &mut events).await.unwrap();
    assert!(std::fs::read_to_string(data.join("logs/remote-shell.log")).unwrap().contains("[INFO]"));

    let Ok(Reply::Json(r)) = call(&mut ws, 93, "stop_capture", Value::Null, &mut events).await else { panic!() };
    assert_eq!(r["ok"], json!(true), "{r}");
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn client_loss_stops_and_saves() {
    let (server, addr, data) = start("loss").await;
    let mut ws = connect(addr).await;
    let mut events = Vec::new();
    call(&mut ws, 1, "init", json!({ "dataDir": "" }), &mut events).await.unwrap();
    call(&mut ws, 2, "configure_mock", json!({ "frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true }), &mut events)
        .await
        .unwrap();
    call(&mut ws, 3, "start_capture", Value::Null, &mut events).await.unwrap();
    tokio::time::sleep(Duration::from_millis(200)).await;
    let recording = data.join("loss.h5");
    let Ok(Reply::Json(r)) =
        call(&mut ws, 4, "start_recording", json!({ "filePath": recording.to_string_lossy() }), &mut events).await
    else {
        panic!()
    };
    assert_eq!(r["ok"], json!(true), "{r}");
    let Ok(Reply::Json(buffer)) = call(&mut ws, 5, "fetch_preview_buffer", Value::Null, &mut events).await else { panic!() };
    assert_eq!(buffer["recording"], json!(true));

    // The link drops without a close handshake.
    drop(ws);
    let deadline = Instant::now() + Duration::from_secs(10);
    while server.stop_and_saves() == 0 && Instant::now() < deadline {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert_eq!(server.stop_and_saves(), 1, "the server finalised after the grace period");
    assert_eq!(server.clients(), 0);

    let mut ws = connect(addr).await;
    let Ok(Reply::Json(buffer)) = call(&mut ws, 6, "fetch_preview_buffer", Value::Null, &mut events).await else { panic!() };
    assert_eq!(buffer["recording"], json!(false), "the raw recording was stopped and saved");
    assert_eq!(buffer["capture_running"], json!(true), "capture keeps running for the next client");
    assert!(recording.exists(), "the recording file is on disk");

    // A reconnect inside the grace period does not stop anything.
    call(&mut ws, 7, "start_recording", json!({ "filePath": data.join("loss2.h5").to_string_lossy() }), &mut events)
        .await
        .unwrap();
    drop(ws);
    tokio::time::sleep(Duration::from_millis(100)).await;
    let mut ws = connect(addr).await;
    tokio::time::sleep(Duration::from_millis(800)).await;
    let Ok(Reply::Json(buffer)) = call(&mut ws, 8, "fetch_preview_buffer", Value::Null, &mut events).await else { panic!() };
    assert_eq!(buffer["recording"], json!(true), "a quick reconnect keeps the recording");
    assert_eq!(server.stop_and_saves(), 1);

    call(&mut ws, 9, "stop_recording", Value::Null, &mut events).await.unwrap();
    call(&mut ws, 10, "stop_capture", Value::Null, &mut events).await.unwrap();
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data);
}
