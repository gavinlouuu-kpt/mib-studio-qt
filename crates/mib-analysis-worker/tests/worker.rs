use mib_analysis_worker::*;
use std::path::PathBuf;
use std::time::{Duration, Instant};

fn request() -> Request {
    Request {
        embedding: None,
        schema: SCHEMA.into(),
        job: "job-1".into(),
        revision: Revision {
            snapshot: "snap-1".into(),
            source_fingerprint: "sha".into(),
            plan: "plan-1".into(),
            application: "app-1".into(),
            override_revision: "none".into(),
            parameters: "params-1".into(),
            qc: "qc-1".into(),
            mechanics: "lut-1".into(),
            engine: "cytoflow-1.3.2".into(),
        },
        vertices: vec![[0., 0.], [2., 0.], [2., 2.], [0., 2.]],
        events: vec![
            Event {
                features: None,
                key: "A/valid/9007199254740993".into(),
                x: Some(1.),
                y: Some(1.),
            },
            Event {
                features: None,
                key: "B/valid/9007199254740993".into(),
                x: None,
                y: Some(1.),
            },
        ],
        scalar_gate: None,
    }
}
fn result(r: &Request) -> ResultBatch {
    ResultBatch {
        embedding: None,
        schema: r.schema.clone(),
        job: r.job.clone(),
        revision: r.revision.clone(),
        rows: vec![
            Row {
                key: r.events[1].key.clone(),
                value: Truth::Unknown,
            },
            Row {
                key: r.events[0].key.clone(),
                value: Truth::True,
            },
        ],
    }
}
#[test]
fn exact_keys_reordered_and_invalid_results() {
    let r = request();
    let good = result(&r);
    let accepted = accept(&r, &serde_json::to_vec(&good).unwrap()).unwrap();
    assert_eq!(accepted.rows[0].key, r.events[0].key);
    for mutation in 0..7 {
        let mut bad = good.clone();
        match mutation {
            0 => bad.job = "old-job".into(),
            1 => bad.revision.plan = "old-plan".into(),
            2 => bad.revision.source_fingerprint = "changed".into(),
            3 => bad.rows[0].key = bad.rows[1].key.clone(),
            4 => bad.rows[0].key = "foreign".into(),
            5 => {
                bad.rows.pop();
            }
            _ => bad.rows[0].value = Truth::True,
        }
        assert!(
            accept(&r, &serde_json::to_vec(&bad).unwrap()).is_err(),
            "mutation {mutation}"
        );
    }
}
#[test]
fn strict_json_and_input_validation() {
    let r = request();
    let raw = serde_json::to_string(&result(&r)).unwrap();
    let duplicate = raw.replacen("{", "{\"job\":\"duplicate\",", 1);
    assert!(accept(&r, duplicate.as_bytes()).is_err());
    let mut invalid = r.clone();
    invalid.events[0].x = Some(f64::NAN);
    assert!(invalid.validate().is_err());
    invalid = r.clone();
    invalid.events[1].key = invalid.events[0].key.clone();
    assert!(invalid.validate().is_err());
    invalid = r.clone();
    invalid.revision.mechanics.clear();
    assert!(invalid.validate().is_err());
    invalid = r;
    invalid.events = vec![invalid.events[0].clone(); MAX_EVENTS + 1];
    assert!(invalid.validate().is_err());
}

#[test]
fn journal_roundtrip_idempotency_collision_and_corruption() {
    use mib_analysis_worker::store::{JobStore, State};
    let dir = tempfile::tempdir().unwrap();
    let request = request();
    let store = JobStore::open(dir.path()).unwrap();
    assert!(
        JobStore::open(dir.path()).is_err(),
        "exclusive scheduler ownership"
    );
    let id = store.begin(&request).unwrap();
    assert_eq!(store.begin(&request).unwrap(), id);
    let mut conflict = request.clone();
    conflict.revision.plan = "different-plan".into();
    assert!(store.begin(&conflict).is_err());
    store.finish(&id, Ok(result(&request))).unwrap();
    assert!(
        store.finish(&id, Err(Failure::Cancelled)).is_err(),
        "never replace prior terminal evidence"
    );
    drop(store);
    let store = JobStore::open(dir.path()).unwrap();
    let State::Terminal(t) = store.query(&id).unwrap() else {
        panic!("lost terminal state")
    };
    assert_eq!(t.result.unwrap().rows[0].key, request.events[0].key);
    let path = dir.path().join(format!("{id}.terminal.json"));
    let bytes = std::fs::read_to_string(&path)
        .unwrap()
        .replace("\"true\"", "\"false\"");
    std::fs::write(path, bytes).unwrap();
    assert!(
        store.query(&id).is_err(),
        "corrupted scientific output cannot become cached success"
    );
}

#[test]
fn journal_interrupted_and_publication_failure() {
    use mib_analysis_worker::store::{JobStore, State};
    let dir = tempfile::tempdir().unwrap();
    let r = request();
    let store = JobStore::open(dir.path()).unwrap();
    let id = store.begin(&r).unwrap();
    assert!(
        store.recover_interrupted(1).is_err(),
        "scan budget fails before modifications"
    );
    assert!(matches!(store.query(&id).unwrap(), State::Pending));
    drop(store);
    let store = JobStore::open(dir.path()).unwrap();
    assert_eq!(store.recover_interrupted(100).unwrap(), vec![id.clone()]);
    let State::Terminal(t) = store.query(&id).unwrap() else {
        panic!("missing recovery state")
    };
    assert_eq!(t.failure, Some(Failure::Interrupted));
    let mut other = r.clone();
    other.job = "job-2".into();
    let second = store.begin(&other).unwrap();
    let terminal = dir.path().join(format!("{second}.terminal.json"));
    std::fs::create_dir(&terminal).unwrap();
    assert!(
        store.finish(&second, Err(Failure::Cancelled)).is_err(),
        "publication fault visible"
    );
    assert!(terminal.is_dir(), "pre-existing destination untouched");
    let request_file = dir.path().join(format!("{second}.request.json"));
    let bytes = std::fs::read_to_string(&request_file)
        .unwrap()
        .replace("snap-1", "snap-2");
    std::fs::write(request_file, bytes).unwrap();
    assert!(
        store.request(&second).is_err(),
        "immutable input checksum verified"
    );
}

#[cfg(unix)]
fn fake(code: &str, timeout: Duration) -> (tempfile::TempDir, Worker) {
    let dir = tempfile::tempdir().unwrap();
    let script = dir.path().join("worker.py");
    std::fs::write(&script, code).unwrap();
    let worker = Worker {
        python: PathBuf::from("/usr/bin/python3"),
        script,
        timeout,
        memory_limit_bytes: 512 * 1024 * 1024,
    };
    (dir, worker)
}
#[cfg(unix)]
#[test]
fn cancel_timeout_crash_output_budget_and_recovery() {
    let r = request();
    let (_dir, worker) = fake("import time\ntime.sleep(60)\n", Duration::from_millis(150));
    assert!(matches!(worker.run(&r, || true), Err(Failure::Cancelled)));
    assert!(matches!(worker.run(&r, || false), Err(Failure::TimedOut)));
    let now = Instant::now();
    assert!(matches!(
        worker.run(&r, || now.elapsed() > Duration::from_millis(50)),
        Err(Failure::Cancelled)
    ));
    std::fs::write(&worker.script, "raise SystemExit(23)\n").unwrap();
    assert!(matches!(
        worker.run(&r, || false),
        Err(Failure::WorkerFailed)
    ));
    std::fs::write(
        &worker.script,
        "import sys\nsys.stdout.write('x'*8000000)\n",
    )
    .unwrap();
    assert!(matches!(
        worker.run(&r, || false),
        Err(Failure::ResourceLimit) | Err(Failure::WorkerFailed)
    ));
    let mut raw = result(&r);
    raw.schema = WORKER_SCHEMA.into();
    let output = serde_json::to_string(&raw).unwrap();
    std::fs::write(&worker.script, format!("print({output:?})\n")).unwrap();
    assert!(
        worker.run(&r, || false).is_ok(),
        "new invocation recovers after terminal failures"
    );
}

#[cfg(unix)]
#[test]
#[ignore = "requires explicitly supplied pinned Cytoflow runtime"]
fn real_engine() {
    let python = std::env::var_os("MIB_CYTOFLOW_PYTHON").expect("pinned Python path");
    let script = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../workers/cytoflow/worker.py");
    let worker = Worker {
        python: python.into(),
        script,
        timeout: Duration::from_secs(60),
        memory_limit_bytes: 8 * 1024 * 1024 * 1024,
    };
    let output = worker.run(&request(), || false).unwrap();
    assert_eq!(output.rows[0].value, Truth::True);
    assert_eq!(output.rows[1].value, Truth::Unknown);
    let mut r = request();
    r.scalar_gate = Some(ScalarGate {
        minimum: Some(2.),
        maximum: Some(2.),
        values: vec![
            ScalarValue {
                key: r.events[1].key.clone(),
                value: Some(1.),
            },
            ScalarValue {
                key: r.events[0].key.clone(),
                value: Some(2.),
            },
        ],
    });
    let selected = worker.run(&r, || false).unwrap();
    assert_eq!(
        selected.rows[0].value,
        Truth::True,
        "native equal-endpoint interval"
    );
    assert_eq!(
        selected.rows[1].value,
        Truth::False,
        "false scalar AND unknown polygon"
    );
    let dir = tempfile::tempdir().unwrap();
    let store = mib_analysis_worker::store::JobStore::open(dir.path()).unwrap();
    let id = store.begin(&r).unwrap();
    store.finish(&id, Ok(selected)).unwrap();
    drop(store);
    let store = mib_analysis_worker::store::JobStore::open(dir.path()).unwrap();
    assert!(matches!(
        store.query(&id).unwrap(),
        mib_analysis_worker::store::State::Terminal(_)
    ));
    r.scalar_gate.as_mut().unwrap().values[1].value = None;
    let selected = worker.run(&r, || false).unwrap();
    assert_eq!(
        selected.rows[0].value,
        Truth::Unknown,
        "missing native E not admitted"
    );
}

#[test]
#[ignore = "requires pinned Cytoflow and sklearn runtime"]
fn seeded_embedding_key_coverage_repeatability_and_reopen() {
    let python = std::env::var("MIB_CYTOFLOW_PYTHON").unwrap();
    let worker = Worker {
        python: python.into(),
        script: PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../workers/cytoflow/worker.py"),
        timeout: Duration::from_secs(60),
        memory_limit_bytes: 4 * 1024 * 1024 * 1024,
    };
    let mut r = request();
    r.events = (0..20)
        .map(|i| Event {
            features: Some(vec![
                Some(i as f64 / 20.),
                Some((i % 4) as f64),
                if i == 14 { None } else { Some((i % 3) as f64) },
            ]),
            key: format!("event-{i}"),
            x: Some(0.1 + i as f64 * 0.05),
            y: Some(0.2 + (i % 4) as f64 * 0.1),
        })
        .collect();
    r.scalar_gate = Some(ScalarGate {
        minimum: Some(1.),
        maximum: Some(1.),
        values: r
            .events
            .iter()
            .enumerate()
            .map(|(i, e)| ScalarValue {
                key: e.key.clone(),
                value: if i < 15 {
                    Some(1.)
                } else if i == 15 {
                    None
                } else {
                    Some(0.)
                },
            })
            .collect(),
    });
    r.embedding = Some(EmbeddingSpec {
        engine: "sklearn-tsne-1.9.1".into(),
        features: vec!["a".into(), "b".into(), "c".into()],
        seed: 42,
        perplexity: 2.,
        max_iter: 500,
        max_points: 8,
    });
    let first = worker.run(&r, || false).unwrap();
    assert_eq!(first.embedding.as_ref().unwrap().eligible, 14);
    assert_eq!(first.embedding.as_ref().unwrap().missing_features, 1);
    assert_eq!(first.embedding.as_ref().unwrap().points.len(), 8);
    let mut reversed = r.clone();
    reversed.events.reverse();
    let second = worker.run(&reversed, || false).unwrap();
    assert_eq!(
        serde_json::to_value(&first.embedding).unwrap(),
        serde_json::to_value(&second.embedding).unwrap()
    );
    let mut bad = first.clone();
    bad.embedding.as_mut().unwrap().points[0].key = "event-19".into();
    assert!(accept(&r, &serde_json::to_vec(&bad).unwrap()).is_err());
    bad = first.clone();
    bad.embedding.as_mut().unwrap().spec.seed = 99;
    assert!(accept(&r, &serde_json::to_vec(&bad).unwrap()).is_err());
    bad = first.clone();
    bad.embedding.as_mut().unwrap().missing_features = 0;
    assert!(accept(&r, &serde_json::to_vec(&bad).unwrap()).is_err());
    let dir = tempfile::tempdir().unwrap();
    let id;
    {
        let store = store::JobStore::open(dir.path()).unwrap();
        id = store.begin(&r).unwrap();
        store.finish(&id, Ok(first.clone())).unwrap();
    }
    let store = store::JobStore::open(dir.path()).unwrap();
    match store.query(&id).unwrap() {
        store::State::Terminal(t) => assert_eq!(
            serde_json::to_value(t.result.unwrap().embedding).unwrap(),
            serde_json::to_value(first.embedding).unwrap()
        ),
        _ => panic!("missing durable embedding"),
    }
}
