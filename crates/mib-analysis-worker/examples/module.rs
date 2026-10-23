//! Isolated analysis-core entry point, not a separate user product.
use mib_analysis_worker::{
    store::{JobStore, State},
    Failure, Request, Worker, MAX_BYTES,
};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::Duration;

fn run() -> Result<(), Failure> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() == 3 && args[0] == "query" {
        let store = JobStore::open(Path::new(&args[1]))?;
        let id = args[2]
            .to_str()
            .ok_or_else(|| Failure::InvalidInput("non-UTF8 job id".into()))?;
        match store.query(id)? {
            State::Pending => println!("{}", serde_json::json!({"id":id, "state":"pending"})),
            State::Terminal(t) => println!(
                "{}",
                serde_json::json!({"id":id, "state":"terminal", "terminal":t})
            ),
        }
        return Ok(());
    }
    if args.len() != 4 || args[0] != "run" {
        return Err(Failure::InvalidInput(
            "module run PYTHON WORKER STORE < request.json | module query STORE ID".into(),
        ));
    }
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(MAX_BYTES as u64 + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() > MAX_BYTES {
        return Err(Failure::ResourceLimit);
    }
    let request: Request =
        serde_json::from_slice(&bytes).map_err(|e| Failure::InvalidInput(e.to_string()))?;
    request.validate()?;
    let store = JobStore::open(Path::new(&args[3]))?;
    store.recover_interrupted(10000)?;
    let id = store.begin(&request)?;
    if matches!(store.query(&id)?, State::Pending) {
        let worker = Worker {
            python: PathBuf::from(&args[1]),
            script: PathBuf::from(&args[2]),
            timeout: Duration::from_secs(60),
            memory_limit_bytes: 8 * 1024 * 1024 * 1024,
        };
        store.finish(&id, worker.run(&request, || false))?;
    }
    if let State::Terminal(t) = store.query(&id)? {
        println!(
            "{}",
            serde_json::json!({"id":id,"state":"terminal","terminal":t})
        );
        if let Some(failure) = t.failure {
            return Err(failure);
        }
    }
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("{}", serde_json::json!({"error": error}));
        std::process::exit(1);
    }
}
