//! Single-owner portable job journal. Atomic no-replace publication, checksummed
//! requests/results and queryable terminal states; never opens a recording.
use crate::{accept, Failure, Request, ResultBatch, MAX_BYTES};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn json<T: Serialize>(value: &T) -> Result<Vec<u8>, Failure> {
    serde_json::to_vec(value).map_err(|e| Failure::InvalidResult(e.to_string()))
}
fn bounded(path: &Path) -> Result<Vec<u8>, Failure> {
    let mut bytes = Vec::new();
    File::open(path)?
        .take(MAX_BYTES as u64 + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() > MAX_BYTES {
        return Err(Failure::ResourceLimit);
    }
    Ok(bytes)
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Terminal {
    pub request_sha256: String,
    pub result: Option<ResultBatch>,
    pub failure: Option<Failure>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Envelope {
    schema: String,
    sha256: String,
    terminal: Terminal,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct StoredRequest {
    schema: String,
    sha256: String,
    request: Request,
}

#[derive(Debug)]
pub enum State {
    Pending,
    Terminal(Box<Terminal>),
}

pub struct JobStore {
    root: PathBuf,
    _owner: File,
}
impl JobStore {
    /// Exclusive OS lock prevents another scheduler or recovery from racing
    /// running work. Reopening never auto-runs a previously interrupted job.
    pub fn open(root: &Path) -> Result<Self, Failure> {
        std::fs::create_dir_all(root)?;
        let owner = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(root.join("owner.lock"))?;
        owner
            .try_lock()
            .map_err(|e| Failure::Io(format!("job store is locked: {e}")))?;
        Ok(Self {
            root: root.into(),
            _owner: owner,
        })
    }
    fn path(&self, id: &str, suffix: &str) -> Result<PathBuf, Failure> {
        if id.len() != 64
            || !id
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        {
            return Err(Failure::InvalidInput("invalid job storage id".into()));
        }
        Ok(self.root.join(format!("{id}.{suffix}.json")))
    }
    fn publish(&self, destination: &Path, bytes: &[u8]) -> Result<(), Failure> {
        if bytes.len() > MAX_BYTES {
            return Err(Failure::ResourceLimit);
        }
        let mut file = tempfile::NamedTempFile::new_in(&self.root)?;
        file.write_all(bytes)?;
        file.as_file().sync_all()?;
        file.persist_noclobber(destination)
            .map_err(|e| Failure::Io(e.error.to_string()))?;
        // A failed sync is reported, never relabeled a successful durable save.
        File::open(&self.root)?.sync_all()?;
        Ok(())
    }
    pub fn begin(&self, request: &Request) -> Result<String, Failure> {
        request.validate()?;
        let bytes = json(&StoredRequest {
            schema: "mib-analysis-input/1".into(),
            sha256: digest(&json(request)?),
            request: request.clone(),
        })?;
        if bytes.len() > MAX_BYTES {
            return Err(Failure::ResourceLimit);
        }
        let id = digest(request.job.as_bytes());
        let path = self.path(&id, "request")?;
        if path.try_exists()? {
            if bounded(&path)? != bytes {
                return Err(Failure::InvalidInput(
                    "job ID reused with different immutable inputs".into(),
                ));
            }
        } else {
            self.publish(&path, &bytes)?;
        }
        Ok(id)
    }
    pub fn request(&self, id: &str) -> Result<Request, Failure> {
        let bytes = bounded(&self.path(id, "request")?)?;
        let stored: StoredRequest =
            serde_json::from_slice(&bytes).map_err(|e| Failure::InvalidResult(e.to_string()))?;
        if stored.schema != "mib-analysis-input/1"
            || stored.sha256 != digest(&json(&stored.request)?)
        {
            return Err(Failure::InvalidResult(
                "immutable input checksum mismatch".into(),
            ));
        }
        let request = stored.request;
        request.validate()?;
        if digest(request.job.as_bytes()) != id {
            return Err(Failure::InvalidResult("job identity mismatch".into()));
        }
        Ok(request)
    }
    pub fn finish(&self, id: &str, outcome: Result<ResultBatch, Failure>) -> Result<(), Failure> {
        let request = self.request(id)?;
        let terminal = match outcome {
            Ok(result) => Terminal {
                request_sha256: digest(&json(&request)?),
                result: Some(accept(&request, &json(&result)?)?),
                failure: None,
            },
            Err(failure) => Terminal {
                request_sha256: digest(&json(&request)?),
                result: None,
                failure: Some(failure),
            },
        };
        let sha256 = digest(&json(&terminal)?);
        let envelope = Envelope {
            schema: "mib-analysis-job/1".into(),
            sha256,
            terminal,
        };
        self.publish(&self.path(id, "terminal")?, &json(&envelope)?)
    }
    pub fn query(&self, id: &str) -> Result<State, Failure> {
        let request = self.request(id)?;
        let path = self.path(id, "terminal")?;
        if !path.try_exists()? {
            return Ok(State::Pending);
        }
        let envelope: Envelope = serde_json::from_slice(&bounded(&path)?)
            .map_err(|e| Failure::InvalidResult(e.to_string()))?;
        let t = &envelope.terminal;
        if envelope.schema != "mib-analysis-job/1"
            || digest(&json(t)?) != envelope.sha256
            || t.request_sha256 != digest(&json(&request)?)
            || (t.result.is_some() == t.failure.is_some())
        {
            return Err(Failure::InvalidResult(
                "job journal integrity mismatch".into(),
            ));
        }
        if let Some(result) = &t.result {
            accept(&request, &json(result)?)?;
        }
        Ok(State::Terminal(Box::new(envelope.terminal)))
    }
    /// Call once after acquiring ownership, before launching any new work.
    /// Limits directory traversal. Refuses an oversized store before changes;
    /// caller must provide a sufficient explicit budget (not loop a partial scan).
    pub fn recover_interrupted(&self, limit: usize) -> Result<Vec<String>, Failure> {
        if limit == 0 || limit > 10000 {
            return Err(Failure::InvalidInput("recovery budget".into()));
        }
        let entries = std::fs::read_dir(&self.root)?
            .take(limit + 1)
            .collect::<Result<Vec<_>, _>>()?;
        if entries.len() > limit {
            return Err(Failure::ResourceLimit);
        }
        let mut recovered = Vec::new();
        for entry in entries {
            let name = entry.file_name();
            let Some(name) = name.to_str() else {
                continue;
            };
            let Some(id) = name.strip_suffix(".request.json") else {
                continue;
            };
            if matches!(self.query(id)?, State::Pending) {
                self.finish(id, Err(Failure::Interrupted))?;
                recovered.push(id.to_string());
            }
        }
        Ok(recovered)
    }
}
