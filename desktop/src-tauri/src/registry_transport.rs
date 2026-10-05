//! HTTPS POST for the backend's central profile registry worker (#398,
//! bridge ABI 24; the ADR 0002 seam: the C++ backend links no HTTP client and
//! the shell supplies one).
//!
//! Contract (`ffi::BridgeHttpRequest`): HTTPS only, platform TLS verification
//! (never disabled), no redirects, `timeout_ms` for the whole exchange, stop
//! reading past `max_response_bytes`, no logging of headers or bodies, and a
//! prompt abort (status 0) once `ffi::registry_request_cancelled` reports the
//! request's handle as cancelled. The function runs on the registry worker
//! thread and must never panic across the FFI boundary.

use std::sync::mpsc;
use std::time::Duration;

use mib_bridge::ffi;

const CANCEL_POLL: Duration = Duration::from_millis(50);

/// Transport failure / timeout / cancellation, as the backend expects it.
fn failure() -> ffi::BridgeHttpResponse {
    ffi::BridgeHttpResponse { status: 0, body: Vec::new() }
}

fn agent(timeout: Duration) -> ureq::Agent {
    let tls = ureq::tls::TlsConfig::builder()
        .root_certs(ureq::tls::RootCerts::PlatformVerifier)
        .build();
    ureq::Agent::config_builder()
        .https_only(true)
        .max_redirects(0)
        // A 3xx/4xx/5xx is a status for the backend to interpret, not an error.
        .http_status_as_error(false)
        .timeout_global(Some(timeout))
        .tls_config(tls)
        .build()
        .new_agent()
}

/// Performs the exchange; `None` is a transport failure (incl. timeout).
fn exchange(
    url: &str,
    body: &str,
    headers: &[(String, String)],
    timeout: Duration,
    max_bytes: u64,
) -> Option<ffi::BridgeHttpResponse> {
    let mut request = agent(timeout).post(url);
    for (name, value) in headers {
        request = request.header(name.as_str(), value.as_str());
    }
    let mut response = request.send(body).ok()?;
    let status = u32::from(response.status().as_u16());
    // Read at most cap+1 bytes: an oversized body reaches the backend as
    // longer than its cap, which it reports as "response exceeds limit".
    let body = match response.body_mut().with_config().limit(max_bytes.saturating_add(1)).read_to_vec() {
        Ok(bytes) => bytes,
        Err(ureq::Error::BodyExceedsLimit(_)) => vec![0u8; usize::try_from(max_bytes).ok()?.saturating_add(1)],
        Err(_) => return None,
    };
    Some(ffi::BridgeHttpResponse { status, body })
}

/// A request that passed `validated`, ready to send.
struct Validated {
    url: String,
    body: String,
    headers: Vec<(String, String)>,
    timeout: Duration,
    max_bytes: u64,
}

/// Validates the request without any network access. `None` = refuse.
fn validated(request: &ffi::BridgeHttpRequest) -> Option<Validated> {
    if !request.url.starts_with("https://") || request.timeout_ms == 0 || request.max_response_bytes == 0 {
        return None;
    }
    let headers = request
        .headers
        .iter()
        .map(|h| (h.name.clone(), h.value.clone()))
        .collect::<Vec<_>>();
    if headers.iter().any(|(n, v)| n.contains(['\r', '\n']) || v.contains(['\r', '\n'])) {
        return None;
    }
    Some(Validated {
        url: request.url.clone(),
        body: request.body.clone(),
        headers,
        timeout: Duration::from_millis(u64::from(request.timeout_ms)),
        max_bytes: request.max_response_bytes,
    })
}

/// The `fn` pointer handed to `BackendBridge::set_registry_transport`. The
/// exchange runs on a helper thread so a cancel (registry cancel or backend
/// shutdown) returns at once; the helper ends by itself within the timeout and
/// owns nothing the caller needs.
pub fn post(request: &ffi::BridgeHttpRequest) -> ffi::BridgeHttpResponse {
    std::panic::catch_unwind(|| post_inner(request, ffi::registry_request_cancelled)).unwrap_or_else(|_| failure())
}

fn post_inner(request: &ffi::BridgeHttpRequest, cancelled: fn(u64) -> bool) -> ffi::BridgeHttpResponse {
    let Some(Validated { url, body, headers, timeout, max_bytes }) = validated(request) else {
        return failure();
    };
    let handle = request.cancel_handle;
    if cancelled(handle) {
        return failure();
    }
    let (tx, rx) = mpsc::channel();
    let spawned = std::thread::Builder::new().name("registry-http".into()).spawn(move || {
        let result = std::panic::catch_unwind(|| exchange(&url, &body, &headers, timeout, max_bytes))
            .ok()
            .flatten();
        let _ = tx.send(result);
    });
    if spawned.is_err() {
        return failure();
    }
    // Upper bound even if the helper misbehaves: the request timeout plus slack.
    let deadline = std::time::Instant::now() + timeout + Duration::from_secs(2);
    loop {
        match rx.recv_timeout(CANCEL_POLL) {
            Ok(Some(response)) => return response,
            Ok(None) | Err(mpsc::RecvTimeoutError::Disconnected) => return failure(),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                if cancelled(handle) || std::time::Instant::now() >= deadline {
                    return failure();
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::time::Instant;

    fn request(url: &str, timeout_ms: u32) -> ffi::BridgeHttpRequest {
        ffi::BridgeHttpRequest {
            url: url.to_string(),
            body: "{}".to_string(),
            headers: vec![ffi::BridgeHttpHeader { name: "apikey".into(), value: "sb_publishable_test".into() }],
            timeout_ms,
            max_response_bytes: 1024,
            cancel_handle: 1,
        }
    }

    fn never(_: u64) -> bool {
        false
    }

    static CANCEL: AtomicBool = AtomicBool::new(false);
    fn flag(_: u64) -> bool {
        CANCEL.load(Ordering::SeqCst)
    }

    #[test]
    fn refuses_plain_http_and_header_injection_without_network() {
        assert_eq!(post_inner(&request("http://127.0.0.1:9/rest", 1000), never).status, 0);
        let mut injected = request("https://127.0.0.1:9/rest", 1000);
        injected.headers.push(ffi::BridgeHttpHeader { name: "x".into(), value: "a\r\nInjected: 1".into() });
        assert!(validated(&injected).is_none(), "CR/LF in a header is refused");
        let mut unbounded = request("https://127.0.0.1:9/rest", 0);
        assert!(validated(&unbounded).is_none(), "a zero timeout is refused");
        unbounded.timeout_ms = 1000;
        unbounded.max_response_bytes = 0;
        assert!(validated(&unbounded).is_none(), "a zero response cap is refused");
    }

    // A listener that accepts TCP (kernel backlog) but never answers the TLS
    // handshake: the request ends on its timeout, or at once when cancelled.
    #[test]
    fn stalled_handshake_times_out_and_cancels_promptly() {
        let silent = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("https://127.0.0.1:{}/rest/v1/rpc/x", silent.local_addr().unwrap().port());

        let started = Instant::now();
        assert_eq!(post_inner(&request(&url, 300), never).status, 0, "stalled request fails");
        assert!(started.elapsed() < Duration::from_secs(4), "timeout bounds it: {:?}", started.elapsed());

        CANCEL.store(false, Ordering::SeqCst);
        let canceller = std::thread::spawn(|| {
            std::thread::sleep(Duration::from_millis(200));
            CANCEL.store(true, Ordering::SeqCst);
        });
        let started = Instant::now();
        assert_eq!(post_inner(&request(&url, 20_000), flag).status, 0, "cancelled request fails");
        canceller.join().unwrap();
        assert!(started.elapsed() < Duration::from_secs(3), "cancel beats the 20 s timeout: {:?}", started.elapsed());
    }

    #[test]
    fn already_cancelled_request_never_starts() {
        fn always(_: u64) -> bool {
            true
        }
        let started = Instant::now();
        assert_eq!(post_inner(&request("https://127.0.0.1:9/rest", 20_000), always).status, 0);
        assert!(started.elapsed() < Duration::from_millis(500));
    }
}
