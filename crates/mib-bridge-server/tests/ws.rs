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
    call_text(ws, id, json!({ "request_id": id, "cmd": cmd, "args": args }).to_string(), events).await
}

/// The same with a request frame written by hand (the exact bytes a client sends).
async fn call_text(ws: &mut Socket, id: u64, text: String, events: &mut Vec<Value>) -> Result<Reply, String> {
    ws.send(Message::Text(text.into())).await.unwrap();
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
    Binary(Vec<u8>),
}

fn mock_frames() -> String {
    std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../data/mock_frames").to_string_lossy().into_owned()
}

/// GET `path` over plain HTTP/1.1; returns the status code and body.
async fn http_get(addr: std::net::SocketAddr, path: &str) -> (u16, String) {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let mut stream = tokio::net::TcpStream::connect(addr).await.unwrap();
    stream.write_all(format!("GET {path} HTTP/1.1\r\nHost: {addr}\r\nConnection: close\r\n\r\n").as_bytes()).await.unwrap();
    let mut response = String::new();
    stream.read_to_string(&mut response).await.unwrap();
    let status = response.split_whitespace().nth(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    let body = response.split("\r\n\r\n").nth(1).unwrap_or("").to_string();
    (status, body)
}

// #501: the UI asks /auth before opening the socket, so a missing token becomes a prompt.
#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn auth_probe_reports_the_token_without_a_socket() {
    let (_server, addr, data) = start("auth").await;
    let (status, body) = http_get(addr, "/auth").await;
    assert_eq!(status, 401, "{body}");
    assert!(body.contains("\"token_required\":true"), "{body}");
    assert_eq!(http_get(addr, "/auth?token=wrong").await.0, 401);
    let (status, body) = http_get(addr, &format!("/auth?token={TOKEN}")).await;
    assert_eq!(status, 200, "{body}");
    assert!(body.contains("\"authorized\":true"), "{body}");
    // The boot id names this server process: the same on every answer (also the 401), so a
    // browser that lost the link can tell a restarted backend from a network blip.
    let boot = |body: &str| body.split("\"boot_id\":\"").nth(1).and_then(|r| r.split('"').next()).map(str::to_owned);
    let first = boot(&body).expect("200 carries a boot id");
    assert_eq!(first.len(), 16, "{first}");
    let (_, refused) = http_get(addr, "/auth?token=wrong").await;
    assert_eq!(boot(&refused).as_deref(), Some(first.as_str()), "{refused}");
    let _ = std::fs::remove_dir_all(&data);
}

// ABI 39 (#712): the server passes startRequestJson to the backend unparsed, so the backend's strict
// parser sees a duplicate key (a serde Value would keep the last) and bounds the client's own bytes
// (a request over 16 KiB only through whitespace is refused, although it would pass re-serialised).
#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn start_request_json_is_passed_through_the_server_unparsed() {
    let (server, addr, data) = start("start_request").await;
    let mut ws = connect(addr).await;
    let mut events = Vec::new();
    let Ok(Reply::Json(ok)) = call(&mut ws, 1, "init", json!({ "dataDir": "" }), &mut events).await else { panic!() };
    assert_eq!(ok, json!(true));
    let out = serde_json::to_string(&data.join("start_request.h5").to_string_lossy()).unwrap();
    let duplicate = r#"{"operator":{"name":"Alice","name":"Bob"}}"#.to_string();
    let padded = format!("{{{}\"sample_ref\":null}}", " ".repeat(16400));
    for (id, doc, why) in [(2u64, duplicate, "duplicate key"), (3u64, padded, "at most 16384")] {
        // Written by hand: the request document is a JSON string inside the frame, as a client sends it.
        let frame = format!(
            r#"{{"request_id":{id},"cmd":"experiment_start","args":{{"outputPath":{out},"startRequestJson":{}}}}}"#,
            serde_json::to_string(&doc).unwrap()
        );
        let Ok(Reply::Json(r)) = call_text(&mut ws, id, frame, &mut events).await else { panic!("a CommandResult") };
        let message = r["message"].as_str().unwrap_or_default();
        assert_eq!(r["ok"], json!(false), "{r}");
        assert!(message.contains("invalid start request") && message.contains(why), "{why}: {message}");
    }
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data);
}

// ABI 40 (#741): over the WebSocket, a start reply carries start_outcome and a stop reply
// stop_outcome; experiment_stop with expectedStartGeneration stops only that run. A stale
// generation is refused with RunMismatch (3) and the run continues; the matching one stops it; a
// stop without the argument is as before; a malformed generation is an error and sends nothing.
#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn experiment_stop_names_its_run_over_the_websocket() {
    let (server, addr, data) = start("stop_generation").await;
    let mut ws = connect(addr).await;
    let mut events = Vec::new();
    let mut id = 0u64;
    let mut next = || {
        id += 1;
        id
    };
    let Ok(Reply::Json(ok)) = call(&mut ws, next(), "init", json!({ "dataDir": "" }), &mut events).await else { panic!() };
    assert_eq!(ok, json!(true));
    let out = data.join("stop_generation.h5").to_string_lossy().into_owned();

    // Idle: a generation is RunMismatch with the plain reason; without one NotActive, as before.
    let Ok(Reply::Json(r)) = call(&mut ws, next(), "experiment_stop", json!({ "expectedStartGeneration": "1" }), &mut events).await
    else {
        panic!()
    };
    assert_eq!((r["ok"].clone(), r["stop_outcome"].clone()), (json!(false), json!(3)), "{r}");
    assert_eq!(r["message"], json!("stop refused: expected run 1, but no run is active; nothing was stopped"), "{r}");
    assert!(r.get("start_outcome").is_none(), "a stop reply has no start outcome: {r}");
    let Ok(Reply::Json(r)) = call(&mut ws, next(), "experiment_stop", Value::Null, &mut events).await else { panic!() };
    assert_eq!((r["stop_outcome"].clone(), r["message"].clone()), (json!(1), json!("notActive")), "{r}");
    for bad in [json!("+1"), json!("01"), json!(""), json!(" 1"), json!("18446744073709551616"), json!(1)] {
        let refused = call(&mut ws, next(), "experiment_stop", json!({ "expectedStartGeneration": bad }), &mut events).await;
        assert!(refused.is_err(), "{bad} is not a canonical decimal generation: {refused:?}");
    }

    // No camera: NotReady (1); a refused start request: InvalidRequest (7), message unchanged.
    let Ok(Reply::Json(r)) = call(&mut ws, next(), "experiment_start", json!({ "outputPath": out }), &mut events).await else { panic!() };
    assert_eq!((r["ok"].clone(), r["start_outcome"].clone()), (json!(false), json!(1)), "{r}");
    assert!(r.get("stop_outcome").is_none(), "{r}");
    let Ok(Reply::Json(r)) = call(
        &mut ws,
        next(),
        "experiment_start",
        json!({ "outputPath": out, "startRequestJson": r#"{"operator_name":"Alice"}"# }),
        &mut events,
    )
    .await
    else {
        panic!()
    };
    assert_eq!(r["start_outcome"], json!(7), "{r}");
    assert!(r["message"].as_str().unwrap().starts_with("invalid start request: "), "{r}");

    // A run: Started (0), then AlreadyActive (3) for a second start.
    call(&mut ws, next(), "configure_mock", json!({ "frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true }), &mut events)
        .await
        .unwrap();
    call(&mut ws, next(), "apply_processing", json!({ "realtimeEnabled": true, "pixelToMicron": 1.0 }), &mut events).await.unwrap();
    call(&mut ws, next(), "start_capture", Value::Null, &mut events).await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    let started = loop {
        let Ok(Reply::Json(r)) = call(&mut ws, next(), "experiment_start", json!({ "outputPath": out }), &mut events).await else {
            panic!()
        };
        if r["ok"] == json!(true) || Instant::now() > deadline {
            break r;
        }
        tokio::time::sleep(Duration::from_millis(50)).await; // readiness settles once frames arrive
    };
    assert_eq!((started["ok"].clone(), started["start_outcome"].clone()), (json!(true), json!(0)), "{started}");
    let Ok(Reply::Json(again)) = call(&mut ws, next(), "experiment_start", json!({ "outputPath": out }), &mut events).await else { panic!() };
    assert_eq!(again["start_outcome"], json!(3), "{again}");
    let Ok(Reply::Json(status)) = call(&mut ws, next(), "fetch_experiment_status", Value::Null, &mut events).await else { panic!() };
    assert_eq!(status["state"], json!(2), "Active: {status}");
    let generation: u64 = status["start_generation"].as_str().unwrap().parse().unwrap();

    // A stale or unknown generation stops nothing.
    for stale in [generation - 1, generation + 1] {
        let Ok(Reply::Json(r)) = call(
            &mut ws,
            next(),
            "experiment_stop",
            json!({ "expectedStartGeneration": stale.to_string() }),
            &mut events,
        )
        .await
        else {
            panic!()
        };
        assert_eq!((r["ok"].clone(), r["stop_outcome"].clone()), (json!(false), json!(3)), "{r}");
        assert_eq!(
            r["message"],
            json!(format!("stop refused: expected run {stale}, but run {generation} is active; nothing was stopped")),
            "{r}"
        );
    }
    tokio::time::sleep(Duration::from_millis(400)).await;
    let Ok(Reply::Json(status)) = call(&mut ws, next(), "fetch_experiment_status", Value::Null, &mut events).await else { panic!() };
    assert_eq!(status["state"], json!(2), "the run continues: {status}");

    // The run's own generation stops it.
    let Ok(Reply::Json(r)) = call(
        &mut ws,
        next(),
        "experiment_stop",
        json!({ "expectedStartGeneration": generation.to_string() }),
        &mut events,
    )
    .await
    else {
        panic!()
    };
    assert_eq!((r["ok"].clone(), r["stop_outcome"].clone(), r["message"].clone()), (json!(true), json!(0), json!("accepted")), "{r}");
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        let Ok(Reply::Json(s)) = call(&mut ws, next(), "fetch_experiment_status", Value::Null, &mut events).await else { panic!() };
        if s["terminal"] == json!(true) {
            assert_eq!(s["state"], json!(0), "Idle: {s}");
            break;
        }
        assert!(Instant::now() < deadline, "the run did not finalize: {s}");
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    call(&mut ws, next(), "stop_capture", Value::Null, &mut events).await.unwrap();
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data);
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

/// A fake dLSP syringe pump (Modbus RTU slave 1) on the master side of a pseudo-terminal; the
/// backend opens the slave as the pump's serial port. `stall()` stops the tty's output
/// (`tcflow(TCOOFF)`), so the kernel refuses every write the backend makes, as a USB-serial adapter
/// that stops draining does; `resume()` restarts it.
#[cfg(target_os = "linux")]
mod fake_pump {
    use std::ffi::{CStr, CString};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::thread::JoinHandle;

    fn crc16(data: &[u8]) -> u16 {
        let mut crc = 0xFFFFu16;
        for &b in data {
            crc ^= u16::from(b);
            for _ in 0..8 {
                crc = if crc & 1 != 0 { (crc >> 1) ^ 0xA001 } else { crc >> 1 };
            }
        }
        crc
    }

    fn with_crc(mut frame: Vec<u8>) -> Vec<u8> {
        let crc = crc16(&frame);
        frame.extend_from_slice(&crc.to_le_bytes());
        frame
    }

    /// The answer to one complete request at the front of `rx`, and its length.
    fn answer(rx: &[u8]) -> Option<(usize, Option<Vec<u8>>)> {
        if rx.len() < 8 {
            return None;
        }
        let len = match rx[1] {
            0x10 if rx.len() >= 7 => 9 + usize::from(rx[6]),
            0x10 => return None,
            _ => 8,
        };
        if rx.len() < len {
            return None;
        }
        let reply = match (rx[0], rx[1]) {
            (1, 0x03) => {
                let count = u16::from_be_bytes([rx[4], rx[5]]) as usize;
                let mut frame = vec![1, 0x03, (count * 2) as u8];
                frame.resize(3 + count * 2, 0);
                Some(with_crc(frame))
            }
            (1, 0x06) => Some(rx[..8].to_vec()),
            (1, 0x10) => Some(with_crc(rx[..6].to_vec())),
            _ => None,
        };
        Some((len, reply))
    }

    pub struct FakePump {
        pub slave_path: String,
        master: libc::c_int,
        control: libc::c_int,
        stop: Arc<AtomicBool>,
        thread: Option<JoinHandle<()>>,
    }

    impl FakePump {
        pub fn start() -> Self {
            // SAFETY: plain POSIX pty calls on descriptors this struct owns.
            unsafe {
                let master = libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY);
                assert!(master >= 0, "pty available");
                assert_eq!(libc::grantpt(master), 0);
                assert_eq!(libc::unlockpt(master), 0);
                let mut name = [0 as libc::c_char; 128];
                assert_eq!(libc::ptsname_r(master, name.as_mut_ptr(), name.len()), 0);
                let slave_path = CStr::from_ptr(name.as_ptr()).to_string_lossy().into_owned();
                let path = CString::new(slave_path.clone()).unwrap();
                // A second handle on the slave: it lets the test stop and restart the tty's output.
                let control = libc::open(path.as_ptr(), libc::O_RDWR | libc::O_NOCTTY | libc::O_NONBLOCK);
                assert!(control >= 0, "second handle on the pty slave");
                let stop = Arc::new(AtomicBool::new(false));
                let flag = stop.clone();
                let thread = std::thread::spawn(move || {
                    let mut rx = Vec::new();
                    while !flag.load(Ordering::SeqCst) {
                        let mut fds = libc::pollfd { fd: master, events: libc::POLLIN, revents: 0 };
                        if libc::poll(&mut fds, 1, 20) <= 0 {
                            continue;
                        }
                        let mut buf = [0u8; 256];
                        let n = libc::read(master, buf.as_mut_ptr().cast(), buf.len());
                        if n <= 0 {
                            continue;
                        }
                        rx.extend_from_slice(&buf[..n as usize]);
                        while let Some((len, reply)) = answer(&rx) {
                            rx.drain(..len);
                            if let Some(reply) = reply {
                                libc::write(master, reply.as_ptr().cast(), reply.len());
                            }
                        }
                    }
                });
                FakePump { slave_path, master, control, stop, thread: Some(thread) }
            }
        }

        pub fn stall(&self) {
            // SAFETY: `control` is an open descriptor of the pty slave.
            assert_eq!(unsafe { libc::tcflow(self.control, libc::TCOOFF) }, 0, "pty output stopped");
        }

        pub fn resume(&self) {
            // SAFETY: as above.
            assert_eq!(unsafe { libc::tcflow(self.control, libc::TCOON) }, 0, "pty output restarted");
        }
    }

    impl Drop for FakePump {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::SeqCst);
            if let Some(thread) = self.thread.take() {
                let _ = thread.join();
            }
            // SAFETY: both descriptors are owned here and closed once.
            unsafe {
                libc::close(self.control);
                libc::close(self.master);
            }
        }
    }
}

// #720: a pump on a stalled serial adapter must not hold the client-loss stop-and-save, or the
// bridge lock, for ever. Before #720 the backend retried EAGAIN without limit: the pump stop spun
// under the bridge lock and the server never finished. Now each pump command fails at the write
// deadline (about a second), the recording is saved, and the next client is answered.
#[cfg(target_os = "linux")]
#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn client_loss_with_a_stalled_pump_still_stops_and_saves() {
    let (server, addr, data) = start("stalled_pump").await;
    let pump = fake_pump::FakePump::start();
    let mut ws = connect(addr).await;
    let mut events = Vec::new();
    call(&mut ws, 1, "init", json!({ "dataDir": "" }), &mut events).await.unwrap();
    let Ok(Reply::Json(r)) = call(&mut ws, 2, "pump_connect_endpoint",
        json!({ "pump": 0, "portName": pump.slave_path, "baudRate": 9600, "modbusAddress": 1 }), &mut events).await
    else {
        panic!()
    };
    assert_eq!(r["ok"], json!(true), "the fake pump answers: {r}");
    call(&mut ws, 3, "configure_mock", json!({ "frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true }), &mut events)
        .await
        .unwrap();
    call(&mut ws, 4, "start_capture", Value::Null, &mut events).await.unwrap();
    tokio::time::sleep(Duration::from_millis(200)).await;
    let recording = data.join("stalled.h5");
    let Ok(Reply::Json(r)) =
        call(&mut ws, 5, "start_recording", json!({ "filePath": recording.to_string_lossy() }), &mut events).await
    else {
        panic!()
    };
    assert_eq!(r["ok"], json!(true), "{r}");

    // The adapter stalls, then the link drops without a close handshake.
    pump.stall();
    drop(ws);
    let dropped = Instant::now();
    let deadline = dropped + Duration::from_secs(60);
    while server.stop_and_saves() == 0 && Instant::now() < deadline {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    eprintln!("stop-and-save with a stalled pump finished {:?} after the drop", dropped.elapsed());
    assert_eq!(server.stop_and_saves(), 1, "the server finalised despite the stalled pump");

    // The bridge lock is free: a new client is answered, the recording was saved, and a pump
    // command on the stalled adapter fails within its bound, naming the timeout.
    let mut ws = connect(addr).await;
    let Ok(Reply::Json(buffer)) = call(&mut ws, 6, "fetch_preview_buffer", Value::Null, &mut events).await else { panic!() };
    assert_eq!(buffer["recording"], json!(false), "the recording was stopped and saved");
    assert!(recording.exists(), "the recording file is on disk");
    let started = Instant::now();
    let Ok(Reply::Json(r)) = call(&mut ws, 7, "pump_stop", json!({ "pump": 0 }), &mut events).await else { panic!() };
    eprintln!("pump_stop on the stalled adapter answered in {:?}: {r}", started.elapsed());
    assert_eq!(r["ok"], json!(false), "{r}");
    let message = r["message"].as_str().unwrap_or_default();
    assert!(message.starts_with("Pump stop failed") && message.contains("timed out"), "{r}");

    // The adapter recovers: the same session stops the pump.
    pump.resume();
    let Ok(Reply::Json(r)) = call(&mut ws, 8, "pump_stop", json!({ "pump": 0 }), &mut events).await else { panic!() };
    assert_eq!(r["ok"], json!(true), "the pump answers again: {r}");
    call(&mut ws, 9, "pump_disconnect", json!({ "pump": 0 }), &mut events).await.unwrap();
    call(&mut ws, 10, "stop_capture", Value::Null, &mut events).await.unwrap();
    ws.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    drop(pump);
    let _ = std::fs::remove_dir_all(&data);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn one_client_controls_the_instrument() {
    let (server, addr, data) = start("control").await;
    let mut first = connect(addr).await;
    let mut second = connect(addr).await;
    let mut events = Vec::new();
    // Session messages arrive before any reply; the call helper skips them.
    call(&mut first, 1, "init", json!({ "dataDir": "" }), &mut events).await.unwrap();
    assert_eq!(server.controller(), Some(1));
    let refused = call(&mut second, 2, "configure_mock", json!({ "frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true }), &mut events)
        .await
        .unwrap_err();
    assert!(refused.starts_with("VIEWER_ONLY"), "{refused}");
    assert!(call(&mut second, 3, "fetch_camera_selection", Value::Null, &mut events).await.is_ok(), "viewers may read");
    let Ok(Reply::Json(claimed)) = call(&mut second, 4, "take_control", Value::Null, &mut events).await else { panic!() };
    assert_eq!(claimed["controller_id"], json!(2));
    assert!(call(&mut second, 5, "configure_mock", json!({ "frameDir": mock_frames(), "frameIntervalMs": 5, "loopFiles": true }), &mut events).await.is_ok());
    assert!(call(&mut first, 6, "start_capture", Value::Null, &mut events).await.unwrap_err().starts_with("VIEWER_ONLY"));
    // The controller leaving hands control to the oldest remaining client.
    second.close(None).await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
    while server.controller() != Some(1) && Instant::now() < deadline {
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    assert_eq!(server.controller(), Some(1));
    assert!(call(&mut first, 7, "start_capture", Value::Null, &mut events).await.is_ok());
    call(&mut first, 8, "stop_capture", Value::Null, &mut events).await.unwrap();
    first.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data);
}


/// Blob frames as 8-bit BMPs (the mock camera decodes BMP as Mono8): a bright disk the lenient
/// config below detects as a valid cell, through the real realtime path.
fn blob_frames(tag: &str) -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("yofo_server_blobs_{tag}_{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let (w, h) = (160u32, 96u32);
    for i in 0..8u32 {
        let (cx, cy, r) = (40 + 10 * i as i32, 48i32, 14i32);
        let offset: u32 = 14 + 40 + 1024;
        let mut b: Vec<u8> = Vec::new();
        b.extend_from_slice(b"BM");
        for v in [offset + w * h, 0, offset, 40] { b.extend_from_slice(&v.to_le_bytes()); }
        b.extend_from_slice(&(w as i32).to_le_bytes());
        b.extend_from_slice(&(h as i32).to_le_bytes());
        b.extend_from_slice(&1u16.to_le_bytes());
        b.extend_from_slice(&8u16.to_le_bytes());
        for v in [0u32, w * h, 2835, 2835, 256, 0] { b.extend_from_slice(&v.to_le_bytes()); }
        for g in 0..256u32 { b.extend_from_slice(&[g as u8, g as u8, g as u8, 0]); }
        for y in (0..h as i32).rev() {
            for x in 0..w as i32 {
                b.push(if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r { 220 } else { 0 });
            }
        }
        std::fs::write(dir.join(format!("frame_{i:04}.bmp")), b).unwrap();
    }
    dir
}

const LENIENT: &str = r#"{"image_processing": {"gaussian_blur_size": 3, "bg_subtract_threshold": 8,
    "morph_kernel_size": 3, "morph_iterations": 1, "empty_frame_pixel_threshold": 1, "auto_background_enabled": false,
    "filters": {"enable_border_check": false, "enable_area_range_check": false, "enable_deformability_range_check": false,
                "enable_area_ratio_check": false, "enable_ring_ratio_check": false,
                "enable_laplacian_variance_check": false, "require_single_inner_contour": false}}}"#;

// ABI 41 (#716 A6a): live cell images over the WebSocket. The pixels come back as binary frames
// (8-byte request id, then the frame packet of pull kind 5 or 6), never inside JSON; every client
// may read the strip, the pins and their images, but pins are one shared bounded set, so pin,
// unpin and clear need control (a viewer gets VIEWER_ONLY).
#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn live_cells_over_the_websocket() {
    let (server, addr, data) = start("cells").await;
    let frames = blob_frames("cells");
    let mut controller = connect(addr).await;
    let mut viewer = connect(addr).await;
    let mut events = Vec::new();
    call(&mut controller, 1, "init", json!({ "dataDir": "" }), &mut events).await.unwrap();
    assert_eq!(server.controller(), Some(1));
    let mut id = 1u64;
    let ok = |r: Result<Reply, String>| match r {
        Ok(Reply::Json(v)) => v,
        other => panic!("{other:?}"),
    };
    for (cmd, args) in [
        ("apply_processing_config_json", json!({ "json": LENIENT })),
        ("apply_processing", json!({ "realtimeEnabled": true, "pixelToMicron": 0.5 })),
        ("configure_mock", json!({ "frameDir": frames.to_string_lossy(), "frameIntervalMs": 5, "loopFiles": true })),
        ("monitoring_set_active", json!({ "active": true })),
        ("start_capture", Value::Null),
    ] {
        id += 1;
        let r = ok(call(&mut controller, id, cmd, args, &mut events).await);
        assert_eq!(r["ok"], json!(true), "{cmd}: {r}");
    }
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        id += 1;
        let snap = ok(call(&mut viewer, id, "fetch_monitoring_snapshot", json!({ "maxRows": 500 }), &mut events).await);
        if snap["rows"].as_array().unwrap().iter().any(|r| r["valid"] == json!(true)) { break; }
        assert!(Instant::now() < deadline, "no valid rows: {snap}");
        tokio::time::sleep(Duration::from_millis(50)).await;
    }

    // A viewer reads the listing and pulls a crop as a binary frame.
    id += 1;
    let listing = ok(call(&mut viewer, id, "fetch_monitoring_cells", Value::Null, &mut events).await);
    let cell = listing["strip"].as_array().unwrap().last().cloned().expect("a strip cell");
    let identity = json!({ "startGeneration": cell["start_generation"], "captureEpoch": cell["capture_epoch"],
                           "frameIndex": cell["frame_index"], "objectId": cell["object_id"] });
    for (layer, kind) in [("image", 5u32), ("mask", 6u32)] {
        let mut args = identity.clone();
        args["layer"] = json!(layer);
        id += 1;
        let Ok(Reply::Binary(p)) = call(&mut viewer, id, "fetch_monitoring_cell_packet", args, &mut events).await else {
            panic!("{layer}: a binary frame")
        };
        assert_eq!(&p[..4], b"MIBF");
        assert_eq!(u32::from_le_bytes(p[12..16].try_into().unwrap()), kind, "{layer}");
        assert_eq!(u32::from_le_bytes(p[8..12].try_into().unwrap()), 1, "{layer}: a valid frame");
        assert_eq!(u64::from_le_bytes(p[32..40].try_into().unwrap()), cell["image"]["width"].as_u64().unwrap());
    }

    // Pins need control.
    for cmd in ["monitoring_cell_pin", "monitoring_cell_unpin", "monitoring_cell_clear_pins"] {
        let args = match cmd {
            "monitoring_cell_pin" => json!({ "expectedStartGeneration": cell["start_generation"], "captureEpoch": cell["capture_epoch"],
                     "frameIndex": cell["frame_index"], "objectId": cell["object_id"] }),
            "monitoring_cell_unpin" => identity.clone(),
            _ => Value::Null,
        };
        id += 1;
        let refused = call(&mut viewer, id, cmd, args, &mut events).await.unwrap_err();
        assert!(refused.starts_with("VIEWER_ONLY"), "{cmd}: {refused}");
    }
    id += 1;
    let pinned = ok(call(&mut controller, id, "monitoring_cell_pin",
        json!({ "expectedStartGeneration": cell["start_generation"], "captureEpoch": cell["capture_epoch"],
                     "frameIndex": cell["frame_index"], "objectId": cell["object_id"] }),
        &mut events).await);
    assert_eq!((pinned["ok"].clone(), pinned["outcome"].clone()), (json!(true), json!(0)), "{pinned}");
    id += 1;
    let seen = ok(call(&mut viewer, id, "fetch_monitoring_cells", Value::Null, &mut events).await);
    assert_eq!(seen["pins"].as_array().unwrap().len(), 1, "the viewer sees the shared pin: {seen}");
    id += 1;
    let cleared = ok(call(&mut controller, id, "monitoring_cell_clear_pins", Value::Null, &mut events).await);
    assert_eq!(cleared["message"], json!("cleared 1 pin(s)"), "{cleared}");

    // Review round 2: once the camera stops, the viewer's pull of that capture's cell is the
    // CAPTURE_MISMATCH error with the reason, over the socket as over Tauri.
    id += 1;
    call(&mut controller, id, "stop_capture", Value::Null, &mut events).await.unwrap();
    let mut args = identity.clone();
    args["layer"] = json!("image");
    id += 1;
    let refused = call(&mut viewer, id, "fetch_monitoring_cell_packet", args, &mut events).await.unwrap_err();
    assert!(refused.starts_with("CAPTURE_MISMATCH: image refused") && refused.contains("the camera stopped"), "{refused}");
    controller.close(None).await.unwrap();
    viewer.close(None).await.unwrap();
    server.state().bridge.lock().unwrap().pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frames);
    let _ = std::fs::remove_dir_all(&data);
}
