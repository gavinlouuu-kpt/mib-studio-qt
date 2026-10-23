//! Bounded headless Cytoflow execution. No scientific calculations or public ABI.
//! Unix process groups are supported first; other platforms refuse explicitly.
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashSet};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub mod app;
mod native;
pub mod store;

pub const MAX_BYTES: usize = 4 * 1024 * 1024;
pub const MAX_EVENTS: usize = 10_000;
pub const SCHEMA: &str = "mib-analysis-batch/1";
pub const WORKER_SCHEMA: &str = "mib-cytoflow-polygon/1";

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Revision {
    pub snapshot: String,
    pub source_fingerprint: String,
    pub plan: String,
    pub application: String,
    pub override_revision: String,
    pub parameters: String,
    pub qc: String,
    pub mechanics: String,
    pub engine: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Event {
    pub key: String,
    pub x: Option<f64>,
    pub y: Option<f64>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct EmbeddingSpec {
    pub engine: String,
    pub seed: u32,
    pub perplexity: f64,
    pub max_iter: u32,
    pub max_points: usize,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EmbeddingPoint {
    pub key: String,
    pub x: f64,
    pub y: f64,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EmbeddingArtifact {
    pub spec: EmbeddingSpec,
    pub eligible: usize,
    pub points: Vec<EmbeddingPoint>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
    pub schema: String,
    pub job: String,
    pub revision: Revision,
    pub vertices: Vec<[f64; 2]>,
    pub events: Vec<Event>,
    #[serde(default)]
    pub scalar_gate: Option<ScalarGate>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub embedding: Option<EmbeddingSpec>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ScalarValue {
    pub key: String,
    pub value: Option<f64>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ScalarGate {
    pub minimum: Option<f64>,
    pub maximum: Option<f64>,
    pub values: Vec<ScalarValue>,
}
fn truth(value: u8) -> Result<Truth, Failure> {
    match value {
        0 => Ok(Truth::False),
        1 => Ok(Truth::True),
        2 => Ok(Truth::Unknown),
        _ => Err(Failure::InvalidResult("invalid native truth".into())),
    }
}
fn native_code(value: Truth) -> u8 {
    match value {
        Truth::False => 0,
        Truth::True => 1,
        Truth::Unknown => 2,
    }
}
fn scalar(value: Option<f64>, gate: &ScalarGate) -> Result<Truth, Failure> {
    truth(
        native::ffi::scalar(
            value.is_some(),
            value.unwrap_or(0.),
            gate.minimum.is_some(),
            gate.minimum.unwrap_or(0.),
            gate.maximum.is_some(),
            gate.maximum.unwrap_or(0.),
        )
        .map_err(|e| Failure::InvalidInput(e.to_string()))?,
    )
}
fn native_and(left: Truth, right: Truth) -> Result<Truth, Failure> {
    truth(
        native::ffi::and(native_code(left), native_code(right))
            .map_err(|e| Failure::InvalidResult(e.to_string()))?,
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Truth {
    True,
    False,
    Unknown,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Row {
    pub key: String,
    pub value: Truth,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ResultBatch {
    pub schema: String,
    pub job: String,
    pub revision: Revision,
    pub rows: Vec<Row>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub embedding: Option<EmbeddingArtifact>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Failure {
    InvalidInput(String),
    Cancelled,
    TimedOut,
    ResourceLimit,
    WorkerFailed,
    InvalidResult(String),
    UnsupportedPlatform,
    Io(String),
    Interrupted,
}
impl From<std::io::Error> for Failure {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error.to_string())
    }
}
fn text(value: &str, max: usize) -> bool {
    !value.is_empty() && value.len() <= max && !value.chars().any(char::is_control)
}

impl Request {
    pub fn validate(&self) -> Result<(), Failure> {
        let r = &self.revision;
        if self.schema != SCHEMA
            || !text(&self.job, 256)
            || r.engine != "cytoflow-1.3.2"
            || [
                &r.snapshot,
                &r.source_fingerprint,
                &r.plan,
                &r.application,
                &r.override_revision,
                &r.parameters,
                &r.qc,
                &r.mechanics,
                &r.engine,
            ]
            .iter()
            .any(|v| !text(v, 256))
        {
            return Err(Failure::InvalidInput("unpinned schema/job/revision".into()));
        }
        if !(3..=129).contains(&self.vertices.len()) || self.events.len() > MAX_EVENTS {
            return Err(Failure::InvalidInput("batch budget".into()));
        }
        if let Some(spec) = &self.embedding {
            if spec.engine != "sklearn-tsne-1.9.1"
                || !spec.perplexity.is_finite()
                || spec.perplexity <= 0.
                || !(500..=2000).contains(&spec.max_iter)
                || !(3..=1000).contains(&spec.max_points)
                || spec.perplexity >= spec.max_points as f64
            {
                return Err(Failure::InvalidInput(
                    "unqualified embedding configuration".into(),
                ));
            }
        }
        let number = |v: f64| v.is_finite() && v.abs() <= 1e100;
        let mut keys = HashSet::new();
        if self.vertices.iter().flatten().any(|v| !number(*v))
            || self.events.iter().any(|e| {
                !text(&e.key, 512)
                    || !keys.insert(&e.key)
                    || e.x.is_some_and(|v| !number(v))
                    || e.y.is_some_and(|v| !number(v))
            })
        {
            return Err(Failure::InvalidInput(
                "invalid measurement or exact key".into(),
            ));
        }
        if let Some(gate) = &self.scalar_gate {
            scalar(None, gate)?; // native interval validation even for empty batches
            let mut supplied = HashSet::new();
            if gate.values.len() != self.events.len()
                || gate.values.iter().any(|v| {
                    !keys.contains(&v.key)
                        || !supplied.insert(&v.key)
                        || v.value.is_some_and(|x| !number(x))
                })
            {
                return Err(Failure::InvalidInput(
                    "invalid native scalar key coverage/value".into(),
                ));
            }
        }
        Ok(())
    }
}

/// Validate before exposure to any native consumer; never accept by position.
pub fn accept(request: &Request, bytes: &[u8]) -> Result<ResultBatch, Failure> {
    accept_stage(request, bytes, true)
}
fn accept_stage(request: &Request, bytes: &[u8], composed: bool) -> Result<ResultBatch, Failure> {
    request.validate()?;
    if bytes.len() > MAX_BYTES {
        return Err(Failure::ResourceLimit);
    }
    let mut result: ResultBatch =
        serde_json::from_slice(bytes).map_err(|e| Failure::InvalidResult(e.to_string()))?;
    if result.schema
        != (if composed {
            request.schema.as_str()
        } else {
            WORKER_SCHEMA
        })
        || result.job != request.job
        || result.revision != request.revision
    {
        return Err(Failure::InvalidResult("stale job/snapshot/revision".into()));
    }
    if result.rows.len() != request.events.len() {
        return Err(Failure::InvalidResult("incomplete key coverage".into()));
    }
    let mut keyed = BTreeMap::new();
    for row in result.rows.drain(..) {
        if keyed.insert(row.key, row.value).is_some() {
            return Err(Failure::InvalidResult("duplicate worker key".into()));
        }
    }
    let scalars: BTreeMap<_, _> = request
        .scalar_gate
        .as_ref()
        .map(|g| g.values.iter().map(|v| (v.key.as_str(), v.value)).collect())
        .unwrap_or_default();
    for event in &request.events {
        let value = keyed
            .remove(&event.key)
            .ok_or_else(|| Failure::InvalidResult("missing or foreign worker key".into()))?;
        if !composed && event.x.is_some() && event.y.is_some() && value == Truth::Unknown {
            return Err(Failure::InvalidResult(
                "worker omitted an evaluable event".into(),
            ));
        }
        let missing_expected = if composed {
            if let Some(gate) = &request.scalar_gate {
                native_and(Truth::Unknown, scalar(scalars[event.key.as_str()], gate)?)?
            } else {
                Truth::Unknown
            }
        } else {
            Truth::Unknown
        };
        if composed {
            if let Some(gate) = &request.scalar_gate {
                let s = scalar(scalars[event.key.as_str()], gate)?;
                if (s == Truth::False && value != Truth::False)
                    || (s == Truth::Unknown && value == Truth::True)
                {
                    return Err(Failure::InvalidResult(
                        "result violates native scalar eligibility".into(),
                    ));
                }
            }
        }
        if (event.x.is_none() || event.y.is_none()) && value != missing_expected {
            return Err(Failure::InvalidResult(
                "worker admitted unevaluable event".into(),
            ));
        }
        result.rows.push(Row {
            key: event.key.clone(),
            value,
        });
    }
    validate_embedding(request, &result, composed)?;
    Ok(result)
}
fn validate_embedding(
    request: &Request,
    result: &ResultBatch,
    composed: bool,
) -> Result<(), Failure> {
    use sha2::{Digest, Sha256};
    match (&request.embedding, &result.embedding) {
        (None, None) => Ok(()),
        (Some(spec), Some(artifact)) => {
            let scalar_values: BTreeMap<_, _> = request
                .scalar_gate
                .as_ref()
                .map(|g| g.values.iter().map(|v| (v.key.as_str(), v.value)).collect())
                .unwrap_or_default();
            let mut keys = Vec::new();
            for row in &result.rows {
                let eligible = if composed {
                    row.value
                } else if let Some(gate) = &request.scalar_gate {
                    native_and(row.value, scalar(scalar_values[row.key.as_str()], gate)?)?
                } else {
                    row.value
                };
                if eligible == Truth::True {
                    keys.push(row.key.clone());
                }
            }
            keys.sort_by_cached_key(|key| {
                (
                    format!(
                        "{:x}",
                        Sha256::digest(format!("{}|{}", spec.seed, key).as_bytes())
                    ),
                    key.clone(),
                )
            });
            if artifact.spec != *spec
                || artifact.eligible != keys.len()
                || artifact.points.len() != keys.len().min(spec.max_points)
                || artifact.points.len() < 3
                || spec.perplexity >= artifact.points.len() as f64
                || artifact.points.iter().zip(keys.iter()).any(|(p, k)| {
                    &p.key != k
                        || !p.x.is_finite()
                        || !p.y.is_finite()
                        || p.x.abs() > 1e100
                        || p.y.abs() > 1e100
                })
            {
                return Err(Failure::InvalidResult(
                    "embedding provenance/key coverage mismatch".into(),
                ));
            }
            Ok(())
        }
        _ => Err(Failure::InvalidResult(
            "missing or unsolicited embedding".into(),
        )),
    }
}

pub struct Worker {
    pub python: PathBuf,
    pub script: PathBuf,
    pub timeout: Duration,
    pub memory_limit_bytes: u64,
}
impl Worker {
    /// Synchronous off-acquisition-thread call. Cancellation polled at <=10 ms
    /// scheduling intervals; process group is killed/reaped on every terminal path.
    pub fn run(
        &self,
        request: &Request,
        mut cancelled: impl FnMut() -> bool,
    ) -> Result<ResultBatch, Failure> {
        request.validate()?;
        if cancelled() {
            return Err(Failure::Cancelled);
        }
        // Worker sees only polygon inputs. Native scalar values/configuration
        // remain in the complete checksummed job request, not Python science.
        let mut projected =
            serde_json::to_value(request).map_err(|e| Failure::InvalidInput(e.to_string()))?;
        projected.as_object_mut().unwrap().remove("scalar_gate");
        if request.embedding.is_some() {
            let mut eligible = Vec::new();
            if let Some(gate) = &request.scalar_gate {
                for row in &gate.values {
                    if scalar(row.value, gate)? == Truth::True {
                        eligible.push(row.key.clone());
                    }
                }
            } else {
                eligible = request.events.iter().map(|e| e.key.clone()).collect();
            }
            projected.as_object_mut().unwrap().insert(
                "nativeEligible".into(),
                serde_json::to_value(eligible).unwrap(),
            );
        }
        projected
            .as_object_mut()
            .unwrap()
            .insert("schema".into(), WORKER_SCHEMA.into());
        let bytes =
            serde_json::to_vec(&projected).map_err(|e| Failure::InvalidInput(e.to_string()))?;
        if bytes.len() > MAX_BYTES {
            return Err(Failure::ResourceLimit);
        }
        let output = execute(
            &self.python,
            &self.script,
            &bytes,
            self.timeout,
            self.memory_limit_bytes,
            &mut cancelled,
        )?;
        let mut result = accept_stage(request, &output, false)?;
        result.schema = request.schema.clone();
        if let Some(gate) = &request.scalar_gate {
            let values: BTreeMap<_, _> = gate
                .values
                .iter()
                .map(|v| (v.key.as_str(), v.value))
                .collect();
            for row in &mut result.rows {
                row.value = native_and(row.value, scalar(values[row.key.as_str()], gate)?)?;
            }
        }
        Ok(result)
    }
}

#[cfg(unix)]
fn execute(
    python: &Path,
    script: &Path,
    input: &[u8],
    timeout: Duration,
    memory: u64,
    cancelled: &mut impl FnMut() -> bool,
) -> Result<Vec<u8>, Failure> {
    use std::os::unix::process::CommandExt;
    if timeout.is_zero() || memory < 64 * 1024 * 1024 {
        return Err(Failure::InvalidInput(
            "invalid worker resource budget".into(),
        ));
    }
    let mut source = tempfile::tempfile()?;
    source.write_all(input)?;
    source.seek(SeekFrom::Start(0))?;
    let mut output = tempfile::tempfile()?;
    let errors = tempfile::tempfile()?;
    let mut command = Command::new(python);
    command
        .arg("-I")
        .arg(script)
        .stdin(Stdio::from(source))
        .stdout(Stdio::from(output.try_clone()?))
        .stderr(Stdio::from(errors.try_clone()?))
        .env_remove("PYTHONPATH")
        .env_remove("PYTHONHOME")
        .process_group(0);
    // Only async-signal-safe libc calls between fork and exec. Each output file
    // has a hard bound even if the worker writes faster than the monitor polls.
    unsafe {
        command.pre_exec(move || {
            let size = libc::rlimit {
                rlim_cur: MAX_BYTES as _,
                rlim_max: MAX_BYTES as _,
            };
            let address = libc::rlimit {
                rlim_cur: memory as _,
                rlim_max: memory as _,
            };
            if libc::setrlimit(libc::RLIMIT_FSIZE, &size) != 0
                || libc::setrlimit(libc::RLIMIT_AS, &address) != 0
            {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let child = command.spawn()?;
    struct Guard(std::process::Child);
    impl Drop for Guard {
        fn drop(&mut self) {
            // Includes descendants retaining inherited handles; only our newly
            // created process group. Never kill another agent's processes.
            unsafe {
                libc::kill(-(self.0.id() as i32), libc::SIGKILL);
            }
            let _ = self.0.wait();
        }
    }
    let mut child = Guard(child);
    let started = Instant::now();
    loop {
        if cancelled() {
            return Err(Failure::Cancelled);
        }
        if started.elapsed() >= timeout {
            return Err(Failure::TimedOut);
        }
        if output.metadata()?.len() >= MAX_BYTES as u64
            || errors.metadata()?.len() >= MAX_BYTES as u64
        {
            return Err(Failure::ResourceLimit);
        }
        if let Some(status) = child.0.try_wait()? {
            if !status.success() {
                return Err(Failure::WorkerFailed);
            }
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    // Stop surviving descendants before reading the final immutable output.
    drop(child);
    output.seek(SeekFrom::Start(0))?;
    let mut bytes = Vec::new();
    output.take(MAX_BYTES as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > MAX_BYTES {
        return Err(Failure::ResourceLimit);
    }
    Ok(bytes)
}

#[cfg(not(unix))]
fn execute(
    _: &Path,
    _: &Path,
    _: &[u8],
    _: Duration,
    _: u64,
    _: &mut impl FnMut() -> bool,
) -> Result<Vec<u8>, Failure> {
    Err(Failure::UnsupportedPlatform)
}
