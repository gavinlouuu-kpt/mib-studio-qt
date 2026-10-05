//! YOFO Studio headless backend server (impl spec S5).
//!
//! Runs the backend in-process (`mib-app-commands` over `mib-bridge`) and exposes the same
//! commands the Tauri shell has over one WebSocket per client, so the React UI can run in a
//! browser against the instrument's PS.
//!
//! Protocol on `/ws` (token on the upgrade: `?token=` or `Authorization: Bearer`):
//! - client -> server text: `{"request_id": n, "cmd": "start_capture", "args": {...}}`, the
//!   name and camelCase arguments exactly as the webview passes them to Tauri's `invoke`;
//! - server -> client text: `{"request_id": n, "ok": value}` or `{"request_id": n, "error": "..."}`;
//! - server -> client binary: 8-byte little-endian request id, then the reply bytes unchanged
//!   (frame packets with their 96-byte MIBF header);
//! - server -> client text `{"event": EventEnvelope}`: the server alone drains backend events
//!   (two clients polling would steal each other's) and broadcasts them; `poll_events_exact` is
//!   refused with `SERVER_OWNS_EVENTS`.
//!
//! The desktop-only platform commands (`app_paths`, `get_preferences`, `set_preferences`,
//! `shell_log`) are answered from the server's data directory, shared by every client.
//!
//! Requests of one connection run in order, each off the async runtime (bridge calls block).
//! Frames stay pulled: a client asks for the next packet when it has drawn the last one.
//!
//! Control: one client at a time controls the instrument (camera, capture, recording, experiment,
//! processing, hardware). The first client gets control; others are viewers whose mutating
//! commands fail with `VIEWER_ONLY`; `take_control` claims it (an operator walking to another
//! browser). Every client receives `{"session": {"client_id", "controller_id"}}` on connect and
//! whenever control changes. Control passes to the oldest remaining client when the controller
//! disconnects. Two browsers switching tabs would otherwise flip the camera's mode against each
//! other.
//!
//! Client loss: the server pings every `ping_interval` and drops a connection that is silent
//! for `ping_timeout`. When the last client has been gone for `client_grace`, it stops and
//! saves: an active experiment gets `experiment_stop` (final flush and finalisation) and a raw
//! recording `stop_recording`. Capture keeps running. This differs on purpose from the desktop
//! close guard, which refuses to close instead: a remote operator who lost the link can no
//! longer see the run (ADR 0008).

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{ConnectInfo, Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use futures_util::{SinkExt, StreamExt};
use mib_app_commands::dispatch::{self, Host, Reply};
use mib_app_commands::AppState;
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::sync::{broadcast, mpsc};

mod platform;

/// Experiment states that a client loss must finalise (`bridgeContract.ts` EXPERIMENT_STATES).
const EXPERIMENT_STARTING: u32 = 1;
const EXPERIMENT_ACTIVE: u32 = 2;

#[derive(Clone, Debug)]
pub struct ServerConfig {
    /// Required bearer token; `None` disables authentication (loopback development only).
    pub token: Option<String>,
    pub data_dir: String,
    pub resource_dir: String,
    pub core_cache_dir: String,
    /// The built React UI (`desktop/dist`), served at `/` when set.
    pub dist_dir: Option<PathBuf>,
    pub event_poll: Duration,
    pub ping_interval: Duration,
    pub ping_timeout: Duration,
    pub client_grace: Duration,
}

impl ServerConfig {
    pub fn new(data_dir: impl Into<String>) -> Self {
        ServerConfig {
            token: None,
            data_dir: data_dir.into(),
            resource_dir: String::new(),
            core_cache_dir: String::new(),
            dist_dir: None,
            event_poll: Duration::from_millis(20),
            ping_interval: Duration::from_secs(2),
            ping_timeout: Duration::from_secs(5),
            client_grace: Duration::from_secs(5),
        }
    }
}

struct ConfigHost(ServerConfig);

impl Host for ConfigHost {
    fn data_dir(&self) -> Result<String, String> {
        Ok(self.0.data_dir.clone())
    }
    fn resource_dir(&self) -> Result<String, String> {
        Ok(self.0.resource_dir.clone())
    }
    fn processing_core_cache_dir(&self) -> Result<String, String> {
        if self.0.core_cache_dir.is_empty() {
            Ok(format!("{}/processing-cores", self.0.data_dir))
        } else {
            Ok(self.0.core_cache_dir.clone())
        }
    }
}

/// One backend, any number of clients.
pub struct Server {
    state: Arc<AppState>,
    host: Arc<ConfigHost>,
    events: broadcast::Sender<Arc<String>>,
    clients: AtomicUsize,
    /// Bumped on every connect, so a grace timer can tell a reconnect from continued absence.
    connects: AtomicU64,
    /// Completed client-loss stop-and-save passes (observability and tests).
    stop_and_saves: AtomicU64,
    /// Connected client ids in connection order; the first is the default controller.
    sessions: std::sync::Mutex<SessionTable>,
}

#[derive(Default)]
struct SessionTable {
    connected: Vec<u64>,
    controller: Option<u64>,
}

/// Commands that change the instrument's state; viewers may not send them.
const CONTROL_COMMANDS: &[&str] = &[
    "configure_mock", "start_capture", "stop_capture", "set_camera_overview", "save_camera_roi",
    "select_hardware_camera", "select_mindvision_camera", "apply_camera_script", "soft_trigger_camera",
    "reset_hardware_camera", "start_recording", "stop_recording", "seek_latest", "seek_index", "load_recording",
    "close_review", "experiment_start", "experiment_stop", "experiment_cancel", "experiment_acknowledge_fault",
    "apply_processing", "apply_processing_config_json", "set_processing_roi", "set_background_from_current_frame",
    "clear_background_image", "background_calibration_command", "startup_discovery_set_preference",
    "startup_discovery_run", "start_device_discovery", "start_camera_discovery", "cancel_device_discovery",
    "pulse_generator_command", "autofocus_connect_endpoint", "autofocus_connect", "autofocus_disconnect",
    "autofocus_set_enabled", "autofocus_jog", "autofocus_set_config", "pump_connect_endpoint", "pump_connect_model", "pump_connect",
    "pump_disconnect", "pump_set_flow_rate", "pump_set_direction", "pump_start", "pump_stop", "pump_purge",
    "pump_stop_purge", "pump_set_syringe_volume", "pump_scan_addresses", "monitoring_set_active",
    "monitoring_clear", "trigger_set_pulse_duration", "trigger_manual_pulse", "trigger_periodic_start",
    "trigger_periodic_stop", "cancel_operation", "review_reanalysis_json", "review_export_json",
    "review_export_csv", "apply_config_document", "profile_command", "processing_core_command",
    "save_preview_buffer", "set_processed_preview_enabled",
];

impl Server {
    pub fn new(config: ServerConfig, state: Arc<AppState>) -> Arc<Self> {
        let (events, _) = broadcast::channel(256);
        Arc::new(Server {
            state,
            host: Arc::new(ConfigHost(config)),
            events,
            clients: AtomicUsize::new(0),
            connects: AtomicU64::new(0),
            stop_and_saves: AtomicU64::new(0),
            sessions: std::sync::Mutex::new(SessionTable::default()),
        })
    }

    pub fn state(&self) -> &Arc<AppState> {
        &self.state
    }

    pub fn clients(&self) -> usize {
        self.clients.load(Ordering::SeqCst)
    }

    pub fn stop_and_saves(&self) -> u64 {
        self.stop_and_saves.load(Ordering::SeqCst)
    }

    fn config(&self) -> &ServerConfig {
        &self.host.0
    }

    pub fn controller(&self) -> Option<u64> {
        self.sessions.lock().map(|t| t.controller).unwrap_or(None)
    }

    fn session_message(&self, client_id: u64) -> String {
        json!({ "session": { "client_id": client_id, "controller_id": self.controller() } }).to_string()
    }

    /// Register a client; it becomes the controller when nobody has control.
    fn client_joined(&self, client_id: u64) {
        let mut table = self.sessions.lock().unwrap();
        table.connected.push(client_id);
        if table.controller.is_none() {
            table.controller = Some(client_id);
        }
    }

    /// Unregister a client; control passes to the oldest remaining client.
    fn client_left(&self, client_id: u64) -> bool {
        let mut table = self.sessions.lock().unwrap();
        table.connected.retain(|&c| c != client_id);
        if table.controller == Some(client_id) {
            table.controller = table.connected.first().copied();
            true
        } else {
            false
        }
    }

    fn take_control(&self, client_id: u64) -> bool {
        let mut table = self.sessions.lock().unwrap();
        let changed = table.controller != Some(client_id);
        table.controller = Some(client_id);
        changed
    }

    fn broadcast_session(&self) {
        // Every client gets the controller id; it compares with its own id.
        let _ = self.events.send(Arc::new(json!({ "session": { "controller_id": self.controller() } }).to_string()));
    }

    pub fn router(self: &Arc<Self>) -> Router {
        let mut router = Router::new()
            .route("/ws", get(upgrade))
            .route("/healthz", get(health))
            .with_state(self.clone());
        if let Some(dist) = &self.config().dist_dir {
            router = router.fallback_service(tower_http::services::ServeDir::new(dist));
        }
        router
    }

    /// Serve until `shutdown` resolves; runs the event pump meanwhile.
    pub async fn serve(
        self: Arc<Self>,
        listener: tokio::net::TcpListener,
        shutdown: impl std::future::Future<Output = ()> + Send + 'static,
    ) -> std::io::Result<()> {
        let pump = tokio::spawn(self.clone().event_pump());
        let app = self.router().into_make_service_with_connect_info::<SocketAddr>();
        let result = axum::serve(listener, app).with_graceful_shutdown(shutdown).await;
        pump.abort();
        result
    }

    async fn event_pump(self: Arc<Self>) {
        let mut tick = tokio::time::interval(self.config().event_poll);
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            tick.tick().await;
            let state = self.state.clone();
            let polled = tokio::task::spawn_blocking(move || -> Option<String> {
                if !mib_app_commands::is_initialized(&state).unwrap_or(false) {
                    return None;
                }
                let envelope = mib_app_commands::poll_events_exact(&state).ok()?;
                let value = serde_json::to_value(envelope).ok()?;
                if value["events"].as_array().is_none_or(|e| e.is_empty()) {
                    return None;
                }
                Some(json!({ "event": value }).to_string())
            })
            .await;
            if let Ok(Some(text)) = polled {
                // No receivers simply means no client is connected.
                let _ = self.events.send(Arc::new(text));
            }
        }
    }

    fn authorized(&self, query: &AuthQuery, headers: &HeaderMap) -> bool {
        let Some(expected) = &self.config().token else { return true };
        let bearer = headers
            .get("authorization")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.strip_prefix("Bearer "));
        let given = query.token.as_deref().or(bearer).unwrap_or("");
        constant_time_eq(given.as_bytes(), expected.as_bytes())
    }

    /// Run one request. Event draining belongs to the server; `init` is idempotent because
    /// every client (re)initialises on load.
    fn run(&self, client: &str, client_id: u64, cmd: &str, args: Value) -> Result<Reply, String> {
        let data = std::path::Path::new(&self.config().data_dir);
        if CONTROL_COMMANDS.contains(&cmd) && self.controller() != Some(client_id) {
            return Err("VIEWER_ONLY: another client controls the instrument (take_control to claim it)".into());
        }
        match cmd {
            "poll_events" | "poll_events_exact" => Err("SERVER_OWNS_EVENTS".into()),
            "take_control" => {
                if self.take_control(client_id) {
                    self.broadcast_session();
                }
                Ok(Reply::Json(json!({ "controller_id": client_id })))
            }
            "app_paths" => Ok(Reply::Json(platform::app_paths(data))),
            "get_preferences" => platform::get_preferences(data).map(Reply::Json),
            "set_preferences" => {
                platform::set_preferences(data, args.get("preferences").unwrap_or(&Value::Null)).map(|_| Reply::Json(Value::Null))
            }
            "shell_log" => platform::shell_log(
                data,
                client,
                args["level"].as_str().unwrap_or("info"),
                args["message"].as_str().unwrap_or(""),
            )
            .map(|_| Reply::Json(Value::Null)),
            "init" if mib_app_commands::is_initialized(&self.state).unwrap_or(false) => Ok(Reply::Json(json!(true))),
            _ => dispatch::dispatch(&self.state, &*self.host, cmd, args),
        }
    }

    fn client_gone(self: &Arc<Self>) {
        if self.clients.fetch_sub(1, Ordering::SeqCst) != 1 {
            return;
        }
        let connects = self.connects.load(Ordering::SeqCst);
        let server = self.clone();
        tokio::spawn(async move {
            tokio::time::sleep(server.config().client_grace).await;
            if server.clients() != 0 || server.connects.load(Ordering::SeqCst) != connects {
                return; // a client came back
            }
            let state = server.state.clone();
            let actions = tokio::task::spawn_blocking(move || stop_and_save(&state)).await.unwrap_or_default();
            for action in &actions {
                eprintln!("yofo-studio-server: no client for {:?}: {action}", server.config().client_grace);
            }
            server.stop_and_saves.fetch_add(1, Ordering::SeqCst);
        });
    }
}

/// Finalise what an absent operator can no longer supervise; returns what was done.
pub fn stop_and_save(state: &AppState) -> Vec<String> {
    let mut actions = Vec::new();
    if !mib_app_commands::is_initialized(state).unwrap_or(false) {
        return actions;
    }
    if let Ok(status) = mib_app_commands::fetch_experiment_status(state) {
        let status = serde_json::to_value(status).unwrap_or_default();
        let running = status["valid"] == json!(true)
            && matches!(status["state"].as_u64(), Some(s) if s == EXPERIMENT_STARTING as u64 || s == EXPERIMENT_ACTIVE as u64);
        if running {
            match mib_app_commands::experiment_stop(state) {
                Ok(r) => actions.push(format!("experiment_stop: {}", serde_json::to_string(&r).unwrap_or_default())),
                Err(e) => actions.push(format!("experiment_stop failed: {e}")),
            }
        }
    }
    if let Ok(preview) = mib_app_commands::preview_buffer::fetch_preview_buffer(state) {
        if preview["recording"] == json!(true) {
            match mib_app_commands::stop_recording(state) {
                Ok(r) => actions.push(format!("stop_recording: {}", serde_json::to_string(&r).unwrap_or_default())),
                Err(e) => actions.push(format!("stop_recording failed: {e}")),
            }
        }
    }
    actions
}

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[derive(Deserialize)]
struct AuthQuery {
    token: Option<String>,
}

async fn health(State(server): State<Arc<Server>>) -> Json<Value> {
    let state = server.state.clone();
    let initialized = tokio::task::spawn_blocking(move || mib_app_commands::is_initialized(&state).unwrap_or(false))
        .await
        .unwrap_or(false);
    Json(json!({ "clients": server.clients(), "initialized": initialized, "stop_and_saves": server.stop_and_saves() }))
}

async fn upgrade(
    State(server): State<Arc<Server>>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    Query(query): Query<AuthQuery>,
    headers: HeaderMap,
    ws: WebSocketUpgrade,
) -> Response {
    if !server.authorized(&query, &headers) {
        return (StatusCode::UNAUTHORIZED, "token required").into_response();
    }
    // Frame packets reach 32 MiB plus the header; requests are small JSON documents.
    ws.max_message_size(64 << 20).on_upgrade(move |socket| connection(server, socket, peer.to_string()))
}

#[derive(Deserialize)]
struct Request {
    request_id: u64,
    cmd: String,
    #[serde(default)]
    args: Value,
}

async fn connection(server: Arc<Server>, socket: WebSocket, peer: String) {
    server.clients.fetch_add(1, Ordering::SeqCst);
    let client_id = server.connects.fetch_add(1, Ordering::SeqCst) + 1;
    server.client_joined(client_id);
    let (mut sink, mut stream) = socket.split();
    let (out, mut outbox) = mpsc::channel::<Message>(64);
    let _ = out.send(Message::Text(server.session_message(client_id).into())).await;

    let writer = tokio::spawn(async move {
        while let Some(message) = outbox.recv().await {
            if sink.send(message).await.is_err() {
                break;
            }
        }
    });
    let mut events = server.events.subscribe();
    let event_out = out.clone();
    let forwarder = tokio::spawn(async move {
        loop {
            match events.recv().await {
                Ok(text) => {
                    if event_out.send(Message::Text(text.as_str().into())).await.is_err() {
                        break;
                    }
                }
                // A slow client misses notifications, not acquisition (json_transport v1).
                Err(broadcast::error::RecvError::Lagged(_)) => continue,
                Err(broadcast::error::RecvError::Closed) => break,
            }
        }
    });
    let ping_out = out.clone();
    let ping_every = server.config().ping_interval;
    let pinger = tokio::spawn(async move {
        let mut tick = tokio::time::interval(ping_every);
        loop {
            tick.tick().await;
            if ping_out.send(Message::Ping(Vec::new().into())).await.is_err() {
                break;
            }
        }
    });

    let silence = server.config().ping_timeout;
    loop {
        let message = match tokio::time::timeout(silence, stream.next()).await {
            Ok(Some(Ok(message))) => message,
            _ => break, // closed, failed, or silent past the ping timeout
        };
        let text = match message {
            Message::Text(text) => text,
            Message::Close(_) => break,
            _ => continue, // pong / ping / binary: proof of life only
        };
        let reply = match serde_json::from_str::<Request>(text.as_str()) {
            Err(e) => Message::Text(json!({ "error": format!("BAD_REQUEST: {e}") }).to_string().into()),
            Ok(request) => {
                let worker = server.clone();
                let Request { request_id, cmd, args } = request;
                let client = peer.clone();
                let result = tokio::task::spawn_blocking(move || worker.run(&client, client_id, &cmd, args))
                    .await
                    .unwrap_or_else(|e| Err(format!("COMMAND_PANICKED: {e}")));
                match result {
                    Ok(Reply::Json(value)) => Message::Text(json!({ "request_id": request_id, "ok": value }).to_string().into()),
                    Ok(Reply::Binary(bytes)) => {
                        let mut framed = Vec::with_capacity(8 + bytes.len());
                        framed.extend_from_slice(&request_id.to_le_bytes());
                        framed.extend_from_slice(&bytes);
                        Message::Binary(framed.into())
                    }
                    Err(error) => Message::Text(json!({ "request_id": request_id, "error": error }).to_string().into()),
                }
            }
        };
        if out.send(reply).await.is_err() {
            break;
        }
    }
    forwarder.abort();
    pinger.abort();
    drop(out);
    let _ = writer.await;
    if server.client_left(client_id) {
        server.broadcast_session();
    }
    server.client_gone();
}
