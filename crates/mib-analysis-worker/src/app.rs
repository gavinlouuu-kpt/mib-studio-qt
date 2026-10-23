//! Application-owned bounded scheduler. No bridge or recording handle enters its worker thread.
use crate::store::{JobStore, State};
use crate::{Event, Failure, Request, Revision, ScalarGate, ScalarValue, Worker, SCHEMA};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::path::Path;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};
use std::thread::JoinHandle;
use std::time::Duration;

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Submission {
    pub command_id: String,
    pub plan_id: String,
    pub revision: String,
    pub source: Value,
    pub source_fingerprint: String,
    pub x: String,
    pub y: String,
    pub vertices: Vec<[f64; 2]>,
}
pub fn request(input: &Submission, resolved: &Value) -> Result<Request, String> {
    if resolved["status"] != "ok" {
        return Err(resolved.to_string());
    }
    let source = &resolved["source"];
    if source["fingerprint"] != input.source_fingerprint {
        return Err("Source snapshot changed".into());
    }
    let parameters = &source["parameters"];
    if !parameters.get(&input.x).is_some() || !parameters.get(&input.y).is_some() {
        return Err("Polygon measurement unavailable".into());
    }
    let rows = source["rows"].as_array().ok_or("Missing source rows")?;
    let membership = resolved["evaluation"]["rows"]
        .as_array()
        .ok_or("Missing graph membership")?;
    let keyed: std::collections::BTreeMap<_, _> = membership
        .iter()
        .map(|r| (r["key"].as_str().unwrap_or(""), &r["membership"]))
        .collect();
    let mut events = Vec::new();
    let mut values = Vec::new();
    for row in rows {
        let key = row["key"].as_str().ok_or("Missing event key")?;
        let member = keyed.get(key).ok_or("Incomplete graph membership")?;
        let value = match member.as_str() {
            Some("selected") => Some(1.),
            Some("excluded") => Some(0.),
            Some("unknown") => None,
            _ => return Err("Invalid membership".into()),
        };
        events.push(Event {
            key: key.into(),
            x: row["measurements"][&input.x].as_f64(),
            y: row["measurements"][&input.y].as_f64(),
        });
        values.push(ScalarValue {
            key: key.into(),
            value,
        });
    }
    let request = Request {
        schema: SCHEMA.into(),
        job: input.command_id.clone(),
        revision: Revision {
            snapshot: input.source_fingerprint.clone(),
            source_fingerprint: input.source_fingerprint.clone(),
            plan: json!([input.plan_id, input.revision]).to_string(),
            application: "production-app/1".into(),
            override_revision: "none".into(),
            parameters: json!({"x":input.x,"y":input.y,"units":parameters}).to_string(),
            qc: "unknown-not-filtered".into(),
            mechanics: "source-authority".into(),
            engine: "cytoflow-1.3.2".into(),
        },
        vertices: input.vertices.clone(),
        events,
        // Native scalar composition implements graph eligibility AND polygon with three-valued truth.
        scalar_gate: Some(ScalarGate {
            minimum: Some(1.),
            maximum: Some(1.),
            values,
        }),
    };
    request.validate().map_err(|e| format!("{e:?}"))?;
    Ok(request)
}
struct Active {
    id: String,
    cancel: Arc<AtomicBool>,
    thread: JoinHandle<()>,
    persistence_error: Arc<Mutex<Option<String>>>,
}
pub struct Scheduler {
    store: Arc<JobStore>,
    active: Option<Active>,
    recovered: Vec<String>,
}
impl Scheduler {
    pub fn open(root: &Path) -> Result<Self, String> {
        let store = Arc::new(JobStore::open(root).map_err(|e| format!("{e:?}"))?);
        let recovered = store
            .recover_interrupted(10000)
            .map_err(|e| format!("{e:?}"))?;
        Ok(Self {
            store,
            active: None,
            recovered,
        })
    }
    pub fn recovered(&self) -> &[String] {
        &self.recovered
    }
    pub fn id(command: &str) -> String {
        format!("{:x}", Sha256::digest(command.as_bytes()))
    }
    pub fn submit(&mut self, request: Request) -> Result<Value, String> {
        if let Some(active) = &self.active {
            if !active.thread.is_finished()
                && !matches!(self.store.query(&active.id), Ok(State::Terminal(_)))
            {
                return Err("An analysis job is already running".into());
            }
            if active
                .persistence_error
                .lock()
                .map_err(|e| e.to_string())?
                .is_some()
            {
                return Err(
                    "Previous terminal journal publication failed; reconcile before submitting"
                        .into(),
                );
            }
        }
        if let Some(active) = self.active.take() {
            active
                .thread
                .join()
                .map_err(|_| "Analysis worker panicked")?;
        }
        let python = std::env::var_os("MIB_CYTOFLOW_PYTHON")
            .ok_or("Cytoflow Python runtime is not configured")?;
        let script = std::env::var_os("MIB_CYTOFLOW_WORKER")
            .ok_or("Cytoflow worker script is not configured")?;
        if !Path::new(&python).is_file() || !Path::new(&script).is_file() {
            return Err("Configured Cytoflow runtime/script is unavailable".into());
        }
        let id = self.store.begin(&request).map_err(|e| format!("{e:?}"))?;
        if matches!(self.store.query(&id), Ok(State::Terminal(_))) {
            return self.query(&id);
        }
        let store = self.store.clone();
        let worker_id = id.clone();
        let cancel = Arc::new(AtomicBool::new(false));
        let worker_cancel = cancel.clone();
        let persistence_error = Arc::new(Mutex::new(None));
        let worker_error = persistence_error.clone();
        let thread = std::thread::Builder::new()
            .name("analysis-cytoflow".into())
            .spawn(move || {
                let worker = Worker {
                    python: python.into(),
                    script: script.into(),
                    timeout: Duration::from_secs(60),
                    memory_limit_bytes: 4 * 1024 * 1024 * 1024,
                };
                let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    worker.run(&request, || worker_cancel.load(Ordering::Acquire))
                }))
                .unwrap_or(Err(Failure::WorkerFailed));
                if let Err(error) = store.finish(&worker_id, outcome) {
                    if let Ok(mut value) = worker_error.lock() {
                        *value = Some(format!("{error:?}"));
                    }
                }
            })
            .map_err(|e| {
                let _ = self.store.finish(&id, Err(Failure::Io(e.to_string())));
                e.to_string()
            })?;
        self.active = Some(Active {
            id: id.clone(),
            cancel,
            thread,
            persistence_error,
        });
        Ok(json!({"status":"pending","jobId":id}))
    }
    pub fn query(&self, id: &str) -> Result<Value, String> {
        if let Some(active) = &self.active {
            if active.id == id {
                if let Some(error) = &*active.persistence_error.lock().map_err(|e| e.to_string())? {
                    return Ok(json!({"status":"durability_failed","jobId":id,"message":error}));
                }
            }
        }
        match self.store.query(id).map_err(|e| format!("{e:?}"))? {
            State::Pending => Ok(json!({"status":"pending","jobId":id})),
            State::Terminal(t) => Ok(json!({"status":"terminal","jobId":id,"terminal":t})),
        }
    }
    pub fn cancel(&self, id: &str) -> Result<Value, String> {
        if let Some(active) = &self.active {
            if active.id == id {
                active.cancel.store(true, Ordering::Release);
                return Ok(json!({"status":"cancelling","jobId":id}));
            }
        }
        self.query(id)
    }
}
impl Drop for Scheduler {
    fn drop(&mut self) {
        if let Some(active) = self.active.take() {
            active.cancel.store(true, Ordering::Release);
            let _ = active.thread.join(); // process group is killed/reaped before host teardown
        }
    }
}

#[derive(Default)]
pub struct Service {
    scheduler: Option<Scheduler>,
    error: Option<String>,
}
impl Service {
    pub fn initialize(&mut self, data: &Path) {
        if self.scheduler.is_some() {
            return;
        }
        match Scheduler::open(&data.join("analysis-jobs")) {
            Ok(s) => {
                self.scheduler = Some(s);
                self.error = None;
            }
            Err(e) => self.error = Some(e),
        }
    }
    pub fn command(
        &mut self,
        value: Value,
        resolve: impl FnOnce(Value) -> Result<Value, String>,
    ) -> Result<Value, String> {
        let scheduler = self.scheduler.as_mut().ok_or_else(|| {
            self.error
                .clone()
                .unwrap_or("Analysis jobs not initialized".into())
        })?;
        let object = value.as_object().ok_or("Expected job request")?;
        let action = value["action"].as_str().ok_or("Missing job action")?;
        match action {
            "submit" => {
                if object.len() != 2 {
                    return Err("Expected action and submission".into());
                }
                let submission: Submission = serde_json::from_value(value["submission"].clone())
                    .map_err(|e| e.to_string())?;
                let resolved = resolve(json!({"action":"jobInput", "planId":submission.plan_id,
                    "revision":submission.revision,"source":submission.source}))?;
                scheduler.submit(request(&submission, &resolved)?)
            }
            "query" | "cancel" => {
                if object.len() != 2 {
                    return Err("Expected action and jobId".into());
                }
                let id = value["jobId"].as_str().ok_or("Missing jobId")?;
                if action == "query" {
                    scheduler.query(id)
                } else {
                    scheduler.cancel(id)
                }
            }
            "reconcile" => {
                if object.len() != 2 {
                    return Err("Expected action and commandId".into());
                }
                scheduler.query(&Scheduler::id(
                    value["commandId"].as_str().ok_or("Missing commandId")?,
                ))
            }
            "status" if object.len() == 1 => {
                Ok(json!({"status":"ok","recovered":scheduler.recovered(),
                "runtimeConfigured":std::env::var_os("MIB_CYTOFLOW_PYTHON").is_some() && std::env::var_os("MIB_CYTOFLOW_WORKER").is_some()}))
            }
            _ => Err("Unsupported job action".into()),
        }
    }
}
