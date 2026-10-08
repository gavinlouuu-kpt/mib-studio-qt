//! `/files` and `/files/download` (#651 G3): read-only, one root, no way out of it.

use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use mib_app_commands::AppState;
use mib_bridge_server::{Server, ServerConfig};
use serde_json::Value;
use serial_test::serial;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

const TOKEN: &str = "files-token";

struct Fixture {
    addr: std::net::SocketAddr,
    root: std::path::PathBuf,
    outside: std::path::PathBuf,
}

async fn start(tag: &str, token: Option<&str>) -> Fixture {
    start_with(tag, token, |_| {}).await
}

async fn start_with(tag: &str, token: Option<&str>, tweak: impl FnOnce(&mut ServerConfig)) -> Fixture {
    let base = std::env::temp_dir().join(format!("yofo_files_{tag}_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&base);
    let root = base.join("data");
    let outside = base.join("outside");
    std::fs::create_dir_all(root.join("runs")).unwrap();
    std::fs::create_dir_all(&outside).unwrap();
    std::fs::write(root.join("b.txt"), b"bravo").unwrap();
    std::fs::write(root.join("a.txt"), b"0123456789").unwrap();
    std::fs::write(root.join(".hidden"), b"secret").unwrap();
    std::fs::write(root.join("runs").join("run1.h5"), b"hdf5").unwrap();
    std::fs::write(outside.join("secret.txt"), b"outside the root").unwrap();
    std::os::unix::fs::symlink(&outside, root.join("escape")).unwrap();
    std::os::unix::fs::symlink(outside.join("secret.txt"), root.join("link.txt")).unwrap();
    std::os::unix::fs::symlink(root.join("a.txt"), root.join("inside-link.txt")).unwrap();
    let mut config = ServerConfig::new(root.to_string_lossy());
    config.token = token.map(str::to_string);
    tweak(&mut config);
    let server = Server::new(config, Arc::new(AppState::new()));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(server.clone().serve(listener, std::future::pending()));
    Fixture { addr, root, outside }
}

struct Reply {
    status: u16,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

impl Reply {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    fn json(&self) -> Value {
        serde_json::from_slice(&self.body).unwrap_or(Value::Null)
    }
}

fn head(method: &str, target: &str, extra: &[(&str, &str)]) -> String {
    let mut text = format!("{method} {target} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
    for (k, v) in extra {
        text.push_str(&format!("{k}: {v}\r\n"));
    }
    text.push_str("\r\n");
    text
}

fn parse(raw: &[u8]) -> Reply {
    let split = raw.windows(4).position(|w| w == b"\r\n\r\n").expect("a full header");
    let header = String::from_utf8_lossy(&raw[..split]).into_owned();
    let mut lines = header.lines();
    let status = lines.next().unwrap().split_whitespace().nth(1).unwrap().parse().unwrap();
    let headers: Vec<(String, String)> = lines
        .filter_map(|l| l.split_once(':').map(|(k, v)| (k.trim().to_string(), v.trim().to_string())))
        .collect();
    let mut body = raw[split + 4..].to_vec();
    // Connection: close with a chunked body is possible for listings; decode it.
    if headers.iter().any(|(k, v)| k.eq_ignore_ascii_case("transfer-encoding") && v.contains("chunked")) {
        let mut out = Vec::new();
        let mut rest = &body[..];
        loop {
            let Some(eol) = rest.windows(2).position(|w| w == b"\r\n") else { break };
            let size = usize::from_str_radix(std::str::from_utf8(&rest[..eol]).unwrap().trim(), 16).unwrap_or(0);
            if size == 0 {
                break;
            }
            out.extend_from_slice(&rest[eol + 2..eol + 2 + size]);
            rest = &rest[eol + 2 + size + 2..];
        }
        body = out;
    }
    Reply { status, headers, body }
}

async fn request(addr: std::net::SocketAddr, method: &str, target: &str, extra: &[(&str, &str)]) -> Reply {
    let mut stream = tokio::net::TcpStream::connect(addr).await.unwrap();
    stream.write_all(head(method, target, extra).as_bytes()).await.unwrap();
    let mut raw = Vec::new();
    tokio::time::timeout(Duration::from_secs(10), stream.read_to_end(&mut raw)).await.expect("reply in time").unwrap();
    parse(&raw)
}

fn names(listing: &Value) -> Vec<String> {
    listing["entries"].as_array().unwrap().iter().map(|e| e["name"].as_str().unwrap().to_string()).collect()
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn lists_the_root_dirs_first_and_hides_dot_files_and_escaping_links() {
    let f = start("list", None).await;
    let reply = request(f.addr, "GET", "/files", &[]).await;
    assert_eq!(reply.status, 200);
    let listing = reply.json();
    assert_eq!(listing["parent"], Value::Null, "the root has no parent");
    let canonical_root = std::fs::canonicalize(&f.root).unwrap();
    assert_eq!(listing["root"], canonical_root.to_string_lossy().as_ref());
    // dirs first, then by name; .hidden, the escaping directory link and the escaping file link are not shown
    assert_eq!(names(&listing), vec!["runs", "a.txt", "b.txt", "inside-link.txt"]);
    let a = listing["entries"].as_array().unwrap().iter().find(|e| e["name"] == "a.txt").unwrap();
    assert_eq!(a["kind"], "file");
    assert_eq!(a["size"], 10);
    assert_eq!(a["in_progress"], false);

    let sub = request(f.addr, "GET", "/files?path=runs", &[]).await.json();
    assert_eq!(names(&sub), vec!["run1.h5"]);
    assert_eq!(sub["parent"], canonical_root.to_string_lossy().as_ref());
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn downloads_with_length_disposition_and_range() {
    let f = start("download", None).await;
    let full = request(f.addr, "GET", "/files/download?path=a.txt", &[]).await;
    assert_eq!(full.status, 200);
    assert_eq!(full.body, b"0123456789");
    assert_eq!(full.header("content-length"), Some("10"));
    assert!(full.header("content-disposition").unwrap().starts_with("attachment; filename=\"a.txt\""));

    let part = request(f.addr, "GET", "/files/download?path=a.txt", &[("Range", "bytes=3-6")]).await;
    assert_eq!(part.status, 206);
    assert_eq!(part.body, b"3456");
    assert!(part.header("content-range").unwrap().starts_with("bytes 3-6/10"));

    // a symlink that stays inside the root is served
    assert_eq!(request(f.addr, "GET", "/files/download?path=inside-link.txt", &[]).await.body, b"0123456789");
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn nothing_outside_the_root_or_under_a_dot_name_is_reachable() {
    let f = start("traversal", None).await;
    let abs_outside = f.outside.join("secret.txt");
    let targets = [
        "/files/download?path=../outside/secret.txt".to_string(),
        "/files/download?path=..%2Foutside%2Fsecret.txt".to_string(),
        "/files/download?path=runs/../../outside/secret.txt".to_string(),
        format!("/files/download?path={}", abs_outside.display()),
        "/files/download?path=/etc/passwd".to_string(),
        "/files/download?path=link.txt".to_string(),           // symlink to a file outside
        "/files/download?path=escape/secret.txt".to_string(),  // through a symlinked directory
        "/files/download?path=.hidden".to_string(),
        "/files?path=..".to_string(),
        "/files?path=escape".to_string(),
        "/files?path=/".to_string(),
        "/files?path=.".to_string(),
    ];
    for target in targets {
        let reply = request(f.addr, "GET", &target, &[]).await;
        let ok_dot = target == "/files?path=."; // "." is the root itself
        if ok_dot {
            assert_eq!(reply.status, 200, "{target}");
        } else {
            assert_eq!(reply.status, 404, "{target}");
            assert!(!String::from_utf8_lossy(&reply.body).contains("outside the root"), "{target}");
        }
    }
    // a directory is not a download
    assert_eq!(request(f.addr, "GET", "/files/download?path=runs", &[]).await.status, 404);
    assert_eq!(request(f.addr, "GET", "/files/download?path=nope.txt", &[]).await.status, 404);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn is_read_only_and_follows_the_token_and_cross_site_rules() {
    let f = start("rules", Some(TOKEN)).await;
    assert_eq!(request(f.addr, "GET", "/files", &[]).await.status, 401, "no token");
    assert_eq!(request(f.addr, "GET", "/files?token=wrong", &[]).await.status, 401);
    assert_eq!(request(f.addr, "GET", &format!("/files?token={TOKEN}"), &[]).await.status, 200);
    let bearer = format!("Bearer {TOKEN}");
    assert_eq!(request(f.addr, "GET", "/files/download?path=a.txt", &[("Authorization", &bearer)]).await.status, 200);
    // Sec-Fetch-Site: same-origin and none pass, anything else is refused
    let url = format!("/files?token={TOKEN}");
    assert_eq!(request(f.addr, "GET", &url, &[("Sec-Fetch-Site", "same-origin")]).await.status, 200);
    assert_eq!(request(f.addr, "GET", &url, &[("Sec-Fetch-Site", "none")]).await.status, 200);
    assert_eq!(request(f.addr, "GET", &url, &[("Sec-Fetch-Site", "cross-site")]).await.status, 403);
    assert_eq!(request(f.addr, "GET", &url, &[("Sec-Fetch-Site", "same-site")]).await.status, 403);
    // no write verbs
    for method in ["POST", "PUT", "DELETE", "PATCH"] {
        let reply = request(f.addr, method, &format!("/files/download?path=a.txt&token={TOKEN}"), &[("Content-Length", "0")]).await;
        assert_eq!(reply.status, 405, "{method}");
    }
    assert!(Path::new(&f.root).join("a.txt").exists());
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn caps_concurrent_downloads() {
    let f = start("cap", None).await;
    // a file bigger than the socket buffers, so a client that only reads the header holds its permit
    let big = f.root.join("big.bin");
    std::fs::write(&big, vec![7u8; 96 * 1024 * 1024]).unwrap();
    let mut holders = Vec::new();
    for _ in 0..2 {
        let mut stream = tokio::net::TcpStream::connect(f.addr).await.unwrap();
        stream.write_all(head("GET", "/files/download?path=big.bin", &[]).as_bytes()).await.unwrap();
        let mut buf = [0u8; 256];
        let n = tokio::time::timeout(Duration::from_secs(5), stream.read(&mut buf)).await.unwrap().unwrap();
        assert!(String::from_utf8_lossy(&buf[..n]).starts_with("HTTP/1.1 200"));
        holders.push(stream); // never read again: the server's body stalls on backpressure
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    let third = request(f.addr, "GET", "/files/download?path=big.bin", &[]).await;
    assert_eq!(third.status, 429);
    drop(holders);
    // once the two clients are gone the permits come back
    let mut ok = false;
    for _ in 0..50 {
        tokio::time::sleep(Duration::from_millis(100)).await;
        if request(f.addr, "GET", "/files/download?path=a.txt", &[]).await.status == 200 {
            ok = true;
            break;
        }
    }
    assert!(ok, "permits are released when the downloads end");
}

// ---- the Codex review of #661 ----

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn another_origin_is_refused_on_files_and_the_socket() {
    let f = start_with("origin", None, |c| c.allowed_origins.push("http://dev.example:5173".into())).await;
    // Host is "localhost": its own origin passes, an absent Origin passes, anything else is refused
    assert_eq!(request(f.addr, "GET", "/files", &[("Origin", "http://localhost")]).await.status, 200);
    assert_eq!(request(f.addr, "GET", "/files", &[]).await.status, 200);
    assert_eq!(request(f.addr, "GET", "/files", &[("Origin", "http://evil.example")]).await.status, 403);
    assert_eq!(request(f.addr, "GET", "/files", &[("Origin", "http://localhost:9999")]).await.status, 403);
    assert_eq!(request(f.addr, "GET", "/files", &[("Origin", "null")]).await.status, 403);
    assert_eq!(request(f.addr, "GET", "/files/download?path=a.txt", &[("Origin", "http://evil.example")]).await.status, 403);
    assert_eq!(request(f.addr, "GET", "/files/download?path=a.txt", &[("Origin", "http://dev.example:5173")]).await.status, 200, "allow-listed");
    assert_eq!(request(f.addr, "GET", "/files", &[("Origin", "http://LOCALHOST")]).await.status, 200, "host names compare case-insensitively");

    // the WebSocket handshake: tokio-tungstenite sends no Origin (accepted); a browser from elsewhere sends one
    use tokio_tungstenite::tungstenite::client::IntoClientRequest;
    let url = format!("ws://{}/ws", f.addr);
    let accepted = tokio_tungstenite::connect_async(url.clone()).await;
    assert!(accepted.is_ok(), "no Origin: accepted");
    let mut cross = url.clone().into_client_request().unwrap();
    cross.headers_mut().insert("Origin", "http://evil.example".parse().unwrap());
    let refused = tokio_tungstenite::connect_async(cross).await;
    match refused {
        Err(tokio_tungstenite::tungstenite::Error::Http(response)) => assert_eq!(response.status(), 403),
        other => panic!("a cross-origin socket must be refused with 403, got {:?}", other.map(|_| ())),
    }
    let mut same = url.into_client_request().unwrap();
    let host = same.headers().get("Host").unwrap().to_str().unwrap().to_string();
    same.headers_mut().insert("Origin", format!("http://{host}").parse().unwrap());
    assert!(tokio_tungstenite::connect_async(same).await.is_ok(), "the server's own origin is accepted");
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn a_fifo_is_refused_without_blocking_and_not_listed() {
    let f = start("fifo", None).await;
    let fifo = f.root.join("pipe");
    assert!(std::process::Command::new("mkfifo").arg(&fifo).status().unwrap().success());
    let started = std::time::Instant::now();
    let reply = request(f.addr, "GET", "/files/download?path=pipe", &[]).await;
    assert_eq!(reply.status, 404);
    assert!(started.elapsed() < Duration::from_secs(3), "the FIFO open must not block");
    let listing = request(f.addr, "GET", "/files", &[]).await.json();
    assert!(!names(&listing).contains(&"pipe".to_string()));
    // the server still serves after that
    assert_eq!(request(f.addr, "GET", "/files/download?path=a.txt", &[]).await.status, 200);
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn several_ranges_and_long_range_headers_are_refused_before_any_work() {
    let f = start("range", None).await;
    let few = request(f.addr, "GET", "/files/download?path=a.txt", &[("Range", "bytes=0-0,2-2")]).await;
    assert_eq!(few.status, 416);
    let many = format!("bytes={}", (0..5000).map(|i| format!("{i}-{i}")).collect::<Vec<_>>().join(","));
    let started = std::time::Instant::now();
    let reply = request(f.addr, "GET", "/files/download?path=a.txt", &[("Range", &many)]).await;
    assert_eq!(reply.status, 416, "5,000 ranges");
    assert!(started.elapsed() < Duration::from_secs(2));
    let long = format!("bytes=0-{}", "9".repeat(100));
    assert_eq!(request(f.addr, "GET", "/files/download?path=a.txt", &[("Range", &long)]).await.status, 416, "over 64 bytes");
    // a satisfiable single range, an open end, a suffix, and one past the end
    assert_eq!(request(f.addr, "GET", "/files/download?path=a.txt", &[("Range", "bytes=8-")]).await.body, b"89");
    assert_eq!(request(f.addr, "GET", "/files/download?path=a.txt", &[("Range", "bytes=-3")]).await.body, b"789");
    let past = request(f.addr, "GET", "/files/download?path=a.txt", &[("Range", "bytes=99-")]).await;
    assert_eq!(past.status, 416);
    assert_eq!(past.header("content-range"), Some("bytes */10"));
}

#[tokio::test(flavor = "multi_thread")]
#[serial]
async fn listings_are_paged_and_limited() {
    let f = start("paging", None).await;
    let many = f.root.join("many");
    std::fs::create_dir_all(&many).unwrap();
    for i in 0..2500 {
        std::fs::write(many.join(format!("f{i:05}.txt")), b"x").unwrap();
    }
    let first = request(f.addr, "GET", "/files?path=many", &[]).await.json();
    assert_eq!(first["entries"].as_array().unwrap().len(), 2000, "one page");
    assert_eq!(first["total"], 2500);
    assert_eq!(first["truncated"], true);
    assert_eq!(first["entries"][0]["name"], "f00000.txt");
    let rest = request(f.addr, "GET", "/files?path=many&offset=2000", &[]).await.json();
    assert_eq!(rest["entries"].as_array().unwrap().len(), 500);
    assert_eq!(rest["truncated"], false);
    let small = request(f.addr, "GET", "/files?path=many&limit=10&offset=5", &[]).await.json();
    assert_eq!(small["entries"].as_array().unwrap().len(), 10);
    assert_eq!(small["entries"][0]["name"], "f00005.txt");
    assert_eq!(request(f.addr, "GET", "/files?path=many&limit=999999", &[]).await.json()["limit"], 2000, "the page size is capped");

    // the listing permit: with none to give, the next listing is 429
    let none = start_with("nopermit", None, |c| c.max_listings = 0).await;
    assert_eq!(request(none.addr, "GET", "/files", &[]).await.status, 429);
    assert_eq!(request(none.addr, "GET", "/files/download?path=a.txt", &[]).await.status, 200, "downloads are a separate limit");
}
