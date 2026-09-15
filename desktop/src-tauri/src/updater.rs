//! Update-manifest verification (BE-9, issue #279, epic #246).
//!
//! The Qt release contract publishes an update manifest whose installer
//! artifact is pinned by SHA-256 (see verify-update-manifest.py and the
//! publish-update tooling). This module is the Rust/Tauri side of that
//! contract: parsing **fails closed** — a manifest missing its version, URL,
//! or SHA-256 (or carrying a malformed digest) is rejected outright, and an
//! artifact whose bytes do not hash to the pinned digest is rejected before
//! any installer launch. Download/launch/rollback build on this and are the
//! Windows half of #279.

use serde::Deserialize;
use sha2::{Digest, Sha256};

/// One update entry as published in the update manifest. Unknown fields are
/// ignored (additive manifests); missing REQUIRED fields fail parsing.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct UpdateManifest {
    pub version: String,
    pub url: String,
    pub sha256: String,
    #[serde(default)]
    pub channel: Option<String>,
    #[serde(default)]
    pub notes: Option<String>,
}

#[derive(Debug, PartialEq, Eq)]
pub enum VerifyError {
    MalformedManifest(String),
    MissingField(&'static str),
    MalformedDigest,
    DigestMismatch,
}

impl std::fmt::Display for VerifyError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            VerifyError::MalformedManifest(e) => write!(f, "malformed update manifest: {e}"),
            VerifyError::MissingField(field) => write!(f, "update manifest missing '{field}'"),
            VerifyError::MalformedDigest => write!(f, "update manifest sha256 is not a 64-char hex digest"),
            VerifyError::DigestMismatch => write!(f, "artifact SHA-256 does not match the manifest"),
        }
    }
}

/// Parse a manifest document, failing closed on anything suspicious.
pub fn parse_manifest(json: &str) -> Result<UpdateManifest, VerifyError> {
    let manifest: UpdateManifest = serde_json::from_str(json)
        .map_err(|e| VerifyError::MalformedManifest(e.to_string()))?;
    if manifest.version.trim().is_empty() {
        return Err(VerifyError::MissingField("version"));
    }
    if manifest.url.trim().is_empty() {
        return Err(VerifyError::MissingField("url"));
    }
    let digest = manifest.sha256.trim();
    if digest.len() != 64 || !digest.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(VerifyError::MalformedDigest);
    }
    Ok(manifest)
}

/// Verify artifact bytes against the manifest's pinned SHA-256. Fails closed:
/// any mismatch (or malformed digest) rejects the artifact.
pub fn verify_artifact(manifest: &UpdateManifest, artifact: &[u8]) -> Result<(), VerifyError> {
    let expected = manifest.sha256.trim().to_ascii_lowercase();
    if expected.len() != 64 || !expected.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(VerifyError::MalformedDigest);
    }
    let actual = hex::encode(Sha256::digest(artifact));
    if actual != expected {
        return Err(VerifyError::DigestMismatch);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn manifest_json(sha: &str) -> String {
        format!(
            r#"{{"version":"1.2.3","url":"https://updates.yofo.bio/x.exe","sha256":"{sha}","channel":"stable"}}"#
        )
    }

    #[test]
    fn valid_manifest_and_matching_artifact_verify() {
        let artifact = b"installer-bytes";
        let sha = hex::encode(Sha256::digest(artifact));
        let manifest = parse_manifest(&manifest_json(&sha)).unwrap();
        assert_eq!(manifest.version, "1.2.3");
        assert!(verify_artifact(&manifest, artifact).is_ok());
    }

    #[test]
    fn tampered_artifact_fails_closed() {
        let sha = hex::encode(Sha256::digest(b"installer-bytes"));
        let manifest = parse_manifest(&manifest_json(&sha)).unwrap();
        assert_eq!(
            verify_artifact(&manifest, b"tampered-bytes"),
            Err(VerifyError::DigestMismatch)
        );
    }

    #[test]
    fn missing_or_empty_fields_fail_closed() {
        assert!(matches!(
            parse_manifest(r#"{"version":"1.0","url":"https://x"}"#),
            Err(VerifyError::MalformedManifest(_))
        ));
        let sha = "a".repeat(64);
        assert_eq!(
            parse_manifest(&format!(r#"{{"version":"","url":"https://x","sha256":"{sha}"}}"#)),
            // Struct compare unavailable; match the variant.
            Err(VerifyError::MissingField("version"))
        );
        assert_eq!(
            parse_manifest(&format!(r#"{{"version":"1.0","url":" ","sha256":"{sha}"}}"#)),
            Err(VerifyError::MissingField("url"))
        );
    }

    #[test]
    fn malformed_digests_fail_closed() {
        assert_eq!(
            parse_manifest(r#"{"version":"1.0","url":"https://x","sha256":"deadbeef"}"#),
            Err(VerifyError::MalformedDigest)
        );
        assert_eq!(
            parse_manifest(&manifest_json(&"z".repeat(64))),
            Err(VerifyError::MalformedDigest)
        );
    }

    #[test]
    fn malformed_json_fails_closed() {
        assert!(matches!(
            parse_manifest("{not json"),
            Err(VerifyError::MalformedManifest(_))
        ));
    }
}
