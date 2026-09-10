#![cfg(unix)]

use mib_analysis::{Calculation, Error, Supervisor, VerifiedBundle};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::time::Duration;

const FAKE_HELPER: &str = r#"
import hashlib,json,struct,sys,time
from pathlib import Path
root=Path(__file__).parent
raw=(root/'manifest.json').read_bytes()
m=json.loads(raw)
mode=(root/'mode').read_text()
(root/'started.pid').write_text(str(__import__('os').getpid()))
if mode=='hang':
    time.sleep(60)
    sys.exit(0)
if mode=='crash': sys.exit(3)
if mode=='oversized':
    sys.stdout.buffer.write(struct.pack('>I',2**20+1));sys.stdout.buffer.flush()
    time.sleep(60)
if mode=='stderr':
    sys.stderr.write('x'*(2**22));sys.stderr.flush()
for i in range(2):
    n=struct.unpack('>I',sys.stdin.buffer.read(4))[0]
    q=json.loads(sys.stdin.buffer.read(n))
    r={k:q[k] for k in ('protocol','request_id','operation_id','generation')}
    r['status']='completed'
    r['result']={'toolkit_version':'0.1.0','bundle_sha256':hashlib.sha256(raw).hexdigest(),
        'wheel_sha256':m['files']['toolkit.whl']['sha256'],'production_ready':False} if i==0 else {'total':3}
    if mode=='wrong_generation' and i==1: r['generation']+=1
    if mode=='wrong_digest': r['result']['bundle_sha256']='bad'
    if mode=='wrong_operation' and i==1: r['operation_id']='900'
    if mode=='late_crash' and i==1:
        b=json.dumps(r).encode();sys.stdout.buffer.write(struct.pack('>I',len(b))+b);sys.stdout.buffer.flush()
        sys.exit(4)
    b=json.dumps(r).encode();sys.stdout.buffer.write(struct.pack('>I',len(b))+b);sys.stdout.buffer.flush()
if mode=='extra': sys.stdout.buffer.write(b'x');sys.stdout.buffer.flush()
"#;

fn hash(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

fn fixture(mode: &str, helper: &str) -> (tempfile::TempDir, String) {
    #[cfg(target_os = "linux")]
    {
        static WATCHDOG: std::sync::Once = std::sync::Once::new();
        WATCHDOG.call_once(|| {
            std::thread::spawn(|| {
                std::thread::sleep(Duration::from_secs(60));
                eprintln!("analysis supervisor test watchdog expired");
                unsafe { libc::_exit(99) }
            });
        });
    }
    let root = tempfile::tempdir().unwrap();
    let python = std::env::var("MIB_ANALYSIS_TEST_PYTHON")
        .expect("set MIB_ANALYSIS_TEST_PYTHON to an absolute interpreter with Toolkit installed");
    assert!(Path::new(&python).is_absolute() && Path::new(&python).is_file());
    // Test-only launcher: production bundles carry their interpreter and all
    // imports. Keeping this fixture small isolates process/transport behavior.
    let launcher = format!(
        "#!/bin/sh\nexec '{}' \"$@\"\n",
        python.replace('\'', "'\\''")
    );
    let files = [
        ("python", launcher.as_bytes()),
        ("helper.py", helper.as_bytes()),
        ("toolkit.whl", b"fixture wheel identity".as_slice()),
        ("mode", mode.as_bytes()),
    ];
    let mut entries = serde_json::Map::new();
    for (name, bytes) in files {
        fs::write(root.path().join(name), bytes).unwrap();
        entries.insert(
            name.into(),
            json!({"sha256":hash(bytes),"size":bytes.len()}),
        );
    }
    fs::set_permissions(
        root.path().join("python"),
        fs::Permissions::from_mode(0o700),
    )
    .unwrap();
    let manifest = json!({"schema":1,"distribution":"development","toolkit_version":"0.1.0",
        "interpreter":"python","helper":"helper.py","toolkit_wheel":"toolkit.whl","files":entries});
    let bytes = serde_json::to_vec(&manifest).unwrap();
    fs::write(root.path().join("manifest.json"), &bytes).unwrap();
    (root, hash(&bytes))
}

fn supervisor(root: &Path, pin: &str) -> Supervisor {
    let owner = Supervisor::new(VerifiedBundle::load(root, pin, true).unwrap());
    owner.select_generation(1).unwrap();
    owner
}
fn job() -> Calculation {
    Calculation::Histogram {
        values: vec![1., 2., 3.],
        metric: "area".into(),
        bins: 3,
    }
}

async fn started(root: &Path) -> u32 {
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if let Ok(text) = fs::read_to_string(root.join("started.pid")) {
                if let Ok(pid) = text.parse() {
                    return pid;
                }
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .expect("child never started")
}

#[cfg(target_os = "linux")]
fn assert_reaped(pid: u32) {
    // /proc entries can linger briefly after wait under concurrent process
    // churn. ECHILD proves this process has no unreaped child at this PID.
    let result = unsafe { libc::waitpid(pid as i32, std::ptr::null_mut(), libc::WNOHANG) };
    assert_eq!(result, -1, "child was still waitable: {pid}");
    assert_eq!(
        std::io::Error::last_os_error().raw_os_error(),
        Some(libc::ECHILD)
    );
}
#[cfg(not(target_os = "linux"))]
fn assert_reaped(_pid: u32) {}

#[tokio::test]
async fn successful_exchange_and_stderr_flood_are_bounded() {
    for mode in ["ok", "stderr"] {
        let (root, pin) = fixture(mode, FAKE_HELPER);
        let owner = supervisor(root.path(), &pin);
        assert_eq!(
            owner
                .run(42, 1, job(), Duration::from_secs(5))
                .await
                .unwrap(),
            json!({"total":3})
        );
        assert_reaped(started(root.path()).await);
    }
}

#[tokio::test]
async fn protocol_faults_and_crashes_do_not_publish_results() {
    for mode in [
        "wrong_generation",
        "wrong_operation",
        "wrong_digest",
        "crash",
        "late_crash",
        "extra",
        "oversized",
    ] {
        let (root, pin) = fixture(mode, FAKE_HELPER);
        let owner = supervisor(root.path(), &pin);
        assert!(
            owner
                .run(42, 1, job(), Duration::from_secs(5))
                .await
                .is_err(),
            "{mode}"
        );
        assert_reaped(started(root.path()).await);
    }
}

#[tokio::test]
async fn deadline_interrupts_child_that_never_reads_stdin() {
    let (root, pin) = fixture("hang", FAKE_HELPER);
    let owner = supervisor(root.path(), &pin);
    assert!(matches!(
        owner.run(42, 1, job(), Duration::from_millis(500)).await,
        Err(Error::TimedOut)
    ));
    assert_reaped(started(root.path()).await);
}

#[tokio::test]
async fn cancellation_generation_and_busy_stress() {
    for replace in [false, true] {
        for _ in 0..10 {
            let (root, pin) = fixture("hang", FAKE_HELPER);
            let owner = supervisor(root.path(), &pin);
            let work = owner.run(42, 1, job(), Duration::from_secs(5));
            let control = async {
                let pid = started(root.path()).await;
                assert!(matches!(
                    owner.run(43, 1, job(), Duration::from_secs(5)).await,
                    Err(Error::Busy)
                ));
                owner.cancel(999); // A stale cancel must not target operation 42.
                if replace {
                    owner.select_generation(2).unwrap();
                } else {
                    owner.cancel(42);
                }
                pid
            };
            let (result, pid) = tokio::join!(work, control);
            assert!(matches!(result, Err(Error::Cancelled)));
            assert_reaped(pid);
        }
    }
}

#[tokio::test]
#[ignore = "requires a locally installed pinned Biowork Toolkit wheel"]
async fn installed_toolkit_helper_round_trip() {
    let source = fs::read_to_string(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../desktop/analysis/helper.py"),
    )
    .unwrap();
    let (root, pin) = fixture("ok", &source);
    let owner = supervisor(root.path(), &pin);
    let result = owner
        .run(42, 1, job(), Duration::from_secs(5))
        .await
        .unwrap();
    assert_eq!(result["total"], 3);
    assert_eq!(
        result["bins"]
            .as_array()
            .unwrap()
            .iter()
            .map(|b| b["count"].as_u64().unwrap())
            .sum::<u64>(),
        3
    );
}

#[tokio::test]
async fn dropping_request_future_kills_and_reaps_child() {
    let (root, pin) = fixture("hang", FAKE_HELPER);
    let owner = supervisor(root.path(), &pin);
    let pid = tokio::select! {
        value = owner.run(42,1,job(),Duration::from_secs(30)) => panic!("unexpected result {value:?}"),
        pid = started(root.path()) => pid,
    };
    #[cfg(target_os = "linux")]
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            let mut info = unsafe { std::mem::zeroed::<libc::siginfo_t>() };
            let status = unsafe {
                libc::waitid(
                    libc::P_PID,
                    pid,
                    &mut info,
                    libc::WEXITED | libc::WNOHANG | libc::WNOWAIT,
                )
            };
            if status == -1 && std::io::Error::last_os_error().raw_os_error() == Some(libc::ECHILD)
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .expect("dropped child's reaper did not finish");
    assert_reaped(pid);
    assert!(matches!(
        owner.run(43, 1, job(), Duration::from_secs(1)).await,
        Err(Error::CleanupUnknown)
    ));
}

#[cfg(target_os = "linux")]
#[test]
fn parent_probe() {
    let Ok(root) = std::env::var("MIB_ANALYSIS_PARENT_PROBE_ROOT") else {
        return;
    };
    let pin = std::env::var("MIB_ANALYSIS_PARENT_PROBE_PIN").unwrap();
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    runtime.block_on(async {
        let owner = supervisor(Path::new(&root), &pin);
        let _ = owner.run(42, 1, job(), Duration::from_secs(60)).await;
    });
}

#[cfg(target_os = "linux")]
#[tokio::test]
async fn forced_parent_exit_terminates_linux_helper() {
    let (root, pin) = fixture("hang", FAKE_HELPER);
    let mut parent = tokio::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "parent_probe"])
        .env("MIB_ANALYSIS_PARENT_PROBE_ROOT", root.path())
        .env("MIB_ANALYSIS_PARENT_PROBE_PIN", pin)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .kill_on_drop(true)
        .spawn()
        .unwrap();
    let pid = started(root.path()).await;
    use std::os::fd::{AsRawFd, FromRawFd};
    let fd = unsafe { libc::syscall(libc::SYS_pidfd_open, pid, 0) };
    assert!(fd >= 0, "pidfd_open failed");
    let pidfd = unsafe { fs::File::from_raw_fd(fd as i32) };
    parent.start_kill().unwrap();
    tokio::time::timeout(Duration::from_secs(5), parent.wait())
        .await
        .unwrap()
        .unwrap();
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            let mut poll = libc::pollfd {
                fd: pidfd.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };
            let ready = unsafe { libc::poll(&mut poll, 1, 0) };
            // pidfd readiness proves exit even when init has not yet reaped it.
            if ready > 0 && poll.revents & libc::POLLIN != 0 {
                break;
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .expect("orphaned helper remained alive after parent exit");
}

#[test]
fn bundle_requires_external_pin_complete_inventory_and_qualified_mode() {
    let (root, pin) = fixture("ok", FAKE_HELPER);
    assert!(VerifiedBundle::load(root.path(), &pin, true).is_ok());
    assert!(VerifiedBundle::load(root.path(), &"0".repeat(64), true).is_err());
    assert!(VerifiedBundle::load(root.path(), &pin, false).is_err());
    fs::write(root.path().join("unlisted.py"), b"untrusted").unwrap();
    assert!(VerifiedBundle::load(root.path(), &pin, true).is_err());
    fs::remove_file(root.path().join("unlisted.py")).unwrap();
    fs::write(root.path().join("helper.py"), b"tampered").unwrap();
    assert!(VerifiedBundle::load(root.path(), &pin, true).is_err());
}

#[test]
fn bundle_rejects_symlinks_and_traversal() {
    let (root, pin) = fixture("ok", FAKE_HELPER);
    std::os::unix::fs::symlink("helper.py", root.path().join("alias.py")).unwrap();
    assert!(VerifiedBundle::load(root.path(), &pin, true).is_err());
    fs::remove_file(root.path().join("alias.py")).unwrap();
    let path = root.path().join("manifest.json");
    let mut manifest: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    manifest["interpreter"] = json!("../python");
    let bytes = serde_json::to_vec(&manifest).unwrap();
    fs::write(path, &bytes).unwrap();
    assert!(VerifiedBundle::load(root.path(), &hash(&bytes), true).is_err());
}
