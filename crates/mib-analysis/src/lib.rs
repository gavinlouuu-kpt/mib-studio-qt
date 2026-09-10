//! Bounded transport/process ownership for the native analysis operation owner.
//! This crate does not allocate operation IDs or maintain a second job ledger.

mod bundle;
pub use bundle::VerifiedBundle;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::fmt;
use std::process::Stdio;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::process::{Child, Command};
use tokio::sync::{watch, Mutex};
use tokio::time::timeout;

pub const MAX_MESSAGE: usize = 1 << 20;
const MAX_GENERATION: u64 = (1 << 53) - 1;
const REAP_TIMEOUT: Duration = Duration::from_secs(2);
pub type Result<T> = std::result::Result<T, Error>;

#[derive(Debug)]
pub enum Error {
    Bundle(&'static str),
    Protocol(&'static str),
    Busy,
    Cancelled,
    TimedOut,
    CleanupUnknown,
    Io(std::io::Error),
    Json(serde_json::Error),
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Self::Io(e)
    }
}
impl From<serde_json::Error> for Error {
    fn from(e: serde_json::Error) -> Self {
        Self::Json(e)
    }
}

pub enum Calculation {
    Histogram {
        values: Vec<f64>,
        metric: String,
        bins: u16,
    },
    Kde {
        x: Vec<f64>,
        y: Vec<f64>,
        columns: u16,
        rows: u16,
        bandwidth: f64,
    },
}
impl Calculation {
    fn encode(self) -> Result<(&'static str, Value)> {
        fn column(values: &[f64]) -> Result<()> {
            if values.len() > 4096 || values.iter().any(|x| !x.is_finite()) {
                return Err(Error::Protocol("invalid or oversized column"));
            }
            Ok(())
        }
        match self {
            Self::Histogram {
                values,
                metric,
                bins,
            } => {
                column(&values)?;
                if !(1..=256).contains(&bins) || metric.is_empty() || metric.chars().count() > 256 {
                    return Err(Error::Protocol("invalid histogram parameters"));
                }
                Ok((
                    "histogram_page",
                    json!({"values": values, "metric": metric, "bins": bins}),
                ))
            }
            Self::Kde {
                x,
                y,
                columns,
                rows,
                bandwidth,
            } => {
                column(&x)?;
                column(&y)?;
                if x.len() != y.len()
                    || !(2..=128).contains(&columns)
                    || !(2..=128).contains(&rows)
                    || !bandwidth.is_finite()
                    || !(0.5..=8.0).contains(&bandwidth)
                {
                    return Err(Error::Protocol("invalid KDE parameters"));
                }
                Ok((
                    "kde_page",
                    json!({"x":x,"y":y,"columns":columns,"rows":rows,"bandwidth":bandwidth}),
                ))
            }
        }
    }
}

#[derive(Serialize, Deserialize, PartialEq, Debug)]
#[serde(deny_unknown_fields)]
struct Protocol {
    major: u32,
    minor: u32,
}
const PROTOCOL: Protocol = Protocol { major: 0, minor: 1 };

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Reply {
    protocol: Protocol,
    request_id: u64,
    operation_id: String,
    generation: u64,
    status: String,
    result: Value,
}

async fn write_frame(output: &mut (impl AsyncWrite + Unpin), request: &Value) -> Result<()> {
    let bytes = serde_json::to_vec(request)?;
    if bytes.len() > MAX_MESSAGE {
        return Err(Error::Protocol("request byte budget"));
    }
    output.write_u32(bytes.len() as u32).await?;
    output.write_all(&bytes).await?;
    output.flush().await?;
    Ok(())
}

async fn read_frame(input: &mut (impl AsyncRead + Unpin)) -> Result<Reply> {
    let size = input.read_u32().await? as usize;
    if size == 0 || size > MAX_MESSAGE {
        return Err(Error::Protocol("reply byte budget"));
    }
    let mut bytes = vec![0; size];
    input.read_exact(&mut bytes).await?;
    Ok(serde_json::from_slice(&bytes)?)
}

fn validate(reply: Reply, id: u64, operation: u64, generation: u64) -> Result<Value> {
    if reply.protocol != PROTOCOL
        || reply.request_id != id
        || reply.operation_id != operation.to_string()
        || reply.generation != generation
        || reply.status != "completed"
    {
        return Err(Error::Protocol("uncorrelated or unsuccessful reply"));
    }
    Ok(reply.result)
}

#[derive(Clone, Copy)]
struct Control {
    generation: u64,
    epoch: u64,
    active: Option<u64>,
}

struct Active<'a> {
    control: &'a watch::Sender<Control>,
    operation: u64,
    healthy: &'a AtomicBool,
    reaped: bool,
}
impl Drop for Active<'_> {
    fn drop(&mut self) {
        if !self.reaped {
            self.healthy.store(false, Ordering::Release);
        }
        self.control.send_modify(|state| {
            if state.active == Some(self.operation) {
                state.active = None;
            }
        });
    }
}

pub struct Supervisor {
    bundle: VerifiedBundle,
    gate: Mutex<()>,
    control: watch::Sender<Control>,
    healthy: AtomicBool,
}

impl Supervisor {
    pub fn new(bundle: VerifiedBundle) -> Self {
        let (control, _) = watch::channel(Control {
            generation: 0,
            epoch: 0,
            active: None,
        });
        Self {
            bundle,
            gate: Mutex::new(()),
            control,
            healthy: AtomicBool::new(true),
        }
    }

    /// Called by the dataset owner when replacing/closing a source. Never accepts
    /// an older generation, and invalidates running work independently of pipes.
    pub fn select_generation(&self, generation: u64) -> Result<()> {
        let mut result = Err(Error::Protocol("generation must advance"));
        self.control.send_modify(|state| {
            if generation > state.generation && generation <= MAX_GENERATION {
                state.generation = generation;
                result = Ok(());
            }
        });
        result
    }

    /// A delayed cancellation cannot cancel a different native operation ID.
    pub fn cancel(&self, operation_id: u64) {
        self.control.send_modify(|state| {
            if state.active == Some(operation_id) {
                state.epoch = state.epoch.wrapping_add(1);
            }
        });
    }

    pub async fn run(
        &self,
        operation_id: u64,
        generation: u64,
        calculation: Calculation,
        deadline: Duration,
    ) -> Result<Value> {
        let _gate = self.gate.try_lock().map_err(|_| Error::Busy)?;
        if !self.healthy.load(Ordering::Acquire) {
            return Err(Error::CleanupUnknown);
        }
        if operation_id == 0 || deadline.is_zero() || deadline > Duration::from_secs(60) {
            return Err(Error::Protocol("invalid operation or deadline"));
        }
        let (method, params) = calculation.encode()?;
        let mut initial = None;
        self.control.send_modify(|state| {
            if state.generation == generation {
                state.active = Some(operation_id);
                initial = Some(*state);
            }
        });
        let initial = initial.ok_or(Error::Cancelled)?;
        let mut active = Active {
            control: &self.control,
            operation: operation_id,
            healthy: &self.healthy,
            reaped: true,
        };
        let mut changes = self.control.subscribe();
        let mut child = self.spawn()?;
        active.reaped = false;
        let io = self.exchange(&mut child, operation_id, generation, method, params);
        let result = tokio::select! {
            biased;
            _ = wait_cancel(&mut changes, initial) => Err(Error::Cancelled),
            value = timeout(deadline, io) => value.unwrap_or(Err(Error::TimedOut)),
        };
        // An error/timeout/cancellation always interrupts the OS child, even if
        // it is not reading stdin. No unbounded join or wait in this transport.
        let cleanup = if child
            .try_wait()
            .map_err(|_| Error::CleanupUnknown)?
            .is_none()
        {
            child.start_kill().map_err(|_| Error::CleanupUnknown)?;
            timeout(REAP_TIMEOUT, child.wait())
                .await
                .map_err(|_| Error::CleanupUnknown)?
        } else {
            child.wait().await
        };
        cleanup.map_err(|_| Error::CleanupUnknown)?;
        active.reaped = true;
        let current = *self.control.borrow();
        if current.generation != initial.generation || current.epoch != initial.epoch {
            return Err(Error::Cancelled);
        }
        result
    }

    fn spawn(&self) -> Result<Child> {
        let mut command = Command::new(&self.bundle.interpreter);
        command
            .args(["-I", "-B"])
            .arg(&self.bundle.helper)
            .arg("--bundle-manifest")
            .arg(self.bundle.root.join("manifest.json"))
            .current_dir(&self.bundle.root)
            .env_clear()
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true);
        #[cfg(windows)]
        if let Some(root) = std::env::var_os("SystemRoot") {
            command.env("SystemRoot", root);
        }
        #[cfg(target_os = "linux")]
        {
            let parent = unsafe { libc::getpid() };
            // Only async-signal-safe syscalls between fork and exec. Checking
            // getppid after prctl closes the parent-exit race before registration.
            unsafe {
                command.pre_exec(move || {
                    if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL) != 0 {
                        return Err(std::io::Error::last_os_error());
                    }
                    if libc::getppid() != parent {
                        libc::_exit(99);
                    }
                    Ok(())
                });
            }
        }
        Ok(command.spawn()?)
    }

    async fn exchange(
        &self,
        child: &mut Child,
        operation: u64,
        generation: u64,
        method: &str,
        params: Value,
    ) -> Result<Value> {
        let mut input = child.stdin.take().ok_or(Error::Protocol("missing stdin"))?;
        let mut output = child
            .stdout
            .take()
            .ok_or(Error::Protocol("missing stdout"))?;
        let request = |id, method: &str, params| {
            json!({"protocol": PROTOCOL,
            "request_id":id,"operation_id":operation.to_string(),"generation":generation,
            "method":method,"params":params})
        };
        write_frame(
            &mut input,
            &request(
                1,
                "handshake",
                json!({
            "toolkit_version":"0.1.0", "bundle_sha256":self.bundle.digest}),
            ),
        )
        .await?;
        let hello = validate(read_frame(&mut output).await?, 1, operation, generation)?;
        if hello["toolkit_version"] != "0.1.0"
            || hello["bundle_sha256"] != self.bundle.digest
            || hello["wheel_sha256"] != self.bundle.wheel_digest
            || hello["production_ready"] != !self.bundle.development
        {
            return Err(Error::Protocol("bundle handshake mismatch"));
        }
        write_frame(&mut input, &request(2, method, params)).await?;
        input.shutdown().await?;
        drop(input);
        let result = validate(read_frame(&mut output).await?, 2, operation, generation)?;
        let mut extra = [0];
        if output.read(&mut extra).await? != 0 {
            return Err(Error::Protocol("unsolicited output"));
        }
        if !child.wait().await?.success() {
            return Err(Error::Protocol("helper failed after reply"));
        }
        Ok(result)
    }
}

async fn wait_cancel(changes: &mut watch::Receiver<Control>, initial: Control) {
    loop {
        let current = *changes.borrow_and_update();
        if current.epoch != initial.epoch || current.generation != initial.generation {
            return;
        }
        if changes.changed().await.is_err() {
            return;
        }
    }
}
