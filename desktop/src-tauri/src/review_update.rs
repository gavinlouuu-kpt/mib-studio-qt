//! YOFO Review auto-update (plan 2026-10-01-standalone-review-app, PR 6).
//!
//! The Tauri updater plugin fetches `https://updates.yofo.bio/review-<channel>/latest.json`
//! (channels `stable`, `beta`; the window's Preferences pick one), and verifies
//! the downloaded bundle against the minisign public key compiled in from
//! `tauri.review.conf.json` (`plugins.updater.pubkey`). As defence in depth —
//! the same fail-closed rule as MIB Studio's `updater.rs` (BE-9) — the bytes
//! must also hash to the `sha256` the publisher writes into the platform entry
//! (`scripts/release/publish-review-update.py`); a missing or wrong digest is
//! refused before anything is installed.
//!
//! No public key configured (developer builds, CI before the key exists) means
//! no updater: the plugin is not registered and the commands report
//! `configured: false`.

use serde::Serialize;
use sha2::{Digest, Sha256};
use tauri::AppHandle;
use tauri_plugin_updater::UpdaterExt;

pub const UPDATE_HOST: &str = "https://updates.yofo.bio";

/// The manifest URL for a channel.
pub fn channel_endpoint(channel: &str) -> Result<String, String> {
    match channel {
        "stable" | "beta" => Ok(format!("{UPDATE_HOST}/review-{channel}/latest.json")),
        other => Err(format!("unknown update channel '{other}' (stable, beta)")),
    }
}

/// The minisign public key compiled into this build, "" when none.
pub fn configured_pubkey(config: &tauri::Config) -> String {
    config
        .plugins
        .0
        .get("updater")
        .and_then(|u| u.get("pubkey"))
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .trim()
        .to_string()
}

/// Fail closed unless `bytes` hash to `platforms.<target>.sha256` of the
/// release document.
pub fn verify_sha256(release: &serde_json::Value, target: &str, bytes: &[u8]) -> Result<(), String> {
    let pinned = release["platforms"][target]["sha256"]
        .as_str()
        .ok_or_else(|| format!("update manifest has no sha256 for {target}; refusing to install"))?
        .trim()
        .to_ascii_lowercase();
    if pinned.len() != 64 || !pinned.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!("update manifest sha256 for {target} is malformed; refusing to install"));
    }
    let actual = hex::encode(Sha256::digest(bytes));
    if actual != pinned {
        return Err(format!("downloaded update does not match its sha256 ({actual} != {pinned}); refusing to install"));
    }
    Ok(())
}

#[derive(Serialize, Clone, Debug, Default)]
pub struct UpdateStatus {
    /// This build carries a public key (otherwise nothing else is set).
    pub configured: bool,
    pub channel: String,
    pub current: String,
    pub available: bool,
    pub version: String,
    pub notes: String,
    pub date: String,
}

fn updater_for(app: &AppHandle, channel: &str) -> Result<tauri_plugin_updater::Updater, String> {
    let url: tauri::Url = channel_endpoint(channel)?.parse().map_err(|e| format!("bad endpoint: {e}"))?;
    app.updater_builder()
        .endpoints(vec![url])
        .map_err(|e| e.to_string())?
        .build()
        .map_err(|e| e.to_string())
}

/// Is there a newer YOFO Review on `channel`?
#[tauri::command]
pub async fn review_check_update(app: AppHandle, channel: String) -> Result<UpdateStatus, String> {
    let mut status = UpdateStatus {
        channel: channel.clone(),
        current: app.package_info().version.to_string(),
        ..Default::default()
    };
    if configured_pubkey(&app.config()).is_empty() {
        return Ok(status);
    }
    status.configured = true;
    if let Some(update) = updater_for(&app, &channel)?.check().await.map_err(|e| e.to_string())? {
        status.available = true;
        status.version = update.version.clone();
        status.notes = update.body.clone().unwrap_or_default();
        status.date = update.date.map(|d| d.to_string()).unwrap_or_default();
    }
    Ok(status)
}

/// Download (minisign-verified by the plugin), check the pinned SHA-256,
/// install and restart. Returns only on failure.
#[tauri::command]
pub async fn review_install_update(app: AppHandle, channel: String) -> Result<(), String> {
    if configured_pubkey(&app.config()).is_empty() {
        return Err("this build has no update key".into());
    }
    let update = updater_for(&app, &channel)?
        .check()
        .await
        .map_err(|e| e.to_string())?
        .ok_or("no update available")?;
    let bytes = update.download(|_, _| {}, || {}).await.map_err(|e| e.to_string())?;
    verify_sha256(&update.raw_json, &update.target, &bytes)?;
    update.install(bytes).map_err(|e| e.to_string())?;
    app.restart();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn endpoints_per_channel() {
        assert_eq!(channel_endpoint("stable").unwrap(), "https://updates.yofo.bio/review-stable/latest.json");
        assert_eq!(channel_endpoint("beta").unwrap(), "https://updates.yofo.bio/review-beta/latest.json");
        assert!(channel_endpoint("nightly").is_err());
    }

    #[test]
    fn sha256_pin_fails_closed() {
        let bytes = b"bundle bytes";
        let digest = hex::encode(Sha256::digest(bytes));
        let doc = |sha: Option<&str>| match sha {
            Some(s) => serde_json::json!({"platforms": {"windows-x86_64": {"url": "u", "signature": "s", "sha256": s}}}),
            None => serde_json::json!({"platforms": {"windows-x86_64": {"url": "u", "signature": "s"}}}),
        };
        assert!(verify_sha256(&doc(Some(&digest)), "windows-x86_64", bytes).is_ok());
        assert!(verify_sha256(&doc(Some(&digest.to_uppercase())), "windows-x86_64", bytes).is_ok());
        assert!(verify_sha256(&doc(Some(&digest)), "darwin-aarch64", bytes).is_err(), "other platform");
        assert!(verify_sha256(&doc(None), "windows-x86_64", bytes).is_err(), "missing digest");
        assert!(verify_sha256(&doc(Some("abc")), "windows-x86_64", bytes).is_err(), "malformed digest");
        assert!(verify_sha256(&doc(Some(&"0".repeat(64))), "windows-x86_64", bytes).is_err(), "mismatch");
    }

    #[test]
    fn pubkey_from_plugin_config() {
        let mut config: tauri::Config = serde_json::from_value(serde_json::json!({"identifier": "x.y.z"})).unwrap();
        assert_eq!(configured_pubkey(&config), "");
        config.plugins.0.insert("updater".into(), serde_json::json!({"pubkey": "  KEY  "}));
        assert_eq!(configured_pubkey(&config), "KEY");
    }
}
