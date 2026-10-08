//! `yofo-studio-server`: the YOFO Studio backend for a remote (browser) UI. See the crate docs
//! for the protocol.
//!
//!   yofo-studio-server [--listen ADDR] [--data-dir DIR] [--resource-dir DIR]
//!                      [--core-cache-dir DIR] [--dist DIR] [--allow-origin ORIGIN]... [--token-file PATH | --no-token]
//!
//! Defaults: listen 0.0.0.0:8427, data dir $XDG_DATA_HOME/yofo-studio (~/.local/share/...),
//! token from /etc/yofo-studio/token. `--no-token` is accepted only on a loopback address.

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;

use mib_app_commands::AppState;
use mib_bridge_server::{stop_and_save, Server, ServerConfig};

fn usage() -> ! {
    eprintln!(
        "usage: yofo-studio-server [--listen ADDR] [--data-dir DIR] [--resource-dir DIR] \
         [--core-cache-dir DIR] [--dist DIR] [--allow-origin ORIGIN]... [--token-file PATH | --no-token]"
    );
    std::process::exit(2)
}

fn default_data_dir() -> String {
    let base = std::env::var("XDG_DATA_HOME")
        .ok()
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var("HOME").ok().map(|h| PathBuf::from(h).join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("/var/lib"));
    base.join("yofo-studio").to_string_lossy().into_owned()
}

#[tokio::main]
async fn main() {
    let mut listen: SocketAddr = "0.0.0.0:8427".parse().unwrap();
    let mut config = ServerConfig::new(default_data_dir());
    let mut token_file = PathBuf::from("/etc/yofo-studio/token");
    let mut no_token = false;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = || args.next().unwrap_or_else(|| usage());
        match arg.as_str() {
            "--listen" => listen = value().parse().unwrap_or_else(|_| usage()),
            "--data-dir" => config.data_dir = value(),
            "--resource-dir" => config.resource_dir = value(),
            "--core-cache-dir" => config.core_cache_dir = value(),
            "--dist" => config.dist_dir = Some(PathBuf::from(value())),
            "--allow-origin" => config.allowed_origins.push(value()),
            "--token-file" => token_file = PathBuf::from(value()),
            "--no-token" => no_token = true,
            _ => usage(),
        }
    }
    if no_token {
        if !listen.ip().is_loopback() {
            eprintln!("yofo-studio-server: --no-token is only allowed on a loopback address");
            std::process::exit(2);
        }
    } else {
        match std::fs::read_to_string(&token_file) {
            Ok(token) if !token.trim().is_empty() => config.token = Some(token.trim().to_string()),
            _ => {
                eprintln!("yofo-studio-server: no token in {} (or pass --no-token on loopback)", token_file.display());
                std::process::exit(2);
            }
        }
    }
    if let Err(e) = std::fs::create_dir_all(&config.data_dir) {
        eprintln!("yofo-studio-server: data dir {}: {e}", config.data_dir);
        std::process::exit(1);
    }

    let state = Arc::new(AppState::new());
    let server = Server::new(config.clone(), state.clone());
    let listener = tokio::net::TcpListener::bind(listen).await.unwrap_or_else(|e| {
        eprintln!("yofo-studio-server: listen {listen}: {e}");
        std::process::exit(1)
    });
    eprintln!(
        "yofo-studio-server: listening on {listen}, data {}, UI {}",
        config.data_dir,
        config.dist_dir.as_ref().map(|d| d.display().to_string()).unwrap_or_else(|| "not served".into())
    );
    let shutdown = async {
        let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()).expect("SIGTERM handler");
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {}
            _ = term.recv() => {}
        }
    };
    if let Err(e) = server.serve(listener, shutdown).await {
        eprintln!("yofo-studio-server: {e}");
    }
    // Same rule as a lost client: finalise, then release the backend.
    let state_for_exit = state.clone();
    let actions = tokio::task::spawn_blocking(move || {
        let actions = stop_and_save(&state_for_exit);
        if let Ok(mut bridge) = state_for_exit.bridge.lock() {
            if bridge.is_initialized() {
                bridge.pin_mut().shutdown();
            }
        }
        actions
    })
    .await
    .unwrap_or_default();
    for action in actions {
        eprintln!("yofo-studio-server: on exit: {action}");
    }
}
