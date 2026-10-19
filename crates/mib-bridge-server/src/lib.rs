//! YOFO Studio headless backend server (impl spec S5).
//!
//! Runs the backend in-process (`mib-app-commands` over `mib-bridge`) and exposes the same
//! commands the Tauri shell has over one WebSocket per client, so the React UI can run in a
//! browser against the instrument's PS.
//!
//! Files: `GET /files?path=` lists and `GET /files/download?path=` downloads (Range supported), read-only, under
//! the data dir only (`files.rs`: canonicalised paths, no symlinks out of the root, no dot-names, files an
//! experiment is still writing are `in_progress` and not downloadable, at most 2 downloads at once, the
//! token rule of `/ws`, a request from another origin refused, listings paged and limited, files opened beneath
//! the root descriptor with `openat2`). The same Origin rule applies to the `/ws` upgrade: a browser always sends
//! `Origin` on a WebSocket handshake, so another site's page cannot drive the instrument through the visitor's
//! browser, with or without a token (`--allow-origin` adds origins for a UI served from elsewhere in development).
//!
//! Diagnostics: `GET /diagnostics?lines=` (`diagnostics.rs`): versions, the installed bundle line, uptime, the tail of
//! `<data>/logs/app.log` (size-capped) and the start-up key lines; read-only, same rules as `/files`.
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
//! recording `stop_recording`; a busy Z stage `stage_stop`; a PZ7035 in Align or Run goes to its safe
//! state (LED off, cell path off, camera released); last, every connected pump gets `pump_stop` and
//! `pump_stop_purge` (a failed stop is reported). On other instruments capture keeps running. This differs on purpose from the desktop
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

mod diagnostics;
mod files;
mod ssd_export;
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
    /// Origins allowed besides the server's own (`--allow-origin`, repeatable): a UI served from elsewhere in development.
    pub allowed_origins: Vec<String>,
    /// Concurrent `/files` listings (a listing scans a directory); the next one is 429.
    pub max_listings: usize,
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
            allowed_origins: Vec::new(),
            max_listings: 2,
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
    /// Identifies this server process: `/auth` reports it, so a browser that lost the link can
    /// tell a restarted (or rebooted) backend from a network blip and reload its state.
    boot_id: String,
    /// Downloads in progress (`/files/download`), capped at `files::MAX_DOWNLOADS`.
    downloads: Arc<tokio::sync::Semaphore>,
    /// One SSD run download at a time (#667): the SSD has one bounce buffer.
    ssd_export: Arc<tokio::sync::Semaphore>,
    listings: Arc<tokio::sync::Semaphore>,
    /// When this server process started (diagnostics uptime).
    started: std::time::Instant,
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
    "review_export_csv", "apply_config_document", "profile_command", "restore_startup_configuration",
    "processing_core_command",
    // Reads and writes files on the instrument (the PZ7035 camera document is confined to <data>/camera/): control only.
    "camera_document",
    "save_preview_buffer", "set_processed_preview_enabled",
    // PZ7035 (#501 P1): camera modes and the LED drive hardware; service mode unlocks raw LED.
    "set_instrument_mode", "sync_wall_clock", "ring_freeze", "ring_resume", "apply_pz_camera_document", "set_service_mode", "set_instrument_led",
    // Z stage (#464). stage_stop is deliberately absent: any client may stop
    // the axis (Stop is always accepted, ADR 0013 §5).
    "stage_connect", "stage_disconnect", "stage_move_to", "stage_move_by", "stage_set_zero",
    "stage_apply_profile",
];

impl Server {
    pub fn new(config: ServerConfig, state: Arc<AppState>) -> Arc<Self> {
        let (events, _) = broadcast::channel(256);
        let max_listings = config.max_listings;
        Arc::new(Server {
            state,
            host: Arc::new(ConfigHost(config)),
            events,
            clients: AtomicUsize::new(0),
            connects: AtomicU64::new(0),
            stop_and_saves: AtomicU64::new(0),
            boot_id: new_boot_id(),
            downloads: Arc::new(tokio::sync::Semaphore::new(files::MAX_DOWNLOADS)),
            ssd_export: Arc::new(tokio::sync::Semaphore::new(1)),
            listings: Arc::new(tokio::sync::Semaphore::new(max_listings)),
            started: std::time::Instant::now(),
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
            .route("/auth", get(auth))
            .route("/diagnostics", get(diagnostics::diagnostics))
            .route("/files", get(files::list))
            .route("/files/download", get(files::download))
            .route("/ssd/runs/{id}/records", get(ssd_export::records))
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
    // A Z stage move left running by a vanished operator is stopped.
    if let Ok(stage) = mib_app_commands::fetch_stage_status(state) {
        let stage = serde_json::to_value(stage).unwrap_or_default();
        if stage["busy"] == json!(true) {
            match mib_app_commands::stage_stop(state) {
                Ok(r) => actions.push(format!("stage_stop: {}", serde_json::to_string(&r).unwrap_or_default())),
                Err(e) => actions.push(format!("stage_stop failed: {e}")),
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
    // The PZ7035 left in Align (LED strobing) or Run with nobody watching goes to its safe state:
    // LED off, cell path off, camera released. The sensor keeps its last timing.
    if let Ok(status) = mib_app_commands::fetch_instrument_status(state) {
        let name = status["mode"]["name"].as_str().unwrap_or("unknown");
        if status["available"] == json!(true) && (name == "align" || name == "run") {
            match mib_app_commands::set_instrument_mode(state, "idle", 0, 0) {
                Ok(r) => actions.push(format!("instrument idle (was {name}): {}", serde_json::to_string(&r).unwrap_or_default())),
                Err(e) => actions.push(format!("instrument idle failed: {e}")),
            }
        }
    }
    // Every connected sample or sheath pump is stopped, run and purge (#711). This runs last: pump
    // commands do serial I/O under the bridge lock, and a stalled adapter must not hold back the
    // recording stop or the PZ7035 safe state above. Since #720 that I/O is bounded (about a second
    // per command on a stalled adapter), so the pass always finishes.
    actions.extend(stop_pumps(
        |pump| mib_app_commands::fetch_pump_status(state, pump).map(|s| serde_json::to_value(s).unwrap_or_default()),
        |cmd, pump| {
            match cmd {
                "pump_stop" => mib_app_commands::pump_stop(state, pump),
                _ => mib_app_commands::pump_stop_purge(state, pump),
            }
            .map(|r| serde_json::to_value(r).unwrap_or_default())
        },
    ));
    actions
}

/// The pump slots (contract `pump_ids`) and the stops each connected one gets on client loss.
const PUMPS: [(u32, &str); 2] = [(0, "sample"), (1, "sheath")];
const PUMP_STOPS: [&str; 2] = ["pump_stop", "pump_stop_purge"];

/// Stop every connected pump and any purge (#711), DLSP syringe and Tushui peristaltic alike.
/// `status` reads a slot's `fetch_pump_status`, `stop` sends a `PUMP_STOPS` command; both return the
/// JSON the client would see. A connected pump gets both stops whatever it reports: the snapshot is
/// only as fresh as the last poll, and the DLSP purges through another register than it runs (the
/// Tushui stops its head either way). A stop that fails is reported, and so is a status that cannot
/// be read (the pump is then stopped anyway).
fn stop_pumps(
    status: impl Fn(u32) -> Result<Value, String>,
    stop: impl Fn(&str, u32) -> Result<Value, String>,
) -> Vec<String> {
    let mut actions = Vec::new();
    for (pump, name) in PUMPS {
        match status(pump) {
            Ok(s) if s["connected"] != json!(true) => continue,
            Ok(_) => {}
            Err(e) => actions.push(format!("pump status {name} failed: {e}")),
        }
        for cmd in PUMP_STOPS {
            match stop(cmd, pump) {
                Ok(r) if r["ok"] == json!(true) => actions.push(format!("{cmd} {name}: {r}")),
                Ok(r) => actions.push(format!("{cmd} {name} failed: {r}")),
                Err(e) => actions.push(format!("{cmd} {name} failed: {e}")),
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

/// Token check without a WebSocket (#501): a browser cannot read the 401 of a refused upgrade,
/// so the UI asks here first and prompts for the token instead of failing silently.
async fn auth(State(server): State<Arc<Server>>, Query(query): Query<AuthQuery>, headers: HeaderMap) -> Response {
    let required = server.config().token.is_some();
    if server.authorized(&query, &headers) {
        Json(json!({ "authorized": true, "token_required": required, "boot_id": server.boot_id })).into_response()
    } else {
        (StatusCode::UNAUTHORIZED, Json(json!({ "authorized": false, "token_required": required, "boot_id": server.boot_id })))
            .into_response()
    }
}

/// A value that differs for every server process (wall clock nanoseconds mixed with the pid).
fn new_boot_id() -> String {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0);
    format!("{:016x}", nanos ^ (u64::from(std::process::id()) << 40))
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
    // A browser always sends Origin on a WebSocket handshake: another site's page cannot drive the
    // instrument through the visitor's browser, token or not (--no-token leaves the socket open).
    if !files::origin_allowed(&server, &headers) {
        return (StatusCode::FORBIDDEN, "requests from another origin are refused").into_response();
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

#[cfg(test)]
mod stage_control_tests {
    use super::CONTROL_COMMANDS;

    // The frame ring (#649, ABI 34): Stop and Resume change what the instrument does (the LED, a new ring), so they need control; reading the
    // status and a buffered frame does not (a viewer may watch the playback).
    #[test]
    fn ring_stop_and_resume_are_control_commands_but_reads_are_not() {
        for cmd in ["ring_freeze", "ring_resume", "apply_pz_camera_document", "camera_document"] {
            assert!(CONTROL_COMMANDS.contains(&cmd), "{cmd} must be a CONTROL command");
        }
        for cmd in ["fetch_ring_status", "fetch_ring_frame", "fetch_ssd_status", "fetch_ssd_runs", "fetch_run_fps", "check_pz_camera_document"] {
            assert!(!CONTROL_COMMANDS.contains(&cmd), "{cmd} is a read: viewers may use it");
        }
    }

    // Z stage (#464): everything that can move or reconfigure the stage needs
    // control; Stop and the status read stay available to every client.
    #[test]
    fn stage_motion_is_control_only_but_stop_is_not() {
        for cmd in ["stage_connect", "stage_disconnect", "stage_move_to", "stage_move_by", "stage_set_zero",
                    "stage_apply_profile"] {
            assert!(CONTROL_COMMANDS.contains(&cmd), "{cmd} must be a CONTROL command");
        }
        assert!(!CONTROL_COMMANDS.contains(&"stage_stop"), "any client may stop the stage");
        assert!(!CONTROL_COMMANDS.contains(&"fetch_stage_status"), "viewers may read the status");
    }
}

#[cfg(test)]
mod pump_stop_tests {
    use super::stop_pumps;
    use serde_json::{json, Value};
    use std::cell::RefCell;

    fn status(connected: &[u32]) -> impl Fn(u32) -> Result<Value, String> + '_ {
        // run_status 0 (Stop) on purpose: a stale snapshot must not spare a pump.
        move |pump| Ok(json!({ "valid": true, "connected": connected.contains(&pump), "run_status": 0 }))
    }

    // #711: the last client leaving stops every connected pump, run and purge, and says so.
    #[test]
    fn client_loss_stops_every_connected_pump_and_its_purge() {
        let sent = RefCell::new(Vec::new());
        let stop = |cmd: &str, pump: u32| {
            sent.borrow_mut().push((cmd.to_string(), pump));
            Ok(json!({ "ok": true, "message": "Pump stopped" }))
        };
        let actions = stop_pumps(status(&[0, 1]), stop);
        assert_eq!(
            *sent.borrow(),
            [("pump_stop".to_string(), 0), ("pump_stop_purge".to_string(), 0), ("pump_stop".to_string(), 1), ("pump_stop_purge".to_string(), 1)]
        );
        assert_eq!(actions.len(), 4, "{actions:?}");
        for expected in ["pump_stop sample: ", "pump_stop_purge sample: ", "pump_stop sheath: ", "pump_stop_purge sheath: "] {
            assert!(actions.iter().any(|a| a.starts_with(expected)), "{expected} in {actions:?}");
        }

        // A slot with no pump connected is left alone.
        sent.borrow_mut().clear();
        let actions = stop_pumps(status(&[1]), stop);
        assert_eq!(*sent.borrow(), [("pump_stop".to_string(), 1), ("pump_stop_purge".to_string(), 1)]);
        assert!(actions.iter().all(|a| a.contains("sheath")), "{actions:?}");
        assert!(stop_pumps(status(&[]), stop).is_empty());
    }

    // #711: a pump that does not stop is reported, never dropped, and does not spare the other pump.
    #[test]
    fn client_loss_reports_a_pump_that_fails_to_stop() {
        let sent = RefCell::new(Vec::new());
        let stop = |cmd: &str, pump: u32| {
            sent.borrow_mut().push((cmd.to_string(), pump));
            match (cmd, pump) {
                ("pump_stop", 0) => Ok(json!({ "ok": false, "message": "Pump stop failed" })),
                ("pump_stop_purge", 0) => Err("bridge lock poisoned".to_string()),
                _ => Ok(json!({ "ok": true, "message": "Pump stopped" })),
            }
        };
        let actions = stop_pumps(status(&[0, 1]), stop);
        assert_eq!(sent.borrow().len(), 4, "the sheath pump is still stopped: {:?}", sent.borrow());
        assert!(
            actions.iter().any(|a| a.starts_with("pump_stop sample failed: ") && a.contains("Pump stop failed")),
            "{actions:?}"
        );
        assert!(actions.iter().any(|a| a == "pump_stop_purge sample failed: bridge lock poisoned"), "{actions:?}");
        assert!(actions.iter().any(|a| a.starts_with("pump_stop sheath: ")), "{actions:?}");

        // A status that cannot be read is reported, and the pump is stopped anyway.
        sent.borrow_mut().clear();
        let unreadable = |pump: u32| if pump == 0 { Err("bridge lock poisoned".to_string()) } else { Ok(json!({ "connected": false })) };
        let actions = stop_pumps(unreadable, stop);
        assert_eq!(actions[0], "pump status sample failed: bridge lock poisoned");
        assert_eq!(*sent.borrow(), [("pump_stop".to_string(), 0), ("pump_stop_purge".to_string(), 0)]);
    }
}
